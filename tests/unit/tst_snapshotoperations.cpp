/**
 * @file tst_snapshotoperations.cpp
 * @brief SnapshotOperationsの復元計画とエントリ適用の特権境界を検証するテスト
 *
 * SnapshotOperationsのスロットをD-Busを介さずに直接呼ぶ (呼び出し元ownerは空文字列になる)
 * rootサブボリュームへの操作はテスト用の差し替え関数で観測し、実システムには触れない
 * 復元計画の終了状態、config操作との排他、エントリ適用の安全性、認可と例外の境界を確認する
 *
 * 依存 : Qt Test、SnapshotOperations、RestoreManifestRegistry、snapshotoperations_internal
 */

#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <cstring>
#include <new>
#include <stdexcept>

#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/fsuid.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "filesystemhelpers.h"
#include "snapshotoperations.h"
#include "restoremanifestregistry.h"
#include "snapshotoperations_internal.h"

/**
 * @brief SnapshotOperationsの復元計画まわりの特権境界を検証するテスト
 *
 * D-Busを介さずにスロットを直接呼ぶ (呼び出し元ownerは空文字列になる)
 * rootサブボリュームへの操作はテスト用の差し替え関数で観測し、実システムには触れない
 */
class TestSnapshotOperations : public QObject
{
    Q_OBJECT

private slots:
    void init();
    /** @brief 各テストの終了時に、metadata適用のテスト用フックを解除する */
    void cleanup();

    void beginThenCancelDoesNotTouchRootSubvolume();

    // --- config操作を止める復元計画の判定 ---
    /** @brief 作成中(Staging)の計画だけではconfig操作が止められないことを検証する */
    void stagingPlanDoesNotBlockConfigOperations();
    /** @brief config操作の直前に対象の計画だけを取り消し、他のconfigの計画は残すことを検証する */
    void configOperationsDiscardStagingPlansOfConfig();
    /** @brief 認可を得られないBeginRestorePlanが計画を作らず予算を消費しないことを検証する */
    void beginRestorePlanRequiresAuthorization();
    /** @brief 同じUIDの計画が上限に達したとき最も古いStaging計画だけを破棄することを検証する */
    void beginRestorePlanEvictsOldestStagingPlanOfSameUid();
    /** @brief 実行中の復元があるconfigだけconfig操作が止められることを検証する */
    void runningRestoreBlocksConfigOperations();
    /** @brief 同じ復元先を指すパスを含むchunkが原子的に拒否されることを検証する */
    void stageRestoreEntriesRejectsDuplicateCanonicalPaths();
    /** @brief 期限切れの計画がGetRestorePlanStatusでowner表から掃除され、有効な計画は残ることを検証する */
    void expiredPlanIsRemovedFromOwnerTableOnStatus();

    // --- 差分生成の資源上限 ---
    void diffStillProducesUnifiedOutputForSmallChanges();
    void diffOmitsBinaryFiles();
    void diffOmitsTooLargeFiles();
    void diffOmitsFilesWithTooManyLines();
    void diffDoesNotOpenFifo();
    void diffOmitsTooManyChanges();
    void diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput();

    // --- 書き込み可能な復元元の拒否 ---
    void writableDirectoryIsNotReadOnlyRestoreSource();
    void btrfsSubvolumeReadOnlyFlagIsRequired();
    void readOnlyMountIsAcceptedForNonBtrfs();

    // --- 認可待ちの上限 ---
    void pendingAuthorizationBudgetLimitsEachUid();
    void pendingAuthorizationBudgetKeepsGlobalLimit();
    void pendingAuthorizationBudgetReleasesSlots();

    // --- エントリの適用と終了処理 ---
    /** @brief FIFOなどの特殊ファイルを、live側に触れずスキップとして扱うことを検証する */
    void specialFileSourceIsSkippedWithoutTouchingLive();
    /** @brief ディレクトリとファイルの間の種別変更を、YaST方式でも復元できることを検証する */
    void typechangedReplacesDirectoryAndFileInYastMode();
    /** @brief 既存のディレクトリと新規に作成するディレクトリの両方で、modeを適用する直前に継承ACLが削除されていることを検証する */
    void applyRestoreEntrySyncsDirectoryAclBeforeModeRestore();
    /** @brief 退避前と退避後の他者所有ディレクトリを対比するケースを提供する */
    void typechangedGuardsForeignDirectories_data();
    /** @brief 他者の木の退避を拒否し、退避後に削除を拒否した場合も失敗を報告する */
    void typechangedGuardsForeignDirectories();
    /** @brief 子エントリの適用で変わるディレクトリの時刻を、計画の終了時に復元することを検証する */
    void directoryTimesAreRestoredAfterChildEntries();
    /** @brief 復元先の同名symlinkの先へmetadataを適用しないことを検証する */
    void existingSymlinkIsNotUsedAsDirectory();
    /** @brief 他ユーザが書き換えられる親の下では、入れ替えられた可能性のある
     *        ディレクトリへmetadataを適用しないことを検証する */
    void untrustedParentRefusesMetadataOnUnprovenDirectories();
    /** @brief 共有親の拒否とprincipal所有親の正常復元を対比する */
    void untrustedParentRefusesMetadataOnUnprovenDirectories_data();
    /** @brief 所有権境界と復元後のfilesystem identityを検証するケースを提供する */
    void directoryMetadataFollowsChainOwnership_data();
    /** @brief 他者所有の祖先や対象を通じたroot権限の増幅を拒否する */
    void directoryMetadataFollowsChainOwnership();
    /** @brief rootをrwへ戻せなかった計画は、全エントリ適用済みでもFailedで終わることを検証する */
    void rootLeftReadOnlyMarksPlanFailed();
    /** @brief 実行中にownerが消えてもrootのrw安全網を実行することを検証する */
    void vanishedOwnerStillRunsRootSafetyNet();
    /** @brief 退避先の名前が推測できない乱数の接尾辞を持つことを検証する */
    void siblingTemporaryPathIsUnpredictable();

    // --- 例外と入力の検査 ---
    /** @brief 認可済みの本体の想定外の例外がスロットの外へ出ないことを検証する */
    void unexpectedExceptionsDoNotEscapeAuthorizedBody();
    /** @brief 未知のcleanupアルゴリズムを認可の前に拒否することを検証する */
    void modifySnapshotRejectsUnknownCleanupBeforeAuthorization();

private:
    /**
     * @brief rootサブボリュームへの操作を観測する差し替え関数を設定する
     * @param operations 対象のSnapshotOperations
     */
    void installRootSubvolumeSpies(SnapshotOperations &operations);

    int m_readOnlyProbeCalls = 0;
    int m_readWriteRestoreCalls = 0;
};

namespace {

