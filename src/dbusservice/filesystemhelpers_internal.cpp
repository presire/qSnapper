/**
 * @file filesystemhelpers_internal.cpp
 * @brief filesystemhelpers系の共有内部helper実装
 *
 * パス検証・fd走査・in-root解決・削除などの内部helperを集める
 * 公開APIからは直接呼ばない
 */
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QStringList>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/ioctl.h>
#include <sys/fsuid.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <vector>
#include "filesystemhelpers.h"
#include "filesystemhelpers_internal.h"

namespace qsnapper::security {
        /**
         * @brief 祖先の所有権を順序どおりに集積してrootによる他者の変更を防ぐ
         * @param ownership これまでの祖先の状態
         * @param directoryStat 今回開いたディレクトリの状態
         */
        void accumulateChainOwnership(DirectoryChainOwnership &ownership, const struct stat &directoryStat)
        {
            if (ownership.trusted) {
                if (parentDirectoryIsSafeForMetadata(directoryStat)) {
                    return;
                }
                ownership.trusted = false;
                if (directoryStat.st_uid != 0) {
                    ownership.principal = directoryStat.st_uid;
                    ownership.hasPrincipal = true;
                }
                else {
                    ownership.conflict = true;
                }
            }
            else if (!ownership.hasPrincipal || directoryStat.st_uid != ownership.principal) {
                ownership.conflict = true;
            }
        }

        /**
         * @brief root共有親や所有者混在のチェーンへの変更を拒否する
         * @param ownership 同一walkで検証した状態
         * @return 信頼チェーンか単一principalが得られた場合true
         */
        bool chainAllowsMutation(const DirectoryChainOwnership &ownership)
        {
            return ownership.trusted || ownership.principalKnown();
        }

        /**
         * @brief groupを先に落とし、失敗したidentityで作成しないようqueryで確認する
         * @param uid 作成を行うprincipal
         * @param gid 復元元のgroup
         */
        ScopedFsIdentity::ScopedFsIdentity(uid_t uid, gid_t gid)
        {
            const int savedErrno = errno;
            ::setfsgid(gid);
            ::setfsuid(uid);
            m_ok = static_cast<gid_t>(::setfsgid(static_cast<gid_t>(-1))) == gid
                && static_cast<uid_t>(::setfsuid(static_cast<uid_t>(-1))) == uid;
            errno = savedErrno;
        }

        /**
         * @brief 後続の権限検証を正しく行うためroot identityを復元し、復元不能なら続行しない
         */
        ScopedFsIdentity::~ScopedFsIdentity()
        {
            const int savedErrno = errno;
            ::setfsuid(0);
            ::setfsgid(0);
            if (::setfsuid(static_cast<uid_t>(-1)) != 0
                    || ::setfsgid(static_cast<gid_t>(-1)) != 0) {
                qFatal("Failed to restore root filesystem identity");
            }
            errno = savedErrno;
        }

        /**
         * @brief principalへの切り替えを検証できた場合だけ作成を許す
         * @return uidとgidが指定どおりになった場合true
         */
        bool ScopedFsIdentity::ok() const
        {
            return m_ok;
        }

        /**
         * @brief 絶対パスを各成分へ分解する
         *
         * @param path 分解対象の絶対パス
         * @param components 分解結果の格納先
         * @return 絶対パスとして分解できた場合: true
         */
        bool splitAbsolutePath(const QString &path, QStringList *components)
        {
            if (!components) {
                errno = EINVAL;
                return false;
            }

            // 呼び出し側から渡された出力先は毎回クリアして使用する
            components->clear();
            if (!path.startsWith(QLatin1Char('/'))) {
                errno = EINVAL;
                return false;
            }

            *components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            return true;
        }

