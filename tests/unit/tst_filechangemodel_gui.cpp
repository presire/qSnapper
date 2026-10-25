/**
 * @file tst_filechangemodel_gui.cpp
 * @brief FileChangeModelのGUI側機能 (空の差分、一覧の上限、削除対象の事前表示、復元結果)の単体テスト
 *
 * 空の差分をエラーではなく正常な空状態として扱うこと
 * 変更一覧が件数または文字数の上限を超えたときに、切り詰めて省略した件数を公開すること
 * 復元の確認ダイアログに出す削除対象 (changeTypeがcreated) を数えること
 * GetRestorePlanStatusの応答 (旧9フィールドと拡張14フィールド) から復元結果を公開すること
 * テスト用のモックは tst_filechangemodel.h を共有する
 * 依存 : FileChangeModel、RestorePlanTransportのモック、Qt Test
 */

#include "tst_filechangemodel.h"

#include <QVariantMap>
#include <limits>

/**
 * @class TestFileChangeModelGui
 * @brief FileChangeModelのGUI側の追加機能のテスト
 */
class TestFileChangeModelGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void emptyDiffIsNormalEmptyStateWithoutError();
    void failedLoadDoesNotMarkAsLoaded();
    void nonEmptyDiffMarksLoadedWithChanges();
    void entryLimitTruncatesAndReportsOmittedCount();
    void charLimitTruncatesAndReportsOmittedCount();
    void defaultLimitsDoNotTruncateSmallLists();
    void deletePreviewCountsOnlyCreatedEntries();
    void deletePreviewTruncatesPathListButKeepsCount();
    void restoreResultParsesExtendedStatus();
    void restoreResultParsesLegacyStatus();
    void restoreResultClearedWhenNextRestoreStarts();

private:
    /**
     * @brief 復元の計画を最後まで進め、終端シグナルとステータス応答を流す
     * @param model 対象のモデル
     * @param fake 差し替えた転送層
     * @param terminalState 終端状態 (completed / failed)
     * @param statusCsv GetRestorePlanStatusの応答
     * @note モックの保留中呼び出しを完了させながら進めるため、呼び出し後はモデルが復元結果を保持している
     */
    static void runRestoreToTerminal(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                     const QString &terminalState, const QString &statusCsv);
};

/**
 * @brief 設定ファイルがユーザ環境に影響しないよう、保存先をテスト専用に切り替える
 */
void TestFileChangeModelGui::initTestCase()
{
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       QDir::temp().filePath(QStringLiteral("qsnapper-tst-filechangemodel-gui")));
}

void TestFileChangeModelGui::runRestoreToTerminal(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                                  const QString &terminalState, const QString &statusCsv)
{
    QVERIFY(model->restoreCheckedItems());
    fake->completeAllPendingOk();
    fake->emitPlanFinished(fake->nextManifestId, terminalState, QString());
    QVERIFY(fake->countKind(FakeRestorePlanTransport::Call::Status) >= 1);
    fake->completePending(true, statusCsv);
}

/**
 * @brief 差分が空の応答をエラーにせず、読み込み済みの空状態として扱うことを確認する
 *
 * 空の差分をエラーにすると、変更のないスナップショットを開くたびにエラーが表示される
 */
void TestFileChangeModelGui::emptyDiffIsNormalEmptyStateWithoutError()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(5);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    model.loadChanges();
    QCOMPARE(model.pendingStringReplies.size(), 1);
    QVERIFY(model.isLoading());

    model.completeStringReply(0, true, QString());

    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(!model.isLoading());
    QVERIFY(model.isLoaded());
    QVERIFY(!model.hasChanges());
    QCOMPARE(model.rowCount(), 0);
}

/**
 * @brief 差分の取得 (D-Bus呼び出し) に失敗したとき、エラーを通知し、一覧を読み込み済みにしないことを確認する
 *
 * 失敗を読み込み済みにすると、取得できていない空の一覧を「変更なし」と誤表示する
 */
void TestFileChangeModelGui::failedLoadDoesNotMarkAsLoaded()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(5);
    QSignalSpy errorSpy(&model, &FileChangeModel::errorOccurred);

    model.loadChanges();
    model.completeStringReply(0, false, QString(), QStringLiteral("boom"));

    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!model.isLoaded());
    QVERIFY(!model.hasChanges());
}

/**
 * @brief 変更を含む差分を読み込むと、読み込み済みになり変更件数が公開されることを確認する
 *
 * 正常な差分で状態が更新されないと、一覧が空のまま復元操作を受け付けない
 */
void TestFileChangeModelGui::nonEmptyDiffMarksLoadedWithChanges()
{
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(5);

    model.loadChanges();
    model.completeStringReply(0, true, QStringLiteral("c.... /etc/a.conf\n"));

    QVERIFY(model.isLoaded());
    QVERIFY(model.hasChanges());
    QVERIFY(!model.isTruncated());
    QCOMPARE(model.omittedCount(), 0);
}

