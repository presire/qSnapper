/**
 * @file tst_filesystemhelpers_replace.cpp
 * @brief 差し替えとmetadata適用の検証
 *
 * 一時object経由の原子的差し替えとmetadata適用、差し替え検出の検証を集める
 */
#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "filesystemhelpers.h"

#include "tst_filesystemhelpers.h"

using namespace qsnapper::security;

// ============================================================================
// 復元時のmetadata保持とrename前後の差し替え検出
// ============================================================================

namespace {

    constexpr quint16 kAclUserObj = 0x01;
    constexpr quint16 kAclUser = 0x02;
    constexpr quint16 kAclGroupObj = 0x04;
    constexpr quint16 kAclMask = 0x10;
    constexpr quint16 kAclOther = 0x20;
    constexpr quint32 kAclUndefinedId = 0xffffffffU;

    /**
     * @brief POSIX ACLの1 entry
     */
    struct AclEntry {
        quint16 tag;
        quint16 perm;
        quint32 id;
    };

    /**
     * @brief system.posix_acl_* のxattr値 (version 2、little endian) を組み立てる
     * @param entries tag順・id順に並べたentry
     * @return xattr値
     */
    QByteArray makeAclXattr(const QList<AclEntry> &entries)
    {
        QByteArray value;
        const auto append = [&value](quint64 number, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                value.append(static_cast<char>((number >> (8 * i)) & 0xff));
            }
        };
        append(2, 4);
        for (const AclEntry &entry : entries) {
            append(entry.tag, 2);
            append(entry.perm, 2);
            append(entry.id, 4);
        }
        return value;
    }

    /**
     * @brief named user entryを1件持つACLを返す (mode 0640相当)
     * @param namedUser named entryのuid
     * @return xattr値
     */
    QByteArray namedUserAcl(quint32 namedUser)
    {
        return makeAclXattr({
            {kAclUserObj, 6, kAclUndefinedId},
            {kAclUser, 4, namedUser},
            {kAclGroupObj, 4, kAclUndefinedId},
            {kAclMask, 4, kAclUndefinedId},
            {kAclOther, 0, kAclUndefinedId},
        });
    }

    /**
     * @brief パスのxattr値を読み出す
     * @param path 対象パス
     * @param name xattr名
     * @param valueOut 値の返却先
     * @return 1: 値あり、0: 属性なし、-1: エラー
     */
    int readXattr(const QString &path, const char *name, QByteArray *valueOut)
    {
        char buffer[4096];
        const ssize_t length = ::lgetxattr(path.toUtf8().constData(), name, buffer, sizeof(buffer));
        if (length < 0) {
            return errno == ENODATA ? 0 : -1;
        }
        *valueOut = QByteArray(buffer, static_cast<qsizetype>(length));
        return 1;
    }

    /**
     * @brief パスへxattrを設定する
     * @param path 対象パス
     * @param name xattr名
     * @param value 値
     * @return 成功時0、失敗時errno
     */
    int writeXattr(const QString &path, const char *name, const QByteArray &value)
    {
        if (::lsetxattr(path.toUtf8().constData(), name, value.constData(),
                        static_cast<size_t>(value.size()), 0) < 0) {
            return errno;
        }
        return 0;
    }

    /**
     * @brief ファイルを指定内容・modeで作成する
     * @param path 作成先
     * @param content 内容
     * @param mode 最終的に設定するmode
     * @return 作成できた場合true
     */
    bool createFile(const QString &path, const QByteArray &content, mode_t mode)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || file.write(content) != content.size()) {
            return false;
        }
        file.close();
        return ::chmod(path.toUtf8().constData(), mode) == 0;
    }

    /**
     * @brief 復元元ファイルを開き、宛先を親dirfd + leaf名で差し替える
     * @param root 宛先の基点
     * @param source 復元元パス
     * @param destination 宛先パス
     * @param metadataOut metadataの適用結果
     * @param mustPreserveMetadata 必須metadataの失敗時に差し替えを中止する場合true
     * @return 差し替えに成功した場合true
     */
    bool replaceFromPath(const QString &root, const QString &source, const QString &destination,
                         RestoredMetadataResult *metadataOut = nullptr, bool mustPreserveMetadata = true)
    {
        const int sourceFd = ::open(source.toUtf8().constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (sourceFd < 0) {
            return false;
        }
        QByteArray leafName;
        const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
        if (parentFd < 0) {
            ::close(sourceFd);
            return false;
        }
        const bool replaced = replaceRegularFileAt(parentFd, leafName, sourceFd, false,
                                                   mustPreserveMetadata, metadataOut);
        const int savedErrno = errno;
        ::close(parentFd);
        ::close(sourceFd);
        errno = savedErrno;
        return replaced;
    }

    /**
     * @brief ディレクトリ内に一時名 (".qsnapper-") が残っていないか確認する
     * @param directory 対象ディレクトリ
     * @return 残っていなければtrue
     */
    bool hasNoTemporaryLeftovers(const QString &directory)
    {
        const QStringList entries = QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (entry.contains(QStringLiteral(".qsnapper-"))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 開いているfdから、xattrの値を読み出す
     *
     * 復元中の一時ファイルの名前はテストから分からないため、パスではなく通知で受け取ったfdから読む
     *
     * @param fd 対象fd
     * @param name xattr名
     * @param valueOut 値の返却先
     * @return 1: 値あり、0: 属性なし、-1: 読み出しエラー
     */
    int readFdXattr(int fd, const char *name, QByteArray *valueOut)
    {
        char buffer[4096];
        const ssize_t length = ::fgetxattr(fd, name, buffer, sizeof(buffer));
        if (length < 0) {
            return errno == ENODATA ? 0 : -1;
        }
        *valueOut = QByteArray(buffer, static_cast<qsizetype>(length));
        return 1;
    }

    /**
     * @brief ACLのxattr値から、指定したエントリの権限ビットを取り出す
     *
     * maskエントリの権限を調べ、継承した名前付きエントリが有効になっているかを判定するために使う
     *
     * @param acl ACLのxattr値 (先頭4バイトのヘッダの後に、8バイトのエントリが並ぶ形式)
     * @param tag エントリの種類 (kAclUser、kAclMaskなど)
     * @param id エントリのid (名前付きユーザ以外のエントリはkAclUndefinedId)
     * @return 権限ビット (0から7)、エントリが無い場合は-1
     */
    int aclEntryPerm(const QByteArray &acl, quint16 tag, quint32 id)
    {
        const auto byteAt = [&acl](qsizetype index) {
            return static_cast<quint32>(static_cast<quint8>(acl.at(index)));
        };

        // 各エントリは、種類(2バイト)、権限(2バイト)、id(4バイト)をlittle endianで並べたもの
        for (qsizetype offset = 4; offset + 8 <= acl.size(); offset += 8) {
            const quint32 entryTag = byteAt(offset) | (byteAt(offset + 1) << 8);
            const quint32 entryId = byteAt(offset + 4) | (byteAt(offset + 5) << 8)
                    | (byteAt(offset + 6) << 16) | (byteAt(offset + 7) << 24);
            if (entryTag == tag && entryId == id) {
                return static_cast<int>(byteAt(offset + 2) | (byteAt(offset + 3) << 8));
            }
        }
        return -1;
    }

    /**
     * @brief 復元元のmodeを適用する直前に観測した、復元先の状態
     *
     * modeを広げる前にACLの同期が終わっているかを、テスト本体で判定するために記録する
     */
    struct ModeRestoreObservation {
        mode_t mode = 0;        // 観測時点のmode (fstatに失敗した場合は全ビットを立てた値)
        int accessState = -1;   // access ACLの有無 (1: あり、0: なし、-1: 読み出し失敗)
        QByteArray accessAcl;   // access ACLの値 (accessStateが1の場合だけ有効)
        int defaultState = 0;   // default ACLの有無 (ディレクトリ以外は読み出さないため常に0)
        QByteArray defaultAcl;  // default ACLの値 (defaultStateが1の場合だけ有効)
    };

    /**
     * @brief BeforeModeRestoreの通知を受けるたびに、復元先fdのmodeとACLを記録する
     *
     * 製品コードの処理の途中から呼ばれるため、QTestのマクロで判定して処理を中断せず、記録だけを行う
     * 記録した内容は、製品コードの処理が戻った後にテスト本体で判定する
     *
     * @param stage 通知された段階
     * @param destinationFd 復元先fd (状態を読み取るだけで、閉じない)
     * @param context 記録先のQList<ModeRestoreObservation>へのポインタ
     * @note contextが指すリストへ、観測結果を1件追加する
     */
    void recordModeRestoreObservation(detail::MetadataStage stage, int destinationFd, void *context)
    {
        if (stage != detail::MetadataStage::BeforeModeRestore) {
            return;
        }

        ModeRestoreObservation observation;
        struct stat destinationStat;
        const bool statOk = ::fstat(destinationFd, &destinationStat) == 0;
        observation.mode = statOk ? (destinationStat.st_mode & 07777) : mode_t(~0);
        observation.accessState = readFdXattr(destinationFd, "system.posix_acl_access",
                                              &observation.accessAcl);

        // default ACLはディレクトリだけが持てるため、通常ファイルでは読み出さない
        if (statOk && S_ISDIR(destinationStat.st_mode)) {
            observation.defaultState = readFdXattr(destinationFd, "system.posix_acl_default",
                                                   &observation.defaultAcl);
        }
        static_cast<QList<ModeRestoreObservation> *>(context)->append(observation);
    }

    /**
     * @brief 観測結果を、テストの失敗メッセージに表示する文字列へ変換する
     *
     * テストが失敗したときのログで、modeを適用する直前に残っていたACLを確認できるようにする
     *
     * @param observation 観測結果
     * @return modeを8進数、ACLのxattr値を16進数で表した文字列
     */
    QByteArray describeObservation(const ModeRestoreObservation &observation)
    {
        return QStringLiteral("before fchmod: mode=%1 access(%2)=%3 default(%4)=%5")
                .arg(static_cast<uint>(observation.mode), 4, 8, QLatin1Char('0'))
                .arg(observation.accessState)
                .arg(QString::fromLatin1(observation.accessAcl.toHex()))
                .arg(observation.defaultState)
                .arg(QString::fromLatin1(observation.defaultAcl.toHex()))
                .toUtf8();
    }

    /**
     * @brief modeを適用する直前の観測を、ガードの生存期間だけ有効にする
     *
     * 観測結果の記録先はガードのメンバであるため、ガードの破棄時にobserverを解除し、
     * 破棄済みのリストを指すポインタがフックに残らないようにする
     */
    struct ModeRestoreObserverGuard {
        QList<ModeRestoreObservation> observations;  // 通知された順の観測結果

        ModeRestoreObserverGuard()
        {
            detail::setMetadataStageObserverForTesting(&recordModeRestoreObservation, &observations);
        }

        ~ModeRestoreObserverGuard()
        {
            detail::setMetadataStageObserverForTesting(nullptr, nullptr);
        }

        ModeRestoreObserverGuard(const ModeRestoreObserverGuard &) = delete;
        ModeRestoreObserverGuard &operator=(const ModeRestoreObserverGuard &) = delete;
    };

    /**
     * @brief ファイルの内容を全て読み出す
     * @param path 対象パス
     * @return ファイルの内容、開けない場合は空のQByteArray
     */
    QByteArray readFileContent(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /**
     * @brief rename直前フックをスコープ終了時に解除する
     */
    struct ReplaceHookGuard {
        explicit ReplaceHookGuard(std::function<void(int, const QByteArray &)> hook)
        {
            detail::setBeforeReplaceRenameHookForTesting(std::move(hook));
        }
        ~ReplaceHookGuard()
        {
            detail::setBeforeReplaceRenameHookForTesting({});
        }
        ReplaceHookGuard(const ReplaceHookGuard &) = delete;
        ReplaceHookGuard &operator=(const ReplaceHookGuard &) = delete;
    };

}

void TestFilesystemHelpers::cleanup()
{
    // テスト関数がQVERIFYなどの失敗で途中で戻った場合でも、フックの設定が次のテストへ残らないよう、各テストの終了時に解除する
    detail::clearMetadataTestHooksForTesting();
}

void TestFilesystemHelpers::replaceRegularFileAtReplacesContentWithNewInode()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/live/file");
    const QString hardlink = root + QStringLiteral("/live/hardlink");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/live")));
    QVERIFY(createFile(source, "snapshot content", 0640));
    QVERIFY(createFile(destination, "live content that is longer", 0666));
    QVERIFY(::link(destination.toUtf8().constData(), hardlink.toUtf8().constData()) == 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QFile restored(destination);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("snapshot content"));

    // 既存inodeを上書きしないため、hardlink先は元の内容のまま残り、緩いmodeも引き継がない
    QFile other(hardlink);
    QVERIFY(other.open(QIODevice::ReadOnly));
    QCOMPARE(other.readAll(), QByteArray("live content that is longer"));

    struct stat sourceStat;
    struct stat restoredStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
    QCOMPARE(restoredStat.st_mtim.tv_sec, sourceStat.st_mtim.tv_sec);
    QCOMPARE(restoredStat.st_mtim.tv_nsec, sourceStat.st_mtim.tv_nsec);
    QVERIFY(hasNoTemporaryLeftovers(root + QStringLiteral("/live")));
}

void TestFilesystemHelpers::replaceRegularFileAtPreservesSetuidAndSetgid()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(createFile(source, "#!/bin/sh\n", 06755));
    QVERIFY(createFile(destination, "old", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    if ((sourceStat.st_mode & 06000) != 06000) {
        QSKIP("This filesystem or user cannot set setuid/setgid bits");
    }

    // 所有者の変更はS_ISUID / S_ISGIDを落とすため、fchown → fchmodの順でなければ失われる
    QVERIFY(replaceFromPath(root, source, destination));

    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, sourceStat.st_mode & 07777);
    QCOMPARE(restoredStat.st_uid, sourceStat.st_uid);
    QCOMPARE(restoredStat.st_gid, sourceStat.st_gid);
}

void TestFilesystemHelpers::replaceRegularFileAtCopiesAccessAcl()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(createFile(source, "acl", 0640));
    QVERIFY(createFile(destination, "old", 0600));

    const QByteArray acl = namedUserAcl(::getuid() + 1000);
    const int setError = writeXattr(source, "system.posix_acl_access", acl);
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QByteArray sourceAcl;
    QByteArray restoredAcl;
    QCOMPARE(readXattr(source, "system.posix_acl_access", &sourceAcl), 1);
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 1);
    QCOMPARE(restoredAcl, sourceAcl);

    struct stat sourceStat;
    struct stat restoredStat;
    QVERIFY(::lstat(source.toUtf8().constData(), &sourceStat) == 0);
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, sourceStat.st_mode & 07777);
}