    /**
     * @brief テスト用ファイルを作成する
     * @param path 作成先
     * @param content 内容
     * @return 作成できた場合true
     */
    bool writeFile(const QString &path, const QByteArray &content)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write(content) == content.size();
    }

    /**
     * @brief 指定した接頭辞と連番で行を作る
     * @param prefix 行の接頭辞
     * @param count 行数
     * @return 改行区切りの内容
     */
    QByteArray numberedLines(const QByteArray &prefix, int count)
    {
        QByteArray content;
        for (int i = 0; i < count; ++i) {
            content += prefix + QByteArray::number(i) + '\n';
        }
        return content;
    }

    /**
     * @brief テスト用ファイルの内容を読む
     * @param path 読み取るファイル
     * @return 内容 (読めない場合は空)
     */
    QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    /**
     * @brief 復元元と復元先の2つのディレクトリを持つ作業領域
     *
     * 復元元はスナップショットの代わり、復元先はconfigのSUBVOLUMEの代わりとして使う
     */
    struct ApplyFixture {
        QTemporaryDir root;     // 作業領域全体 (テスト終了時に削除する)
        QString sourcePath;     // 復元元ディレクトリ
        QString livePath;       // 復元先ディレクトリ

        /**
         * @brief 復元元と復元先のディレクトリを作る
         * @return 作成できた場合true
         */
        bool setUp()
        {
            if (!root.isValid()) {
                return false;
            }
            sourcePath = root.filePath(QStringLiteral("source"));
            livePath = root.filePath(QStringLiteral("live"));
            return QDir().mkpath(sourcePath) && QDir().mkpath(livePath);
        }

        /**
         * @brief 復元元を開いて、mountせずに実行状態を作る
         * @param operations 対象のSnapshotOperations
         * @param manifestId 実行状態のID
         * @param useReflink ダイレクトコピー方式として扱う場合true
         * @return 作成できた場合true
         */
        bool begin(SnapshotOperations &operations, const QString &manifestId, bool useReflink)
        {
            const int sourceFd = ::open(QFile::encodeName(sourcePath).constData(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sourceFd < 0) {
                return false;
            }
            operations.beginRestoreApplyForTesting(manifestId, sourcePath, sourceFd, livePath, useReflink,
                                                   qsnapper::restore::RootReadWriteSafetyNetState{});
            return true;
        }
    };

    /**
     * @brief ディレクトリにqsnapperの一時退避物が残っていないか調べる
     * @param directoryPath 調べるディレクトリ
     * @return 退避物や一時ファイルが無い場合true
     */
    bool hasNoQsnapperLeftovers(const QString &directoryPath)
    {
        const QStringList entries = QDir(directoryPath).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                                  | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (entry.contains(QStringLiteral("qsnapper-"))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 名前付きユーザへ読み取りと検索を許すdefault ACLの、xattrの値を作る
     *
     * 復元先の下にあるディレクトリが、名前付きユーザのエントリを継承する状況を再現するために使う
     * maskをr-xにして、ACLを継承したディレクトリのgroupのビットに書き込みが含まれないようにする
     *
     * @param namedUser 名前付きユーザのuid (ACLのエントリとして使うだけなので、実在しなくてよい)
     * @return system.posix_acl_defaultへ設定する値 (先頭4バイトのヘッダの後に、8バイトのエントリが並ぶ形式)
     */
    QByteArray namedUserDefaultAcl(quint32 namedUser)
    {
        /**
         * @brief ACLの1エントリ
         */
        struct AclEntry {
            quint16 tag;   // エントリの種類
            quint16 perm;  // 権限ビット (0から7)
            quint32 id;    // 名前付きユーザのuid (それ以外のエントリは未定義を表す0xffffffff)
        };

        const AclEntry entries[] = {
            {0x01, 7, 0xffffffffU},  // 所有者 (user_obj)
            {0x02, 5, namedUser},    // 名前付きユーザ (user)
            {0x04, 5, 0xffffffffU},  // 所有グループ (group_obj)
            {0x10, 5, 0xffffffffU},  // 名前付きエントリとgroupに許す権限の上限 (mask)
            {0x20, 0, 0xffffffffU},  // その他のユーザ (other)
        };

        // xattrの値は、各フィールドをlittle endianで並べる
        QByteArray value;
        const auto appendLittleEndian = [&value](quint32 number, int byteCount) {
            for (int index = 0; index < byteCount; ++index) {
                value.append(static_cast<char>((number >> (8 * index)) & 0xff));
            }
        };

        // ヘッダはACLの形式のversionで、Linuxのxattrでは2を使う
        appendLittleEndian(2, 4);
        for (const AclEntry &entry : entries) {
            appendLittleEndian(entry.tag, 2);
            appendLittleEndian(entry.perm, 2);
            appendLittleEndian(entry.id, 4);
        }
        return value;
    }

    /**
     * @brief パスのxattrが存在するかを調べる
     * @param path 対象パス (シンボリックリンクはたどらない)
     * @param name xattr名
     * @return 1: 値あり、0: 属性なし、-1: 読み出しエラー
     */
    int pathXattrState(const QString &path, const char *name)
    {
        if (::lgetxattr(QFile::encodeName(path).constData(), name, nullptr, 0) >= 0) {
            return 1;
        }
        return errno == ENODATA ? 0 : -1;
    }

    /**
     * @brief 復元元のmodeを適用する直前に観測した、ディレクトリのmodeとACLの有無
     *
     * modeを広げる前にACLの同期が終わっているかを、テスト本体で判定するために記録する
     */
    struct DirectoryAclObservation {
        mode_t mode = 0;        // 観測時点のmode (fstatに失敗した場合は全ビットを立てた値)
        int accessState = -1;   // access ACLの有無 (1: あり、0: なし、-1: 読み出し失敗)
        int defaultState = -1;  // default ACLの有無 (1: あり、0: なし、-1: 読み出し失敗)
    };

    /**
     * @brief BeforeModeRestoreの通知を受けるたびに、復元先のディレクトリのmodeとACLの有無を記録する
     *
     * 製品コードの処理の途中から呼ばれるため、QTestのマクロで判定して処理を中断せず、記録だけを行う
     *
     * @param stage 通知された段階
     * @param destinationFd 復元先のディレクトリのfd (状態を読み取るだけで、閉じない)
     * @param context 記録先のQList<DirectoryAclObservation>へのポインタ
     * @note contextが指すリストへ、観測結果を1件追加する
     */
    void recordDirectoryAclObservation(qsnapper::security::detail::MetadataStage stage, int destinationFd,
                                       void *context)
    {
        if (stage != qsnapper::security::detail::MetadataStage::BeforeModeRestore) {
            return;
        }

        const auto xattrState = [destinationFd](const char *name) {
            if (::fgetxattr(destinationFd, name, nullptr, 0) >= 0) {
                return 1;
            }
            return errno == ENODATA ? 0 : -1;
        };

        DirectoryAclObservation observation;
        struct stat destinationStat;
        observation.mode = ::fstat(destinationFd, &destinationStat) == 0 ? (destinationStat.st_mode & 07777)
                                                                          : mode_t(~0);
        observation.accessState = xattrState("system.posix_acl_access");
        observation.defaultState = xattrState("system.posix_acl_default");
        static_cast<QList<DirectoryAclObservation> *>(context)->append(observation);
    }

    /**
     * @brief 観測結果を、テストの失敗メッセージに表示する文字列へ変換する
     * @param observation 観測結果
     * @return modeを8進数で表し、ACLの有無を並べた文字列
     */
    QByteArray describeDirectoryAclObservation(const DirectoryAclObservation &observation)
    {
        return QStringLiteral("before fchmod: mode=%1 access=%2 default=%3")
                .arg(static_cast<uint>(observation.mode), 4, 8, QLatin1Char('0'))
                .arg(observation.accessState)
                .arg(observation.defaultState)
                .toUtf8();
    }

}

/** @brief 元の名前を守る拒否と、退避物を保存してFailedを返す拒否を区別する */
void TestSnapshotOperations::typechangedGuardsForeignDirectories_data()
{
    QTest::addColumn<bool>("foreignChild");
    QTest::newRow("g-foreign-top-not-moved") << false;
    QTest::newRow("h-detached-cleanup-fails") << true;
}

/** @brief 退避物を消せない場合にAppliedを返さず、復旧先のログと保存内容を確認する */
void TestSnapshotOperations::typechangedGuardsForeignDirectories()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to exercise move-aside ownership guards");
    }
    QFETCH(bool, foreignChild);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::chmod(QFile::encodeName(fixture.livePath).constData(), 0755), 0);
    const QString alice = fixture.livePath + QStringLiteral("/alice");
    const QString target = alice + QStringLiteral("/victim");
    const QString protectedPath = foreignChild ? target + QStringLiteral("/protected") : target;
    QVERIFY(QDir().mkpath(protectedPath));
    QCOMPARE(::chown(QFile::encodeName(alice).constData(), 1000, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(alice).constData(), 0755), 0);
    QCOMPARE(::chown(QFile::encodeName(target).constData(), foreignChild ? 1000 : 0, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(protectedPath).constData(), 0700), 0);
    QVERIFY(writeFile(protectedPath + QStringLiteral("/file"), "preserved content"));
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/alice")));
    QVERIFY(writeFile(fixture.sourcePath + QStringLiteral("/alice/victim"), "restored content"));
    struct stat before {};
    QCOMPARE(::lstat(QFile::encodeName(protectedPath).constData(), &before), 0);
    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("guarded-typechanged");
    QVERIFY(fixture.begin(operations, manifestId, false));
    if (foreignChild) {
        // ランダム名の退避先もログに必須とし、警告の有無だけで成功扱いにしない
        QTest::ignoreMessage(QtCriticalMsg, QRegularExpression(
            QStringLiteral(".*preserved at:.*") + QRegularExpression::escape(alice)
            + QStringLiteral("/\\.victim\\.qsnapper-old\\.[0-9a-f]{16}.*")));
    }
    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(
        manifestId, {QStringLiteral("/alice/victim"), QStringLiteral("typechanged")});
    const int applyError = errno;
    QCOMPARE(outcome, qsnapper::restore::RestoreEntryOutcome::Failed);
    QCOMPARE(applyError, EPERM);
    QString preserved = protectedPath;
    if (foreignChild) {
        QCOMPARE(readFile(target), QByteArray("restored content"));
        const QStringList detached = QDir(alice).entryList(
            {QStringLiteral(".victim.qsnapper-old.*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(detached.size(), 1);
        preserved = alice + QLatin1Char('/') + detached.constFirst() + QStringLiteral("/protected");
    }
    else {
        QVERIFY(hasNoQsnapperLeftovers(alice));
    }
    struct stat after {};
    QCOMPARE(::lstat(QFile::encodeName(preserved).constData(), &after), 0);
    QCOMPARE(after.st_dev, before.st_dev);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_uid, before.st_uid);
    QCOMPARE(after.st_mode, before.st_mode);
    QCOMPARE(readFile(preserved + QStringLiteral("/file")), QByteArray("preserved content"));
    operations.finishRestorePlanForTesting(manifestId, qsnapper::restore::ManifestState::Failed);
}

void TestSnapshotOperations::init()
{
    m_readOnlyProbeCalls = 0;
    m_readWriteRestoreCalls = 0;
}

void TestSnapshotOperations::cleanup()
{
    // テスト関数がQVERIFYなどの失敗で途中で戻った場合でも、フックの設定が次のテストへ残らないよう、各テストの終了時に解除する
    qsnapper::security::detail::clearMetadataTestHooksForTesting();
}

void TestSnapshotOperations::installRootSubvolumeSpies(SnapshotOperations &operations)
{
    operations.setRootSubvolumeAccessForTesting(
        [this](bool *readOnly) {
            ++m_readOnlyProbeCalls;
            *readOnly = true;
            return true;
        },
        [this]() {
            ++m_readWriteRestoreCalls;
            return true;
        });
}

void TestSnapshotOperations::beginThenCancelDoesNotTouchRootSubvolume()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    // 計画の作成 (view-diffの認可) とCancelだけで、復元の実行に伴うrootのrw化へ到達してはならない
    const QString manifestId = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!manifestId.isEmpty());

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    QVERIFY(operations.CancelRestorePlan(manifestId));

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), manifestId);
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);

    // system bus全体へ届く終了通知には、終端状態ごとの固定文言だけを載せる
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Restore cancelled"));

    // 終了直後も、ownerはGetRestorePlanStatusで終端状態を取得できる
    const QStringList status = operations.GetRestorePlanStatus(manifestId).split(QLatin1Char(','));
    QVERIFY(status.size() >= 2);
    QCOMPARE(status.at(0), manifestId);
    QCOMPARE(status.at(1), QStringLiteral("cancelled"));
}

