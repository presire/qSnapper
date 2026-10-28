/**
 * @file filesystemhelpers_beneathroot.cpp
 * @brief rootPath配下の宛先に、ディレクトリと通常ファイルを作成し、書き込みとrenameと削除を行い、symlinkを作成する
 *
 * rootPathのdirfdから最終成分までを1成分ずつO_NOFOLLOWで辿るため、途中のsymlinkを伝ってrootPathの外へ出ることはない
 * 親ディレクトリのfdを保持したまま操作するので、検査した経路と実際に変更する経路が食い違うTOCTOU(検査と使用の間に状態が変わる隙を突く攻撃)を防げる
 * 復元先の作成とmetadataの適用は、filesystemhelpers_internal.hが提供する所有者の判定とmount境界の検査に依存する
 *
 * 入力 : rootPath、rootPathから見た相対パス、作成・移動・削除の対象
 * 出力 : 変更操作の成否とerrno、開いたfd (呼び出し側がcloseする)
 * 呼び出し元 : 復元計画の実行と、その前提を確かめるテスト
 * 依存 : filesystemhelpers.h、filesystemhelpers_internal.h、Linuxのopenat2・renameat2・statx・sendfile
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
     * @brief 宛先の絶対パスがrootPath配下かを文字列として判定し、rootPathから見た相対パスを取り出す
     *
     * 文字列を解析するだけでファイルシステムには触れない
     * 判定の後に経路をsymlinkへ差し替えられる余地は残るため、この関数を通っただけでは安全性は保証されない
     * 取り出した相対パスを使い、rootPathのdirfdから1成分ずつO_NOFOLLOWで開く各変更関数が実際の保証を担う
     *
     * @param rootPath 基点となるルートディレクトリの絶対パス
     * @param absolutePath 検証する宛先の絶対パス
     * @param relativeOut rootPathから見た相対パスの格納先
     * @return rootPath配下と判定した場合はtrue、拒否した場合はfalse (errno=EINVAL)
     */
    bool splitDestinationBeneathRoot(const QString &rootPath, const QString &absolutePath,
                                     QString *relativeOut)
    {
        if (!relativeOut) {
            errno = EINVAL;
            return false;
        }

        relativeOut->clear();

        if (!absolutePath.startsWith(QLatin1Char('/'))
                || !rootPath.startsWith(QLatin1Char('/'))) {
            errno = EINVAL;
            return false;
        }

        // 制御文字(埋め込みNULを含む)があると、パスがC文字列として途中で切られる
        // 検査した文字列と実際に開くパスが食い違うため、入力段階で拒否する
        if (containsControlChar(rootPath) || containsControlChar(absolutePath)) {
            errno = EINVAL;
            return false;
        }

        const QStringList rootComponents = rootPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList pathComponents =
                absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);

        // 宛先が空や"/"の場合は変更する名前を持たないため、宛先として扱わない
        if (pathComponents.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // ".."を含むと文字列の比較だけではrootPathの外を指せてしまうため、入力段階で拒否する
        for (const QString &component : pathComponents) {
            if (component == QLatin1String(".") || component == QLatin1String("..")) {
                errno = EINVAL;
                return false;
            }
        }

        // rootPath自身は変更対象にできないため、rootPathより後ろに名前を1つ以上要求する
        if (pathComponents.size() <= rootComponents.size()) {
            errno = EINVAL;
            return false;
        }

        // /srv/root-otherのような兄弟ディレクトリをrootPath配下と誤認しないよう、成分の完全一致で比較する
        for (int i = 0; i < rootComponents.size(); ++i) {
            if (pathComponents.at(i) != rootComponents.at(i)) {
                errno = EINVAL;
                return false;
            }
        }

        *relativeOut = pathComponents.mid(rootComponents.size()).join(QLatin1Char('/'));
        return true;
    }

    /**
     * @brief rootPathを基点に相対パスをO_NOFOLLOWで辿り、末端ディレクトリのfdを返す
     *
     * 呼び出し側は返されたfdを変更操作に使い、直ちにcloseする
     * fdを認可の判断から実行まで保持すると、その間に経路を差し替えられるTOCTOUが生じるため、保持してはならない
     *
     * @param rootPath 基点となるルートディレクトリ (実在するディレクトリであること)
     * @param relativePath rootPathから見た相対パス
     *        空・'/'で始まるパス・制御文字・"." ".."成分は拒否する
     * @param createMissing 存在しない成分をmodeで指定したモードで作成する場合true
     * @param mode createMissingがtrueのときに作成するディレクトリのモード
     * @return 成功時は末端ディレクトリのfd、失敗時は-1 (errno設定)
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode)
    {
        // 空や絶対パスを受け付けると、rootPathから見た相対パスとして解釈できない
        // 制御文字はC文字列として途中で切られるため、openat()へ渡す前に拒否する
        if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/'))
                || containsControlChar(relativePath)) {
            errno = EINVAL;
            return -1;
        }

        // 空になった場合は、開く対象のディレクトリがない
        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            errno = EINVAL;
            return -1;
        }

        for (const QString &component : components) {
            // ".."を許すとrootPathの外を指せるため、名前の成分として受け付けない
            if (component == QLatin1String(".") || component == QLatin1String("..")) {
                errno = EINVAL;
                return -1;
            }
        }

        const UniqueFd rootFd(openRootPathDirectory(rootPath));
        if (!rootFd.isValid()) {
            return -1;
        }

        return openDirectoryChainAt(rootFd.get(), components,
                                    static_cast<int>(components.size()), createMissing, mode, true);
    }

    /**
     * @brief rootPath配下に宛先ディレクトリを作成し、必要なら中間成分も作成する
     *
     * 最終成分が既にある場合は、fstatat()にAT_SYMLINK_NOFOLLOWを付けて種類を確かめる
     * ディレクトリ以外(symlinkを含む)ならEEXISTで拒否する
     * 既存のsymlinkをディレクトリと誤認して、その先のディレクトリを変更しないようにするためである
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成するディレクトリのモード
     * @return 成功時はtrue、失敗時はfalse (errno設定)
     */
    bool safeCreateDirectoryBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                        mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, true, mode, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        if (::mkdirat(parentFd.get(), leafName.constData(), mode) < 0 && errno != EEXIST) {
            return false;
        }

        // 既存だった場合も開き直さず、親のfdからsymlinkなどでないことを確かめる
        struct stat st;
        if (::fstatat(parentFd.get(), leafName.constData(), &st, AT_SYMLINK_NOFOLLOW) < 0
                || !S_ISDIR(st.st_mode)) {
            errno = EEXIST;
            return false;
        }

        return true;
    }

    namespace {
        /**
         * @brief 欠けている親ディレクトリを1階層だけ作成し、スナップショット側の同じディレクトリのmetadataを適用する
         *
         * スナップショット側の状態を先に確定させ、判定できない場合は何も作成しない
         * 信頼チェーン(root所有かつgroupとotherが書き込めないディレクトリだけを通る経路)では、
         * 作成直後は0700かつ作成者所有とし、開き直したinodeの所有者とpermissionを確かめてからmetadataを適用する
         * 非信頼チェーンでは、最初の非信頼点の所有者(principal、以降の変更を許す唯一の利用者)と
         * 復元元の所有者が一致する場合だけ作成する
         * 作成はprincipalのfilesystem identity(カーネルが権限判定に使うuidとgid)で0700から行い、
         * 作成直後からprincipalの所有物にする
         * root所有のinodeを経由して作成すると、他の利用者が差し替えてmetadataを奪えるためである
         *
         * @param dirFd 作成先の親ディレクトリのfd
         * @param name 作成する成分名
         * @param sourceDirFd pin済みのスナップショットdirfd
         * @param sourceRelativePath スナップショットdirfdから見た相対パス (作成する成分まで)
         * @param mustPreserveMetadata 必須metadataを適用できない場合に失敗させる場合true
         * @param ownership 同じwalkで集積した祖先の所有者状態
         * @return 成功時は作成したディレクトリのfd (呼び出し側でclose)、失敗時は-1 (errno設定)
         */
        int createMissingParentFromSourceAt(int dirFd, const QByteArray &name, int sourceDirFd,
                                            const QString &sourceRelativePath,
                                            bool mustPreserveMetadata,
                                            const DirectoryChainOwnership &ownership)
        {
            // スナップショット側に同じディレクトリが無い場合(存在しない・ディレクトリ以外)だけを「無し」と判定する
            // それ以外のエラーではスナップショット側の状態を判定できないため、作成せずに失敗する
            const UniqueFd sourceFd(safeOpenDirectoryReadAt(sourceDirFd, sourceRelativePath));
            if (!sourceFd.isValid() && errno != ENOENT && errno != ENOTDIR && errno != ELOOP) {
                return -1;
            }

            struct stat sourceStat;
            if (sourceFd.isValid() && ::fstat(sourceFd.get(), &sourceStat) < 0) {
                return -1;
            }

            // 非信頼チェーンで作成してよいのは、復元元の所有者がprincipalと一致する場合だけにする
            // 所有者を確かめられない復元元に合わせて作成すると、principalの権限が及ばない所有者の領域を作ることになるためである
            const bool untrustedMutation = ::geteuid() == 0 && !ownership.trusted;
            if (untrustedMutation
                    && (!ownership.principalKnown() || !sourceFd.isValid()
                        || sourceStat.st_uid != ownership.principal)) {
                errno = EPERM;
                return -1;
            }

            // 非信頼チェーンでは、mkdiratの間だけfilesystem identityをprincipalへ落とす
            // 権限を落とした区間を短く保つため、権限を落としている間はfdの解放もログ出力も行わず、
            // 権限が戻ってから次の処理へ進む
            bool createdHere = false;
            if (untrustedMutation) {
                bool identityOk = false;
                int mkdirError = 0;
                {
                    ScopedFsIdentity identity(ownership.principal, sourceStat.st_gid);
                    identityOk = identity.ok();
                    if (identityOk && ::mkdirat(dirFd, name.constData(), 0700) == 0) {
                        createdHere = true;
                    }
                    else if (identityOk) {
                        mkdirError = errno;
                    }
                }
                if (!identityOk) {
                    errno = EPERM;
                    return -1;
                }
                // 既に作られていた場合は、所有者がprincipalであることを確かめて同じinodeを使う
                // 名前から開き直すと差し替えられた別のディレクトリへmetadataを適用し得るため、
                // 作成した名前をopenatで開いてfdを固定する
                if (!createdHere && mkdirError != EEXIST) {
                    errno = mkdirError;
                    return -1;
                }
            }
            else {
                createdHere = ::mkdirat(dirFd, name.constData(), 0700) == 0;
                if (!createdHere) {
                    return -1;
                }
            }

            UniqueFd createdFd(::openat(dirFd, name.constData(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!createdFd.isValid()) {
                return -1;
            }

            // mkdirat()とopenat()の間で名前を差し替えられた場合に備え、開いたinodeの所有者を確かめる
            // 非信頼チェーンではprincipal以外の所有物へmetadataを適用しない
            struct stat createdStat;
            if (::fstat(createdFd.get(), &createdStat) < 0) {
                return -1;
            }
            if (!S_ISDIR(createdStat.st_mode)) {
                errno = EEXIST;
                return -1;
            }
            // EEXISTで既存のinodeを使う場合や作成直後に差し替えられた場合は、別のsubvolume(Btrfsの管理単位)のディレクトリを開いている可能性がある
            // metadataを適用する前に、親と同じファイルシステム上かを開いたfdで確かめる
            struct stat parentStat;
            struct statx openedInfo;
            if (::fstat(dirFd, &parentStat) != 0
                    || ::statx(createdFd.get(), "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO,
                               &openedInfo) != 0) {
                return -1;
            }
            if (isMountBoundary(openedInfo, parentStat.st_dev)) {
                errno = EXDEV;
                return -1;
            }
            if (untrustedMutation) {
                if (createdStat.st_uid != ownership.principal) {
                    errno = EPERM;
                    return -1;
                }
            }
            else if (createdStat.st_uid != ::geteuid() || (createdStat.st_mode & 0022) != 0) {
                errno = EEXIST;
                return -1;
            }

            if (sourceFd.isValid()) {
                const RestoredMetadataResult metadata =
                        applyRestoredMetadata(sourceFd.get(), createdFd.get(), true);
                if (metadata.mandatoryFailed && mustPreserveMetadata) {
                    const int failureErrno = metadata.mandatoryErrno;
                    // 失敗したinodeは、自分が作成したままであることをdevとinoで確かめてから削除する
                    // 競合で既存のinodeを使っていた場合に、呼び出し元のディレクトリを消さないためである
                    if (createdHere) {
                        struct stat currentStat;
                        if (::fstatat(dirFd, name.constData(), &currentStat, AT_SYMLINK_NOFOLLOW) == 0
                                && currentStat.st_dev == createdStat.st_dev
                                && currentStat.st_ino == createdStat.st_ino
                                && S_ISDIR(currentStat.st_mode)) {
                            ::unlinkat(dirFd, name.constData(), AT_REMOVEDIR);
                        }
                    }
                    errno = failureErrno;
                    return -1;
                }
            }

            return ::dup(createdFd.get());
        }
    } // namespace

    /**
     * @brief 作成したディレクトリへスナップショットのmetadataを安全に適用できる親ディレクトリかを判定する
     *
     * 信頼できるのは、root所有かつgroupとotherが書き込めないディレクトリだけである
     * sticky bitがあっても、他の利用者がrootより先に名前を作れてしまうため信頼しない
     * root以外が所有するディレクトリは、modeが0555でも所有者がmodeを変えて差し替えられるため信頼しない
     *
     * @param parentStat 親ディレクトリのstat
     * @return 安全に適用できる場合はtrue
     */
    bool parentDirectoryIsSafeForMetadata(const struct stat &parentStat)
    {
        return parentStat.st_uid == 0 && (parentStat.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    }

    /**
     * @brief rootPath配下に親ディレクトリを作成し、スナップショット側のmetadataを合わせる
     *
     * 信頼できる親の下では、スナップショット側に対応するディレクトリがなくても0700で作成する
     * 信頼できない親の下では、復元元の所有者を確認できる場合だけ作成する
     * スナップショット側の状態を判定できないエラーでは、作成せずに失敗する
     * 復元先の木から別のファイルシステムへ降りないよう、既存の成分はrootPathと同じst_devであることを確かめる
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param parentPath 作成する親ディレクトリの絶対パス (rootPath配下であること)
     * @param sourceDirFd pin済みのスナップショットdirfd
     * @param mustPreserveMetadata 必須metadataを適用できない場合に失敗させるならtrue
     * @return 成功時はtrue、失敗時はfalse (errno設定)
     */
    bool safeCreateParentDirectoriesFromSourceBeneathRoot(const QString &rootPath,
                                                          const QString &parentPath,
                                                          int sourceDirFd,
                                                          bool mustPreserveMetadata)
    {
        if (sourceDirFd < 0) {
            errno = EINVAL;
            return false;
        }

        QStringList components;
        if (!splitDestinationComponents(rootPath, parentPath, &components)) {
            return false;
        }

        // 同じwalkで開いた祖先の所有者を集積し、rootが変更してよい相手を単一に限定する
        // root実行時はopenRootPathDirectoryが/からrootPath自身までを集計し、所有者が混在していればEPERMで拒否済みである
        // 同じディレクトリを2回集計すると、集計の間にmodeやACLが変わった場合に判定が食い違う
        // そのため、rootPathは再集計しない
        DirectoryChainOwnership ownership;
        int dirFd = openRootPathDirectory(rootPath, &ownership);
        if (dirFd < 0) {
            return false;
        }

        // 復元先の木から別のファイルシステムへ降りないよう、既存の成分をrootPathのデバイスと比べる
        // 別のmountの下へ作成すると、復元先のsubvolume(Btrfsの管理単位)の外へ書き込むことになるため
        struct stat rootInfo;
        if (::fstat(dirFd, &rootInfo) != 0) {
            const int savedErrno = errno;
            ::close(dirFd);
            errno = savedErrno;
            return false;
        }
        if (::geteuid() != 0) {
            accumulateChainOwnership(ownership, dirFd, rootInfo);
        }

        // 開いたinodeの所有者を集積してから、rootPathと同じファイルシステム上かを確かめる
        // 所有者が混在していればEPERM、statxに失敗した場合は同一性を確かめられないため、安全側に倒して拒否する
        const auto validateOpenedDirectory = [&ownership, &rootInfo](int fd, bool checkMountBoundary) {
            struct stat stepInfo;
            if (::fstat(fd, &stepInfo) != 0) {
                return false;
            }
            accumulateChainOwnership(ownership, fd, stepInfo);
            if (::geteuid() == 0 && !chainAllowsMutation(ownership)) {
                errno = EPERM;
                return false;
            }
            if (!checkMountBoundary) {
                return true;
            }

            struct statx openedInfo;
            if (::statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO, &openedInfo) != 0) {
                return false;
            }
            if (isMountBoundary(openedInfo, rootInfo.st_dev)) {
                errno = EXDEV;
                return false;
            }
            return true;
        };

        for (int i = 0; i < components.size(); ++i) {
            const QByteArray name = components.at(i).toUtf8();

            // 既存の成分は辿るだけで、所有者やmodeを変更しない
            int nextFd = ::openat(dirFd, name.constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            bool checkMountBoundary = true;
            if (nextFd < 0 && errno == ENOENT) {
                const QString sourceRelativePath =
                        components.mid(0, i + 1).join(QLatin1Char('/'));
                nextFd = createMissingParentFromSourceAt(dirFd, name, sourceDirFd,
                                                         sourceRelativePath,
                                                         mustPreserveMetadata, ownership);
                // mount境界の検査は、metadataの適用より前にcreateMissingParentFromSourceAt()の中で済ませている
                checkMountBoundary = false;
            }
            // 既存の成分と作成した成分の両方に同じ所有者の検証を適用し、失敗した場合は開いたfdだけを捨てる
            if (nextFd >= 0 && !validateOpenedDirectory(nextFd, checkMountBoundary)) {
                const int savedErrno = errno;
                ::close(nextFd);
                nextFd = -1;
                errno = savedErrno;
            }

            const int savedErrno = errno;
            ::close(dirFd);
            if (nextFd < 0) {
                errno = savedErrno;
                return false;
            }
            dirFd = nextFd;
        }

        ::close(dirFd);
        return true;
    }

    /**
     * @brief テスト専用としてrootPath配下の通常ファイルを上書きで開く
     *
     * 本番コードからは呼ばない
     * O_EXCLを付けずO_TRUNCで開くため、実際の復元先で使うと、ハードリンクで共有された既存ファイルの中身まで消してしまう
     * 呼び出し側がこの危険を見落とさないよう、名前にForTestingを付けている
     * 末尾の成分はopenat(..., O_NOFOLLOW)で開くため、symlinkならELOOPで拒否する
     * 開いた後もfstat()で通常ファイルであることを確かめる
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param destinationPath 対象ファイルの絶対パス (rootPath配下であること)
     * @param mode 新規作成時のモード
     * @return 成功時はfile descriptor (呼び出し側でclose)、失敗時は-1 (errno設定)
     */
    int safeOpenRegularFileWriteBeneathRootForTesting(const QString &rootPath,
                                                      const QString &destinationPath,
                                                      mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        const int fd = ::openat(parentFd.get(), leafName.constData(),
                                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
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
     * @brief rootPath配下に通常ファイルを排他的に新規作成して開く
     *
     * 最終成分はopenat(..., O_CREAT | O_EXCL | O_NOFOLLOW)で作成するため、既存のエントリ(symlinkを含む)を掴むことはない
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 新規作成時のモード
     * @return 成功時はfile descriptor (呼び出し側でclose)、失敗時は-1 (errno設定)
     */
    int safeCreateRegularFileExclusiveBeneathRoot(const QString &rootPath,
                                                  const QString &destinationPath,
                                                  mode_t mode)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        const int fd = ::openat(parentFd.get(), leafName.constData(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
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
     * @brief rootPath配下でsourcePathをdestinationPathへ移動する(退避のためのrenameに使う)
     *
     * 移動元と移動先はそれぞれrootPathから再解決する
     * 片方の親だけを差し替えられても、変更がrootPathの外へ出ることはなく、外へ出る場合は失敗する
     *
     * 退避先や戻し先に、解決からrenameまでの間に作られたエントリを上書きしないよう、RENAME_NOREPLACEを指定する
     * 未対応のファイルシステム(EINVAL / ENOSYS)でrenameat()へ切り替えると上書きの余地が戻るため、そのまま失敗として返す
     *
     * root実行時は、移動元と移動先の親までの所有者をそれぞれ集計し、所有者が混在する場合と、
     * 所有者以外の利用者が名前を変更できるディレクトリを通る場合はEPERMで拒否する
     * typechangedの復元に失敗して退避した元の内容を戻すときに、入れ替えられた別のディレクトリへ移動させないためである
     * 拒否した場合、元の内容は退避先の名前のまま残る
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param sourcePath 移動元の絶対パス (rootPath配下であること)
     * @param destinationPath 移動先の絶対パス (rootPath配下であること)
     * @return 成功時はtrue、失敗時はfalse (errno設定、root実行時に親までの経路で変更を拒否した場合はEPERM)
     */
    bool safeRenamePathNoFollowBeneathRoot(const QString &rootPath, const QString &sourcePath,
                                           const QString &destinationPath)
    {
        QStringList sourceComponents;
        if (!splitDestinationComponents(rootPath, sourcePath, &sourceComponents)) {
            return false;
        }

        QStringList destinationComponents;
        if (!splitDestinationComponents(rootPath, destinationPath, &destinationComponents)) {
            return false;
        }

        // root実行時は、両方の親までの所有者を集計し、所有者以外の利用者が名前を変更できる親ならEPERMで拒否する
        // そのような親では、移動の直前に名前の先を入れ替えられ、検査していないディレクトリへ移動させられるためである
        QByteArray sourceLeaf;
        DirectoryChainOwnership sourceOwnership;
        const UniqueFd sourceParentFd(
                openLeafParentBeneathRoot(rootPath, sourceComponents, false, 0, &sourceLeaf, &sourceOwnership));
        if (!sourceParentFd.isValid()) {
            return false;
        }

        QByteArray destinationLeaf;
        DirectoryChainOwnership destinationOwnership;
        const UniqueFd destinationParentFd(openLeafParentBeneathRoot(
                rootPath, destinationComponents, false, 0, &destinationLeaf, &destinationOwnership));
        if (!destinationParentFd.isValid()) {
            return false;
        }

        return ::renameat2(sourceParentFd.get(), sourceLeaf.constData(),
                           destinationParentFd.get(), destinationLeaf.constData(),
                           RENAME_NOREPLACE) == 0;
    }

    /**
     * @brief rootPath配下のパスをsymlink非追従で再帰削除する
     *
     * 親までの解決に失敗した場合は何も削除しない
     * 最終成分自体が存在しない場合だけENOENTを成功扱いとする(safeRemoveAllと同じ契約)
     * root実行時は、親までの所有者の状態に削除対象と各降下先を加えて判定し、
     * 単一の所有者に限定できないディレクトリをEPERMで拒否する
     * 親までが信頼チェーンでも、削除対象やその内部で初めて非信頼になる場合は同じ判定を行う
     * トップと各降下先の所有者を検査したfdだけで内容を削除し、名前から開き直して降下しない
     * 最後の名前の削除は空ディレクトリだけを対象とするため、非空のディレクトリに差し替えられていればENOTEMPTYになる
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除に成功した場合はtrue、失敗した場合はfalse (errno設定)
     */
    bool safeRemoveAllBeneathRoot(const QString &rootPath, const QString &destinationPath)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        DirectoryChainOwnership ownership;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName, &ownership));
        if (!parentFd.isValid()) {
            // 親までの解決がENOENTで失敗した場合は削除対象が存在しないため、削除済みとして成功を返す
            return errno == ENOENT;
        }

        return removeEntryAt(parentFd.get(), leafName, ownership);
    }

    /**
     * @brief rootPath配下にsymlinkat()でsymlinkを作成する
     *
     * 最終成分の位置に既存のエントリ(symlinkを含む)がある場合は、EEXISTで失敗する
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param target 作成するsymlinkのtarget
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @return 成功時はtrue、失敗時はfalse (errno設定)
     */
    bool safeCreateSymlinkNoFollowBeneathRoot(const QString &rootPath, const QByteArray &target,
                                              const QString &destinationPath)
    {
        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        return ::symlinkat(target.constData(), parentFd.get(), leafName.constData()) == 0;
    }

    /**
     * @brief rootPath配下のsymlink自体の所有者と時刻をAT_SYMLINK_NOFOLLOWで更新する
     *
     * 所有者の更新と時刻の更新は別のsyscallであり、片方が失敗しても他方は実行する
     * 部分的な適用を呼び出し側が把握できるよう、それぞれの成否を個別に返す
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param path 対象symlinkの絶対パス (rootPath配下であること)
     * @param owner 設定する所有者のUID
     * @param group 設定するグループのGID
     * @param times 設定するaccess時刻とmodification時刻
     * @param ownerUpdated 所有者の更新に成功したかを返す先
     * @param timesUpdated 時刻の更新に成功したかを返す先
     * @return 両方に成功した場合はtrue
     */
    bool safeSetSymlinkMetadataNoFollowBeneathRoot(const QString &rootPath, const QString &path,
                                                   uid_t owner, gid_t group,
                                                   const struct timespec times[2],
                                                   bool *ownerUpdated, bool *timesUpdated)
    {
        if (!ownerUpdated || !timesUpdated || !times) {
            errno = EINVAL;
            return false;
        }

        *ownerUpdated = false;
        *timesUpdated = false;

        QStringList components;
        if (!splitDestinationComponents(rootPath, path, &components)) {
            return false;
        }

        QByteArray leafName;
        const UniqueFd parentFd(
                openLeafParentBeneathRoot(rootPath, components, false, 0, &leafName));
        if (!parentFd.isValid()) {
            return false;
        }

        bool allOk = true;
        if (::fchownat(parentFd.get(), leafName.constData(), owner, group,
                       AT_SYMLINK_NOFOLLOW) == 0) {
            *ownerUpdated = true;
        }
        else {
            allOk = false;
        }

        if (::utimensat(parentFd.get(), leafName.constData(), times, AT_SYMLINK_NOFOLLOW) == 0) {
            *timesUpdated = true;
        }
        else {
            allOk = false;
        }

        return allOk;
    }

    /**
     * @brief subvolume配下の.snapshots/<番号>/snapshotを1成分ずつsymlink非追従で開く
     *
     * .snapshotsと各スナップショットはBtrfsでは別のsubvolumeなので、subvolumeを跨ぐことが正常であり、mount境界の制限は付けない
     * .snapshotsと番号ディレクトリはsnapperがroot所有で作るため、それ以外の所有者やgroupとotherの書き込み権限は、
     * 一般利用者が名前を差し替えられる状態とみなして拒否する
     *
     * @param subvolumeFd snapper設定のsubvolumeのディレクトリfd
     * @param snapshotNumber スナップショット番号
     * @param statOut 開いたスナップショットディレクトリのstatを返す先 (nullptr可)
     * @return 成功時はスナップショットディレクトリのfd、失敗時は-1 (errno設定)
     */
    int openSnapshotDirectoryBeneathSubvolume(int subvolumeFd, unsigned int snapshotNumber,
                                              struct stat *statOut)
    {
        struct stat opened {};
        const auto openStep = [&opened](int parentFd, const QString &name, bool requireSnapperOwned) {
            UniqueFd stepFd(openDirectoryChainAt(parentFd, QStringList {name}, 1, false, 0, false));
            if (!stepFd.isValid() || ::fstat(stepFd.get(), &opened) != 0) {
                return -1;
            }
            if (requireSnapperOwned
                && (opened.st_uid != 0 || (opened.st_mode & (S_IWGRP | S_IWOTH)) != 0)) {
                errno = EPERM;
                return -1;
            }
            return stepFd.release();
        };

        const UniqueFd snapshotsFd(openStep(subvolumeFd, QStringLiteral(".snapshots"), true));
        if (!snapshotsFd.isValid()) {
            return -1;
        }
        const UniqueFd numberFd(openStep(snapshotsFd.get(), QString::number(snapshotNumber), true));
        if (!numberFd.isValid()) {
            return -1;
        }
        UniqueFd snapshotFd(openStep(numberFd.get(), QStringLiteral("snapshot"), false));
        if (!snapshotFd.isValid()) {
            return -1;
        }
        if (statOut) {
            *statOut = opened;
        }
        return snapshotFd.release();
    }
} // namespace qsnapper::security