void TestFilesystemHelpers::replaceRegularFileAtDropsInheritedDefaultAcl()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString liveDir = root + QStringLiteral("/live");
    const QString destination = liveDir + QStringLiteral("/file");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(source, "no acl", 0640));

    // 親ディレクトリのdefault ACLが新しいinodeへ継承されても、snapshot側に無いentryを残さない
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 7, ::getuid() + 1000},
        {kAclGroupObj, 7, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 7, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    QVERIFY(replaceFromPath(root, source, destination));

    QByteArray restoredAcl;
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 0);

    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
}

void TestFilesystemHelpers::applyRestoredMetadataSyncsDirectoryAcls()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString withAcl = tempDir.path() + QStringLiteral("/with-acl");
    const QString withoutAcl = tempDir.path() + QStringLiteral("/without-acl");
    QVERIFY(QDir().mkpath(withAcl));
    QVERIFY(QDir().mkpath(withoutAcl));
    QVERIFY(::chmod(withAcl.toUtf8().constData(), 02750) == 0);
    QVERIFY(::chmod(withoutAcl.toUtf8().constData(), 0755) == 0);

    const QByteArray defaultAcl = makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 5, ::getuid() + 1000},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 5, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    });
    const int setError = writeXattr(withAcl, "system.posix_acl_default", defaultAcl);
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);
    QCOMPARE(writeXattr(withAcl, "system.posix_acl_access", namedUserAcl(::getuid() + 1000)), 0);

    const auto apply = [](const QString &from, const QString &to) {
        const int sourceFd = ::open(from.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int destinationFd = ::open(to.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const RestoredMetadataResult result = applyRestoredMetadata(sourceFd, destinationFd, true);
        ::close(sourceFd);
        ::close(destinationFd);
        return result;
    };

    // snapshot側のaccess / default ACLを、ACLを持たない既存ディレクトリへ適用する
    const QString copyTarget = tempDir.path() + QStringLiteral("/copy-target");
    QVERIFY(QDir().mkpath(copyTarget));
    QVERIFY(!apply(withAcl, copyTarget).mandatoryFailed);
    QByteArray value;
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_default", &value), 1);
    QCOMPARE(value, defaultAcl);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_access", &value), 1);

    // snapshot側にACLが無い場合は、live側のaccess / default ACLを削除する
    QVERIFY(!apply(withoutAcl, copyTarget).mandatoryFailed);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_default", &value), 0);
    QCOMPARE(readXattr(copyTarget, "system.posix_acl_access", &value), 0);

    struct stat restoredStat;
    QVERIFY(::lstat(copyTarget.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0755U);
}

void TestFilesystemHelpers::replaceRegularFileAtSyncsInheritedAclBeforeModeRestore()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString liveDir = root + QStringLiteral("/live");
    const QString destination = liveDir + QStringLiteral("/file");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(source, "secret", 0640));

    // 復元先ディレクトリに、別のユーザ(名前付きユーザ)の読み取りを許すdefault ACLを設定し、
    // 復元中に作成する一時ファイルが、そのACLを継承する状況を再現する
    // 名前付きユーザのuidは、ACLのエントリとして使うだけなので、実在しなくてよい
    const quint32 namedUser = ::getuid() + 1000;
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 4, namedUser},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    ModeRestoreObserverGuard observer;
    QVERIFY(replaceFromPath(root, source, destination));

    // 一時ファイルのmodeを0640へ広げると、ACLのmaskがr--になり、継承した名前付きユーザの読み取りが有効になる
    // 名前付きユーザが一時ファイルを読めないよう、modeを適用する直前には継承したACLが削除されている必要がある
    QCOMPARE(observer.observations.size(), 1);
    const ModeRestoreObservation &beforeMode = observer.observations.constFirst();
    QVERIFY2(beforeMode.accessState == 0, describeObservation(beforeMode).constData());

    QByteArray restoredAcl;
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 0);
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
}