/**
 * @brief 作成中(Staging)の復元計画があっても、config操作が止められないことを検証する
 *
 * Staging計画は差分を閲覧できる利用者なら誰でも作れるため、空の計画を作るだけで
 * 管理者のCreateConfig / DeleteConfig / DeleteSnapshotsを妨害できてはならない
 */
void TestSnapshotOperations::stagingPlanDoesNotBlockConfigOperations()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const QString emptyPlan = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!emptyPlan.isEmpty());
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));

    const QString stagedPlan = operations.BeginRestorePlan(
        QStringLiteral("root"), 2, 0, QStringLiteral("yast"));
    QVERIFY(!stagedPlan.isEmpty());
    QVERIFY(operations.StageRestoreEntries(stagedPlan, {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
}

/**
 * @brief 認可を得られない呼び出しでは、BeginRestorePlanが計画を作らないことを検証する
 *
 * 計画は件数とパスの予算を消費するため、view-diffの認可を得られない利用者に枠を使わせない
 *
 * 拒否された呼び出しのIDは返らず、登録簿の件数もテストから参照できないため、上限による破棄の有無で間接的に検出する
 * 拒否された呼び出しが計画を作っていた場合、続けて上限数の計画を作った時点でその計画が最も古いものとして破棄され、終了通知が出る
 * 上限の値を変えてもテストが壊れないよう、件数はkMaxManifestsPerUidから求める
 * 検出の仕組み自体が働くことを保証するため、最後に上限を超える1件を作り、そのとき初めて破棄が起きることも確認する
 */
void TestSnapshotOperations::beginRestorePlanRequiresAuthorization()
{
    using qsnapper::restore::RestoreManifestRegistry;

    // 直接呼び出しのownerは空文字列で、ownerの上限もUIDの上限と同じ値でなければ破棄の時点がずれる
    static_assert(RestoreManifestRegistry::kMaxManifestsPerUid == RestoreManifestRegistry::kMaxManifestsPerOwner);

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);

    // テスト用の認可の差し替えがない状態では、D-Bus経由でない呼び出しは本番と同じく拒否される
    QVERIFY(operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct")).isEmpty());

    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    QStringList plans;
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsPerUid; ++index) {
        const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
        QVERIFY(!plan.isEmpty());
        plans.append(plan);
    }

    // 拒否された呼び出しが枠を使っていなければ、上限数ちょうどまでは何も破棄されない
    QCOMPARE(finishedSpy.count(), 0);
    for (const QString &plan : std::as_const(plans)) {
        QCOMPARE(operations.GetRestorePlanStatus(plan).split(QLatin1Char(',')).value(1),
                 QStringLiteral("staging"));
    }

    // 上限を超える1件で、認可済みの最も古い計画が破棄されることから、上記の検出が有効であることを示す
    QVERIFY(!operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct")).isEmpty());
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), plans.first());
}

/**
 * @brief 同じUIDの計画数が上限に達したとき、最も古いStaging計画だけを破棄して新しい計画を作ることを検証する
 *
 * 空の計画を作り続けられても、同じUIDの利用者が新しい復元を始められなくなってはならない
 * 破棄した計画はcancelledで終了を通知し、元のownerが終端状態を取得できるようにする
 */
void TestSnapshotOperations::beginRestorePlanEvictsOldestStagingPlanOfSameUid()
{
    using qsnapper::restore::RestoreManifestRegistry;

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    QStringList plans;
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsPerUid; ++index) {
        const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), index + 1, 0,
                                                         QStringLiteral("yast"));
        QVERIFY(!plan.isEmpty());
        plans.append(plan);
    }

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    const QString newest = operations.BeginRestorePlan(QStringLiteral("root"), 9, 0, QStringLiteral("yast"));
    QVERIFY(!newest.isEmpty());

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), plans.first());
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    QCOMPARE(operations.GetRestorePlanStatus(plans.first()).split(QLatin1Char(',')).value(1),
             QStringLiteral("cancelled"));

    // 破棄するのは最も古い1件だけで、残りの計画は引き続きエントリを追加できる
    for (qsizetype index = 1; index < plans.size(); ++index) {
        QCOMPARE(operations.GetRestorePlanStatus(plans.at(index)).split(QLatin1Char(',')).value(1),
                 QStringLiteral("staging"));
    }
    QVERIFY(operations.StageRestoreEntries(newest, {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);
}

/**
 * @brief 同じ復元先を指すパスを含むchunkが原子的に拒否されることを検証する
 *
 * "/a"、"//a"、"/a/"は同じ場所への操作になるため、1回のchunkに含まれても後続のchunkでも受け付けない
 */
void TestSnapshotOperations::stageRestoreEntriesRejectsDuplicateCanonicalPaths()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0,
                                                     QStringLiteral("yast"));
    QVERIFY(!plan.isEmpty());

    QVERIFY(!operations.StageRestoreEntries(plan, {QStringLiteral("/a"), QStringLiteral("//a")},
                                            {QStringLiteral("modified"), QStringLiteral("modified")}));
    QStringList status = operations.GetRestorePlanStatus(plan).split(QLatin1Char(','));
    QVERIFY(status.size() > 2);
    QCOMPARE(status.value(2), QStringLiteral("0"));

    QVERIFY(operations.StageRestoreEntries(plan, {QStringLiteral("/a")},
                                           {QStringLiteral("modified")}));
    QVERIFY(!operations.StageRestoreEntries(plan, {QStringLiteral("/a/")},
                                            {QStringLiteral("modified")}));
    status = operations.GetRestorePlanStatus(plan).split(QLatin1Char(','));
    QCOMPARE(status.value(2), QStringLiteral("1"));
}

/**
 * @brief 期限切れの計画がGetRestorePlanStatusでowner表から掃除され、有効な計画は残ることを検証する
 *
 * TTL切れの計画をowner表に残すと、破棄済みのIDがウィンドウのwatchを保持し続ける
 * 一方、まだ有効な計画を消すと、そのownerが終端状態を取得できなくなる
 */