/**
 * @brief 件数の上限を超えた一覧を切り詰め、省略した件数を公開することを確認する
 *
 * 省略件数を示さないと、上限を超えた差分があることを利用者が見落とす
 */
void TestFileChangeModelGui::entryLimitTruncatesAndReportsOmittedCount()
{
    TestableFileChangeModel model;
    model.setChangeLimitsForTesting(3, std::numeric_limits<qsizetype>::max());
    QSignalSpy truncatedSpy(&model, &FileChangeModel::truncatedChanged);

    QString output;
    for (int i = 0; i < 10; ++i) {
        output += QStringLiteral("c.... /data/file%1\n").arg(i);
    }
    model.setupModelData(output, true);

    QVERIFY(model.isTruncated());
    QCOMPARE(model.omittedCount(), 7);
    QVERIFY(truncatedSpy.count() >= 1);
    // フラット表示では、取り込んだ3件だけが並ぶ
    QCOMPARE(model.rowCount(), 3);

    // 上限内の一覧を読み直すと、省略の表示が消える
    model.setupModelData(QStringLiteral("c.... /data/only\n"), true);
    QVERIFY(!model.isTruncated());
    QCOMPARE(model.omittedCount(), 0);
}

/**
 * @brief 文字数の上限を超えた一覧を切り詰め、省略した件数を公開することを確認する
 *
 * 1件のパスが極端に長い場合に文字数の上限が効かないと、GUIが大量の文字列を描画して固まる
 */
void TestFileChangeModelGui::charLimitTruncatesAndReportsOmittedCount()
{
    TestableFileChangeModel model;
    const QString line = QStringLiteral("c.... /data/file0000\n");
    // 3行ぶんの文字数だけ許可する
    model.setChangeLimitsForTesting(1000000, line.size() * 3);

    QString output;
    for (int i = 0; i < 8; ++i) {
        output += QStringLiteral("c.... /data/file%1\n").arg(i, 4, 10, QLatin1Char('0'));
    }
    model.setupModelData(output, true);

    QVERIFY(model.isTruncated());
    QVERIFY(model.omittedCount() > 0);
    QVERIFY(model.rowCount() <= 3);
    QVERIFY(model.rowCount() >= 1);
    QCOMPARE(model.rowCount() + model.omittedCount(), 8);
}

/**
 * @brief 既定の上限では通常の規模の一覧を切り詰めないことを確認する
 *
 * 既定の上限が小さすぎると、通常の差分まで欠けて表示される
 */
void TestFileChangeModelGui::defaultLimitsDoNotTruncateSmallLists()
{
    TestableFileChangeModel model;
    QString output;
    for (int i = 0; i < 2000; ++i) {
        output += QStringLiteral("c.... /data/file%1\n").arg(i);
    }
    model.setupModelData(output, true);
    QVERIFY(!model.isTruncated());
    QCOMPARE(model.rowCount(), 2000);
}

/**
 * @brief 確認ダイアログ用の削除対象が、現在のファイルシステムにだけ存在する項目 (changeTypeがcreated) だけを数えることを確認する
 *
 * 削除対象の数え方を誤ると、確認ダイアログの件数が実際の削除と食い違う
 */
void TestFileChangeModelGui::deletePreviewCountsOnlyCreatedEntries()
{
    TestableFileChangeModel model;
    model.setupModelData(QStringLiteral("+.... /data/new1\n"
                                        "+.... /data/new2\n"
                                        "-.... /data/removed\n"
                                        "c.... /data/modified\n"),
                         true);
    model.setItemChecked(QStringLiteral("/data/new1"), true);
    model.setItemChecked(QStringLiteral("/data/new2"), true);
    model.setItemChecked(QStringLiteral("/data/removed"), true);
    model.setItemChecked(QStringLiteral("/data/modified"), true);

    const QVariantMap preview = model.checkedDeletePreview(100);
    QCOMPARE(preview.value(QStringLiteral("count")).toInt(), 2);
    QStringList paths = preview.value(QStringLiteral("paths")).toStringList();
    paths.sort();
    QCOMPARE(paths, (QStringList{QStringLiteral("/data/new1"), QStringLiteral("/data/new2")}));

    // チェックを外したエントリは含まれない
    model.setItemChecked(QStringLiteral("/data/new2"), false);
    QCOMPARE(model.checkedDeletePreview(100).value(QStringLiteral("count")).toInt(), 1);
}

/**
 * @brief 削除対象のパス一覧を表示用に切り詰めても、削除件数は減らさないことを確認する
 *
 * パス一覧の上限で件数まで減らすと、実際に削除される件数を過小に示す
 */
