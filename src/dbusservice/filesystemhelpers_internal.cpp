/**
 * @file filesystemhelpers_internal.cpp
 * @brief filesystemhelpersで共有する内部処理の実装
 *
 * パスの検証、dirfdを基点にしたディレクトリの走査、snapshot内でのin-root解決、snapshot配下の削除を実装する
 *
 * 公開APIはfilesystemhelpers.cpp側にだけ置き、本ファイルの関数はfilesystemhelpers.cpp、
 * filesystemhelpers_beneathroot.cpp、filesystemhelpers_replace.cpp、
 * snapshotoperations_restore_apply.cppから呼び出す
 *
 * Qt CoreとLinuxのsyscall (openat2、statx、拡張属性の取得) のみに依存し、snapperdのD-Bus APIやpolkitには触れない
 *
 * fd : ファイルディスクリプタ
 * dirfd : ディレクトリを指すファイルディスクリプタ
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
#include <deque>
#include <cstring>
#include <vector>
#include "filesystemhelpers.h"
#include "filesystemhelpers_internal.h"

namespace qsnapper::security {
        namespace {
            constexpr size_t NSS_BUFFER_FALLBACK_SIZE = 16 * 1024; // sysconfが推奨値を返さない場合の初期サイズ
            constexpr size_t NSS_BUFFER_MAX_SIZE = 1024 * 1024;    // ERANGEのたびに倍にする上限 (1 MiB)

            /**
             * @brief NSSの取得関数へ渡すバッファの初期サイズを返す
             *
             * NSS (Name Service Switch、passwdやgroupの取得先を切り替える仕組み) は
             * sysconfでエントリ1件の推奨サイズを公開しているため、その値から始める
             * @param sysconfName _SC_GETPW_R_SIZE_MAXまたは_SC_GETGR_R_SIZE_MAX
             * @return sysconfの推奨サイズ、正の値を返さない場合は16 KiB
             */
            size_t initialNssBufferSize(int sysconfName)
            {
                const long suggested = ::sysconf(sysconfName);
                return suggested > 0 ? static_cast<size_t>(suggested) : NSS_BUFFER_FALLBACK_SIZE;
            }

            /**
             * @brief ERANGEの間はバッファを倍にしながら、1 MiBまで同じ取得をやり直す
             *
             * 長いGECOS (passwdの5番目の欄、利用者のフルネームなど) や
             * 多数のメンバを持つgroupのエントリでも、判定を諦めずに読めるようにする
             * glibcのfgetpwent_rとfgetgrent_rは、ERANGEのときにストリームの位置を
             * 行の先頭へ戻すため、同じ行を読み直せる
             * @param buffer 取得に使うバッファ (大きさを変える)
             * @param lookup バッファの先頭と大きさを受け取り、戻り値でエラー番号を返す取得関数
             * @return 最後の取得の戻り値 (0は成功、それ以外はエラー番号)
             * @note 戻り値がERANGEのまま1 MiBに達した場合は、そこで打ち切る
             */
            template <typename Lookup>
            int lookupWithGrowingBuffer(std::vector<char> &buffer, Lookup lookup)
            {
                for (;;) {
                    const int result = lookup(buffer.data(), buffer.size());
                    if (result != ERANGE || buffer.size() >= NSS_BUFFER_MAX_SIZE) {
                        return result;
                    }
                    buffer.resize(std::min(buffer.size() * 2, NSS_BUFFER_MAX_SIZE));
                }
            }
        } // namespace

        /**
         * @brief 今回開いたディレクトリの所有者を、これまでの集計へ取り込む
         *
         * rootが所有者の代わりに変更してよい相手をprincipalと呼び、これを1人に限定できるか判定する
         * 信頼できる経路で初めて現れた非信頼ディレクトリがroot所有の場合は、変更主体を1人に絞れないためconflictとする
         * 所有者が分かれた時点でconflictとし、以降は別の所有者が現れても状態を変えない
         * @param ownership これまでの祖先の集計 (結果を書き戻す)
         * @param directoryFd 今回開いたディレクトリのfd
         * @param directoryStat 今回開いたディレクトリのfstat結果
         */
        void accumulateChainOwnership(DirectoryChainOwnership &ownership, int directoryFd,
                                      const struct stat &directoryStat)
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
            // 所有者が同じでも、所有者以外の利用者が名前を変更できるディレクトリでは、
            // 検査した名前の先を同じ所有者の別のディレクトリへ入れ替えられるため、変更主体を確定できない
            // 既にconflictと確定した後は、passwdとgroupの問い合わせと拡張属性 (xattr) の読み出しを省く
            if (!ownership.conflict && directoryDelegatesNames(directoryFd, directoryStat)) {
                ownership.conflict = true;
            }
        }

        /**
         * @brief 検証した祖先の状態から、rootが変更を行ってよいか判定する
         *
         * 所有者がrootだけで名前の変更を委譲していないチェーンと、
         * 単一のprincipalへ所有者を限定できたチェーンだけを許可する
         * @param ownership 同じ走査で検証した状態
         * @return 信頼できるチェーンか、単一のprincipalが得られた場合true
         */
        bool chainAllowsMutation(const DirectoryChainOwnership &ownership)
        {
            return ownership.trusted || ownership.principalKnown();
        }

        /**
         * @brief passwdとgroupのエントリだけから、UPGの条件を満たすか判定する
         *
         * UPG (user private group、所有者専用のグループ) は、所有者の主グループで、
         * 名前が所有者と同じで、補助メンバが所有者だけのグループである
         * NSSや/etc/passwdを走査せずに済むよう、取得済みの2つのエントリだけで判定する
         * @param owner 所有者のpasswdエントリ
         * @param group 所有グループのgroupエントリ
         * @return 所有者の主グループ、名前の一致、補助メンバが所有者だけの全てを満たす場合true
         */
        bool isUserPrivateGroupEntry(const struct passwd &owner, const struct group &group)
        {
            if (owner.pw_gid != group.gr_gid || !owner.pw_name || !group.gr_name
                    || std::strcmp(owner.pw_name, group.gr_name) != 0) {
                return false;
            }
            if (!group.gr_mem) {
                return true;
            }
            for (char *const *member = group.gr_mem; *member; ++member) {
                if (std::strcmp(*member, owner.pw_name) != 0) {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief passwd形式のストリームから、所有者以外でgroupを主グループとする利用者を探す
         *
         * groupのエントリのgr_memには補助グループとしての所属しか現れないため、
         * 主グループとしてだけ属する利用者はpasswdを走査しないと分からない
         * @param stream passwd形式のストリーム (呼び出し側が開いて閉じる)
         * @param owner 所有者のuid
         * @param group 調べるgid
         * @return 他にいなければExclusive、いればShared、読み取りに失敗した場合はFailed
         */
        LocalScanResult scanPasswdForOtherPrimaryMember(FILE *stream, uid_t owner, gid_t group)
        {
            if (!stream) {
                return LocalScanResult::Failed;
            }

            std::vector<char> buffer(initialNssBufferSize(_SC_GETPW_R_SIZE_MAX));
            for (;;) {
                struct passwd entry = {};
                struct passwd *result = nullptr;
                const int error = lookupWithGrowingBuffer(buffer, [&](char *data, size_t size) {
                    return ::fgetpwent_r(stream, &entry, data, size, &result);
                });
                if (error == ENOENT) {
                    return LocalScanResult::Exclusive;
                }
                if (error != 0 || !result) {
                    return LocalScanResult::Failed;
                }
                if (entry.pw_uid != owner && entry.pw_gid == group) {
                    return LocalScanResult::Shared;
                }
            }
        }

        /**
         * @brief group形式のストリームから、同じgidのレコードの件数を数える
         *
         * getgrgid_rは同じgidのレコードのうち1件しか返さないため、
         * 別のレコードのメンバがinitgroupsでそのgidを補助グループとして得ることを見落とす
         * そのため、メンバの無いレコードが返された場合でも、同じgidのレコードが他に無いことを確かめる
         * @param stream group形式のストリーム (呼び出し側が開いて閉じる)
         * @param group 調べるgid
         * @return 1件ならExclusive、2件以上ならShared、0件か読み取りに失敗した場合はFailed
         */
        LocalScanResult scanGroupForDuplicateGid(FILE *stream, gid_t group)
        {
            if (!stream) {
                return LocalScanResult::Failed;
            }

            std::vector<char> buffer(initialNssBufferSize(_SC_GETGR_R_SIZE_MAX));
            int matches = 0;
            for (;;) {
                struct group entry = {};
                struct group *result = nullptr;
                const int error = lookupWithGrowingBuffer(buffer, [&](char *data, size_t size) {
                    return ::fgetgrent_r(stream, &entry, data, size, &result);
                });
                if (error == ENOENT) {
                    return matches == 1 ? LocalScanResult::Exclusive : LocalScanResult::Failed;
                }
                if (error != 0 || !result) {
                    return LocalScanResult::Failed;
                }
                if (entry.gr_gid == group && ++matches > 1) {
                    return LocalScanResult::Shared;
                }
            }
        }

        /**
         * @brief NSSと/etc/passwd、/etc/groupの全てで、groupがownerのUPGと確かめられるか判定する
         *
         * どれかが失敗した場合は、groupが共有されている可能性を否定できないためfalseを返す
         * 結果は保持せず、呼び出すたびに取得し直す
         * @param owner ディレクトリの所有者のuid
         * @param group ディレクトリの所有グループのgid
         * @return 全ての取得に成功し、全てが許可を示した場合true
         * @note テスト用のビルドでは、設定されたフックの結果を返す
         */
        bool isUserPrivateGroup(uid_t owner, gid_t group)
        {
#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
            if (const detail::UserPrivateGroupCheck check = detail::userPrivateGroupCheck()) {
                return check(owner, group);
            }
#endif
            struct passwd ownerEntry = {};
            struct passwd *ownerResult = nullptr;
            std::vector<char> ownerBuffer(initialNssBufferSize(_SC_GETPW_R_SIZE_MAX));
            const int ownerError = lookupWithGrowingBuffer(ownerBuffer, [&](char *data, size_t size) {
                return ::getpwuid_r(owner, &ownerEntry, data, size, &ownerResult);
            });
            if (ownerError != 0 || !ownerResult) {
                return false;
            }

            struct group groupEntry = {};
            struct group *groupResult = nullptr;
            std::vector<char> groupBuffer(initialNssBufferSize(_SC_GETGR_R_SIZE_MAX));
            const int groupError = lookupWithGrowingBuffer(groupBuffer, [&](char *data, size_t size) {
                return ::getgrgid_r(group, &groupEntry, data, size, &groupResult);
            });
            if (groupError != 0 || !groupResult) {
                return false;
            }

            if (!isUserPrivateGroupEntry(ownerEntry, groupEntry)) {
                return false;
            }

            FILE *passwdFile = ::fopen("/etc/passwd", "re");
            if (!passwdFile) {
                return false;
            }
            const LocalScanResult passwdResult = scanPasswdForOtherPrimaryMember(passwdFile, owner, group);
            ::fclose(passwdFile);
            if (passwdResult != LocalScanResult::Exclusive) {
                return false;
            }

            FILE *groupFile = ::fopen("/etc/group", "re");
            if (!groupFile) {
                return false;
            }
            const LocalScanResult groupFileResult = scanGroupForDuplicateGid(groupFile, group);
            ::fclose(groupFile);
            return groupFileResult == LocalScanResult::Exclusive;
        }

        /**
         * @brief ディレクトリの中の名前を、所有者以外にも変えさせているか判定する
         *
         * otherの書き込み、拡張ACL (Access Control List、ファイルごとの利用者やグループの権限)、
         * UPGでないグループの書き込みのどれかがあれば委譲ありとする
         * 拡張ACLがあるとき、groupの権限ビットはACLのmaskを表すため、
         * groupの書き込みビットが無ければ、名前付きの利用者やグループの書き込みも効かない
         * stickyビットは判定に使わない
         * stickyでも、書き込みを持つ利用者は、別の場所から同じ所有者のディレクトリを空いた名前へ移し入れられるためである
         * @param directoryFd 判定するディレクトリのfd (拡張ACLのxattrを読む)
         * @param directoryStat directoryFdのfstat結果
         * @return 所有者以外が名前を変えられる可能性がある場合true
         * @note errnoは呼び出し前の値に戻す
         */
        bool directoryDelegatesNames(int directoryFd, const struct stat &directoryStat)
        {
            if ((directoryStat.st_mode & S_IWOTH) != 0) {
                return true;
            }
            if ((directoryStat.st_mode & S_IWGRP) == 0) {
                return false;
            }

            const int savedErrno = errno;
            bool delegates = true;
            if (::fgetxattr(directoryFd, "system.posix_acl_access", nullptr, 0) < 0
                    && (errno == ENODATA || errno == EOPNOTSUPP)) {
                delegates = !isUserPrivateGroup(directoryStat.st_uid, directoryStat.st_gid);
            }
            errno = savedErrno;
            return delegates;
        }

        /**
         * @brief 「/」からrootPath自身までの祖先を1つの集計へ順に取り込む
         *
         * 祖先ごとに別々に集計すると、親の所有者がrootPathと同じ所有者の別のディレクトリを
         * 入れ替えられることを見落とすため、1つの状態へまとめる
         * @param ancestors 「/」から順に並べた祖先
         * @return 集計した状態
         */
        DirectoryChainOwnership accumulateAncestorChainOwnership(const std::vector<ObservedDirectory> &ancestors)
        {
            DirectoryChainOwnership ownership;
            for (const ObservedDirectory &ancestor : ancestors) {
                accumulateChainOwnership(ownership, ancestor.fd, ancestor.info);
            }
            return ownership;
        }

        /**
         * @brief fsuidとfsgidをprincipalへ切り替え、切り替えられたかを記録する
         *
         * fsuid (ファイル操作の権限判定に使うuid) とfsgidを切り替えて、principalの権限で作成させる
         * gidを先に落とすのは、uidを非rootへ切り替えた後ではfsgidを変更できないためである
         * setfsuidとsetfsgidは失敗してもerrnoを返さず、直前の値を返すだけなので、
         * -1を渡して現在値を問い合わせ、指定した値になったことを確認する
         * 確認できなかった場合はm_okがfalseになり、呼び出し側は作成を行わない
         * @param uid 作成を行うprincipal
         * @param gid 復元元のgroup
         * @note errnoは呼び出し前の値に戻す
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
         * @brief rootのfsuidとfsgidを復元し、復元できなければプロセスを終了する
         *
         * 復元に失敗したまま処理を続けると、以降の権限検証がprincipalの権限で行われ、
         * rootとして行うべき検査を通過してしまうため、続行せずqFatalで停止する
         * @note errnoは呼び出し前の値に戻す
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
         * @brief principalへの切り替えを検証できたかを返す
         *
         * 呼び出し側はtrueの場合だけ、principalの権限で作成を行う
         * @return fsuidとfsgidが指定どおりになった場合true
         */
        bool ScopedFsIdentity::ok() const
        {
            return m_ok;
        }

        /**
         * @brief 絶対パスを各成分へ分解する
         *
         * 連続する"/"と末尾の"/"は空の成分として扱わず、取り除く
         * @param path 分解対象の絶対パス
         * @param components 分解結果の格納先
         * @return 絶対パスとして分解できた場合: true、それ以外はerrno=EINVALでfalse
         */
        bool splitAbsolutePath(const QString &path, QStringList *components)
        {
            if (!components) {
                errno = EINVAL;
                return false;
            }

            // 呼び出し側が前回の結果を誤って参照しないよう、出力先は毎回クリアする
            components->clear();
            if (!path.startsWith(QLatin1Char('/'))) {
                errno = EINVAL;
                return false;
            }

            *components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            return true;
        }

        /**
         * @brief 制御文字 (C0: U+0000..U+001F、DEL: U+007F、C1: U+0080..U+009F) を含むか判定する
         *
         * 埋め込みNULはsyscall引数の切り詰めを招き、検証したパスと実際に操作するパスが
         * ずれる原因となるため、パスには一切許容しない (inputvalidatorと同じ方針)
         * C0とC1は端末の制御シーケンスとして解釈され得るため、表示やログを乱さないよう併せて拒否する
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
         * @brief dirfd基点の相対パスを検証し、UTF-8へ符号化する
         *
         * 空パス、絶対パス、"."と".."の成分、制御文字を拒否する
         * openat系は絶対パスを渡すとdirfdを無視するため、検査済みのdirfdを基点にしているのに
         * 意図しない位置へ解決されることを入力段階で防ぐ
         *
         * @param relativePath 検証対象の相対パス
         * @param encodedOut 符号化結果の格納先 (省略可)
         * @return 有効な相対パスの場合: true、それ以外はerrno=EINVALでfalse
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
         * @return 成功時: "/"のfd (呼び出し側でclose)、失敗時: -1 (errno設定)
         */
        int openRootDirectory()
        {
            return ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }

        /**
         * @brief 基点ディレクトリrootPathを、全成分O_NOFOLLOWで開く
         *
         * 単純なopen(rootPath, O_NOFOLLOW)は末尾の成分にしかO_NOFOLLOWが効かず、
         * rootPathの中間成分がsymlinkへ差し替えられていた場合に追従してしまう
         * そのため「/」から1成分ずつO_NOFOLLOWで開き、rootPathの祖先の差し替えも拒否する
         *
         * root実行時は、「/」からrootPath自身までの祖先を1つの所有権の集計へ取り込む
         * 祖先ごとに別々に集計すると、親の所有者がrootPathと同じ所有者の別のディレクトリを
         * 入れ替えられることを見落とすためである
         * 集計の結果、rootが変更してよい相手を1人に限定できない場合はEPERMで拒否する
         * 非rootで実行している場合は、カーネルの権限検査が利用者自身の権限で働くため、
         * 祖先の集計を省略する
         *
         * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
         * @param ancestorOwnershipOut rootPath自身まで検査した所有権の返却先 (root実行時だけ書き込む)
         * @return 成功時: 検査に使ったdirfd (呼び出し側でclose)、失敗時: -1 (祖先の所有者が混在するか、
         *         祖先で所有者以外の利用者が名前を変更できる場合はEPERM)
         */
        int openRootPathDirectory(const QString &rootPath, DirectoryChainOwnership *ancestorOwnershipOut)
        {
            if (rootPath.isEmpty() || !rootPath.startsWith(QLatin1Char('/'))
                    || containsControlChar(rootPath)) {
                errno = EINVAL;
                return -1;
            }

            if (::geteuid() != 0) {
                return safeOpenDirectory(rootPath);
            }

            QStringList components;
            if (!splitAbsolutePath(rootPath, &components)) {
                return -1;
            }

            // 集計し終えるまで全ての祖先のfdを開いたままにし、検査したinodeと返すfdを一致させる
            // UniqueFdは移動できないため、要素の位置が変わらないdequeへ置く
            std::deque<UniqueFd> openedDirectories;
            openedDirectories.emplace_back(openRootDirectory());
            if (!openedDirectories.back().isValid()) {
                return -1;
            }
            for (const QString &component : std::as_const(components)) {
                const int nextFd = openDirectoryChainAt(openedDirectories.back().get(), QStringList {component}, 1,
                                                        false, 0, false);
                if (nextFd < 0) {
                    return -1;
                }
                openedDirectories.emplace_back(nextFd);
            }

            std::vector<ObservedDirectory> ancestors;
            ancestors.reserve(openedDirectories.size());
            for (const UniqueFd &directory : openedDirectories) {
                ObservedDirectory observed {directory.get(), {}};
                if (::fstat(directory.get(), &observed.info) != 0) {
                    return -1;
                }
                ancestors.push_back(observed);
            }

            const DirectoryChainOwnership ownership = accumulateAncestorChainOwnership(ancestors);
            if (!chainAllowsMutation(ownership)) {
                errno = EPERM;
                return -1;
            }
            if (ancestorOwnershipOut) {
                *ancestorOwnershipOut = ownership;
            }
            return openedDirectories.back().release();
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
         * @brief baseFd配下の成分列を1成分ずつopenat2で辿り、ディレクトリfdを開く
         *
         * 複数成分を1回のopenat2で解決すると途中のmount境界を観測できないため、
         * 1成分ごとに開いてstatxする
         * btrfsのネストしたsubvolumeはmountではないためRESOLVE_NO_XDEVでは止まらず、
         * 基点とst_devが異なることをstatxで検出してEXDEVにする
         * openat2がEAGAINを返すのは競合による一時的な失敗なので、同じ操作を最大3回まで再試行する
         * openat2が無いカーネル5.6未満では代替実装を持たないため、ENOSYSのまま失敗する
         *
         * @param baseFd 走査基点のディレクトリfd
         * @param components 相対パス成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分をmkdirat()で作成するか
         * @param mode createMissing = true時の作成モード
         * @param sameMountOnly mount境界 (別mountのroot、または親と異なるデバイス) を越える成分をEXDEVで拒否する場合true
         * @param ownershipOut 基点と各成分の所有権状態の返却先 (再走査を避けるため同時に集積する)
         * @param initialOwnership baseFdまで検査済みの所有権 (渡した場合はbaseFdを再集計しない)
         * @return 成功時: 最終ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
         */
        int openDirectoryChainAt(int baseFd, const QStringList &components, int componentCount,
                                 bool createMissing, mode_t mode, bool sameMountOnly,
                                 DirectoryChainOwnership *ownershipOut,
                                 const DirectoryChainOwnership *initialOwnership)
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
                if (ownershipOut && initialOwnership) {
                    // baseFdまでは呼び出し側が同じfdで検査済みのため、その結果を引き継ぐ
                    *ownershipOut = *initialOwnership;
                }
                else if (ownershipOut) {
                    *ownershipOut = DirectoryChainOwnership {};
                    accumulateChainOwnership(*ownershipOut, baseFd, baseInfo);
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
                    accumulateChainOwnership(*ownershipOut, nextFd, stepInfo);
                }

                ::close(dirFd);
                dirFd = nextFd;
            }

            return dirFd;
        }

        /**
         * @brief 絶対パスの成分列を「/」からO_NOFOLLOWで辿り、ディレクトリfdを開く
         *
         * symlinkを追従せずに解決するため、取得したfdが攻撃者の用意したディレクトリを指すことはない
         * openDirectoryChainAtがopenat2のRESOLVE_BENEATHとRESOLVE_NO_SYMLINKSで解決する
         * 絶対パスの解決ではsubvolumeやmount pointを跨ぐことが正当なため、mount境界は制限しない
         *
         * @param components 絶対パスを分解した成分列
         * @param componentCount 辿る成分数
         * @param createMissing 存在しない成分をmkdirat()で作成するか
         * @param mode createMissing = true時の作成モード
         * @return 成功時: 最終ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
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
         * @brief 宛先の絶対パスをrootPath配下の相対成分列へ分解する
         *
         * rootPathの外を指すパス、rootPath自身を指すパスは拒否する
         *
         * @param rootPath 基点ルートディレクトリ
         * @param absolutePath 宛先の絶対パス
         * @param components 相対成分列の格納先 (空成分は取り除かれ、非空になる)
         * @return 分解できた場合: true、それ以外はerrno=EINVALでfalse
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
         * @brief rootPathを開き、相対成分列の最終成分の親までO_NOFOLLOWで辿る
         *
         * rootPath自身も1成分ずつO_NOFOLLOWで開くため、rootPathがsymlinkへ差し替えられている場合も拒否する
         * 中間成分はopenDirectoryChainAtがopenat2のRESOLVE_BENEATHとRESOLVE_NO_SYMLINKSで解決するため、
         * どの階層でsymlinkへ差し替えられてもELOOPで失敗する
         * leaf (最終成分) は削除や置換の対象として名前だけを返し、ここでは開かない
         *
         * @param rootPath 基点ルートディレクトリ
         * @param relativeComponents splitDestinationComponents由来の相対成分列 (非空)
         * @param createMissing 中間成分を作成するか
         * @param mode createMissing = true時の作成モード
         * @param leafName 最終成分名の返却先
         * @param ownershipOut 同じ走査で集積する所有権状態 (rootが変更主体を確定できなければEPERM)
         * @return 成功時: 親dirfd (呼び出し側でclose)、失敗時: -1 (errno設定)
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

            // root実行時は、rootPath自身まで検査した所有権を初期値にし、rootPathより下を同じ集計で続ける
            // rootPathの祖先とrootPathより下を別々に集計すると、祖先の所有者による入れ替えを見落とすためである
            DirectoryChainOwnership ancestorOwnership;
            const UniqueFd rootFd(openRootPathDirectory(rootPath, &ancestorOwnership));
            if (!rootFd.isValid()) {
                return -1;
            }
            const DirectoryChainOwnership *initialOwnership = ::geteuid() == 0 ? &ancestorOwnership : nullptr;

            UniqueFd parentFd(openDirectoryChainAt(rootFd.get(), relativeComponents,
                                        static_cast<int>(relativeComponents.size()) - 1,
                                        createMissing, mode, true, ownershipOut, initialOwnership));
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
         * 境界 (USB、NFS、FUSE、bind mount、ネストしたsubvolume) を見つけた時点でerrno=EXDEVで失敗を返す
         * 境界を黙ってスキップして成功扱いにすると、呼び出し元は削除が完了したと誤認するため、必ず失敗として返す
         *
         * statxはAT_STATX_DONT_SYNCを付けて呼び、FUSEやNFSといった境界の先へ属性取得要求を送らない
         * 応答しないFUSEでサービスが停止するのを防ぐためである
         *
         * @param parentFd 親ディレクトリfd
         * @param parentDev 削除をこのデバイス内に限定するための基準デバイス番号 (親のst_dev)
         * @param entryName 対象のleaf名
         * @param dirOut Descendを返した場合に、開いたディレクトリを受け取る (呼び出し元がclosedirする)
         * @param ownership 入力は親までの所有権の状態、Descendを返した場合は開いたディレクトリを含む状態に更新する
         * @return 処理結果 (Failedの場合はerrno設定済み)
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
                accumulateChainOwnership(ownership, childFd.get(), directoryStat);
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
         * @brief 親dirfd配下の1エントリを、symlinkを追従せずmount境界を越えずに削除する
         *
         * 攻撃者が深い階層を作ってもスタックを使い切らないよう、再帰呼び出しではなく明示的なスタックで降りる
         * 深さがremovalDepthLimit()を超える場合は、途中まで削除した状態でerrno=ELOOPの失敗を返す
         * mount境界を見つけた場合はerrno=EXDEVで失敗し、境界の内側は1件も削除しない
         * root実行時は、所有者を検査したfdでのみ内容を削除し、単一principalに限定できないディレクトリへは降りない
         * 最後の削除は空になったディレクトリへのAT_REMOVEDIRだけなので、principal自身が削除できる範囲を超えない
         * 非空のディレクトリへ差し替えられた場合はENOTEMPTYで失敗し、その中身を削除しない
         *
         * @param parentFd 親ディレクトリfd
         * @param parentDev 親ディレクトリのst_dev
         * @param entryName 削除対象のleaf名
         * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
         * @return 削除成功時 (対象が存在しない場合を含む): true、失敗時はerrno設定済みでfalse
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
         * @brief 親dirfdのst_devを基準に、配下の1エントリを削除する
         *
         * @param parentFd 親ディレクトリfd
         * @param entryName 削除対象のleaf名
         * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
         * @return 削除成功時 (対象が存在しない場合を含む): true、失敗時はerrno設定済みでfalse
         * @note 実際の削除は、親のst_devを渡してremoveEntryWithinDeviceAtへ委譲する
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
         * @return regular fileの場合: true、失敗時はerrno設定済みでfalse (regular fileでなければEINVAL)
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
         * FIFOやデバイスはopen()自体がブロックしたり副作用を起こしたりするため、
         * 開く前にfstatat()でregular fileであることを確認する
         * 確認とopenの間にFIFOへ差し替えられても待ち続けないよう、O_NONBLOCKとO_NOCTTYを付けて開き、
         * 開いたfdをfstat()で再確認する (regular fileの読み込みには影響しない)
         *
         * @param parentFd 親ディレクトリのfd
         * @param leafName 親ディレクトリ内の名前
         * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
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
         * @brief dirfd基点で親成分列をopenat2のRESOLVE_IN_ROOTで解決する
         *
         * snapshot dirfdは「/」のbtrfs snapshotであるため、"var/run -> /run"のような
         * 絶対symlinkはsnapshot内で正当な存在であり、chrootと同じくroot内での解決が要求される
         * RESOLVE_IN_ROOTはカーネル内でroot外への逸脱を構造的に不可能にする
         * RESOLVE_NO_MAGICLINKSにより/proc系のmagic linkも追従しない
         * openat2がEAGAINを返すのは競合による一時的な失敗なので、同じ操作を最大3回まで再試行する
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param parentComponents leaf成分を除いた親成分列 (非空)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
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
         * TOCTOU (Time-of-check to time-of-use、検査と使用の間の競合) を避けるため、
         * 検証したrelativePathと同じ入力から親dirfdとleaf名を導出し、
         * 「検証した値」と「操作する対象」を一致させる
         * openat2が無いカーネル5.6未満では代替実装を持たないため、ENOSYSのまま失敗する
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param components 検証済み相対パスの成分列 (非空)
         * @param leafNameOut 最終成分名の返却先 (省略可)
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
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