void TestSnapshotOperations::expiredPlanIsRemovedFromOwnerTableOnStatus()
{
    using qsnapper::restore::RestoreManifestRegistry;

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    qint64 nowMs = 1000;
    operations.setRestoreRegistryClockForTesting([&nowMs]() { return nowMs; });

    const QString expired = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0,
                                                        QStringLiteral("yast"));
    QVERIFY(!expired.isEmpty());
    QVERIFY(operations.hasRestorePlanOwnerForTesting(expired));

    nowMs += RestoreManifestRegistry::kDefaultTtlMs / 2;
    const QString live = operations.BeginRestorePlan(QStringLiteral("root"), 2, 0,
                                                     QStringLiteral("yast"));
    QVERIFY(!live.isEmpty());

    nowMs += RestoreManifestRegistry::kDefaultTtlMs / 2 + 1;

    QVERIFY(operations.GetRestorePlanStatus(expired).isEmpty());
    QVERIFY(!operations.hasRestorePlanOwnerForTesting(expired));
    QVERIFY(operations.hasRestorePlanOwnerForTesting(live));
    QCOMPARE(operations.GetRestorePlanStatus(live).split(QLatin1Char(',')).value(1),
             QStringLiteral("staging"));
}

/**
 * @brief 実行中の復元があるconfigだけ、config操作が止められることを検証する
 *
 * 実行中の復元は、configとスナップショットが残っていることを前提に進むため、その間は削除させない
 */
void TestSnapshotOperations::runningRestoreBlocksConfigOperations()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    operations.beginRestoreExecutionForTesting(QStringLiteral("root"));
    QVERIFY(operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("home")));
    operations.endRestoreExecutionForTesting();
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
}

void TestSnapshotOperations::diffStillProducesUnifiedOutputForSmallChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\r\nb\nc\n"));
    QVERIFY(writeFile(newPath, "a\nB\nc\n"));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QCOMPARE(result.text,
             QStringLiteral("--- %1\n+++ %2\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n").arg(oldPath, newPath));

    // 差分が無い場合は空で、省略理由も付かない
    const qsnapper::diff::UnifiedDiffResult same = qsnapper::diff::generateUnifiedDiff(newPath, newPath);
    QVERIFY(same.text.isEmpty());
    QVERIFY(same.omittedReason.isEmpty());
}

void TestSnapshotOperations::diffOmitsBinaryFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString binaryPath = tempDir.filePath(QStringLiteral("binary"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(writeFile(binaryPath, QByteArray("ELF\0\1\2", 6)));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(textPath, binaryPath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("binary"));
}

void TestSnapshotOperations::diffOmitsTooLargeFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString smallPath = tempDir.filePath(QStringLiteral("small"));
    const QString largePath = tempDir.filePath(QStringLiteral("large"));
    QVERIFY(writeFile(smallPath, "line\n"));

    const qsnapper::diff::UnifiedDiffLimits defaults;
    QVERIFY(writeFile(largePath, QByteArray(static_cast<qsizetype>(defaults.maxFileBytes + 1), 'x')));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(smallPath, largePath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

void TestSnapshotOperations::diffOmitsFilesWithTooManyLines()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\n"));
    QVERIFY(writeFile(newPath, QByteArray(64, '\n')));

    qsnapper::diff::UnifiedDiffLimits limits;
    limits.maxLinesPerFile = 32;
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, limits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

void TestSnapshotOperations::diffDoesNotOpenFifo()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString fifoPath = tempDir.filePath(QStringLiteral("fifo"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(::mkfifo(fifoPath.toUtf8().constData(), 0600) == 0);

    // 書き込み側のいないFIFOを開くと永久にブロックするため、開かずに省略理由を返す必要がある
    QElapsedTimer timer;
    timer.start();
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(fifoPath, textPath);
    QVERIFY(timer.elapsed() < 5000);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("special_file"));

    // 読み込み用helperもFIFOを開かない
    errno = 0;
    QCOMPARE(qsnapper::security::safeOpenRegularFileRead(fifoPath), -1);
    QCOMPARE(errno, EINVAL);
}

void TestSnapshotOperations::diffOmitsTooManyChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, numberedLines("old ", 2000)));
    QVERIFY(writeFile(newPath, numberedLines("new ", 2000)));

    // 作業領域の上限
    qsnapper::diff::UnifiedDiffLimits traceLimits;
    traceLimits.maxTraceBytes = 256 * 1024;
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, traceLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 探索ステップ数の上限
    qsnapper::diff::UnifiedDiffLimits stepLimits;
    stepLimits.maxSteps = 10000;
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, stepLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 既定の上限内であれば差分を生成する
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QVERIFY(result.text.contains(QStringLiteral("@@ -1,2000 +1,2000 @@")));
}

void TestSnapshotOperations::diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));

    // 全行が異なる (編集距離が最大) 入力と、同一行の繰り返しに異なる行を混ぜた (全対角線のsnakeが長い) 入力
    QVERIFY(writeFile(oldPath, numberedLines("a", 200000)));
    QVERIFY(writeFile(newPath, numberedLines("b", 200000)));

    QElapsedTimer timer;
    timer.start();
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    QByteArray repeated;
    QByteArray interleaved;
    for (int i = 0; i < 200000; ++i) {
        repeated += "x\n";
        interleaved += (i % 2 == 0) ? "x\n" : "y\n";
    }
    QVERIFY(writeFile(oldPath, repeated));
    QVERIFY(writeFile(newPath, interleaved));
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    qInfo("pathological diff inputs took %lld ms", static_cast<long long>(timer.elapsed()));
    QVERIFY(timer.elapsed() < 10000);
}

// ============================================================================
// 書き込み可能な復元元の拒否
// ============================================================================

namespace {

    /**
     * @brief ディレクトリをO_DIRECTORYで開く
     * @param path 対象
     * @return 開いたfd、失敗時は-1
     */
    int openDirectory(const QString &path)
    {
        return ::open(path.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

}

void TestSnapshotOperations::writableDirectoryIsNotReadOnlyRestoreSource()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int dirFd = openDirectory(tempDir.path());
    QVERIFY(dirFd >= 0);
    // btrfs以外の場所でFSTYPE=btrfsと申告されても、subvolumeとして確認できなければ拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs"));
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs"));
    ::close(dirFd);
}

void TestSnapshotOperations::btrfsSubvolumeReadOnlyFlagIsRequired()
{
    const QString base = QDir::homePath();
    struct statfs fsInfo;
    if (::statfs(base.toUtf8().constData(), &fsInfo) < 0
            || fsInfo.f_type != BTRFS_SUPER_MAGIC) {
        QSKIP("HOME is not on btrfs");
    }

    QTemporaryDir tempDir(base + QStringLiteral("/.qsnapper-test-XXXXXX"));
    QVERIFY(tempDir.isValid());

    const int parentFd = openDirectory(tempDir.path());
    QVERIFY(parentFd >= 0);
    struct btrfs_ioctl_vol_args args;
    std::memset(&args, 0, sizeof(args));
    std::strncpy(args.name, "snapshot", sizeof(args.name) - 1);
    const int created = ::ioctl(parentFd, BTRFS_IOC_SUBVOL_CREATE, &args);
    ::close(parentFd);
    if (created < 0) {
        QSKIP("Cannot create a btrfs subvolume as this user");
    }

    const QString subvolume = tempDir.path() + QStringLiteral("/snapshot");
    const int subvolumeFd = openDirectory(subvolume);
    QVERIFY(subvolumeFd >= 0);

    // 書き込み可能なsubvolume (rollbackのwritable copy相当) は拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));

    quint64 flags = BTRFS_SUBVOL_RDONLY;
    const bool flagged = ::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0;
    if (flagged) {
        QVERIFY(qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));
        flags = 0;
        QVERIFY(::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0);
    }
    ::close(subvolumeFd);

    // 後片付け: 空のsubvolumeは所有者がrmdirできる (kernel 4.18以降)
    if (::rmdir(subvolume.toUtf8().constData()) < 0) {
        qWarning("Could not remove test subvolume: %s", std::strerror(errno));
    }
    if (!flagged) {
        QSKIP("Cannot set the btrfs read-only flag as this user");
    }
}

