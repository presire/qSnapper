/**
 * @file filesystemhelpers_replace.cpp
 * @brief 通常ファイルとsymlinkを原子的に差し替え、metadataを適用し、スナップショット一覧の指紋を計算する
 *
 * 宛先と同じディレクトリに一時objectを作り、fd経由で内容とmetadataを整えてからrenameat()で差し替える
 * 差し替えの前後でinodeを突き合わせ、経路を差し替えられた場合はESTALEで失敗する
 * security.selinuxの書き込みがSELinuxユーザの違いで拒否された場合は、ユーザ部分だけを復元先の値に合わせて再試行する
 *
 * 呼び出し元 : 復元計画の実行と、その前提を確かめるテスト
 * 入力 : 復元元と復元先のfd、復元先のパス、.snapshotsのパス
 * 出力 : 差し替えの成否とerrno、適用したmetadataの結果、一覧の指紋
 * 依存 : filesystemhelpers.h、filesystemhelpers_internal.h、Linuxのrenameat・sendfile・fgetxattr
 */
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QRandomGenerator>
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
    namespace {
        constexpr const char *kAclAccessXattr = "system.posix_acl_access";    // アクセスACL(アクセス制御リスト)のxattr名
        constexpr const char *kAclDefaultXattr = "system.posix_acl_default";  // ディレクトリのdefault ACLのxattr名
        constexpr const char *kCapabilityXattr = "security.capability";       // file capabilityのxattr名
        constexpr const char *kSelinuxXattr = "security.selinux";             // SELinuxラベルのxattr名

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
        /**
         * @brief metadata適用のテスト用フックの設定
         *
         * QSNAPPER_ENABLE_TEST_METADATA_HOOKSを定義したテスト用ターゲットだけでコンパイルし、
         * 製品のhelperにはフックの設定を持たせない
         */
        struct MetadataTestHooks {
            detail::MetadataStageObserver observer = nullptr;              // 段階の通知先 (nullptrなら通知しない)
            void *context = nullptr;                                       // observerへ渡すポインタ (観測結果の記録先など)
            QByteArray failingAclXattr;                                    // 同期を失敗させるACLのxattr名 (空なら失敗させない)
            int failingAclErrno = 0;                                       // 失敗時に設定するerrno (0なら失敗させない)
            // 所有者専用のグループかどうかの判定の置き換え先 (nullptrなら本来の判定)
            detail::UserPrivateGroupCheck userPrivateGroupCheck = nullptr;
        };

        /**
         * @brief テスト用フックの設定を返す
         *
         * 他の翻訳単位の静的初期化から呼ばれた場合でも初期化済みの設定を返せるよう、関数内のstatic変数で保持する
         *
         * @return 設定への参照
         */
        MetadataTestHooks &metadataTestHooks()
        {
            static MetadataTestHooks hooks;
            return hooks;
        }

        /**
         * @brief テストが設定したobserverへ段階を通知する
         *
         * @param stage 通知する段階
         * @param destinationFd 復元先のfd
         * @note observerが設定されていない場合は何もしない
         */
        void notifyMetadataStage(detail::MetadataStage stage, int destinationFd)
        {
            const MetadataTestHooks &hooks = metadataTestHooks();
            if (hooks.observer) {
                hooks.observer(stage, destinationFd, hooks.context);
            }
        }
#endif

        /**
         * @brief fdからxattrの値を読み出す
         *
         * 読み出しの間に値が伸びた場合 (ERANGE) は数回だけ再試行する
         *
         * @param fd 対象のfd
         * @param name xattr名
         * @param valueOut 値を返す先
         * @return 値があれば1、属性がなければ0、エラーなら-1 (errno設定、ENOTSUPを含む)
         */
        int readXattrFd(int fd, const char *name, QByteArray *valueOut)
        {
            for (int attempt = 0; attempt < 4; ++attempt) {
                const ssize_t size = ::fgetxattr(fd, name, nullptr, 0);
                if (size < 0) {
                    return errno == ENODATA ? 0 : -1;
                }

                QByteArray value(static_cast<qsizetype>(size), '\0');
                const ssize_t length = ::fgetxattr(fd, name, value.data(), static_cast<size_t>(value.size()));
                // 長さを調べた時点では空だった値に、その直後に別の処理が値を書き込むと、長さ0のバッファでの呼び出しは値を読まずに新しい長さだけを返す
                // この戻り値を空の値と取り違えないよう、長さを調べるところからやり直す
                if (length > 0 && size == 0) {
                    continue;
                }
                if (length >= 0) {
                    value.truncate(static_cast<qsizetype>(length));
                    *valueOut = value;
                    return 1;
                }
                if (errno == ENODATA) {
                    return 0;
                }
                if (errno != ERANGE) {
                    return -1;
                }
            }

            errno = ERANGE;
            return -1;
        }

        /**
         * @brief POSIX ACLを復元元と一致させる
         *
         * 復元元にあれば設定し、無ければ復元先から削除する
         * 復元元のファイルシステムがACLに対応していない(ENOTSUP)場合は、ACLなしとして扱う
         *
         * @param sourceFd 復元元のfd
         * @param destinationFd 復元先のfd
         * @param name ACLのxattr名
         * @return 一致させられた場合はtrue、失敗した場合はfalse (errno設定)
         */
        bool syncAclXattrFd(int sourceFd, int destinationFd, const char *name)
        {
#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
            // テストが失敗を指定したACLは、復元先のACLを変更する前に失敗させる
            // 実際のファイルシステムで書き込みに失敗した場合と同じく、falseを返してerrnoを設定する
            const MetadataTestHooks &hooks = metadataTestHooks();
            if (hooks.failingAclErrno != 0 && hooks.failingAclXattr == name) {
                errno = hooks.failingAclErrno;
                return false;
            }
#endif

            QByteArray value;
            const int state = readXattrFd(sourceFd, name, &value);
            if (state < 0 && errno != ENOTSUP) {
                return false;
            }

            if (state == 1) {
                return ::fsetxattr(destinationFd, name, value.constData(),
                                   static_cast<size_t>(value.size()), 0) == 0;
            }

            if (::fremovexattr(destinationFd, name) == 0 || errno == ENODATA || errno == ENOTSUP) {
                return true;
            }

            return false;
        }

        /**
         * @brief SELinuxユーザの違いで拒否されたsecurity.selinuxを、ユーザ部分だけを復元先の値に合わせて書き直す
         *
         * qsnapper_dbus_tがcan_change_object_identity属性を持たない古いポリシーでは、
         * SELinuxユーザの異なるコンテキストの書き込みがSELinuxの制約で拒否される
         * ロール・タイプ・レベルはスナップショットの値を保ち、アクセス制御に効くタイプを失わないようにする
         *
         * @param destinationFd 復元先のfd
         * @param snapshotContext スナップショット側のsecurity.selinuxの値
         * @return 書き直せた場合はtrue、書き直せなかった場合はfalse (errno設定)
         */
        bool writeSelinuxContextWithDestinationUser(int destinationFd, const QByteArray &snapshotContext)
        {
            QByteArray destinationContext;
            if (readXattrFd(destinationFd, kSelinuxXattr, &destinationContext) != 1) {
                return false;
            }

            const QByteArray adjusted = selinuxContextWithDestinationUser(snapshotContext, destinationContext);
            if (adjusted.isEmpty()) {
                return false;
            }

            return ::fsetxattr(destinationFd, kSelinuxXattr, adjusted.constData(),
                               static_cast<size_t>(adjusted.size()), 0) == 0;
        }

        /**
         * @brief 必須項目と個別に扱うxattr以外を、失敗しても差し替えを止めずにコピーする
         *
         * POSIX ACLとsecurity.capabilityはapplyRestoredMetadataが必須項目として別に適用するため、ここでは扱わない
         * 一覧の取得に失敗した場合も1件の失敗として数え、呼び出し側が失敗の有無を判断できるようにする
         *
         * @param sourceFd 復元元のfd
         * @param destinationFd 復元先のfd
         * @return コピーできなかったxattrの件数
         * @note security.selinuxの書き込みがSELinuxに拒否された場合(EACCES / EPERM)は、
         *       ユーザ部分だけを復元先の値にして1回だけ再試行する
         */
        int copyRemainingXattrsBestEffort(int sourceFd, int destinationFd)
        {
            QByteArray names;
            bool listed = false;
            for (int attempt = 0; attempt < 4 && !listed; ++attempt) {
                const ssize_t size = ::flistxattr(sourceFd, nullptr, 0);
                if (size < 0) {
                    return errno == ENOTSUP ? 0 : 1;
                }

                names = QByteArray(static_cast<qsizetype>(size), '\0');
                const ssize_t length = ::flistxattr(sourceFd, names.data(), static_cast<size_t>(names.size()));
                if (length >= 0) {
                    names.truncate(static_cast<qsizetype>(length));
                    listed = true;
                }
                else if (errno != ERANGE) {
                    return 1;
                }
            }
            if (!listed) {
                return 1;
            }

            int failures = 0;
            for (const QByteArray &name : names.split('\0')) {
                if (name.isEmpty() || name == kAclAccessXattr || name == kAclDefaultXattr
                        || name == kCapabilityXattr) {
                    continue;
                }

                QByteArray value;
                const int state = readXattrFd(sourceFd, name.constData(), &value);
                if (state == 0) {
                    continue;
                }
                if (state < 0) {
                    ++failures;
                    continue;
                }

                if (::fsetxattr(destinationFd, name.constData(), value.constData(),
                                static_cast<size_t>(value.size()), 0) == 0) {
                    continue;
                }

                const bool deniedBySelinux = name == kSelinuxXattr && (errno == EACCES || errno == EPERM);
                if (!deniedBySelinux || !writeSelinuxContextWithDestinationUser(destinationFd, value)) {
                    ++failures;
                }
            }

            return failures;
        }

        /**
         * @brief 宛先と同じディレクトリに置く、一時的な名前を生成する
         *
         * 形式は "." + 宛先の名前 + "." + tag + "." + 16桁の16進乱数
         * 宛先の親ディレクトリには他の利用者が書き込める場合があるため、pidや時刻のような推測できる値は使わない
         * 名前を予測されると、先にsymlinkやFIFOを置かれて作成を妨害されるためである
         * NAME_MAXを超える場合は、宛先の名前の部分を文字単位で切り詰める (UTF-8の途中で切らない)
         *
         * @param leafName 宛先の最終成分名
         * @param tag 用途を示すASCIIのタグ
         * @return 一時的な名前
         */
        QByteArray temporarySiblingLeafName(const QByteArray &leafName, const char *tag)
        {
            const QByteArray suffix = QByteArray(".") + tag + '.'
                    + QByteArray::number(QRandomGenerator::system()->generate64(), 16)
                          .rightJustified(16, '0');

            const qsizetype maxBaseBytes = static_cast<qsizetype>(NAME_MAX) - 1 - suffix.size();
            QString base = QString::fromUtf8(leafName);
            while (!base.isEmpty() && base.toUtf8().size() > maxBaseBytes) {
                base.chop(1);
            }

            return QByteArray(".") + base.toUtf8() + suffix;
        }

        /**
         * @brief 親dirfd上の名前が、期待するinodeを指しているかを確かめる
         *
         * fstatat()にAT_SYMLINK_NOFOLLOWを付けるため、名前がsymlinkへ差し替えられていても、その先ではなく名前自体のinodeを比べる
         *
         * @param parentFd 親ディレクトリのfd
         * @param name 名前
         * @param expected 期待するinodeのstat
         * @return 同一のinodeならtrue、指していない場合はfalse (errno=ESTALE)、statに失敗した場合もfalse (errno設定)
         */
        bool isSameInodeAt(int parentFd, const QByteArray &name, const struct stat &expected)
        {
            struct stat current;
            if (::fstatat(parentFd, name.constData(), &current, AT_SYMLINK_NOFOLLOW) < 0) {
                return false;
            }
            if (current.st_dev != expected.st_dev || current.st_ino != expected.st_ino) {
                errno = ESTALE;
                return false;
            }
            return true;
        }

        /**
         * @brief 一時objectを、自分が作成したinodeを指している場合に限り削除する
         *
         * 差し替えられていた場合は他の利用者のobjectを消さないよう、何もしない
         * errnoは、呼び出し元が元の失敗理由をそのまま受け取れるよう、呼び出し前の値を保持する
         *
         * @param parentFd 親ディレクトリのfd
         * @param temporaryName 一時的な名前
         * @param created 作成直後に記録したstat
         */
        void discardTemporaryAt(int parentFd, const QByteArray &temporaryName, const struct stat &created)
        {
            const int savedErrno = errno;
            if (isSameInodeAt(parentFd, temporaryName, created)) {
                ::unlinkat(parentFd, temporaryName.constData(), 0);
            }
            errno = savedErrno;
        }

        /**
         * @brief renameの直前の検証、renameat()、rename直後の検証を順に行う
         *
         * 直前の検証は、renameの直前に一時名を差し替えられた場合に、他の利用者のobjectを宛先へ移さないためである
         * 直後の検証は、renameの直後に宛先を差し替えられた場合に、成功を返して呼び出し元の検証をすり抜けさせないためである
         *
         * @param parentFd 親ディレクトリのfd
         * @param temporaryName 一時的な名前
         * @param leafName 宛先の最終成分名
         * @param created 作成直後に記録した一時objectのstat
         * @return 自分のinodeを宛先へ差し替えられた場合はtrue、失敗した場合はfalse (errno設定)
         */
        bool renameVerifiedAt(int parentFd, const QByteArray &temporaryName,
                              const QByteArray &leafName, const struct stat &created)
        {
            if (const auto &hook = detail::beforeReplaceRenameHook()) {
                hook(parentFd, temporaryName);
            }

            if (!isSameInodeAt(parentFd, temporaryName, created)) {
                // 差し替えられた一時名は自分のobjectではないため、削除もrenameもしない
                errno = ESTALE;
                return false;
            }

            if (::renameat(parentFd, temporaryName.constData(), parentFd, leafName.constData()) < 0) {
                discardTemporaryAt(parentFd, temporaryName, created);
                return false;
            }

            // renameの直後に宛先が別objectへ差し替えられていないかを確かめる
            if (!isSameInodeAt(parentFd, leafName, created)) {
                errno = ESTALE;
                return false;
            }

            return true;
        }

        /**
         * @brief 親dirfdと宛先の名前の引数を検証する
         *
         * 名前は同じディレクトリ内の1成分としてrenameat()へ渡すため、空・"."・".."・"/"・NULを含む値は拒否する
         *
         * @param parentFd 親ディレクトリのfd
         * @param leafName 宛先の最終成分名
         * @return 妥当な場合はtrue、不正な場合はfalse (errno=EINVAL)
         */
        bool isValidReplaceTarget(int parentFd, const QByteArray &leafName)
        {
            if (parentFd < 0 || leafName.isEmpty() || leafName == "." || leafName == ".."
                    || leafName.contains('/') || leafName.contains('\0')) {
                errno = EINVAL;
                return false;
            }
            return true;
        }
    } // namespace

    /**
     * @brief dirFdを基点に、相対パス上の既存ディレクトリを読み取り用に開く
     *
     * 中間成分の解決はsafeOpenRegularFileReadAtと同じで、dirFdの外へは解決しない
     * 最終成分はO_NOFOLLOW | O_DIRECTORYで開くため、symlinkやディレクトリ以外は拒否する
     *
     * @param dirFd 基点となるディレクトリのfd (pin済みのスナップショットdirfdなど)
     * @param relativePath dirFdから見た相対パス
     * @return 成功時はディレクトリのfd (呼び出し側でclose)、失敗時は-1 (errno設定)
     */
    int safeOpenDirectoryReadAt(int dirFd, const QString &relativePath)
    {
        if (dirFd < 0) {
            errno = EINVAL;
            return -1;
        }

        if (!validateRelativePathAt(relativePath, nullptr)) {
            return -1;
        }

        // 検証済みの同じrelativePathから親dirfdとleaf名を導出するため、検証と使用の間で経路が変わらない
        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QByteArray leafName;
        const UniqueFd parentFd(openParentDirectoryInRootAt(dirFd, components, &leafName));
        if (!parentFd.isValid()) {
            return -1;
        }

        return ::openat(parentFd.get(), leafName.constData(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }

    /**
     * @brief rootPath配下の宛先について、親ディレクトリを1回だけ解決して開き、変更主体の判定材料も同じwalkから得る
     *
     * 一時objectの作成からrenameat()まで同じ親dirfdを使い続け、名前を解決し直さないための入口である
     * 中間ディレクトリは作成せず、O_NOFOLLOWで1成分ずつ辿る
     * 親の解決と所有者の集計を別々に行うと、その間の差し替えで矛盾した判定になるため、同じwalkの結果を使う
     *
     * @param rootPath 基点となるルートディレクトリ
     * @param destinationPath 宛先の絶対パス (rootPath配下であること)
     * @param leafNameOut 最終成分名を返す先
     * @param ownershipOut 変更主体を判定するための所有者の状態を返す先 (nullptr可)
     * @return 成功時は親ディレクトリのfd (呼び出し側でclose)、失敗時は-1 (errno設定)
     */
    int openDestinationParentBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                          QByteArray *leafNameOut, DirectoryChainOwnership *ownershipOut)
    {
        if (!leafNameOut) {
            errno = EINVAL;
            return -1;
        }

        QStringList components;
        if (!splitDestinationComponents(rootPath, destinationPath, &components)) {
            return -1;
        }

        return openLeafParentBeneathRoot(rootPath, components, false, 0, leafNameOut, ownershipOut);
    }

    /**
     * @brief スナップショット側のSELinuxコンテキストのユーザ部分だけを、復元先の現在の値に置き換える
     *
     * user:role:type[:level]の形をしている値だけを扱い、壊れた値は空で返して書き戻さない
     * ユーザ部分が同じならSELinuxユーザの違いが原因ではないため、置き換えずに空で返す
     * 末尾のNULは、入力に付いていた場合だけ書き戻す
     *
     * @param snapshotContext スナップショット側のsecurity.selinuxの値 (末尾のNULを含んでもよい)
     * @param destinationContext 復元先の現在のsecurity.selinuxの値 (末尾のNULを含んでもよい)
     * @return 置き換えた値、置き換えない場合は空のQByteArray
     */
    QByteArray selinuxContextWithDestinationUser(const QByteArray &snapshotContext,
                                                 const QByteArray &destinationContext)
    {
        // xattrの値は末尾にNULを含むことが多いため、解析の前に外し、書き戻すときに元の形へ戻す
        const auto withoutTerminator = [](const QByteArray &context) {
            return context.endsWith('\0') ? context.left(context.size() - 1) : context;
        };
        const QByteArray snapshotText = withoutTerminator(snapshotContext);
        const QByteArray destinationText = withoutTerminator(destinationContext);

        // user:role:type[:level]の形だけを扱う (壊れた値を書き戻してSELinuxの解釈を変えないため)
        const auto isWellFormed = [](const QByteArray &context) {
            return !context.contains('\0') && context.count(':') >= 2 && context.indexOf(':') > 0;
        };
        if (!isWellFormed(snapshotText) || !isWellFormed(destinationText)) {
            return {};
        }

        const qsizetype snapshotUserEnd = snapshotText.indexOf(':');
        const QByteArray destinationUser = destinationText.left(destinationText.indexOf(':'));
        // ユーザが同じならSELinuxユーザの違いが原因ではないため、書き換えずに失敗として返す
        if (snapshotText.left(snapshotUserEnd) == destinationUser) {
            return {};
        }

        QByteArray adjusted = destinationUser + snapshotText.mid(snapshotUserEnd);
        if (snapshotContext.endsWith('\0')) {
            adjusted.append('\0');
        }
        return adjusted;
    }

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
    namespace detail {
        /**
         * @brief applyRestoredMetadataの段階を通知する関数を設定する
         *
         * ACLの同期がmodeの適用より先に終わっていることを、競合のタイミングに頼らずテストで確かめるために使う
         *
         * @param observer 通知を受け取る関数 (nullptrなら通知を止める)
         * @param context observerの第3引数へそのまま渡すポインタ (観測結果の記録先など、observerがnullptrなら無視する)
         */
        void setMetadataStageObserverForTesting(MetadataStageObserver observer, void *context)
        {
            MetadataTestHooks &hooks = metadataTestHooks();
            hooks.observer = observer;
            hooks.context = observer ? context : nullptr;
        }

        /**
         * @brief 指定したACLのxattrの同期を、復元先のACLを変更する前に失敗させる
         *
         * ACLの同期に失敗した場合にmodeを広げないことを、実際のファイルシステムでACLの書き込みを失敗させずにテストで確かめるために使う
         *
         * @param xattrName 失敗させるxattr名
         * @param errorNumber 失敗時に設定するerrno (0を渡すと失敗させる設定を解除する)
         */
        void setAclSyncFailureForTesting(const char *xattrName, int errorNumber)
        {
            MetadataTestHooks &hooks = metadataTestHooks();
            hooks.failingAclXattr = errorNumber != 0 && xattrName ? QByteArray(xattrName) : QByteArray();
            hooks.failingAclErrno = hooks.failingAclXattr.isEmpty() ? 0 : errorNumber;
        }

        /**
         * @brief 所有者専用のグループかどうかの判定を、指定した関数に置き換える
         *
         * テストを実行するホストの利用者とグループの登録に左右されないよう、
         * 所有者専用のグループかどうかの判定を置き換えるために使う
         * 置き換わるのはこの判定だけで、otherの書き込み、groupの書き込み、拡張ACLの判定は置き換わらない
         *
         * @param check 所有者専用のグループかどうかを判定する関数 (nullptrなら本来の判定へ戻す)
         */
        void setUserPrivateGroupCheckForTesting(UserPrivateGroupCheck check)
        {
            metadataTestHooks().userPrivateGroupCheck = check;
        }

        /**
         * @brief 設定されている所有者専用グループの判定関数を返す
         *
         * @return 判定関数、設定されていない場合はnullptr
         */
        UserPrivateGroupCheck userPrivateGroupCheck()
        {
            return metadataTestHooks().userPrivateGroupCheck;
        }

        /**
         * @brief metadata適用のテスト用フックをすべて解除する
         *
         * フックの設定が次のテストへ残ると、無関係なテストの結果まで変わるため、各テストの終了時に呼ぶ
         */
        void clearMetadataTestHooksForTesting()
        {
            metadataTestHooks() = MetadataTestHooks{};
        }
    } // namespace detail
#endif

    /**
     * @brief 復元元の所有者・ACL・mode・xattr・capabilityを、復元先のfdへ順に適用する
     *
     * 所有者の変更はsetuid/setgidビットとsecurity.capabilityを落とすため、modeとcapabilityより先に行う
     * ACLはmodeより先に同期する (modeを広げた瞬間から、継承した名前付きエントリが効いてしまうため)
     * 必須項目に失敗した場合は最初のerrnoを記録し、差し替えを中止するかどうかを呼び出し元が決められるようにする
     *
     * @param sourceFd 復元元のfd
     * @param destinationFd 復元先のfd
     * @param isDirectory ディレクトリとして扱う場合true (default ACLを同期し、capabilityは扱わない)
     * @return 適用結果 (必須項目の成否と、失敗しても差し替えを止めないxattrの失敗件数)
     */
    RestoredMetadataResult applyRestoredMetadata(int sourceFd, int destinationFd, bool isDirectory)
    {
        RestoredMetadataResult result;
        const auto recordMandatoryFailure = [&result]() {
            if (!result.mandatoryFailed) {
                result.mandatoryFailed = true;
                result.mandatoryErrno = errno;
            }
        };

        struct stat sourceStat;
        if (::fstat(sourceFd, &sourceStat) < 0) {
            recordMandatoryFailure();
            return result;
        }

        // 所有者の変更はS_ISUID / S_ISGIDとsecurity.capabilityを落とすため、modeとcapabilityより先に行う
        bool ownershipRestored = true;
        if (::fchown(destinationFd, sourceStat.st_uid, sourceStat.st_gid) < 0) {
            recordMandatoryFailure();
            ownershipRestored = false;
        }

        // 所有者を復元できていない場合はsetuid/setgidを立てず、誤った所有者に特権が付くのを防ぐ
        const mode_t restoredMode = ownershipRestored
                ? (sourceStat.st_mode & 07777)
                : (sourceStat.st_mode & (07777 & ~(S_ISUID | S_ISGID)));

        // ACLは、復元元のmodeを適用するより先に同期する
        // 復元先に作った一時ファイルやディレクトリは、親ディレクトリのdefault ACLから名前付きエントリを継承している
        // 作成直後のmodeは0600、ディレクトリは0700であり、ACLのmaskが---になるため、継承したエントリは効いていない
        // 先にfchmodでgroupのビットを広げるとmaskも広がり、ACLを同期するまでの間、名前付きユーザが復元中の内容を開けてしまう
        // ACLの同期に失敗した場合は、modeを広げずに戻る
        // 差し替えを中止しないrelaxed方針の呼び出し元でも、復元元のmodeまでは広がらない
        if (!syncAclXattrFd(sourceFd, destinationFd, kAclAccessXattr)) {
            recordMandatoryFailure();
            return result;
        }

        if (isDirectory && !syncAclXattrFd(sourceFd, destinationFd, kAclDefaultXattr)) {
            recordMandatoryFailure();
            return result;
        }

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
        notifyMetadataStage(detail::MetadataStage::BeforeModeRestore, destinationFd);
#endif
        if (::fchmod(destinationFd, restoredMode) < 0) {
            recordMandatoryFailure();
        }

        // 最初のfchmodが失敗した場合や、ACLの設定でgroupのビットが変わった場合に備えて、
        // 最終的なmodeが復元元と異なるときだけ、もう一度fchmodする
        struct stat destinationStat;
        if (::fstat(destinationFd, &destinationStat) == 0
                && (destinationStat.st_mode & 07777) != restoredMode
                && ::fchmod(destinationFd, restoredMode) < 0) {
            recordMandatoryFailure();
        }

        result.optionalFailures = copyRemainingXattrsBestEffort(sourceFd, destinationFd);

        // file capabilityは所有者とmodeの変更で失われるため、最後に設定する
        // 新規inodeには存在しないため、復元元に無い場合の削除は不要
        // 所有者を復元できていない場合は、誤った所有者に権限を与えないよう付与しない
        if (!isDirectory && ownershipRestored) {
            QByteArray capability;
            const int state = readXattrFd(sourceFd, kCapabilityXattr, &capability);
            if (state < 0 && errno != ENOTSUP) {
                recordMandatoryFailure();
            }
            else if (state == 1
                     && ::fsetxattr(destinationFd, kCapabilityXattr, capability.constData(),
                                    static_cast<size_t>(capability.size()), 0) < 0) {
                recordMandatoryFailure();
            }
        }

        return result;
    }

    /**
     * @brief 通常ファイルを、内容とmetadataを整えた一時objectへ作ってから、宛先と原子的に差し替える
     *
     * 内容はreflink(可能な場合)かsendfile()で写し、metadataを適用したあとにrenameする
     * 途中で失敗した場合は宛先を変更せず、作成した一時objectだけを削除する
     *
     * @param destinationParentFd 宛先の親ディレクトリのfd
     * @param leafName 宛先の最終成分名
     * @param sourceFd 復元元のfd (読み取り位置は変更しない)
     * @param tryReflink FICLONEによる複製を試す場合true
     * @param mustPreserveMetadata metadataを保てない場合に差し替えを中止する場合true
     * @param metadataOut 適用結果を返す先 (nullptr可)
     * @return 差し替えに成功した場合はtrue、失敗した場合はfalse (errno設定、作成した一時objectは削除済み)
     */
    bool replaceRegularFileAt(int destinationParentFd, const QByteArray &leafName, int sourceFd,
                              bool tryReflink, bool mustPreserveMetadata,
                              RestoredMetadataResult *metadataOut)
    {
        if (!isValidReplaceTarget(destinationParentFd, leafName) || sourceFd < 0) {
            errno = EINVAL;
            return false;
        }

        struct stat sourceStat;
        if (::fstat(sourceFd, &sourceStat) < 0) {
            return false;
        }
        if (!S_ISREG(sourceStat.st_mode)) {
            errno = EINVAL;
            return false;
        }

        // 権限を絞った状態 (0600) で作成し、所有者とmodeは後からfd経由で設定する
        // 一時名は予測できないため、EEXISTは偶然の衝突に限られる
        QByteArray temporaryName;
        int rawFd = -1;
        for (int attempt = 0; attempt < 16 && rawFd < 0; ++attempt) {
            temporaryName = temporarySiblingLeafName(leafName, "qsnapper-copy");
            rawFd = ::openat(destinationParentFd, temporaryName.constData(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (rawFd < 0 && errno != EEXIST) {
                return false;
            }
        }
        if (rawFd < 0) {
            return false;
        }

        UniqueFd destinationFd(rawFd);
        struct stat created;
        if (::fstat(destinationFd.get(), &created) < 0) {
            // 作成したinodeを識別できないため、同名の別objectを消さないよう一時名は残す
            // 自分のfdに対するfstatの失敗は実質起きず、残る場合も中身は空で、modeは0600、所有者はrootになる
            return false;
        }

        const auto abortReplace = [&]() {
            const int savedErrno = errno;
            destinationFd.reset();
            discardTemporaryAt(destinationParentFd, temporaryName, created);
            errno = savedErrno;
            return false;
        };

        bool copied = tryReflink && ::ioctl(destinationFd.get(), FICLONE, sourceFd) == 0;
        if (!copied) {
            off_t offset = 0;
            off_t remaining = sourceStat.st_size;
            while (remaining > 0) {
                const ssize_t written = ::sendfile(destinationFd.get(), sourceFd, &offset,
                                                   static_cast<size_t>(remaining));
                if (written < 0) {
                    return abortReplace();
                }
                if (written == 0) {
                    // 期待したバイト数に達する前にEOFになった
                    errno = EIO;
                    return abortReplace();
                }
                remaining -= written;
            }
        }

        const RestoredMetadataResult metadata = applyRestoredMetadata(sourceFd, destinationFd.get(), false);
        if (metadataOut) {
            *metadataOut = metadata;
        }
        if (metadata.mandatoryFailed && mustPreserveMetadata) {
            errno = metadata.mandatoryErrno;
            return abortReplace();
        }

        const struct timespec times[2] = { sourceStat.st_atim, sourceStat.st_mtim };
        if (::futimens(destinationFd.get(), times) < 0 && mustPreserveMetadata) {
            return abortReplace();
        }

        destinationFd.reset();
        return renameVerifiedAt(destinationParentFd, temporaryName, leafName, created);
    }

    /**
     * @brief symlinkを、所有者と時刻を整えた一時symlinkへ作ってから、宛先と原子的に差し替える
     *
     * symlinkのtargetは作成時にしか設定できないため、正しいtargetを持つ一時symlinkを作り、fdで固定してからmetadataを適用する
     * 途中で失敗した場合は宛先を変更せず、作成した一時symlinkだけを削除する
     *
     * @param destinationParentFd 宛先の親ディレクトリのfd
     * @param leafName 宛先の最終成分名
     * @param linkTarget symlinkのtarget
     * @param sourceStat 復元元の所有者と時刻
     * @param metadataApplied 所有者と時刻を適用できたかを返す先 (nullptr可)
     * @param mustPreserveMetadata metadataを保てない場合に差し替えを中止する場合true
     * @return 差し替えに成功した場合はtrue、失敗した場合はfalse (errno設定)
     */
    bool replaceSymlinkAt(int destinationParentFd, const QByteArray &leafName,
                          const QByteArray &linkTarget, const struct stat &sourceStat,
                          bool *metadataApplied, bool mustPreserveMetadata)
    {
        if (metadataApplied) {
            *metadataApplied = false;
        }
        if (!isValidReplaceTarget(destinationParentFd, leafName) || linkTarget.isEmpty()) {
            errno = EINVAL;
            return false;
        }

        // 一時名は予測できないため、EEXISTは偶然の衝突に限られる
        QByteArray temporaryName;
        bool createdLink = false;
        for (int attempt = 0; attempt < 16 && !createdLink; ++attempt) {
            temporaryName = temporarySiblingLeafName(leafName, "qsnapper-link");
            if (::symlinkat(linkTarget.constData(), destinationParentFd, temporaryName.constData()) == 0) {
                createdLink = true;
            }
            else if (errno != EEXIST) {
                return false;
            }
        }
        if (!createdLink) {
            return false;
        }

        // 失敗時の後始末で他者のobjectを消さないよう、作成直後の一時名のinodeを記録する
        // euid(実効ユーザID)所有のsymlinkでなければ、作成直後に差し替えられたものとして扱い、記録しない
        struct stat createdByName;
        bool cleanupIdentityKnown = ::fstatat(destinationParentFd, temporaryName.constData(),
                                              &createdByName, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(createdByName.st_mode) && createdByName.st_uid == ::geteuid();
        const auto abortReplace = [&]() {
            if (cleanupIdentityKnown) {
                discardTemporaryAt(destinationParentFd, temporaryName, createdByName);
            }
            return false;
        };
        if (!cleanupIdentityKnown) {
            errno = ESTALE;
            return false;
        }

        // 作成した一時symlink自体をfdで固定し、以降の操作で名前を再解決しない
        // 名前を再解決すると、そのたびに差し替えの影響を受けるためである
        const UniqueFd linkFd(::openat(destinationParentFd, temporaryName.constData(),
                                       O_PATH | O_NOFOLLOW | O_CLOEXEC));
        if (!linkFd.isValid()) {
            return abortReplace();
        }

        struct stat created;
        if (::fstat(linkFd.get(), &created) < 0) {
            return abortReplace();
        }

        // 作成直後に差し替えられていないかを、記録したinode、symlinkであること、所有者がeuid、targetの一致で確かめる
        // 一つでも一致しない場合は、他の利用者のobjectを宛先へ移さないようESTALEで中止する
        QByteArray currentTarget(linkTarget.size() + 1, '\0');
        const ssize_t targetLength = ::readlinkat(linkFd.get(), "", currentTarget.data(),
                                                  static_cast<size_t>(currentTarget.size()));
        if (created.st_dev != createdByName.st_dev || created.st_ino != createdByName.st_ino
                || !S_ISLNK(created.st_mode) || created.st_uid != ::geteuid()
                || targetLength != static_cast<ssize_t>(linkTarget.size())
                || currentTarget.left(targetLength) != linkTarget) {
            errno = ESTALE;
            return abortReplace();
        }

        const bool ownerUpdated = ::fchownat(linkFd.get(), "", sourceStat.st_uid, sourceStat.st_gid,
                                       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == 0;
        int metadataErrno = ownerUpdated ? 0 : errno;   // 失敗理由をrenameせずに返すため保持する
        const struct timespec times[2] = { sourceStat.st_atim, sourceStat.st_mtim };
        bool timesUpdated = ::utimensat(linkFd.get(), "", times, AT_EMPTY_PATH) == 0;
        if (!timesUpdated && errno == EINVAL) {
            // utimensat()のAT_EMPTY_PATHに対応していないカーネルではEINVALになる
            // その場合は同じ親dirfd上の一時名へ適用し、前後で同じinodeであることを確かめる
            timesUpdated = isSameInodeAt(destinationParentFd, temporaryName, created)
                    && ::utimensat(destinationParentFd, temporaryName.constData(), times, AT_SYMLINK_NOFOLLOW) == 0
                    && isSameInodeAt(destinationParentFd, temporaryName, created);
        }
        if (!timesUpdated && metadataErrno == 0) {
            metadataErrno = errno;
        }
        if (metadataApplied) {
            *metadataApplied = ownerUpdated && timesUpdated;
        }

        // 所有者を保てないとroot所有のsymlinkが残り、fs.protected_symlinksの判定が変わる
        // fs.protected_symlinksはsticky付きディレクトリでの追従可否を決めるカーネル設定である
        // 必須の場合はrenameせず、live側を元のまま残す
        if (mustPreserveMetadata && !(ownerUpdated && timesUpdated)) {
            errno = metadataErrno != 0 ? metadataErrno : EPERM;
            const int savedErrno = errno;
            abortReplace();
            errno = savedErrno;
            return false;
        }

        return renameVerifiedAt(destinationParentFd, temporaryName, leafName, created);
    }

    /**
     * @brief .snapshots直下のスナップショット一覧から、変更を検出するための指紋を作る
     *
     * ディレクトリ自身と各エントリ、info.xmlのstat、数字だけの名前の集合からSHA-256を計算する
     * 一覧の変化を検出するために使う
     *
     * @param snapshotsDirPath .snapshotsディレクトリのパス
     * @return 指紋、一覧を読めなかった場合は空のQByteArray
     */
    QByteArray snapshotListFingerprint(const QString &snapshotsDirPath)
    {
        // .snapshotsやその祖先がsymlinkへ差し替えられた場合に別の木の指紋を作らないよう、
        // 成分ごとにsymlinkを辿らずに開く
        const int dirFd = safeOpenDirectory(snapshotsDirPath);
        if (dirFd < 0) {
            return {};
        }

        DIR *dir = ::fdopendir(dirFd);
        if (!dir) {
            ::close(dirFd);
            return {};
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        const auto addValue = [&hash](qint64 value) {
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(&value), sizeof(value)));
        };
        const auto addStat = [&addValue](const struct stat &st) {
            addValue(static_cast<qint64>(st.st_dev));
            addValue(static_cast<qint64>(st.st_ino));
            addValue(static_cast<qint64>(st.st_size));
            addValue(static_cast<qint64>(st.st_mtim.tv_sec));
            addValue(static_cast<qint64>(st.st_mtim.tv_nsec));
            addValue(static_cast<qint64>(st.st_ctim.tv_sec));
            addValue(static_cast<qint64>(st.st_ctim.tv_nsec));
        };

        struct stat dirStat;
        if (::fstat(dirFd, &dirStat) < 0) {
            ::closedir(dir);
            return {};
        }
        addStat(dirStat);

        // libsnapperは数字だけの名前のエントリをスナップショットとして扱う
        // 先頭に0が付いた名前 (例 : 01) も含めるため、数値に変換せず名前のまま比較する
        std::vector<QByteArray> names;
        errno = 0;
        while (const struct dirent *entry = ::readdir(dir)) {
            const QByteArray name(entry->d_name);
            const bool allDigits = !name.isEmpty()
                    && std::all_of(name.cbegin(), name.cend(),
                                   [](char c) { return c >= '0' && c <= '9'; });
            if (allDigits) {
                names.push_back(name);
            }
            errno = 0;
        }
        if (errno != 0) {
            ::closedir(dir);
            return {};
        }
        std::sort(names.begin(), names.end());

        for (const QByteArray &name : names) {
            // 名前の境界が曖昧にならないように長さも指紋へ含める
            addValue(static_cast<qint64>(name.size()));
            hash.addData(name);

            struct stat entryStat;
            if (::fstatat(dirFd, name.constData(), &entryStat, AT_SYMLINK_NOFOLLOW) < 0) {
                ::closedir(dir);
                return {};
            }
            addStat(entryStat);

            // 作成途中でinfo.xmlがまだ無いエントリは、無いことを指紋に含める
            // 後から作られると指紋が変わるためである
            struct stat infoStat;
            const QByteArray infoPath = name + QByteArrayLiteral("/info.xml");
            if (::fstatat(dirFd, infoPath.constData(), &infoStat, AT_SYMLINK_NOFOLLOW) == 0) {
                addStat(infoStat);
            }
            else if (errno == ENOENT || errno == ENOTDIR) {
                addValue(-1);
            }
            else {
                ::closedir(dir);
                return {};
            }
        }

        ::closedir(dir);
        return hash.result();
    }
} // namespace qsnapper::security