void TestFilesystemHelpers::replaceRegularFileAtSyncsSourceAclBeforeModeRestore()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString liveDir = root + QStringLiteral("/live");
    const QString destination = liveDir + QStringLiteral("/file");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(source, "secret", 0640));

    // 復元先ディレクトリのdefault ACLと、復元元のaccess ACLに、異なる名前付きユーザを設定する
    // これにより、modeを適用する直前のACLが、継承したACLか復元元のACLかを区別できる
    const quint32 inheritedUser = ::getuid() + 1000;
    const quint32 sourceUser = ::getuid() + 2000;
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 7, inheritedUser},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);
    QCOMPARE(writeXattr(source, "system.posix_acl_access", namedUserAcl(sourceUser)), 0);
    QByteArray sourceAcl;
    QCOMPARE(readXattr(source, "system.posix_acl_access", &sourceAcl), 1);

    ModeRestoreObserverGuard observer;
    QVERIFY(replaceFromPath(root, source, destination));

    // modeを適用する直前に、継承したACLが復元元のACLへ置き換わり、継承した名前付きユーザが残っていないことを確認する
    QCOMPARE(observer.observations.size(), 1);
    const ModeRestoreObservation &beforeMode = observer.observations.constFirst();
    QVERIFY2(beforeMode.accessState == 1 && beforeMode.accessAcl == sourceAcl,
             describeObservation(beforeMode).constData());
    QCOMPARE(aclEntryPerm(beforeMode.accessAcl, kAclUser, inheritedUser), -1);

    QByteArray restoredAcl;
    QCOMPARE(readXattr(destination, "system.posix_acl_access", &restoredAcl), 1);
    QCOMPARE(restoredAcl, sourceAcl);
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
}