void TestSnapshotOperations::readOnlyMountIsAcceptedForNonBtrfs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray mountPoint = tempDir.path().toUtf8();

    // 非特権のuser + mount namespaceで、読み取り専用のtmpfsをmountして確認する
    constexpr int kNamespaceUnavailable = 77;
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        const auto writeText = [](const char *path, const QByteArray &text) {
            const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            const bool written = fd >= 0 && ::write(fd, text.constData(), text.size()) == text.size();
            if (fd >= 0) {
                ::close(fd);
            }
            return written;
        };
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0
                || !writeText("/proc/self/setgroups", "deny")
                || !writeText("/proc/self/uid_map", "0 " + QByteArray::number(uid) + " 1")
                || !writeText("/proc/self/gid_map", "0 " + QByteArray::number(gid) + " 1")
                || ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0
                || ::mount("tmpfs", mountPoint.constData(), "tmpfs", MS_RDONLY, "size=1m") < 0) {
            ::_exit(kNamespaceUnavailable);
        }
        const int dirFd = ::open(mountPoint.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            ::_exit(1);
        }
        const bool accepted = qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs");
        // 読み取り専用mountでも、btrfsのsubvolumeでなければbtrfsとしては受け入れない
        const bool rejectedAsBtrfs = !qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs");
        ::_exit(accepted && rejectedAsBtrfs ? 0 : 2);
    }

    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);
    QVERIFY(WIFEXITED(status));
    if (WEXITSTATUS(status) == kNamespaceUnavailable) {
        QSKIP("Unprivileged user and mount namespaces are unavailable");
    }
    QCOMPARE(WEXITSTATUS(status), 0);
}

// 認可待ちの上限

/**
 * @brief 1つのUIDが上限まで積んでも、別のUIDは枠を確保できることを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetLimitsEachUid()
{
    qsnapper::security::PendingAuthorizationBudget budget(4, 16);

    for (int i = 0; i < 4; ++i) {
        QVERIFY(budget.tryAcquire(1000));
    }
    QVERIFY(!budget.tryAcquire(1000));
    QCOMPARE(budget.countFor(1000), 4);

    QVERIFY(budget.tryAcquire(1001));
    QCOMPARE(budget.countFor(1001), 1);
    QCOMPARE(budget.total(), 5);
}

/**
 * @brief UIDごとの枠に余裕があっても、全体の上限を超えては確保できないことを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetKeepsGlobalLimit()
{
    qsnapper::security::PendingAuthorizationBudget budget(4, 16);

    for (uint uid = 1000; uid < 1004; ++uid) {
        for (int i = 0; i < 4; ++i) {
            QVERIFY(budget.tryAcquire(uid));
        }
    }
    QCOMPARE(budget.total(), 16);
    QVERIFY(!budget.tryAcquire(2000));
    QCOMPARE(budget.countFor(2000), 0);
    QCOMPARE(budget.total(), 16);
}

/**
 * @brief 返却した枠が同じUIDと全体の両方へ戻り、未確保のUIDの返却は件数を変えないことを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetReleasesSlots()
{
    qsnapper::security::PendingAuthorizationBudget budget(2, 3);

    QVERIFY(budget.tryAcquire(1000));
    QVERIFY(budget.tryAcquire(1000));
    QVERIFY(budget.tryAcquire(1001));
    QVERIFY(!budget.tryAcquire(1000));
    QVERIFY(!budget.tryAcquire(1002));

    // 確保していないUIDの返却では、他のUIDの枠も全体の件数も変わらない
    budget.release(1002);
    QCOMPARE(budget.total(), 3);
    QCOMPARE(budget.countFor(1000), 2);

    budget.release(1000);
    QCOMPARE(budget.countFor(1000), 1);
    QCOMPARE(budget.total(), 2);
    QVERIFY(budget.tryAcquire(1002));

    budget.release(1000);
    budget.release(1001);
    budget.release(1002);
    QCOMPARE(budget.total(), 0);
    QCOMPARE(budget.countFor(1000), 0);
    QVERIFY(budget.tryAcquire(1000));
}

/**
 * @brief FIFOなどの特殊ファイルは計画を失敗させず、live側に触れずにスキップされることを検証する
 */