void TestFileChangeModelGui::deletePreviewTruncatesPathListButKeepsCount()
{
    TestableFileChangeModel model;
    QString output;
    for (int i = 0; i < 20; ++i) {
        output += QStringLiteral("+.... /data/new%1\n").arg(i);
    }
    model.setupModelData(output, true);
    for (int i = 0; i < 20; ++i) {
        model.setItemChecked(QStringLiteral("/data/new%1").arg(i), true);
    }

    const QVariantMap preview = model.checkedDeletePreview(5);
    QCOMPARE(preview.value(QStringLiteral("count")).toInt(), 20);
    QCOMPARE(preview.value(QStringLiteral("paths")).toStringList().size(), 5);
}

/**
 * @brief 拡張14フィールドの状態応答から、適用数・削除数・スキップしたパスを復元結果として公開することを確認する
 *
 * 拡張フィールドを捨てると、スキップしたパスや削除件数が復元後に表示されない
 */
void TestFileChangeModelGui::restoreResultParsesExtendedStatus()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    QString output;
    QStringList paths;
    for (int i = 0; i < 4; ++i) {
        paths << QStringLiteral("/data/f%1").arg(i);
        output += QStringLiteral("+.... /data/f%1\n").arg(i);
    }
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(42);
    model.setUseDirectRestore(true);
    model.setRestoreBatchSize(10);
    model.setupModelData(output, true);
    model.setRestorePlanTransportForTesting(&fake);
    for (const QString &path : paths) {
        model.setItemChecked(path, true);
    }
    QSignalSpy resultSpy(&model, &FileChangeModel::restoreResultChanged);

    const QString csv = QStringLiteral("manifest-1,completed,4,4,4,direct,\"root\",42,\"\",3,1,-1,\"\",\"/data/f3\"");
    runRestoreToTerminal(&model, &fake, QStringLiteral("completed"), csv);

    QVERIFY(resultSpy.count() >= 1);
    const QVariantMap result = model.restoreResult();
    QVERIFY(result.value(QStringLiteral("valid")).toBool());
    QCOMPARE(result.value(QStringLiteral("appliedCount")).toInt(), 3);
    QCOMPARE(result.value(QStringLiteral("deletedCount")).toInt(), 3);
    QCOMPARE(result.value(QStringLiteral("skippedCount")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("skippedPaths")).toStringList(), QStringList{QStringLiteral("/data/f3")});
}

/**
 * @brief 旧バージョンのqsnapper-dbus-serviceが返す9フィールドの状態応答でも、復元結果を公開し、拡張項目を空にすることを確認する
 *
 * 旧バージョンの応答を無効とすると、復元が完了しても結果が表示されない
 */
void TestFileChangeModelGui::restoreResultParsesLegacyStatus()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(42);
    model.setUseDirectRestore(true);
    model.setRestoreBatchSize(10);
    model.setupModelData(QStringLiteral("c.... /data/a\n"), true);
    model.setRestorePlanTransportForTesting(&fake);
    model.setItemChecked(QStringLiteral("/data/a"), true);

    const QString csv = QStringLiteral("manifest-1,failed,1,0,0,direct,\"root\",42,\"copy failed\"");
    runRestoreToTerminal(&model, &fake, QStringLiteral("failed"), csv);

    const QVariantMap result = model.restoreResult();
    QVERIFY(result.value(QStringLiteral("valid")).toBool());
    QVERIFY(!result.value(QStringLiteral("hasExtended")).toBool());
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    QCOMPARE(result.value(QStringLiteral("skippedCount")).toInt(), 0);
    QCOMPARE(result.value(QStringLiteral("failedIndex")).toInt(), -1);
}

/**
 * @brief 次の復元を始めたときに、前回の復元結果が消えることを確認する
 *
 * 前回の結果が残ると、今回の復元結果として古い内容を表示する
 */
void TestFileChangeModelGui::restoreResultClearedWhenNextRestoreStarts()
{
    FakeRestorePlanTransport fake;
    TestableFileChangeModel model;
    model.setConfigName(QStringLiteral("root"));
    model.setSnapshotNumber(42);
    model.setUseDirectRestore(true);
    model.setRestoreBatchSize(10);
    model.setupModelData(QStringLiteral("c.... /data/a\n"), true);
    model.setRestorePlanTransportForTesting(&fake);
    model.setItemChecked(QStringLiteral("/data/a"), true);

    runRestoreToTerminal(&model, &fake, QStringLiteral("completed"),
                         QStringLiteral("manifest-1,completed,1,1,1,direct,\"root\",42,\"\""));
    QVERIFY(!model.restoreResult().isEmpty());

    QVERIFY(model.restoreCheckedItems());
    QVERIFY(model.restoreResult().isEmpty());
}

QTEST_APPLESS_MAIN(TestFileChangeModelGui)
#include "tst_filechangemodel_gui.moc"