void TestFilesystemHelpers::replaceRegularFileAtKeepsFinalAclAndMode()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString plainSource = root + QStringLiteral("/plain-source");
    const QString aclSource = root + QStringLiteral("/acl-source");
    const QString liveDir = root + QStringLiteral("/live");
    const QString plainDestination = liveDir + QStringLiteral("/plain");
    const QString aclDestination = liveDir + QStringLiteral("/acl");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(plainSource, "plain", 0644));
    QVERIFY(createFile(aclSource, "acl", 0640));
    QVERIFY(createFile(plainDestination, "old", 0600));
    QVERIFY(createFile(aclDestination, "old", 0600));

    // 復元先ディレクトリにdefault ACLがある場合でも、
    // 復元後のmodeとACLは、ACLの同期をmodeの適用より先に行う前と同じく、復元元と一致する必要がある
    // 復元元にACLが無い場合と有る場合の両方を確認する
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 7, ::getuid() + 1000},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);
    QCOMPARE(writeXattr(aclSource, "system.posix_acl_access", namedUserAcl(::getuid() + 2000)), 0);

    QVERIFY(replaceFromPath(root, plainSource, plainDestination));
    QVERIFY(replaceFromPath(root, aclSource, aclDestination));

    QByteArray value;
    QCOMPARE(readXattr(plainDestination, "system.posix_acl_access", &value), 0);
    QByteArray sourceAcl;
    QByteArray restoredAcl;
    QCOMPARE(readXattr(aclSource, "system.posix_acl_access", &sourceAcl), 1);
    QCOMPARE(readXattr(aclDestination, "system.posix_acl_access", &restoredAcl), 1);
    QCOMPARE(restoredAcl, sourceAcl);

    struct stat restoredStat;
    QVERIFY(::lstat(plainDestination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0644U);
    QVERIFY(::lstat(aclDestination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0640U);
    QCOMPARE(readFileContent(plainDestination), QByteArray("plain"));
    QCOMPARE(readFileContent(aclDestination), QByteArray("acl"));
}

void TestFilesystemHelpers::applyRestoredMetadataDoesNotWidenModeOnAccessAclFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString source = tempDir.path() + QStringLiteral("/source");
    const QString liveDir = tempDir.path() + QStringLiteral("/live");
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(createFile(source, "secret", 0640));

    const quint32 namedUser = ::getuid() + 1000;
    const int setError = writeXattr(liveDir, "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 4, namedUser},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    // replaceRegularFileAtは、metadataの適用に失敗すると一時ファイルを削除するため、失敗後のmodeを観測できない
    // そのため、復元先のファイルをテスト側で作成してfdを保持し、applyRestoredMetadataを直接呼び出す
    const int sourceFd = ::open(source.toUtf8().constData(), O_RDONLY | O_CLOEXEC);
    const int destinationFd = ::open((liveDir + QStringLiteral("/file")).toUtf8().constData(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    QByteArray inheritedAcl;
    const int inheritedState = destinationFd >= 0
            ? readFdXattr(destinationFd, "system.posix_acl_access", &inheritedAcl) : -1;

    detail::setAclSyncFailureForTesting("system.posix_acl_access", EACCES);
    RestoredMetadataResult result;
    struct stat afterStat {};
    bool afterStatOk = false;
    QByteArray afterAcl;
    int afterState = -1;
    int observationCount = -1;

    // QVERIFYで途中終了するとfdが閉じられないため、判定より先に観測とfdのクローズを済ませる
    if (sourceFd >= 0 && destinationFd >= 0) {
        ModeRestoreObserverGuard observer;
        result = applyRestoredMetadata(sourceFd, destinationFd, false);
        observationCount = static_cast<int>(observer.observations.size());
        afterStatOk = ::fstat(destinationFd, &afterStat) == 0;
        afterState = readFdXattr(destinationFd, "system.posix_acl_access", &afterAcl);
    }

    if (sourceFd >= 0) {
        ::close(sourceFd);
    }

    if (destinationFd >= 0) {
        ::close(destinationFd);
    }

    QVERIFY(sourceFd >= 0);
    QVERIFY(destinationFd >= 0);

    // 前提として、0600で作成した直後はACLのmaskが---になり、継承した名前付きユーザの読み取りは無効になっている
    QCOMPARE(inheritedState, 1);
    QCOMPARE(aclEntryPerm(inheritedAcl, kAclUser, namedUser), 4);
    QCOMPARE(aclEntryPerm(inheritedAcl, kAclMask, kAclUndefinedId), 0);

    // access ACLを同期できない場合は、modeを0600から広げず、maskも---のままにする
    // これにより、継承した名前付きユーザの読み取りは無効のまま保たれる
    // modeを適用しないため、modeの適用直前の通知も行われない
    QVERIFY(result.mandatoryFailed);
    QCOMPARE(result.mandatoryErrno, EACCES);
    QVERIFY(afterStatOk);
    QCOMPARE(afterStat.st_mode & 07777, 0600U);
    QCOMPARE(afterState, 1);
    QCOMPARE(aclEntryPerm(afterAcl, kAclMask, kAclUndefinedId), 0);
    QCOMPARE(observationCount, 0);
}

void TestFilesystemHelpers::replaceRegularFileAtRelaxedPathDoesNotWidenModeOnAclFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/live/file");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/live")));
    QVERIFY(createFile(source, "relaxed", 0640));
    QVERIFY(createFile(destination, "old", 0644));

    // metadataを必須としない経路(relaxed)は、ACLの同期に失敗しても差し替えを続ける
    // そのため、差し替え後の復元先のmodeが、一時ファイルを作成したときの0600から広がっていないことを確認する
    detail::setAclSyncFailureForTesting("system.posix_acl_access", EACCES);
    RestoredMetadataResult metadata;
    QVERIFY(replaceFromPath(root, source, destination, &metadata, false));

    QVERIFY(metadata.mandatoryFailed);
    QCOMPARE(metadata.mandatoryErrno, EACCES);
    QCOMPARE(readFileContent(destination), QByteArray("relaxed"));
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0600U);
    QVERIFY(hasNoTemporaryLeftovers(root + QStringLiteral("/live")));
}