void TestSnapshotOperations::specialFileSourceIsSkippedWithoutTouchingLive()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::mkfifo(QFile::encodeName(fixture.sourcePath + QStringLiteral("/pipe")).constData(), 0600), 0);
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/pipe"), "live data\n"));

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-special"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-special"),
                                                    {QStringLiteral("/pipe"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Skipped);
    QCOMPARE(readFile(fixture.livePath + QStringLiteral("/pipe")), QByteArray("live data\n"));

    // 復元元に無いpathは、スキップではなく失敗として扱う
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-special"),
                                                    {QStringLiteral("/missing"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);

    operations.finishRestorePlanForTesting(QStringLiteral("apply-special"),
                                           qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief YaST方式でも、ディレクトリとファイルの間で種別が変わったエントリを復元できることを検証する
 */
void TestSnapshotOperations::typechangedReplacesDirectoryAndFileInYastMode()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元ではファイル、live側ではディレクトリ
    QVERIFY(writeFile(fixture.sourcePath + QStringLiteral("/item"), "source file\n"));
    QVERIFY(QDir().mkpath(fixture.livePath + QStringLiteral("/item/child")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/item/child/data"), "live child\n"));

    // 復元元ではディレクトリ、live側ではファイル
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/node")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/node"), "live file\n"));

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-typechanged"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-typechanged"),
                                                    {QStringLiteral("/item"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-typechanged"),
                                                    {QStringLiteral("/node"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);

    struct stat itemStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/item")).constData(), &itemStat), 0);
    QVERIFY(S_ISREG(itemStat.st_mode));
    QCOMPARE(readFile(fixture.livePath + QStringLiteral("/item")), QByteArray("source file\n"));

    struct stat nodeStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/node")).constData(), &nodeStat), 0);
    QVERIFY(S_ISDIR(nodeStat.st_mode));

    // 退避した元のobjectは、成功後に片付けられている
    QVERIFY(hasNoQsnapperLeftovers(fixture.livePath));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-typechanged"),
                                           qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief 既存のディレクトリと新規に作成するディレクトリの両方で、modeを適用する直前に継承ACLが削除されていることを検証する
 *
 * 復元先にdefault ACLがあると、その下のディレクトリは名前付きユーザのエントリを継承する
 * 復元元のmodeを適用してgroupのビットが広がると、継承したエントリが有効になり、名前付きユーザが復元中のディレクトリを参照できる
 * そのため、ACLの同期がmodeの適用より先に終わっていることを、applyDirectoryBeneathRootを通る両方の経路で確認する
 */
void TestSnapshotOperations::applyRestoreEntrySyncsDirectoryAclBeforeModeRestore()
{
    const QString manifestId = QStringLiteral("apply-directory-acl");
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元のディレクトリは、ACLを持たないmode 0710とする
    // live側にあるexistingは既存のディレクトリとして、live側に無いfreshは新規に作成するディレクトリとして復元する
    const QStringList names = {QStringLiteral("existing"), QStringLiteral("fresh")};
    for (const QString &name : names) {
        const QString sourceDir = fixture.sourcePath + QLatin1Char('/') + name;
        QVERIFY(QDir().mkpath(sourceDir));
        QCOMPARE(::chmod(QFile::encodeName(sourceDir).constData(), 0710), 0);
    }

    // 復元先にdefault ACLを設定してから既存のディレクトリを作成し、既存のディレクトリにもACLを継承させる
    const QByteArray defaultAcl = namedUserDefaultAcl(::getuid() + 1000);
    const int setResult = ::lsetxattr(QFile::encodeName(fixture.livePath).constData(), "system.posix_acl_default",
                                      defaultAcl.constData(), static_cast<size_t>(defaultAcl.size()), 0);
    const int setError = setResult == 0 ? 0 : errno;
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    const QString existingDir = fixture.livePath + QStringLiteral("/existing");
    QVERIFY(QDir().mkpath(existingDir));
    QCOMPARE(pathXattrState(existingDir, "system.posix_acl_access"), 1);
    QCOMPARE(pathXattrState(existingDir, "system.posix_acl_default"), 1);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, manifestId, false));

    // 既存のディレクトリはmodifiedとして、live側に無いディレクトリはdeletedとして復元する
    // 観測結果の記録先はこの関数のローカル変数であるため、判定で途中終了する前にobserverを解除する
    QList<DirectoryAclObservation> observations;
    qsnapper::security::detail::setMetadataStageObserverForTesting(&recordDirectoryAclObservation, &observations);
    const qsnapper::restore::RestoreEntryOutcome existingOutcome =
            operations.applyRestoreEntryForTesting(manifestId, {QStringLiteral("/existing"), QStringLiteral("modified")});
    const qsnapper::restore::RestoreEntryOutcome freshOutcome =
            operations.applyRestoreEntryForTesting(manifestId, {QStringLiteral("/fresh"), QStringLiteral("deleted")});
    qsnapper::security::detail::setMetadataStageObserverForTesting(nullptr, nullptr);

    QCOMPARE(existingOutcome, qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(freshOutcome, qsnapper::restore::RestoreEntryOutcome::Applied);

    // 2つのディレクトリのどちらも、modeを適用する直前には継承したaccess ACLとdefault ACLが削除されている必要がある
    QCOMPARE(observations.size(), 2);
    for (const DirectoryAclObservation &beforeMode : std::as_const(observations)) {
        QVERIFY2(beforeMode.accessState == 0 && beforeMode.defaultState == 0,
                 describeDirectoryAclObservation(beforeMode).constData());
    }

    // 復元後のmodeとACLは、復元元と一致する
    for (const QString &name : names) {
        const QString restoredDir = fixture.livePath + QLatin1Char('/') + name;
        struct stat restoredStat;
        QCOMPARE(::lstat(QFile::encodeName(restoredDir).constData(), &restoredStat), 0);
        QCOMPARE(restoredStat.st_mode & 07777, 0710U);
        QCOMPARE(pathXattrState(restoredDir, "system.posix_acl_access"), 0);
        QCOMPARE(pathXattrState(restoredDir, "system.posix_acl_default"), 0);
    }

    operations.finishRestorePlanForTesting(manifestId, qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief 子エントリの適用で変わったディレクトリの時刻が、計画の終了時に復元元へそろうことを検証する
 */
void TestSnapshotOperations::directoryTimesAreRestoredAfterChildEntries()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/dir")));
    QVERIFY(QDir().mkpath(fixture.livePath + QStringLiteral("/dir")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/dir/extra"), "created after snapshot\n"));

    // 復元元のディレクトリへ既知の時刻を設定し、子の削除後もその時刻が復元されることを観測できるようにする
    const struct timespec sourceTimes[2] = {{1000000000, 0}, {1000000000, 123456789}};
    QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(fixture.sourcePath + QStringLiteral("/dir")).constData(),
                         sourceTimes, AT_SYMLINK_NOFOLLOW), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-times"), false));

    // ディレクトリを先に適用し、その後に子を削除する (削除で親のmtimeが更新される)
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-times"),
                                                    {QStringLiteral("/dir"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-times"),
                                                    {QStringLiteral("/dir/extra"), QStringLiteral("created")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QVERIFY(!QFileInfo::exists(fixture.livePath + QStringLiteral("/dir/extra")));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-times"),
                                           qsnapper::restore::ManifestState::Completed);

    struct stat liveStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/dir")).constData(), &liveStat), 0);
    QCOMPARE(static_cast<qint64>(liveStat.st_mtim.tv_sec), static_cast<qint64>(1000000000));
    QCOMPARE(static_cast<qint64>(liveStat.st_mtim.tv_nsec), static_cast<qint64>(123456789));
}

/**
 * @brief live側の同名objectがsymlinkの場合、その先のディレクトリへmetadataを適用しないことを検証する
 *
 * 適用中に名前をsymlinkへ差し替えられても、rootがsymlinkの先 (例えば他人のディレクトリ) の所有者やmodeを変えないようにする
 */
void TestSnapshotOperations::existingSymlinkIsNotUsedAsDirectory()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/conf")));
    QCOMPARE(::chmod(QFile::encodeName(fixture.sourcePath + QStringLiteral("/conf")).constData(), 0700), 0);

    // symlinkの先にrootが変えてはならないディレクトリを置き、modeが変わらないことを観測できるようにする
    const QString victimPath = fixture.root.filePath(QStringLiteral("victim"));
    QVERIFY(QDir().mkpath(victimPath));
    QCOMPARE(::chmod(QFile::encodeName(victimPath).constData(), 0755), 0);
    QCOMPARE(::symlink(QFile::encodeName(victimPath).constData(),
                       QFile::encodeName(fixture.livePath + QStringLiteral("/conf")).constData()), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-symlink"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-symlink"),
                                                    {QStringLiteral("/conf"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);

    struct stat victimStat;
    QCOMPARE(::stat(QFile::encodeName(victimPath).constData(), &victimStat), 0);
    QCOMPARE(victimStat.st_mode & 07777, static_cast<mode_t>(0755));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-symlink"),
                                           qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief root共有親と単一principalの親で旧ポリシーの適用範囲を区別する
 */
void TestSnapshotOperations::untrustedParentRefusesMetadataOnUnprovenDirectories_data()
{
    QTest::addColumn<int>("parentUid");
    QTest::addColumn<int>("parentMode");
    QTest::addColumn<bool>("allowed");
    QTest::newRow("root-shared-refused") << 0 << 0777 << false;
    QTest::newRow("principal-parent-applied") << 1000 << 0755 << true;
}

/**
 * @brief 他ユーザの書き換えられる親の下では、入れ替えられた可能性のあるディレクトリへrootがmetadataを適用しないことを検証する
 *
 * 所有者の変更を伴うため、rootで実行した場合だけ確認する
 */
void TestSnapshotOperations::untrustedParentRefusesMetadataOnUnprovenDirectories()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to change directory owners");
    }

    constexpr uid_t kUserUid = 1000;    // 親ディレクトリを書き換えられる一般ユーザ
    QFETCH(int, parentUid);
    QFETCH(int, parentMode);
    QFETCH(bool, allowed);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元の3件を一般ユーザ所有にし、復元先の所有者とmodeを変えて3つの場合分けを作る
    const QString sourceShared = fixture.sourcePath + QStringLiteral("/shared");
    const QString liveShared = fixture.livePath + QStringLiteral("/shared");
    for (const QString &name : {QStringLiteral("swapped"), QStringLiteral("owned"), QStringLiteral("created")}) {
        const QString path = sourceShared + QLatin1Char('/') + name;
        QVERIFY(QDir().mkpath(path));
        QCOMPARE(::chown(QFile::encodeName(path).constData(), kUserUid, kUserUid), 0);
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0700), 0);
    }
    QVERIFY(QDir().mkpath(liveShared + QStringLiteral("/swapped")));
    QVERIFY(QDir().mkpath(liveShared + QStringLiteral("/owned")));
    QCOMPARE(::chown(QFile::encodeName(liveShared).constData(), parentUid, parentUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared).constData(), parentMode), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared + QStringLiteral("/swapped")).constData(), 0755), 0);
    QCOMPARE(::chown(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), kUserUid, kUserUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), 0755), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-untrusted"), false));

    // rootが所有する既存のディレクトリは、入れ替えられた機密のディレクトリかもしれないため変更しない
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/swapped"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);
    struct stat swappedStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/swapped")).constData(), &swappedStat), 0);
    QCOMPARE(swappedStat.st_uid, static_cast<uid_t>(0));
    QCOMPARE(swappedStat.st_mode & 07777, static_cast<mode_t>(0755));

    // sourceとの所有者一致だけでは不十分で、親の単一principalとも一致する場合だけ復元する
    errno = 0;
    const auto ownedOutcome = operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/owned"), QStringLiteral("modified")});
    const int ownedError = errno;
    QCOMPARE(ownedOutcome, allowed ? qsnapper::restore::RestoreEntryOutcome::Applied
                                  : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (!allowed) {
        QCOMPARE(ownedError, EPERM);
    }
    struct stat ownedStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), &ownedStat), 0);
    QCOMPARE(ownedStat.st_uid, kUserUid);
    QCOMPARE(ownedStat.st_mode & 07777, static_cast<mode_t>(allowed ? 0700 : 0755));

    // 作成時からprincipal所有にできる親だけに限定し、root共有親では何も作らない
    errno = 0;
    const auto createdOutcome = operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/created"), QStringLiteral("deleted")});
    const int createdError = errno;
    QCOMPARE(createdOutcome, allowed ? qsnapper::restore::RestoreEntryOutcome::Applied
                                    : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (allowed) {
        struct stat createdStat;
        QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/created")).constData(), &createdStat), 0);
        QCOMPARE(createdStat.st_uid, kUserUid);
        QCOMPARE(createdStat.st_mode & 07777, static_cast<mode_t>(0700));
    }
    else {
        QCOMPARE(createdError, EPERM);
        QVERIFY(!QFileInfo::exists(liveShared + QStringLiteral("/created")));
    }

    operations.finishRestorePlanForTesting(QStringLiteral("apply-untrusted"),
                                           qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief trusted-chain規則の拒否と正常系を独立した行にしてSKIPによる見かけの成功を防ぐ
 */
void TestSnapshotOperations::directoryMetadataFollowsChainOwnership_data()
{
    QTest::addColumn<QString>("relative");
    QTest::addColumn<int>("parentUid");
    QTest::addColumn<int>("parentMode");
    QTest::addColumn<int>("targetUid");
    QTest::addColumn<int>("sourceUid");
    QTest::addColumn<int>("sourceMode");
    QTest::addColumn<int>("expectedError");
    QTest::addColumn<bool>("regularFile");
    QTest::newRow("i-root-victim") << QStringLiteral("home/alice/victim") << 1000 << 0755 << 0 << 1000 << 0750 << EPERM << false;
    QTest::newRow("ii-foreign-owner-match") << QStringLiteral("home/alice/bobdir") << 1000 << 0755 << 2000 << 2000 << 0750 << EPERM << false;
    QTest::newRow("iii-principal-existing") << QStringLiteral("home/alice/own") << 1000 << 0755 << 1000 << 1000 << 0750 << 0 << false;
    QTest::newRow("iv-principal-missing") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 1000 << 0750 << 0 << false;
    QTest::newRow("v-root-shared") << QStringLiteral("shared/d") << 0 << 0777 << 1000 << 1000 << 0750 << EPERM << false;
    QTest::newRow("vi-root-sticky-shared") << QStringLiteral("shared/d") << 0 << 01777 << 1000 << 1000 << 0750 << EPERM << false;
    QTest::newRow("vii-trusted-chain") << QStringLiteral("home/d") << 0 << 0755 << 0 << 1000 << 0750 << 0 << false;
    QTest::newRow("viii-protected-ancestor") << QStringLiteral("home/alice/protected/file") << 1000 << 0755 << 0 << 1000 << 0640 << EPERM << true;
    QTest::newRow("ix-readonly-principal-parent") << QStringLiteral("home/alice/new") << 1000 << 0555 << -1 << 1000 << 0750 << EACCES << false;
    QTest::newRow("x-identity-restored") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 1000 << 0700 << 0 << false;
    QTest::newRow("xi-principal-group-writable") << QStringLiteral("home/alice/grp/x") << 1000 << 0755 << -1 << 1000 << 0775 << 0 << false;
    QTest::newRow("source-foreign-refused-before-mkdir") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 2000 << 02775 << EPERM << false;
    QTest::newRow("principal-root-source-refused") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 0 << 04755 << EPERM << false;
}

/**
 * @brief 攻撃者が同じ親内で他者のディレクトリをrenameできてもmetadataや内容が変わらないことを確認する
 */
void TestSnapshotOperations::directoryMetadataFollowsChainOwnership()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to exercise directory ownership and filesystem identity");
    }
    QFETCH(QString, relative);
    QFETCH(int, parentUid);
    QFETCH(int, parentMode);
    QFETCH(int, targetUid);
    QFETCH(int, sourceUid);
    QFETCH(int, sourceMode);
    QFETCH(int, expectedError);
    QFETCH(bool, regularFile);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::chmod(QFile::encodeName(fixture.livePath).constData(), 0755), 0);
    const QString target = fixture.livePath + QLatin1Char('/') + relative;
    const QString source = fixture.sourcePath + QLatin1Char('/') + relative;
    const QString parentRelative = relative.left(relative.lastIndexOf(QLatin1Char('/')));
    const QString parent = fixture.livePath + QLatin1Char('/') + parentRelative;
    QVERIFY(QDir().mkpath(parent));
    if (relative.startsWith(QStringLiteral("home/alice/"))) {
        const QByteArray home = QFile::encodeName(fixture.livePath + QStringLiteral("/home"));
        const QByteArray alice = QFile::encodeName(fixture.livePath + QStringLiteral("/home/alice"));
        QCOMPARE(::chmod(home.constData(), 0755), 0);
        QCOMPARE(::chown(alice.constData(), parentUid, parentUid), 0);
        QCOMPARE(::chmod(alice.constData(), parentMode), 0);
    }
    else {
        QCOMPARE(::chown(QFile::encodeName(parent).constData(), parentUid, parentUid), 0);
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), parentMode), 0);
    }
    if (relative.contains(QStringLiteral("/grp/"))) {
        QCOMPARE(::chown(QFile::encodeName(parent).constData(), 1000, 1000), 0);
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), 0775), 0);
    }
    if (regularFile) {
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), 0700), 0);
        QVERIFY(writeFile(target, "protected live content\n"));
        QVERIFY(QDir().mkpath(source.left(source.lastIndexOf(QLatin1Char('/')))));
        QVERIFY(writeFile(source, "snapshot content\n"));
    }
    else {
        QVERIFY(QDir().mkpath(source));
        if (targetUid >= 0) {
            QVERIFY(QDir().mkpath(target));
            QCOMPARE(::chown(QFile::encodeName(target).constData(), targetUid, targetUid), 0);
            QCOMPARE(::chmod(QFile::encodeName(target).constData(), 0711), 0);
        }
    }
    QCOMPARE(::chown(QFile::encodeName(source).constData(), sourceUid, sourceUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(source).constData(), sourceMode), 0);
    struct stat before {};
    if (targetUid >= 0) {
        QCOMPARE(::lstat(QFile::encodeName(target).constData(), &before), 0);
    }
    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("chain-policy");
    QVERIFY(fixture.begin(operations, manifestId, false));
    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(
        manifestId, {QLatin1Char('/') + relative, QStringLiteral("modified")});
    const int applyError = errno;
    // 成功と失敗のどちらでもidentityを戻し、次のroot操作に権限状態を持ち越さない
    QCOMPARE(::setfsuid(static_cast<uid_t>(-1)), 0);
    QCOMPARE(::setfsgid(static_cast<gid_t>(-1)), 0);
    QCOMPARE(outcome, expectedError == 0 ? qsnapper::restore::RestoreEntryOutcome::Applied
                                        : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (expectedError != 0) {
        QCOMPARE(applyError, expectedError);
        if (targetUid < 0) {
            QVERIFY(!QFileInfo::exists(target));
        }
        else {
            struct stat after {};
            QCOMPARE(::lstat(QFile::encodeName(target).constData(), &after), 0);
            QCOMPARE(after.st_ino, before.st_ino);
            QCOMPARE(after.st_uid, before.st_uid);
            QCOMPARE(after.st_mode, before.st_mode);
        }
        if (regularFile) {
            QCOMPARE(readFile(target), QByteArray("protected live content\n"));
            struct stat protectedStat {};
            QCOMPARE(::stat(QFile::encodeName(parent).constData(), &protectedStat), 0);
            QCOMPARE(protectedStat.st_uid, uid_t(0));
            QCOMPARE(protectedStat.st_mode & 07777, mode_t(0700));
        }
    }
    else {
        struct stat after {};
        QCOMPARE(::stat(QFile::encodeName(target).constData(), &after), 0);
        QCOMPARE(after.st_uid, static_cast<uid_t>(sourceUid));
        QCOMPARE(after.st_gid, static_cast<gid_t>(sourceUid));
        QCOMPARE(after.st_mode & 07777, static_cast<mode_t>(sourceMode));
    }
    QVERIFY(hasNoQsnapperLeftovers(parent));
    operations.finishRestorePlanForTesting(manifestId,
        expectedError == 0 ? qsnapper::restore::ManifestState::Completed : qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief rootサブボリュームをrwへ戻せなかった計画は、全エントリを適用できていてもFailedで終わることを検証する
 */
void TestSnapshotOperations::rootLeftReadOnlyMarksPlanFailed()
{
    SnapshotOperations operations;
    int restoreCalls = 0;
    operations.setRootSubvolumeAccessForTesting(
        [](bool *readOnly) {
            *readOnly = true;
            return true;
        },
        [&restoreCalls]() {
            ++restoreCalls;
            return false;
        });

    qsnapper::restore::RootReadWriteSafetyNetState state;
    state.executionStarted = true;
    state.targetsRootSubvolume = true;
    state.preRestoreStateKnown = true;
    state.preRestoreReadOnly = false;
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const int sourceFd = ::open(QFile::encodeName(sourceDir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(sourceFd >= 0);
    operations.beginRestoreApplyForTesting(QStringLiteral("root-left-ro"), sourceDir.path(), sourceFd,
                                           QStringLiteral("/"), false, state);

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    operations.finishRestorePlanForTesting(QStringLiteral("root-left-ro"),
                                           qsnapper::restore::ManifestState::Completed);

    QCOMPARE(restoreCalls, 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("failed"));
    QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Restore failed"));

    const QString status = operations.GetRestorePlanStatus(QStringLiteral("root-left-ro"));
    QVERIFY(status.startsWith(QStringLiteral("root-left-ro,failed,")));
    QVERIFY(status.contains(QStringLiteral("could not be confirmed writable")));
}

/**
 * @brief 退避先の名前が推測できない乱数の接尾辞を持つことを検証する
 *
 * 書き込み可能な親ディレクトリでは、他のユーザが名前を先回りして作成し、退避や差し替えを妨害できる
 * そのため、接尾辞は推測できない値でなければならない
 * ただし、推測できないことはテストでは証明できないため、同じ元パスから8回生成した名前について
 * 形式 (16桁の16進数の接尾辞) と重複の有無だけを確認する
 * 推測できないことの根拠は、実装がQRandomGenerator::system()->generate64()で接尾辞を生成していることにある
 *
 * @note 接尾辞にPIDの数字列が含まれないことは確認しない
 *       16桁の乱数にPIDの数字列が偶然現れる場合があり、正しい実装でもテストが失敗するため
 */
void TestSnapshotOperations::siblingTemporaryPathIsUnpredictable()
{
    constexpr int kGenerationCount = 8;  // 形式と重複の有無を確かめる生成回数

    const QRegularExpression pattern(QStringLiteral("^/srv/data/\\.file\\.conf\\.qsnapper-old\\.([0-9a-f]{16})$"));

    QSet<QString> names;
    QSet<QString> suffixes;
    for (int index = 0; index < kGenerationCount; ++index) {
        const QString name = qsnapper::internal::siblingTemporaryPath(QStringLiteral("/srv/data/file.conf"),
                                                                      QStringLiteral("qsnapper-old"));
        const QRegularExpressionMatch match = pattern.match(name);
        QVERIFY2(match.hasMatch(), qPrintable(name));

        names.insert(name);
        suffixes.insert(match.captured(1));
    }

    // 接尾辞が固定値であれば、8回の生成のどこかで名前が重複する
    QCOMPARE(names.size(), kGenerationCount);
    QCOMPARE(suffixes.size(), kGenerationCount);

    // 長い名前でもNAME_MAXを超えず、接尾辞は保たれる
    const QString longName = QStringLiteral("/tmp/") + QString(300, QLatin1Char('a'));
    const QString trimmed = qsnapper::internal::siblingTemporaryPath(longName, QStringLiteral("qsnapper-old"));
    QVERIFY(trimmed.mid(5).toUtf8().size() <= NAME_MAX);
    QVERIFY(trimmed.contains(QStringLiteral(".qsnapper-old.")));
}

/**
 * @brief 認可済みの本体で起きた想定外の例外が、スロットの外へ出ないことを確認する
 *
 * 例外がQtのイベントループまで届くとサービスが異常終了するため、bad_allocとstd::exception以外の例外も止める
 */
void TestSnapshotOperations::unexpectedExceptionsDoNotEscapeAuthorizedBody()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    bool result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw std::bad_alloc(); });
    }
    catch (...) {
        QFAIL("bad_alloc escaped from the authorized body");
    }
    QVERIFY(!result);

    result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw std::runtime_error("secret path"); });
    }
    catch (...) {
        QFAIL("std::exception escaped from the authorized body");
    }
    QVERIFY(!result);

    result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw 42; });
    }
    catch (...) {
        QFAIL("a non-standard exception escaped from the authorized body");
    }
    QVERIFY(!result);

    // 例外を止めた後も、通常の本体はそのまま実行できる
    QVERIFY(operations.authorizeThrowingBodyForTesting([]() { return true; }));
}