        /**
         * @brief 制御文字 (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含むか判定する
         *
         * 埋め込みNULはsyscall引数の切り詰めを招き、検証したパスと実際に変異するパスが
         * ズレる原因となるため、パスには一切許容しない (inputvalidatorと同じ方針)
         */
        bool containsControlChar(const QString &value)
        {
            for (const QChar &c : value) {
                const ushort u = c.unicode();
                if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief dirfd相対パスを検証してUTF-8へ符号化する
         *
         * 空パス・絶対パス・"." / ".." 成分・制御文字を拒否する
         * openat系は絶対パスを渡すとdirfdを無視するため、pin済みdirfdから意図しない位置へ解決されるのを入力段階で防ぐ
         *
         * @param relativePath 検証対象の相対パス
         * @param encodedOut 符号化結果の格納先 (省略可)
         * @return 有効な相対パスの場合: true
         */
        bool validateRelativePathAt(const QString &relativePath, QByteArray *encodedOut)
        {
            if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/'))
                    || containsControlChar(relativePath)) {
                errno = EINVAL;
                return false;
            }

            const QStringList components =
                    relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (components.isEmpty()) {
                errno = EINVAL;
                return false;
            }
            for (const QString &component : components) {
                if (component == QLatin1String(".") || component == QLatin1String("..")) {
                    errno = EINVAL;
                    return false;
                }
            }

            if (encodedOut) {
                *encodedOut = relativePath.toUtf8();
            }
            return true;
        }

        /**
         * @brief ルートディレクトリ "/" を開く
         *
         * @return 成功時: dirfd、失敗時: -1
         */
        int openRootDirectory()
        {
            return ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }

        /**
         * @brief 基点ディレクトリrootPathを全成分O_NOFOLLOWで開く
         *
         * 単純なopen(rootPath, O_NOFOLLOW)は末尾成分にしかO_NOFOLLOWが効かず、rootPathの中間成分がsymlink化されていた場合に追従してしまう
         * そのため既存のsafeOpenDirectory() (「/」起点の成分単位no-follow walk) へ委譲し、root祖先の差し替えも拒否する
         *
         * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
         * @return 成功時: dirfd、失敗時: -1
         */
        int openRootPathDirectory(const QString &rootPath)
        {
            if (rootPath.isEmpty() || !rootPath.startsWith(QLatin1Char('/'))
                    || containsControlChar(rootPath)) {
                errno = EINVAL;
                return -1;
            }

            return safeOpenDirectory(rootPath);
        }

        /**
         * @brief openat2が使えないカーネルで失敗したことを記録する
         *
         * 呼び出し側はerrnoをENOSYSのまま返し、代替実装へ切り替えない
         * 5.6未満のカーネルでは復元先の安全な解決を保証できないため、警告だけ出して失敗させる
         * @note qWarningのlog handlerがerrnoを変更し得るため、保存したENOSYSを戻す前に復元する
         */
        void warnIfOpenat2Unavailable()
        {
            if (errno != ENOSYS) {
                return;
            }

            const int savedErrno = errno;
            qWarning("openat2 is not available; kernel 5.6 or later is required");
            errno = savedErrno;
        }

        /**
         * @brief baseFd配下の成分列を1成分ずつopenat2で辿りながらディレクトリfdを開く
         *
         * 複数成分を1回のopenat2で解決すると途中のmount境界を観測できないため、1成分ごとに開いてstatxする
         * nested btrfs subvolumeはmountではないのでRESOLVE_NO_XDEVでは止まらず、基点とst_devが異なることをstatxで検出してEXDEVにする
         * openat2のEAGAINは競合による一時失敗なので同じ操作を最大3回まで再試行する
         * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSのまま失敗する
         *
         * @param baseFd 走査基点のディレクトリfd
         * @param components 相対パス成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分をmkdirat()で作成するか
         * @param mode createMissing = true時の作成モード
         * @param sameMountOnly mount境界 (別mountのroot、または親と異なるデバイス) を越える成分をEXDEVで拒否する場合true
         * @param ownershipOut 基点と各成分の所有権状態の返却先 (再walkを避けるため同時に集積する)
         * @return 成功時: 最終ディレクトリのfd、失敗時: -1
         */
        int openDirectoryChainAt(int baseFd, const QStringList &components, int componentCount,
                                 bool createMissing, mode_t mode, bool sameMountOnly,
                                 DirectoryChainOwnership *ownershipOut)
        {
            if (baseFd < 0 || componentCount < 0 || componentCount > components.size()) {
                errno = EINVAL;
                return -1;
            }

            dev_t baseDev = 0;
            if (sameMountOnly || ownershipOut) {
                struct stat baseInfo;
                if (::fstat(baseFd, &baseInfo) != 0) {
                    return -1;
                }
                baseDev = baseInfo.st_dev;
                if (ownershipOut) {
                    *ownershipOut = DirectoryChainOwnership {};
                    accumulateChainOwnership(*ownershipOut, baseInfo);
                }
            }

            struct open_how how = {};
            how.flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
            how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
            if (sameMountOnly) {
                // 復元先の木から別のmountへ降りて書き込まないようにする
                how.resolve |= RESOLVE_NO_XDEV;
            }

            auto openBeneath = [&how](int parentFd, const QByteArray &name) {
                constexpr int MAX_EAGAIN_RETRIES = 3;
                for (int attempt = 0; attempt < MAX_EAGAIN_RETRIES; ++attempt) {
                    const long result = ::syscall(SYS_openat2, parentFd, name.constData(),
                                                  &how, sizeof(how));
                    if (result >= 0) {
                        return static_cast<int>(result);
                    }
                    if (errno != EAGAIN || attempt == MAX_EAGAIN_RETRIES - 1) {
                        return -1;
                    }
                }
                return -1;
            };

            int dirFd = openBeneath(baseFd, QByteArrayLiteral("."));
            if (dirFd < 0) {
                warnIfOpenat2Unavailable();
                return -1;
            }

            for (int i = 0; i < componentCount; ++i) {
                const QByteArray encodedName = components.at(i).toUtf8();
                if (createMissing) {
                    if (::mkdirat(dirFd, encodedName.constData(), mode) < 0 && errno != EEXIST) {
                        const int savedErrno = errno;
                        ::close(dirFd);
                        errno = savedErrno;
                        return -1;
                    }
                }

                const int nextFd = openBeneath(dirFd, encodedName);
                if (nextFd < 0) {
                    const int savedErrno = errno;
                    if (savedErrno == ENOSYS) {
                        warnIfOpenat2Unavailable();
                    }
                    ::close(dirFd);
                    errno = savedErrno;
                    return -1;
                }

                if (sameMountOnly) {
                    struct statx openedInfo = {};
                    if (::statx(nextFd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                                STATX_TYPE | STATX_MODE | STATX_UID | STATX_INO,
                                &openedInfo) != 0) {
                        const int savedErrno = errno;
                        if (savedErrno == ENOSYS) {
                            warnIfOpenat2Unavailable();
                        }
                        ::close(nextFd);
                        ::close(dirFd);
                        errno = savedErrno;
                        return -1;
                    }

                    if (isMountBoundary(openedInfo, baseDev)) {
                        // 境界の下は復元先のサブボリュームではないため、EXDEVで拒否する
                        ::close(nextFd);
                        ::close(dirFd);
                        errno = EXDEV;
                        return -1;
                    }
                }

                if (ownershipOut) {
                    struct stat stepInfo;
                    if (::fstat(nextFd, &stepInfo) != 0) {
                        const int savedErrno = errno;
                        ::close(nextFd);
                        ::close(dirFd);
                        errno = savedErrno;
                        return -1;
                    }
                    accumulateChainOwnership(*ownershipOut, stepInfo);
                }

                ::close(dirFd);
                dirFd = nextFd;
            }

            return dirFd;
        }

        /**
         * @brief パス成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く ("/"起点)
         *
         * @param components 絶対パスを分解した成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分を mkdirat() で作成するか
         * @param mode createMissing = true時の作成モード
         * @return 成功時: 最終ディレクトリのfd、失敗時: -1
         */
        int openDirectoryChainNoFollow(const QStringList &components, int componentCount,
                                       bool createMissing, mode_t mode)
        {
            const int rootFd = openRootDirectory();
            if (rootFd < 0) {
                return -1;
            }

            // 絶対パスの解決ではsubvolumeやmount pointを跨ぐことが正当なため、mount境界を制限しない
            const int leafFd = openDirectoryChainAt(rootFd, components, componentCount,
                                                    createMissing, mode, false);
            const int savedErrno = errno;
            ::close(rootFd);
            errno = savedErrno;
            return leafFd;
        }

        /**
         * @brief 宛先絶対パスをrootPath配下の相対成分列へ分解する (内部用)
         *
         * @param rootPath 基点ルートディレクトリ
         * @param absolutePath 宛先の絶対パス
         * @param components 相対成分列の格納先 (非空であることを保証する)
         * @return 分解できた場合: true
         */
        bool splitDestinationComponents(const QString &rootPath, const QString &absolutePath,
                                        QStringList *components)
        {
            QString relative;
            if (!splitDestinationBeneathRoot(rootPath, absolutePath, &relative)) {
                return false;
            }

            *components = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (components->isEmpty()) {
                errno = EINVAL;
                return false;
            }
            return true;
        }

        /**
         * @brief rootPathを開き、相対成分列のleaf「親」までO_NOFOLLOWで辿る
         *
         * root自身はO_NOFOLLOWで開くため、rootがsymlinkへ差し替えられている場合も拒否する
         * 中間成分は openat(..., O_DIRECTORY | O_NOFOLLOW) の連鎖で解決するため、どの階層のsymlink差し替えでもELOOPで失敗する
         *
         * @param rootPath 基点ルートディレクトリ
         * @param relativeComponents splitDestinationComponents由来の相対成分列 (非空)
         * @param createMissing 中間成分を作成するか
         * @param mode createMissing = true時の作成モード
         * @param leafName 最終成分名の返却先
         * @param ownershipOut 同一walkで集積する所有権状態 (rootが変更主体を確定できなければEPERM)
         * @return 成功時: 親dirfd (呼び出し側でclose)、失敗時: -1
         */
        int openLeafParentBeneathRoot(const QString &rootPath,
                                      const QStringList &relativeComponents,
                                      bool createMissing, mode_t mode, QByteArray *leafName,
                                      DirectoryChainOwnership *ownershipOut)
        {
            if (relativeComponents.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafName) {
                *leafName = relativeComponents.constLast().toUtf8();
            }

            const UniqueFd rootFd(openRootPathDirectory(rootPath));
            if (!rootFd.isValid()) {
                return -1;
            }

            UniqueFd parentFd(openDirectoryChainAt(rootFd.get(), relativeComponents,
                                        static_cast<int>(relativeComponents.size()) - 1,
                                        createMissing, mode, true, ownershipOut));
            if (!parentFd.isValid()) {
                return -1;
            }
            if (::geteuid() == 0 && ownershipOut && !chainAllowsMutation(*ownershipOut)) {
                errno = EPERM;
                return -1;
            }
            return parentFd.release();
        }

        /**
         * @brief パスの親ディレクトリをO_NOFOLLOWで開き、leaf名も返す
         *
         * @param path 対象パス
         * @param createMissing 親ディレクトリが無い場合に作成するか
         * @param mode createMissing = true時の作成モード
         * @param leafName 最終要素の返却先
         * @return 成功時: 親ディレクトリfd、失敗時: -1
         */
        int openParentDirectoryNoFollow(const QString &path, bool createMissing, mode_t mode,
                                        QByteArray *leafName)
        {
            QStringList components;
            if (!splitAbsolutePath(path, &components) || components.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafName) {
                *leafName = components.constLast().toUtf8();
            }

            return openDirectoryChainNoFollow(components, components.size() - 1, createMissing, mode);
        }

        /**
         * @brief statxの結果がmount境界 (別mountのroot、または親と異なるデバイス) を示すかを判定する
         *
         * STATX_ATTR_MOUNT_ROOTはattributes_maskで対応が示された場合のみ信用する
         * btrfsのネストしたsubvolumeはmountではないが、st_devが親と異なるため境界として扱う
         *
         * @param stx 判定対象のstatx結果
         * @param parentDev 親ディレクトリのst_dev
         * @return 境界である場合: true
         */
        bool isMountBoundary(const struct statx &stx, dev_t parentDev)
        {
            const bool mountRootKnown = (stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) != 0;
            if (mountRootKnown && (stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0) {
                return true;
            }
            return makedev(stx.stx_dev_major, stx.stx_dev_minor) != parentDev;
        }

        /**
         * @brief 削除処理で1つのエントリをどう扱ったかを表す
         *
         * ディレクトリは中身を削除してから本体を消す必要があるため、削除済みと降下を区別する
         */
        enum class RemovalStep {
            Removed,    // 削除した (既に存在しなかった場合を含む)
            Descend,    // ディレクトリを開いた (中身を削除した後に本体を削除する)
            Failed      // 失敗した (errno設定済み)
        };

        /**
         * @brief 走査中のディレクトリ1段分
         *
         * 中身を削除し終えたら、親ディレクトリからnameInParentをAT_REMOVEDIRで削除する
         */
        struct RemovalFrame {
            DIR *dir = nullptr;                 // 走査中のディレクトリ (fdを所有する)
            QByteArray nameInParent;            // 親ディレクトリ上の名前
            DirectoryChainOwnership ownership;  // 基点からこのディレクトリまでの所有権の状態
        };

        constexpr int kMaxRemovalDepth = 256;   // 明示スタックで降りる深さの上限 (攻撃者が作った深い階層でfdとメモリを使い切らないため)

        /**
         * @brief 削除処理で同時に開いてよいディレクトリの段数を返す
         *
         * 各段のfdは子を削除した後のAT_REMOVEDIRで必要なため、降りている間は閉じられない
         * プロセスのfd上限の半分までに抑え、D-Bus接続など他の処理が使うfdを残す
         *
         * @return 降りてよい深さの上限 (1以上)
         */
        int removalDepthLimit()
        {
            struct rlimit limit;
            if (::getrlimit(RLIMIT_NOFILE, &limit) < 0 || limit.rlim_cur == RLIM_INFINITY) {
                return kMaxRemovalDepth;
            }
            const rlim_t half = limit.rlim_cur / 2;
            if (half < 1) {
                return 1;
            }
            return half < static_cast<rlim_t>(kMaxRemovalDepth) ? static_cast<int>(half)
                                                                : kMaxRemovalDepth;
        }

        /**
         * @brief 親dirfd配下の1エントリを削除するか、ディレクトリなら走査のために開く
         *
         * 削除対象そのものと、降りていく各ディレクトリについてmount境界を確認する
         * 境界 (USB / NFS / FUSE / bind mount / ネストしたsubvolume等) を見つけた時点でerrno=EXDEVで失敗を返す
         * 境界を黙ってスキップして成功扱いにすると、呼び出し元は削除が完了したと誤認するため、必ず失敗として返す
         *
         * statxはAT_STATX_DONT_SYNCで呼び、FUSE / NFS等の境界先へ属性取得要求を送らない (応答しないFUSEでサービスが停止するのを防ぐ)
         *
         * @param parentFd 親ディレクトリfd
         * @param parentDev 削除をこのデバイス内に限定するための最上位の親デバイス番号
         * @param entryName 対象のleaf名
         * @param dirOut Descendを返した場合に、開いたディレクトリを受け取る (呼び出し元がclosedirする)
         * @param ownership 入力は親までの所有権の状態、Descendを返した場合は開いたディレクトリを含む状態に更新する
         * @return 処理結果
         * @note root実行時は、開いたディレクトリを加えた状態がchainAllowsMutationを満たさなければerrno=EPERMで失敗する
         *       親までが信頼できても削除対象やその内部で初めて非信頼になる場合があり、
         *       利用者が他者のディレクトリをそこへrenameで持ち込めるため、各段で判定する
         */
        RemovalStep removeOrOpenEntryAt(int parentFd, dev_t parentDev,
                                        const QByteArray &entryName, DIR **dirOut,
                                        DirectoryChainOwnership &ownership)
        {
            constexpr int kStatxFlags = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC;
            struct statx stx;
            if (::statx(parentFd, entryName.constData(), kStatxFlags,
                        STATX_TYPE | STATX_INO, &stx) < 0) {
                return errno == ENOENT ? RemovalStep::Removed : RemovalStep::Failed;
            }

            if (isMountBoundary(stx, parentDev)) {
                errno = EXDEV;
                return RemovalStep::Failed;
            }

            // symlink / regular file / fifo / deviceは、unlinkat()側で削除する
            if (!S_ISDIR(stx.stx_mode)) {
                if (::unlinkat(parentFd, entryName.constData(), 0) == 0 || errno == ENOENT) {
                    return RemovalStep::Removed;
                }
                return RemovalStep::Failed;
            }

            UniqueFd childFd(::openat(parentFd, entryName.constData(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!childFd.isValid()) {
                return RemovalStep::Failed;
            }

            // statxからopenatまでの間に名前を差し替えられていないか、開いたディレクトリ自体が境界でないかを確認する
            struct statx opened;
            if (::statx(childFd.get(), "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC,
                        STATX_TYPE | STATX_INO, &opened) < 0) {
                return RemovalStep::Failed;
            }
            if (isMountBoundary(opened, parentDev) || opened.stx_ino != stx.stx_ino) {
                errno = EXDEV;
                return RemovalStep::Failed;
            }

            if (::geteuid() == 0) {
                struct stat directoryStat;
                if (::fstat(childFd.get(), &directoryStat) < 0) {
                    return RemovalStep::Failed;
                }
                accumulateChainOwnership(ownership, directoryStat);
                if (!chainAllowsMutation(ownership)) {
                    errno = EPERM;
                    return RemovalStep::Failed;
                }
            }

            DIR *dir = ::fdopendir(childFd.get());
            if (!dir) {
                return RemovalStep::Failed;
            }
            childFd.release();

            *dirOut = dir;
            return RemovalStep::Descend;
        }

        /**
         * @brief 親dirfd配下の1エントリを、symlink非追従かつmount境界を越えずに削除する
         *
         * 攻撃者が深い階層を作ってもスタックを使い切らないよう、再帰呼び出しではなく明示的なスタックで降りる
         * 深さがremovalDepthLimit()を超える場合は、途中まで削除した状態でerrno=ELOOPの失敗を返す
         * mount境界を見つけた場合はerrno=EXDEVで失敗し、境界の内側は1件も削除しない
         * root実行時は、所有者を検査したfdでのみ内容を削除し、単一principalに限定できないディレクトリへは降りない
         * 最後の名前によるAT_REMOVEDIRは空の場合に限るためprincipal自身でも削除可能な範囲に留まる
         * 非空のディレクトリへ差し替えられた場合はENOTEMPTYで失敗し、その中身を削除しない
         *
         * @param parentFd 親ディレクトリfd
         * @param parentDev 親ディレクトリのst_dev
         * @param entryName 削除対象のleaf名
         * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
         * @return 削除成功時 (対象が存在しない場合を含む): true
         */
        bool removeEntryWithinDeviceAt(int parentFd, dev_t parentDev, const QByteArray &entryName,
                                       const DirectoryChainOwnership &parentOwnership)
        {
            DIR *topDir = nullptr;
            DirectoryChainOwnership topOwnership = parentOwnership;
            const RemovalStep topStep = removeOrOpenEntryAt(parentFd, parentDev, entryName, &topDir,
                                                           topOwnership);
            if (topStep != RemovalStep::Descend) {
                return topStep == RemovalStep::Removed;
            }

            const int maxDepth = removalDepthLimit();
            std::vector<RemovalFrame> stack;
            stack.push_back(RemovalFrame{topDir, entryName, topOwnership});

            // 失敗時は開いている全段を閉じ、原因のerrnoを呼び出し元へ返す (closedirがerrnoを上書きするため)
            const auto abortRemoval = [&stack]() {
                const int savedErrno = errno;
                for (RemovalFrame &frame : stack) {
                    ::closedir(frame.dir);
                }
                stack.clear();
                errno = savedErrno;
                return false;
            };

            while (!stack.empty()) {
                DIR *currentDir = stack.back().dir;
                errno = 0;
                const dirent *entry = ::readdir(currentDir);
                if (!entry) {
                    if (errno != 0) {
                        return abortRemoval();
                    }

                    // 中身を削除し終えたディレクトリを、1段上のディレクトリから削除する
                    const QByteArray finishedName = stack.back().nameInParent;
                    const int closeResult = ::closedir(currentDir);
                    stack.pop_back();
                    if (closeResult < 0) {
                        return abortRemoval();
                    }

                    // 最上位のディレクトリは呼び出し元の親から削除する (1段上を表すfdがスタックに残っていないため)
                    const int ownerFd = stack.empty() ? parentFd : ::dirfd(stack.back().dir);
                    if (::unlinkat(ownerFd, finishedName.constData(), AT_REMOVEDIR) < 0
                            && errno != ENOENT) {
                        return abortRemoval();
                    }
                    continue;
                }

                // "." と ".." は削除対象から除外する
                const char *name = entry->d_name;
                if ((name[0] == '.' && name[1] == '\0')
                        || (name[0] == '.' && name[1] == '.' && name[2] == '\0')) {
                    continue;
                }

                const QByteArray childName(name);
                DIR *childDir = nullptr;
                // 兄弟の判定が混ざらないよう、子ごとに親の状態の写しから判定する
                DirectoryChainOwnership childOwnership = stack.back().ownership;
                const RemovalStep step = removeOrOpenEntryAt(::dirfd(currentDir), parentDev,
                                                            childName, &childDir, childOwnership);
                if (step == RemovalStep::Failed) {
                    return abortRemoval();
                }
                if (step == RemovalStep::Descend) {
                    // fdを使い切る前に止める (ELOOPは呼び出し元が部分削除として扱う)
                    if (static_cast<int>(stack.size()) >= maxDepth) {
                        ::closedir(childDir);
                        errno = ELOOP;
                        return abortRemoval();
                    }
                    stack.push_back(RemovalFrame{childDir, childName, childOwnership});
                }
            }

            return true;
        }

        /**
         * @brief 親dirfd配下の1エントリを、symlink非追従かつmount境界を越えずに削除する
         *
         * 親ディレクトリと異なるデバイス上のエントリ (mount境界) を検出した場合はerrno=EXDEVで失敗する
         *
         * @param parentFd 親ディレクトリfd
         * @param entryName 削除対象のleaf名
         * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
         * @return 削除成功時: true
         */
        bool removeEntryAt(int parentFd, const QByteArray &entryName,
                           const DirectoryChainOwnership &parentOwnership)
        {
            struct stat parentStat;
            if (::fstat(parentFd, &parentStat) < 0) {
                return false;
            }
            return removeEntryWithinDeviceAt(parentFd, parentStat.st_dev, entryName, parentOwnership);
        }

        /**
         * @brief 開いたfdがregular fileかを検証する
         *
         * @param fd 検証対象fd
         * @return regular fileの場合: true
         */
        bool validateRegularFileDescriptor(int fd)
        {
            struct stat st;
            if (::fstat(fd, &st) < 0) {
                return false;
            }

            if (!S_ISREG(st.st_mode)) {
                errno = EINVAL;
                return false;
            }

            return true;
        }

        /**
         * @brief 親dirfd上のleafを、regular fileである場合に限り読み取り用に開く
         *
         * FIFOやデバイスはopen()自体がブロックしたり副作用を起こしたりするため、開く前にfstatat()でregular fileであることを確認する
         * 確認とopenの間にFIFOへ差し替えられても待ち続けないよう、O_NONBLOCK | O_NOCTTYで開き (regular fileの読み込みには影響しない)、fstat()で再確認する
         *
         * @param parentFd 親ディレクトリのfd
         * @param leafName 親ディレクトリ内の名前
         * @return 成功時: file descriptor、失敗時: -1 (errno設定)
         */
        int openRegularFileReadInParent(int parentFd, const QByteArray &leafName)
        {
            struct stat leafStat;
            if (::fstatat(parentFd, leafName.constData(), &leafStat, AT_SYMLINK_NOFOLLOW) < 0) {
                return -1;
            }
            if (!S_ISREG(leafStat.st_mode)) {
                // symlinkはO_NOFOLLOWで開いた場合と同じELOOPを返し、従来のerrno契約を保つ
                errno = S_ISLNK(leafStat.st_mode) ? ELOOP : EINVAL;
                return -1;
            }

            const int fd = ::openat(parentFd, leafName.constData(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
            if (fd < 0) {
                return -1;
            }

            if (!validateRegularFileDescriptor(fd)) {
                const int validationErrno = errno;
                ::close(fd);
                errno = validationErrno;
                return -1;
            }

            return fd;
        }

        /**
         * @brief dirfd基点で親成分列をopenat2(RESOLVE_IN_ROOT)で解決する (内部用)
         *
         * snapshot dirfdは"/"のbtrfs snapshotであるため、"var/run -> /run" のような
         * 絶対symlinkはsnapshot内で正当な存在であり、chrootと同じくroot内解決が要求される
         * RESOLVE_IN_ROOTはカーネル内でroot外への逸脱を構造的に不可能にする
         * RESOLVE_NO_MAGICLINKSにより/proc系のmagic linkも追従しない
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param parentComponents leaf成分を除いた親成分列 (非空)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootAtOpenat2(int dirFd, const QStringList &parentComponents)
        {
            struct open_how how = {};
            how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
            how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;

            const QByteArray encodedParent =
                    parentComponents.join(QLatin1Char('/')).toUtf8();

            constexpr int MAX_EAGAIN_RETRIES = 3;
            for (int attempt = 0; attempt < MAX_EAGAIN_RETRIES; ++attempt) {
                const long result = ::syscall(SYS_openat2, dirFd, encodedParent.constData(),
                                              &how, sizeof(how));
                if (result >= 0) {
                    return static_cast<int>(result);
                }
                if (errno != EAGAIN || attempt == MAX_EAGAIN_RETRIES - 1) {
                    return -1;
                }
            }
            return -1;
        }

        /**
         * @brief dirfd基点で親ディレクトリをin-root解決により開き、leaf名も返す
         *
         * 検証済みrelativePathと同一の入力から親dirfdとleaf名を導出することで、
         * 「検証した値」と「操作する対象」の一致 (TOCTOUなし) を保証する
         * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSのまま失敗する
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param components 検証済み相対パスの成分列 (非空)
         * @param leafNameOut 最終成分名の返却先 (省略可)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootAt(int dirFd, const QStringList &components,
                                        QByteArray *leafNameOut)
        {
            if (dirFd < 0 || components.isEmpty()) {
                errno = EINVAL;
                return -1;
            }

            if (leafNameOut) {
                *leafNameOut = components.constLast().toUtf8();
            }

            // leafが1成分のみの場合、親はdirfd自身
            // 呼び出し側が常にcloseできるように、所有fdを複製して返す
            if (components.size() == 1) {
                return ::openat(dirFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            }

            const QStringList parentComponents =
                    components.mid(0, components.size() - 1);

            const int parentFd =
                    openParentDirectoryInRootAtOpenat2(dirFd, parentComponents);
            if (parentFd < 0) {
                warnIfOpenat2Unavailable();
            }
            return parentFd;
        }
} // namespace qsnapper::security