void TestFilesystemHelpers::replaceRegularFileAtStrictPathDoesNotRenameOnAclFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/live/file");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/live")));
    QVERIFY(createFile(source, "snapshot", 0640));
    QVERIFY(createFile(destination, "live", 0644));

    // metadataを必須とする経路(strict)は、ACLの同期に失敗すると差し替えを中止する
    // 復元先の内容とmodeが変わらず、一時ファイルも残らないことを確認する
    detail::setAclSyncFailureForTesting("system.posix_acl_access", EACCES);
    RestoredMetadataResult metadata;
    errno = 0;
    const bool replaced = replaceFromPath(root, source, destination, &metadata);
    const int replaceErrno = errno;

    QVERIFY(!replaced);
    QCOMPARE(replaceErrno, EACCES);
    QVERIFY(metadata.mandatoryFailed);
    QCOMPARE(metadata.mandatoryErrno, EACCES);
    QCOMPARE(readFileContent(destination), QByteArray("live"));
    struct stat liveStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &liveStat) == 0);
    QCOMPARE(liveStat.st_mode & 07777, 0644U);
    QVERIFY(hasNoTemporaryLeftovers(root + QStringLiteral("/live")));
}

void TestFilesystemHelpers::applyRestoredMetadataDoesNotWidenDirectoryModeOnDefaultAclFailure()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString sourceDir = tempDir.path() + QStringLiteral("/source-dir");
    const QString destinationDir = tempDir.path() + QStringLiteral("/destination-dir");
    QVERIFY(QDir().mkpath(sourceDir));
    QVERIFY(QDir().mkpath(destinationDir));
    QVERIFY(::chmod(sourceDir.toUtf8().constData(), 0750) == 0);
    QVERIFY(::chmod(destinationDir.toUtf8().constData(), 0700) == 0);

    const int sourceFd = ::open(sourceDir.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int destinationFd = ::open(destinationDir.toUtf8().constData(),
                                     O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    // default ACLの同期だけを失敗させ、access ACLの同期に成功した場合でも、
    // ディレクトリのmodeを復元元の0750へ広げないことを確認する
    detail::setAclSyncFailureForTesting("system.posix_acl_default", EROFS);
    RestoredMetadataResult result;
    struct stat afterStat {};
    bool afterStatOk = false;
    int observationCount = -1;

    // QVERIFYで途中終了するとfdが閉じられないため、判定より先に観測とfdのクローズを済ませる
    if (sourceFd >= 0 && destinationFd >= 0) {
        ModeRestoreObserverGuard observer;
        result = applyRestoredMetadata(sourceFd, destinationFd, true);
        observationCount = static_cast<int>(observer.observations.size());
        afterStatOk = ::fstat(destinationFd, &afterStat) == 0;
    }

    if (sourceFd >= 0) {
        ::close(sourceFd);
    }

    if (destinationFd >= 0) {
        ::close(destinationFd);
    }

    QVERIFY(sourceFd >= 0);
    QVERIFY(destinationFd >= 0);

    QVERIFY(result.mandatoryFailed);
    QCOMPARE(result.mandatoryErrno, EROFS);
    QVERIFY(afterStatOk);
    QCOMPARE(afterStat.st_mode & 07777, 0700U);
    QCOMPARE(observationCount, 0);
}

void TestFilesystemHelpers::safeCreateParentDirectoriesSyncsAclBeforeModeRestore()
{
    QTemporaryDir liveDir;
    QTemporaryDir snapshotDir;
    QVERIFY(liveDir.isValid() && snapshotDir.isValid());

    // スナップショット側に、ACLを持たない親ディレクトリaとa/bを、異なるmodeで用意する
    QVERIFY(QDir(snapshotDir.path()).mkpath(QStringLiteral("a/b")));
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a")).constData(), 0750), 0);
    QCOMPARE(::chmod(QFile::encodeName(snapshotDir.path() + QStringLiteral("/a/b")).constData(), 0710), 0);

    // 復元先にdefault ACLを設定し、復元時に作成する親ディレクトリが、
    // そのACLをaccess ACLとdefault ACLの両方として継承する状況を再現する
    const int setError = writeXattr(liveDir.path(), "system.posix_acl_default", makeAclXattr({
        {kAclUserObj, 7, kAclUndefinedId},
        {kAclUser, 5, ::getuid() + 1000},
        {kAclGroupObj, 5, kAclUndefinedId},
        {kAclMask, 7, kAclUndefinedId},
        {kAclOther, 0, kAclUndefinedId},
    }));
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    const int snapshotFd = ::open(QFile::encodeName(snapshotDir.path()).constData(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(snapshotFd >= 0);
    bool created = false;
    QList<ModeRestoreObservation> observations;

    // 観測を親ディレクトリの作成中だけに限定し、作成後の確認でobserverが呼ばれないようにする
    {
        ModeRestoreObserverGuard observer;
        created = safeCreateParentDirectoriesFromSourceBeneathRoot(
                liveDir.path(), liveDir.path() + QStringLiteral("/a/b"), snapshotFd, true);
        observations = observer.observations;
    }
    ::close(snapshotFd);
    QVERIFY(created);

    // 親ディレクトリaとa/bの2つを作成するため、modeの適用直前の通知も2回ある
    // どちらのディレクトリも、modeを適用する直前には継承したaccess ACLとdefault ACLが削除されている必要がある
    QCOMPARE(observations.size(), 2);
    for (const ModeRestoreObservation &beforeMode : std::as_const(observations)) {
        QVERIFY2(beforeMode.accessState == 0 && beforeMode.defaultState == 0,
                 describeObservation(beforeMode).constData());
    }

    QByteArray value;
    struct stat createdStat;
    const QString createdA = liveDir.path() + QStringLiteral("/a");
    const QString createdB = liveDir.path() + QStringLiteral("/a/b");
    QVERIFY(::lstat(createdA.toUtf8().constData(), &createdStat) == 0);
    QCOMPARE(createdStat.st_mode & 07777, 0750U);
    QVERIFY(::lstat(createdB.toUtf8().constData(), &createdStat) == 0);
    QCOMPARE(createdStat.st_mode & 07777, 0710U);
    QCOMPARE(readXattr(createdA, "system.posix_acl_access", &value), 0);
    QCOMPARE(readXattr(createdA, "system.posix_acl_default", &value), 0);
    QCOMPARE(readXattr(createdB, "system.posix_acl_access", &value), 0);
    QCOMPARE(readXattr(createdB, "system.posix_acl_default", &value), 0);
}

void TestFilesystemHelpers::replaceRegularFileAtPreservesFileCapability()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(createFile(source, "#!/bin/sh\n", 0755));
    QVERIFY(createFile(destination, "old", 0644));

    // security.capabilityの値の形式はVFS_CAP_REVISION_2で、permittedにCAP_NET_BIND_SERVICE(bit 10)だけを持つ
    const QByteArray capability = QByteArray::fromHex("0000000200040000000000000000000000000000");
    const int setError = writeXattr(source, "security.capability", capability);
    if (setError == EPERM || setError == ENOTSUP) {
        QSKIP("Setting file capabilities requires CAP_SETFCAP on a supporting filesystem");
    }
    QCOMPARE(setError, 0);
    QByteArray sourceCapability;
    QCOMPARE(readXattr(source, "security.capability", &sourceCapability), 1);

    // file capabilityは、所有者やmodeを変更するとカーネルが削除する
    // ACLとmodeの適用より後に設定していれば、復元後のファイルにcapabilityが残る
    QVERIFY(replaceFromPath(root, source, destination));

    QByteArray restoredCapability;
    QCOMPARE(readXattr(destination, "security.capability", &restoredCapability), 1);
    QCOMPARE(restoredCapability, sourceCapability);
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QCOMPARE(restoredStat.st_mode & 07777, 0755U);
}

void TestFilesystemHelpers::replaceRegularFileAtDetectsTemporaryNameSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString destination = root + QStringLiteral("/destination");
    const QString stolen = root + QStringLiteral("/stolen");
    QVERIFY(createFile(source, "snapshot", 0644));
    QVERIFY(createFile(destination, "live", 0644));

    // rename直前に一時名を攻撃者のobjectへ差し替える
    const ReplaceHookGuard guard([&](int parentFd, const QByteArray &temporaryName) {
        QVERIFY(::renameat(parentFd, temporaryName.constData(), parentFd, "stolen") == 0);
        const int fd = ::openat(parentFd, temporaryName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        QVERIFY(fd >= 0);
        QVERIFY(::write(fd, "attacker", 8) == 8);
        ::close(fd);
    });

    errno = 0;
    QVERIFY(!replaceFromPath(root, source, destination));
    QCOMPARE(errno, ESTALE);

    // 攻撃者のobjectは宛先へrenameされず、宛先は元のまま
    QFile live(destination);
    QVERIFY(live.open(QIODevice::ReadOnly));
    QCOMPARE(live.readAll(), QByteArray("live"));
    QVERIFY(QFile::exists(stolen));
}

void TestFilesystemHelpers::replaceRegularFileAtKeepsResolvedParentAfterParentSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString source = root + QStringLiteral("/source");
    const QString parent = root + QStringLiteral("/parent");
    const QString moved = root + QStringLiteral("/moved");
    const QString outside = root + QStringLiteral("/outside");
    QVERIFY(createFile(source, "snapshot", 0644));
    QVERIFY(QDir().mkpath(parent));
    QVERIFY(QDir().mkpath(outside));

    // 解決済みの親をsymlinkへ差し替えても、保持している親dirfd上で完結し、symlink先へは書き込まない
    const ReplaceHookGuard guard([&](int, const QByteArray &) {
        QVERIFY(::rename(parent.toUtf8().constData(), moved.toUtf8().constData()) == 0);
        QVERIFY(::symlink(outside.toUtf8().constData(), parent.toUtf8().constData()) == 0);
    });

    QVERIFY(replaceFromPath(root, source, parent + QStringLiteral("/file")));
    QVERIFY(QFile::exists(moved + QStringLiteral("/file")));
    QVERIFY(!QFile::exists(outside + QStringLiteral("/file")));
    QVERIFY(hasNoTemporaryLeftovers(outside));
}

void TestFilesystemHelpers::replaceSymlinkAtAppliesMetadataAndDetectsSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString sourceLink = root + QStringLiteral("/source-link");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(::symlink("target/path", sourceLink.toUtf8().constData()) == 0);
    const struct timespec times[2] = { {1000000000, 0}, {1100000000, 123} };
    QVERIFY(::utimensat(AT_FDCWD, sourceLink.toUtf8().constData(), times, AT_SYMLINK_NOFOLLOW) == 0);
    QVERIFY(createFile(destination, "live", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(sourceLink.toUtf8().constData(), &sourceStat) == 0);

    QByteArray leafName;
    const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(parentFd >= 0);

    bool metadataApplied = false;
    QVERIFY(replaceSymlinkAt(parentFd, leafName, "target/path", sourceStat, &metadataApplied));
    QVERIFY(metadataApplied);
    QCOMPARE(QFile::symLinkTarget(destination).endsWith(QStringLiteral("target/path")), true);
    struct stat restoredStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &restoredStat) == 0);
    QVERIFY(S_ISLNK(restoredStat.st_mode));
    QCOMPARE(restoredStat.st_mtim.tv_sec, sourceStat.st_mtim.tv_sec);
    QCOMPARE(restoredStat.st_mtim.tv_nsec, sourceStat.st_mtim.tv_nsec);

    // rename直前に一時symlinkを別のsymlinkへ差し替えると検出する
    {
        const ReplaceHookGuard guard([&](int hookParentFd, const QByteArray &temporaryName) {
            QVERIFY(::unlinkat(hookParentFd, temporaryName.constData(), 0) == 0);
            QVERIFY(::symlinkat("/etc/shadow", hookParentFd, temporaryName.constData()) == 0);
        });
        errno = 0;
        QVERIFY(!replaceSymlinkAt(parentFd, leafName, "other/target", sourceStat, nullptr));
        QCOMPARE(errno, ESTALE);
    }
    ::close(parentFd);

    char target[64] = {};
    QVERIFY(::readlink(destination.toUtf8().constData(), target, sizeof(target) - 1) > 0);
    QCOMPARE(QByteArray(target), QByteArray("target/path"));
}