/**
 * @brief ModifySnapshotが未知のcleanupアルゴリズムを認可の前に拒否することを確認する
 *
 * 未知の名前を保存すると、そのスナップショットがcleanupの対象から黙って外れる
 * また、通らない値のために利用者へ認可のプロンプトを出さないよう、認可の要求そのものが起きないことを確かめる
 * 許可する名前でも、説明文に制御文字があれば本体の先頭で拒否し、実在のスナップショットに触れずに認可だけが要求されることを確かめる
 */
void TestSnapshotOperations::modifySnapshotRejectsUnknownCleanupBeforeAuthorization()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const int before = operations.testAuthorizationRequestCount();
    const QStringList rejected = {
        QStringLiteral("bogus"),
        QStringLiteral("Number"),
        QStringLiteral("number "),
        QStringLiteral("timeline,number"),
    };
    for (const QString &cleanup : rejected) {
        QVERIFY(!operations.ModifySnapshot(QStringLiteral("root"), 1, QStringLiteral("description"), cleanup, {}));
    }
    QCOMPARE(operations.testAuthorizationRequestCount(), before);

    const QStringList allowed = {
        QString(),
        QStringLiteral("number"),
        QStringLiteral("timeline"),
        QStringLiteral("empty-pre-post"),
    };
    for (const QString &cleanup : allowed) {
        QVERIFY(!operations.ModifySnapshot(QStringLiteral("root"), 1, QStringLiteral("line\nbreak"), cleanup, {}));
    }
    QCOMPARE(operations.testAuthorizationRequestCount(), before + static_cast<int>(allowed.size()));
}

