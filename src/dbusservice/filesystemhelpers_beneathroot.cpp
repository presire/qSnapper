/**
 * @file filesystemhelpers_beneathroot.cpp
 * @brief root配下宛先への変異操作群
 *
 * componentwiseなO_NOFOLLOW走査で親まで辿り、作成・書込・rename・削除・symlinkと
 * 親ディレクトリ復元を行う
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
     * @brief 宛先絶対パスがrootPath配下であることを検証し、rootからの相対表現を取り出す
     *
     * 文字列レベルの入力解析を行うのみで、ファイルシステムにはアクセスしない
     * 本関数を通過しても安全は保証されない
     *
     * 実際の保証は、本関数の結果を用いて、root dirfdからcomponentwiseなO_NOFOLLOW走査を行う各変異ヘルパーが担う
     *
     * @param rootPath 基点ルートディレクトリ (絶対パス)
     * @param absolutePath 検証対象の宛先絶対パス
     * @param relativeOut rootPathからの相対表現の格納先
     * @return rootPath配下と確定した場合: true、拒否した場合: false (errno=EINVAL)
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

        // 制御文字 (埋め込みNUL含む) はsyscall引数の切り詰めを招くため入力段階で拒否する
        if (containsControlChar(rootPath) || containsControlChar(absolutePath)) {
            errno = EINVAL;
            return false;
        }

        const QStringList rootComponents = rootPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QStringList pathComponents =
                absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);

        // 宛先が空や"/"の場合は、leaf成分を持たないため宛先になり得ない
        if (pathComponents.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // "." / ".."成分による相対脱出を入力段階で拒否する
        for (const QString &component : pathComponents) {
            if (component == QLatin1String(".") || component == QLatin1String("..")) {
                errno = EINVAL;
                return false;
            }
        }

        // root自身は宛先にならない (leaf成分を最低1つ要求する)
        if (pathComponents.size() <= rootComponents.size()) {
            errno = EINVAL;
            return false;
        }

        // root成分列が真のプレフィックスであること (兄弟ディレクトリtrickを排除)
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
     * 呼び出し側は返されたfdを変異に使い、直ちにcloseすること
     * fdを認可(凍結)時点から実行時点へ跨いで保持してはならない
     *
     * @param rootPath 基点ルートディレクトリ (実ディレクトリであること)
     * @param relativePath rootPathからの相対パス ('/'開始や"." ".."成分は拒否)
     * @param createMissing 存在しない成分を作成するか
     * @param mode createMissing = true時の作成モード
     * @return 成功時: 末端ディレクトリのfd、失敗時: -1 (errno設定)
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode)
    {
        if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/'))
                || containsControlChar(relativePath)) {
            errno = EINVAL;
            return -1;
        }

        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            errno = EINVAL;
            return -1;
        }

        for (const QString &component : components) {
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
     * @brief rootPath配下に宛先ディレクトリを作成する (中間成分も必要に応じて作成)
     *
     * leafが既存の場合はfstatat(AT_SYMLINK_NOFOLLOW) で再確認し、ディレクトリ以外 (symlink含む) ならばEEXISTで拒否する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成するディレクトリのモード
     * @return 成功時: true、失敗時: false (errno設定)
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

        // 既存だった場合にsymlink等ではないことを変異時に再確認する
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
         * @brief 欠けている親ディレクトリを1階層だけ作成し、snapshot側の同じディレクトリのmetadataを適用する
         *
         * snapshot側の状態を先に確定させ、判定できない場合は何も作成しない
         * 信頼チェーンでは作成直後は0700・作成者所有とし、開き直したinodeの所有者とpermissionを確認してからmetadataを適用する
         * 非信頼チェーンでは、最初の非信頼点の所有者 (principal) と復元元の所有者が一致する場合だけ作成する
         * 作成はprincipalのfilesystem identityで0700から行い、作成直後からprincipalの所有物にする
         * root所有のinodeを経由して作成すると、他ユーザーが差し替えてmetadataを奪えるため
         *
         * @param dirFd 作成先の親ディレクトリfd
         * @param name 作成する成分名
         * @param sourceDirFd pin済みのsnapshot dirfd
         * @param sourceRelativePath snapshot dirfdからの相対パス (作成する成分まで)
         * @param mustPreserveMetadata 必須metadataを適用できない場合に失敗させる場合true
         * @param ownership 同じwalkで集積した祖先の所有者状態
         * @return 成功時: 作成したディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
         */
        int createMissingParentFromSourceAt(int dirFd, const QByteArray &name, int sourceDirFd,
                                            const QString &sourceRelativePath,
                                            bool mustPreserveMetadata,
                                            const DirectoryChainOwnership &ownership)
        {
            // snapshot側に同じディレクトリが無い場合 (存在しない・ディレクトリ以外) だけを「無し」と判定する
            // それ以外のエラーはsnapshot側の状態を判定できないため、作成せずに失敗する
            const UniqueFd sourceFd(safeOpenDirectoryReadAt(sourceDirFd, sourceRelativePath));
            if (!sourceFd.isValid() && errno != ENOENT && errno != ENOTDIR && errno != ELOOP) {
                return -1;
            }

            struct stat sourceStat;
            if (sourceFd.isValid() && ::fstat(sourceFd.get(), &sourceStat) < 0) {
                return -1;
            }

            // 非信頼チェーンでは、復元元の所有者がprincipalと一致する場合だけ作成を許可する
            // 所有者を確認できない復元元に合わせて作成すると、principalの権限の外にある所有者の領域を作ることになる
            const bool untrustedMutation = ::geteuid() == 0 && !ownership.trusted;
            if (untrustedMutation
                    && (!ownership.principalKnown() || !sourceFd.isValid()
                        || sourceStat.st_uid != ownership.principal)) {
                errno = EPERM;
                return -1;
            }

            // 非信頼チェーンではmkdiratの間だけfilesystem identityをprincipalへ落とす
            // scopeの中ではfdの解放もログ出力も行わず、権限が復旧してから次の処理へ進む
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
                // 既に作られていた場合は、所有者がprincipalであることを確認して同じinodeを使う
                // 名前を再解決すると別のディレクトリへmetadataを適用し得るため、下のopenatでfdを固定する
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

            // mkdirat()とopenat()の間に差し替えられた場合に備え、開いたinodeの所有者を確認する
            // 非信頼チェーンではprincipal以外の所有物へmetadataを適用しない
            struct stat createdStat;
            if (::fstat(createdFd.get(), &createdStat) < 0) {
                return -1;
            }
            if (!S_ISDIR(createdStat.st_mode)) {
                errno = EEXIST;
                return -1;
            }
            // EEXISTで既存のinodeを使う場合や作成直後に差し替えられた場合は、別subvolumeのディレクトリを開いている可能性がある
            // metadataを適用する前に親と同じファイルシステム上かを開いたfdで確認する
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
                    // 失敗したinodeを片付ける: 自分が作成したinodeのままであることを確認してから削除する
                    // 競合で既存のinodeを使った場合は、呼び出し元のディレクトリを消さない
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
     * @brief 作成したディレクトリへsnapshotのmetadataを安全に適用できる親ディレクトリかを判定する
     *
     * root所有かつgroup/other書き込み不可だけを信頼する
     * sticky bitがあっても他ユーザーがrootより先に名前を作れるため信頼しない
     * 非root所有者は0555からでもmodeを変更して差し替えられるため信頼しない
     *
     * @param parentStat 親ディレクトリのstat
     * @return 安全に適用できる場合: true
     */
    bool parentDirectoryIsSafeForMetadata(const struct stat &parentStat)
    {
        return parentStat.st_uid == 0 && (parentStat.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    }

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

        int dirFd = openRootPathDirectory(rootPath);
        if (dirFd < 0) {
            return false;
        }

        // 復元先の木から別のファイルシステムへ降りないよう、既存の成分をrootPathのデバイスと比べる
        // 別mountの下へ作成すると、復元先のサブボリュームの外へ書き込むことになるため
        struct stat rootInfo;
        if (::fstat(dirFd, &rootInfo) != 0) {
            const int savedErrno = errno;
            ::close(dirFd);
            errno = savedErrno;
            return false;
        }

        // 同じwalkで開いた祖先の所有者を集積し、rootが変更してよい相手を単一に限定する
        // 途中で所有者が混在した場合は、EPERMで即座に止める
        DirectoryChainOwnership ownership;
        accumulateChainOwnership(ownership, rootInfo);
        if (::geteuid() == 0 && !chainAllowsMutation(ownership)) {
            ::close(dirFd);
            errno = EPERM;
            return false;
        }

        // 開いたinodeの所有者を集積してから、rootPathと同じファイルシステム上かを確認する
        // 所有者の混在はEPERM、statxに失敗した場合は同一性を確認できないためfail-closedで拒否する
        const auto validateOpenedDirectory = [&ownership, &rootInfo](int fd, bool checkMountBoundary) {
            struct stat stepInfo;
            if (::fstat(fd, &stepInfo) != 0) {
                return false;
            }
            accumulateChainOwnership(ownership, stepInfo);
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
                // 境界検査はmetadataの適用前にcreateMissingParentFromSourceAt()の中で済ませている
                checkMountBoundary = false;
            }
            // 既存成分と作成成分の両方に同じ所有者検証を適用し、失敗時は開いたfdだけを捨てる
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
     * @brief テスト専用としてrootPath配下の通常ファイルを上書きオープンする
     *
     * 本番コードからは呼び出さない
     * O_EXCLを付けずにO_TRUNCで開くため、実際の復元先で使うと、ハードリンクで共有された既存ファイルの中身まで消してしまう
     * この危険を呼び出し側が見落とさないよう、名前にForTestingを付けている
     * 末尾の要素はopenat(..., O_NOFOLLOW)で開くため、symlinkであればELOOPで拒否する
     * 開いた後もfstat()で通常ファイルであることを再確認する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 対象ファイルの絶対パス (rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeOpenRegularFileWriteBeneathRootForTesting(const QString &rootPath, const QString &destinationPath,
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
     * @brief rootPath配下に通常ファイルを排他的に新規作成してオープンする
     *
     * leafはopenat(..., O_CREAT | O_EXCL | O_NOFOLLOW)で作成するため、既存エントリ (symlink含む) を掴むことはない
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
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
     * @brief rootPath配下でsourcePathをdestinationPathへ移動する (rename-aside用)
     *
     * source / destinationはそれぞれ独立してrootから再解決されるため、
     * 片方の親だけが差し替えられた場合でも、変異は必ずroot配下に収まるか失敗する
     *
     * 退避先や戻し先に、解決からrenameまでの間に作られたエントリを上書きしないよう、RENAME_NOREPLACEを指定する
     * 未対応 (EINVAL / ENOSYS) の場合にrenameat()へ切り替えると上書きの余地が戻るため、そのまま失敗として返す
     *
     * @param rootPath 基点ルートディレクトリ
     * @param sourcePath 移動元の絶対パス (rootPath配下であること)
     * @param destinationPath 移動先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
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

        QByteArray sourceLeaf;
        const UniqueFd sourceParentFd(
                openLeafParentBeneathRoot(rootPath, sourceComponents, false, 0, &sourceLeaf));
        if (!sourceParentFd.isValid()) {
            return false;
        }

        QByteArray destinationLeaf;
        const UniqueFd destinationParentFd(openLeafParentBeneathRoot(
                rootPath, destinationComponents, false, 0, &destinationLeaf));
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
     * leaf自体が存在しない場合のみENOENTを成功扱いとする (safeRemoveAllと同じ契約)
     * root実行時は、親までの所有権の状態に削除対象と各降下先を加えて判定し、
     * 単一principalに限定できないディレクトリをEPERMで拒否する
     * 親までが信頼チェーンでも、削除対象やその内部で初めて非信頼になる場合に同じ判定を行う
     * トップと各降下先の所有者を検査したfdだけで内容を削除し、名前を開き直して降下しない
     * 最後の名前によるrmdirは空ディレクトリだけを対象とし、非空の差し替えはENOTEMPTYになる
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除成功時: true、失敗時: false (errno設定)
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
            return errno == ENOENT;
        }

        return removeEntryAt(parentFd.get(), leafName, ownership);
    }

    /**
     * @brief rootPath配下にsymlinkat()でsymlinkを作成する
     *
     * leaf位置が既存の場合 (symlink含む) はEEXISTで失敗する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param target 作成するsymlinkのtarget
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
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
     * @brief rootPath配下のsymlink自体のowner / timesをAT_SYMLINK_NOFOLLOWで更新する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param path 対象symlinkの絶対パス (rootPath配下であること)
     * @param owner 設定するowner UID
     * @param group 設定するgroup GID
     * @param times 設定するaccess / modification time
     * @param ownerUpdated owner更新成功結果の返却先
     * @param timesUpdated times更新成功結果の返却先
     * @return 両方成功した場合 true
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
     * @brief subvolume配下の .snapshots/<番号>/snapshot を1成分ずつsymlink非追従で開く
     *
     * .snapshots と各スナップショットはbtrfsでは別subvolumeなので、mount境界の制限は付けない
     * .snapshots と番号ディレクトリはsnapperがroot所有で作るため、それ以外の所有者やgroupとotherの書き込み権は
     * 一般ユーザが名前を差し替えられる状態として拒否する
     *
     * @param subvolumeFd snapper設定のsubvolumeのディレクトリfd
     * @param snapshotNumber スナップショット番号
     * @param statOut 開いたsnapshotディレクトリのstatの返却先 (nullptr可)
     * @return 成功時snapshotディレクトリのfd、失敗時-1 (errno設定)
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