/**
 * @brief 所有者と時刻の適用を必須にした場合、適用できなければlive側を差し替えず、一時symlinkも残さないことを検証する
 *
 * rootで動作する復元では、所有者を保てないsymlinkへ差し替えると別ユーザ所有のlinkが残るため失敗として扱う
 * 一般ユーザはroot所有へ変更できないため、その失敗の経路を再現できる
 */
void TestFilesystemHelpers::replaceSymlinkAtRequiringMetadataLeavesLiveUntouched()
{
    if (::geteuid() == 0) {
        QSKIP("root can always change the link owner");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString root = tempDir.path();
    const QString sourceLink = root + QStringLiteral("/source-link");
    const QString destination = root + QStringLiteral("/destination");
    QVERIFY(::symlink("target/path", sourceLink.toUtf8().constData()) == 0);
    QVERIFY(createFile(destination, "live", 0644));

    struct stat sourceStat;
    QVERIFY(::lstat(sourceLink.toUtf8().constData(), &sourceStat) == 0);
    sourceStat.st_uid = 0;
    sourceStat.st_gid = 0;

    QByteArray leafName;
    const int parentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(parentFd >= 0);

    bool metadataApplied = true;
    errno = 0;
    QVERIFY(!replaceSymlinkAt(parentFd, leafName, "target/path", sourceStat, &metadataApplied, true));
    QVERIFY(errno != 0);
    QVERIFY(!metadataApplied);
    ::close(parentFd);

    struct stat liveStat;
    QVERIFY(::lstat(destination.toUtf8().constData(), &liveStat) == 0);
    QVERIFY(S_ISREG(liveStat.st_mode));
    QVERIFY(hasNoTemporaryLeftovers(root));

    // 必須にしない場合は、従来どおり所有者を保てなくても差し替える
    const int retryParentFd = openDestinationParentBeneathRoot(root, destination, &leafName);
    QVERIFY(retryParentFd >= 0);
    QVERIFY(replaceSymlinkAt(retryParentFd, leafName, "target/path", sourceStat, &metadataApplied, false));
    QVERIFY(!metadataApplied);
    ::close(retryParentFd);
    QVERIFY(::lstat(destination.toUtf8().constData(), &liveStat) == 0);
    QVERIFY(S_ISLNK(liveStat.st_mode));
}

/**
 * @brief SELinuxコンテキストのユーザ部分だけが復元先の値に置き換わることを確認する
 *
 * SELinuxユーザを変更できない古いポリシーで書き込みが拒否された場合に、ユーザだけを合わせて書き直すための処理である
 * アクセス制御に効くタイプとレベルが変わらないこと、末尾のNULの有無が保たれること、
 * 置き換える意味がない場合と形式が不正な場合に空を返すことを確かめる
 */
void TestFilesystemHelpers::selinuxContextUserIsReplacedOnlyInUserPart()
{
    const QByteArray snapshotContext("unconfined_u:object_r:shadow_t:s0", 33);
    QCOMPARE(selinuxContextWithDestinationUser(snapshotContext, QByteArray("system_u:object_r:etc_t:s0")),
             QByteArray("system_u:object_r:shadow_t:s0"));

    // xattrの値に多い末尾のNULは、スナップショット側の形に合わせて保つ
    const QByteArray terminated = snapshotContext + QByteArray(1, '\0');
    const QByteArray destinationTerminated = QByteArray("system_u:object_r:etc_t:s0") + QByteArray(1, '\0');
    QCOMPARE(selinuxContextWithDestinationUser(terminated, destinationTerminated),
             QByteArray("system_u:object_r:shadow_t:s0") + QByteArray(1, '\0'));
    QCOMPARE(selinuxContextWithDestinationUser(snapshotContext, destinationTerminated),
             QByteArray("system_u:object_r:shadow_t:s0"));

    // MCSのカテゴリに含まれるコロンも、ユーザ以外の部分としてそのまま保つ
    QCOMPARE(selinuxContextWithDestinationUser(QByteArray("staff_u:object_r:user_home_t:s0-s0:c0.c1023"),
                                               QByteArray("system_u:object_r:default_t:s0")),
             QByteArray("system_u:object_r:user_home_t:s0-s0:c0.c1023"));

    // ユーザが同じなら再試行しても結果は変わらないため、空を返す
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("system_u:object_r:etc_t:s0"),
                                              QByteArray("system_u:object_r:var_t:s0")).isEmpty());

    // 形式が不正な値は書き込みに使わない
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("shadow_t"),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray(":object_r:shadow_t:s0"),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(snapshotContext, QByteArray("system_u")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray("unconfined_u:object_r:\0shadow_t:s0", 34),
                                              QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
    QVERIFY(selinuxContextWithDestinationUser(QByteArray(), QByteArray("system_u:object_r:etc_t:s0")).isEmpty());
}