/**
 * @brief 認可済みのconfig操作の直前に、対象configのStaging計画をownerを問わず取り消すことを検証する
 *
 * 取り消した計画は作成者へcancelledで通知し、別のconfigの計画や番号の異なる計画は残す
 */
void TestSnapshotOperations::configOperationsDiscardStagingPlansOfConfig()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    // rootの計画を2件と、同じ復元元番号2を持つhomeの計画を用意し、取り消す範囲を区別できるようにする
    const QString first = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    const QString second = operations.BeginRestorePlan(QStringLiteral("root"), 2, 0, QStringLiteral("direct"));
    const QString other = operations.BeginRestorePlan(QStringLiteral("home"), 2, 0, QStringLiteral("direct"));
    QVERIFY(!first.isEmpty() && !second.isEmpty() && !other.isEmpty());

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);

    // 単体削除では、削除するスナップショットを参照する計画だけを取り消す
    operations.discardStagingRestorePlansForTesting(QStringLiteral("root"), 2);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), second);
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));

    // config単位の操作では、そのconfigのStaging計画を全て取り消す
    operations.discardStagingRestorePlansForTesting(QStringLiteral("root"), -1);
    QCOMPARE(finishedSpy.count(), 2);
    QCOMPARE(finishedSpy.at(1).at(0).toString(), first);
    QVERIFY(operations.GetRestorePlanStatus(first).startsWith(first + QStringLiteral(",cancelled,")));
    QVERIFY(operations.GetRestorePlanStatus(other).startsWith(other + QStringLiteral(",staging,")));
}

/**
 * @brief 実行中にownerの接続が切れても、rootサブボリュームをrwへ戻す安全ネットを実行することを検証する
 */
void TestSnapshotOperations::vanishedOwnerStillRunsRootSafetyNet()
{
    SnapshotOperations operations;
    int restoreCalls = 0;
    operations.setRootSubvolumeAccessForTesting(
        [](bool *readOnly) {
            *readOnly = true;
            return true;
        },
        [&restoreCalls]() {
            ++restoreCalls;
            return true;
        });
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    const QString planId = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!planId.isEmpty());

    qsnapper::restore::RootReadWriteSafetyNetState state;
    state.executionStarted = true;
    state.targetsRootSubvolume = true;
    state.preRestoreStateKnown = true;
    state.preRestoreReadOnly = false;
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const int sourceFd = ::open(QFile::encodeName(sourceDir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(sourceFd >= 0);
    operations.beginRestoreApplyForTesting(planId, sourceDir.path(), sourceFd, QStringLiteral("/"), false, state);

    // D-Busを介さない呼び出しでは、ownerは空文字列として記録される
    operations.handleRestoreOwnerUnregisteredForTesting(QString());

    QCOMPARE(restoreCalls, 1);
    QVERIFY(operations.GetRestorePlanStatus(planId).isEmpty());
}

QTEST_GUILESS_MAIN(TestSnapshotOperations)
#include "tst_snapshotoperations.moc"
