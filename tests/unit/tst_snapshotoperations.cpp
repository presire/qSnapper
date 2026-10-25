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
#include <sys/vfs.h>
#include <sys/wait.h>
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

    void beginThenCancelDoesNotTouchRootSubvolume();
    void stagedPlanCancelDoesNotTouchRootSubvolume();

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
    /** @brief 子エントリの適用で変わるディレクトリの時刻を、計画の終了時に復元することを検証する */
    void directoryTimesAreRestoredAfterChildEntries();
    /** @brief 復元先の同名symlinkの先へmetadataを適用しないことを検証する */
    void existingSymlinkIsNotUsedAsDirectory();
    /** @brief 他ユーザが書き換えられる親の下では、入れ替えられた可能性のある
     *        ディレクトリへmetadataを適用しないことを検証する */
    void untrustedParentRefusesMetadataOnUnprovenDirectories();
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

}

void TestSnapshotOperations::init()
{
    m_readOnlyProbeCalls = 0;
    m_readWriteRestoreCalls = 0;
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

void TestSnapshotOperations::stagedPlanCancelDoesNotTouchRootSubvolume()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const QString manifestId = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("yast"));
    QVERIFY(!manifestId.isEmpty());
    QVERIFY(operations.StageRestoreEntries(manifestId,
                                           {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    QVERIFY(operations.CancelRestorePlan(manifestId));

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);
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
    QCOMPARE(::chmod(QFile::encodeName(liveShared).constData(), 0777), 0);
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

    // 所有者が既にsnapshot側と同じ一般ユーザであれば、modeを復元する
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/owned"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    struct stat ownedStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), &ownedStat), 0);
    QCOMPARE(ownedStat.st_mode & 07777, static_cast<mode_t>(0700));

    // 自分で作成した空のディレクトリには、snapshot側の所有者とmodeを適用する
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/created"), QStringLiteral("deleted")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    struct stat createdStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/created")).constData(), &createdStat), 0);
    QCOMPARE(createdStat.st_uid, kUserUid);
    QCOMPARE(createdStat.st_mode & 07777, static_cast<mode_t>(0700));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-untrusted"),
                                           qsnapper::restore::ManifestState::Failed);
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
