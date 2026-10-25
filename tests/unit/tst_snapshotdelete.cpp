/**
 * @file tst_snapshotdelete.cpp
 * @brief SnapshotListModel::deleteSnapshotsが、削除する件数に応じて呼び出すD-Busメソッドを選ぶことを検証する単体テスト
 *
 * 1件だけ削除する場合は、DeleteSnapshotを呼び出すことを確認する
 * DeleteSnapshotは、polkitのdelete-snapshot (auth_admin_keep) で認可し、ヘルパがlibsnapperで直接削除する
 * 複数件を削除する場合だけ、DeleteSnapshotsを呼び出すことを確認する
 * DeleteSnapshotsは、polkitのdelete-snapshots (auth_admin) で毎回認可し、snapperdを経由して削除する
 * 1件の削除でDeleteSnapshotsを呼び出すと、削除のたびに認証を求められる
 * また、作成した直後のスナップショットがSnapshotListOutdatedで拒否される
 * 一覧を現在のconfigから取得し終えていない間は、番号がどのconfigを指すか確定しないため削除を拒否する
 * configを切り替えた直後も、前のconfigの一覧に基づく削除を拒否し、同じ番号を切り替え先のスナップショットとして消さない
 * 本テストは、1件の削除が再びDeleteSnapshotsを使うように戻る不具合を検出する
 *
 * テストでは、テスト専用のdbus-daemonを起動し、DBUS_SYSTEM_BUS_ADDRESSでシステムバスの接続先をそのdbus-daemonへ切り替える
 * そのうえで、偽のcom.presire.qsnapper.Operationsサービスを登録するため、実際のシステムバスとヘルパには接続しない
 *
 * 依存 : Qt Test、Qt D-Bus、dbus-daemon (見つからない場合はテストを省略する)、SnapshotListModel、SnapperService
 */

#include <QtTest/QtTest>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include "snapperservice.h"
#include "snapshotlistmodel.h"

namespace {

const QString OPERATIONS_SERVICE = QStringLiteral("com.presire.qsnapper.Operations");  // 偽サービスが名乗るバス名 (本物のヘルパと同じ)
const QString OPERATIONS_PATH = QStringLiteral("/com/presire/qsnapper/Operations");    // 偽サービスを登録するオブジェクトパス
const QString FAKE_CONNECTION = QStringLiteral("tst_snapshotdelete_fake");             // 偽サービスがバスへ接続するときの接続名

} // namespace

/**
 * @class FakeOperations
 * @brief 削除に関わるD-Busメソッドだけを実装した、偽のcom.presire.qsnapper.Operationsサービス
 *
 * 受け取った削除の呼び出しを記録し、テストが呼び出されたメソッドと引数を確認できるようにする
 * DeleteSnapshotの応答 (true、false、エラー応答) は、テストから切り替えられる
 * SnapshotListModelは、一覧を取得し終えるまで削除を受け付けないため、各テストは先にListSnapshotsで一覧を読み込ませる
 * テストのスレッドがQTRY_*で待つ間も確実に応答できるよう、偽サービスは別のスレッドで動かす
 * 呼び出しの記録と応答の設定は、テストのスレッドと本オブジェクトのスレッドの両方から読み書きするため、ミューテックスで保護する
 */
class FakeOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

public:
    /**
     * @brief 偽のDeleteSnapshotが返す応答の種類
     */
    enum class SingleReply {
        ReturnTrue,   // trueを返す (削除に成功した場合を模擬する)
        ReturnFalse,  // falseを返す (エラー応答を伴わずに失敗した場合を模擬する)
        ReturnError   // Snapshot not foundのエラー応答を返す (削除対象がない場合を模擬する)
    };

    /**
     * @brief 呼び出しの記録を消去し、DeleteSnapshotの応答をtrueに戻す
     * @note 前のテストの記録と応答の設定が、次のテストの結果に影響しないようにする
     */
    void reset()
    {
        QMutexLocker locker(&m_mutex);
        m_calls.clear();
        m_listedConfigs.clear();
        m_singleReply = SingleReply::ReturnTrue;
    }

    /**
     * @brief 偽のDeleteSnapshotが返す応答を切り替える
     * @param reply 次の呼び出しから返す応答の種類
     */
    void setSingleReply(SingleReply reply)
    {
        QMutexLocker locker(&m_mutex);
        m_singleReply = reply;
    }

    /**
     * @brief 受け取った削除の呼び出しの記録を、受け取った順に返す
     * @return 呼び出し1回につき1つの文字列 ("メソッド名:設定名:番号,番号"形式) を持つ一覧
     */
    QStringList calls() const
    {
        QMutexLocker locker(&m_mutex);
        return m_calls;
    }

public slots:
    /**
     * @brief 1件の削除を受け付ける偽のDeleteSnapshot
     * @param configName Snapperの設定名
     * @param number 削除するスナップショット番号
     * @return setSingleReplyで指定した真偽値
     * @note 引数と戻り値の型は、本物のDeleteSnapshotと同じD-Busの署名 (引数はsとi、戻り値はb) にする
     *       署名が異なると、SnapshotListModelの呼び出しが本物と異なる結果になるためである
     *       実際には削除せず、呼び出しを記録するだけである
     */
    bool DeleteSnapshot(const QString &configName, int number)
    {
        QMutexLocker locker(&m_mutex);
        m_calls.append(QStringLiteral("DeleteSnapshot:%1:%2").arg(configName).arg(number));

        // エラー応答を送った場合、戻り値は呼び出し元へ届かない
        if (m_singleReply == SingleReply::ReturnError) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("Snapshot not found"));
            return false;
        }
        return m_singleReply == SingleReply::ReturnTrue;
    }

    /**
     * @brief 複数件の削除を受け付ける偽のDeleteSnapshots
     * @param configName Snapperの設定名
     * @param numbers 削除するスナップショット番号の一覧
     * @return 全ての番号を削除できたものとして、要求された番号の一覧をそのまま返す
     * @note 引数と戻り値の型は、本物のDeleteSnapshotsと同じD-Busの署名 (引数はsとau、戻り値はau) にする
     *       実際には削除せず、呼び出しを記録するだけである
     */
    QList<uint> DeleteSnapshots(const QString &configName, const QList<uint> &numbers)
    {
        QMutexLocker locker(&m_mutex);
        QStringList parts;
        for (uint number : numbers) {
            parts.append(QString::number(number));
        }
        m_calls.append(QStringLiteral("DeleteSnapshots:%1:%2").arg(configName, parts.join(QLatin1Char(','))));
        return numbers;
    }

    /**
     * @brief SnapshotListModel::refreshが行う一覧の取得に応答する偽のListSnapshots
     * @param configName Snapperの設定名
     * @return スナップショットが1つもないことを表す空文字列 (CSV形式の一覧が空)
     * @note 取得を記録し、どのconfigの一覧を読み込んだかをテストから確認できるようにする
     *       削除の呼び出しの記録とは分けて、削除の確認を一覧の取得が妨げないようにする
     */
    QString ListSnapshots(const QString &configName)
    {
        QMutexLocker locker(&m_mutex);
        m_listedConfigs.append(configName);
        return QString();
    }

    /**
     * @brief ListSnapshotsで一覧を求められたconfigの記録を返す
     * @return 呼び出された順のconfig名の一覧
     */
    QStringList listedConfigs() const
    {
        QMutexLocker locker(&m_mutex);
        return m_listedConfigs;
    }

private:
    mutable QMutex m_mutex;                               // 記録と応答の設定を2つのスレッドから読み書きするための排他
    QStringList m_calls;                                  // 受け取った削除の呼び出しの記録
    QStringList m_listedConfigs;                          // ListSnapshotsで一覧を求められたconfig名の記録
    SingleReply m_singleReply = SingleReply::ReturnTrue;  // 偽のDeleteSnapshotが返す応答の種類
};

/**
 * @class TestSnapshotDelete
 * @brief SnapshotListModel::deleteSnapshotsが選ぶD-Busメソッドと、削除結果のシグナルによる通知を検証する
 *
 * m_busProcess : テスト専用のdbus-daemonのプロセスであり、cleanupTestCaseで終了させる
 * m_fake : m_fakeThreadで動かす偽サービスであり、m_fakeThreadの終了時にdeleteLaterで削除する
 */
class TestSnapshotDelete : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void singleSelectionUsesDeleteSnapshot();
    void duplicatedSingleNumberUsesDeleteSnapshot();
    void singleDeleteReturningFalseIsReportedAsFailure();
    void singleDeleteErrorIsReported();
    void multipleSelectionUsesDeleteSnapshots();
    /** @brief 一覧を取得し終える前の削除要求が、D-Busを呼ばずに失敗として通知されることを確認する */
    void deletionIsRefusedBeforeListIsLoaded();
    /** @brief configを切り替えた後、前のconfigの一覧に基づく削除要求が拒否されることを確認する */
    void deletionIsRefusedAfterConfigSwitch();

private:
    void loadList(SnapshotListModel &model);

    QTemporaryDir m_busDir;            // テスト専用のdbus-daemonのソケットを置く一時ディレクトリ
    QProcess m_busProcess;             // テスト専用のdbus-daemonのプロセス
    QThread m_fakeThread;              // 偽サービスを動かすスレッド
    FakeOperations *m_fake = nullptr;  // 偽のcom.presire.qsnapper.Operationsサービス
};

/**
 * @brief テスト専用のdbus-daemonを起動し、偽サービスをそのバスへ登録する
 * @note QDBusConnection::systemBusは、最初に使われたときに接続先を決め、以降はその接続を使い続ける
 *       そのため、SnapperServiceがシステムバスへ接続する前に、DBUS_SYSTEM_BUS_ADDRESSをテスト専用のバスへ切り替える
 *       dbus-daemonが見つからない環境では、テストを省略する
 */
void TestSnapshotDelete::initTestCase()
{
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    if (daemon.isEmpty()) {
        QSKIP("dbus-daemon is not installed");
    }
    QVERIFY(m_busDir.isValid());

    // --sessionで起動したdbus-daemonは、どのバス名でも取得を許可する
    // そのため、偽サービスがcom.presire.qsnapper.Operationsを名乗れる
    const QString address = QStringLiteral("unix:path=%1/bus").arg(m_busDir.path());
    m_busProcess.start(daemon, {QStringLiteral("--session"), QStringLiteral("--nofork"),
                                QStringLiteral("--address=%1").arg(address),
                                QStringLiteral("--print-address=1")});
    QVERIFY2(m_busProcess.waitForStarted(5000), "dbus-daemon did not start");
    QVERIFY2(m_busProcess.waitForReadyRead(5000), "dbus-daemon did not print its address");
    const QByteArray printed = m_busProcess.readLine().trimmed();
    QVERIFY(!printed.isEmpty());
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", printed);

    QDBusConnection fakeBus = QDBusConnection::connectToBus(QString::fromUtf8(printed), FAKE_CONNECTION);
    QVERIFY2(fakeBus.isConnected(), qPrintable(fakeBus.lastError().message()));

    m_fake = new FakeOperations;
    m_fake->moveToThread(&m_fakeThread);

    // m_fakeThreadの終了を受け取り、m_fakeThreadの上で偽サービスを削除する
    connect(&m_fakeThread, &QThread::finished, m_fake, &QObject::deleteLater);
    m_fakeThread.start();

    QVERIFY(fakeBus.registerObject(OPERATIONS_PATH, m_fake, QDBusConnection::ExportAllSlots));
    QVERIFY(fakeBus.registerService(OPERATIONS_SERVICE));
}

/**
 * @brief 偽サービスのバスへの接続を切断し、偽サービスのスレッドとテスト専用のdbus-daemonを終了させる
 * @note dbus-daemonが5秒以内に終了しない場合は、強制終了させる
 */
void TestSnapshotDelete::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(FAKE_CONNECTION);
    m_fakeThread.quit();
    m_fakeThread.wait();

    if (m_busProcess.state() != QProcess::NotRunning) {
        m_busProcess.terminate();
        if (!m_busProcess.waitForFinished(5000)) {
            m_busProcess.kill();
            m_busProcess.waitForFinished(5000);
        }
    }
}

/**
 * @brief 各テストの前に、偽サービスの呼び出しの記録と応答の設定を初期状態に戻す
 */
void TestSnapshotDelete::init()
{
    m_fake->reset();
}

/**
 * @brief 一覧を現在のconfigから読み込ませ、取得が終わるまで待つ
 * @param model 一覧を読み込ませるモデル
 * @note SnapshotListModelは、表示中の一覧が現在のconfigから取得し終えたものでない場合に削除を受け付けない
 *       削除の振る舞いを確認するテストは、本関数で前提の状態を作ってから削除を要求する
 */
void TestSnapshotDelete::loadList(SnapshotListModel &model)
{
    model.refresh();
    QTRY_VERIFY(!model.loading());
}

/**
 * @brief 1件だけ削除する場合に、DeleteSnapshotを呼び出すことを確認する
 * @note 成功した場合は、snapshotsDeletionCompletedシグナルで成功数1と失敗数0が通知されることも確認する
 *       設定名のrootは、SnapperServiceの既定の設定名である
 */
void TestSnapshotDelete::singleSelectionUsesDeleteSnapshot()
{
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);
    QSignalSpy failed(&model, &SnapshotListModel::snapshotsDeletionFailed);

    loadList(model);
    model.deleteSnapshots({5});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(completed.at(0).at(0).toInt(), 1);
    QCOMPARE(completed.at(0).at(1).toInt(), 0);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(m_fake->calls(), QStringList{QStringLiteral("DeleteSnapshot:root:5")});
}

/**
 * @brief 同じ番号が重複して渡された場合に、重複を除いた件数で判定し、DeleteSnapshotを呼び出すことを確認する
 */
void TestSnapshotDelete::duplicatedSingleNumberUsesDeleteSnapshot()
{
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);

    loadList(model);
    model.deleteSnapshots({6, 6});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(m_fake->calls(), QStringList{QStringLiteral("DeleteSnapshot:root:6")});
}

/**
 * @brief DeleteSnapshotがエラー応答ではなくfalseを返した場合に、失敗として扱うことを確認する
 * @note snapshotsDeletionCompletedシグナルで、成功数0と失敗数1が通知されることを確認する
 */
void TestSnapshotDelete::singleDeleteReturningFalseIsReportedAsFailure()
{
    m_fake->setSingleReply(FakeOperations::SingleReply::ReturnFalse);
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);

    loadList(model);
    model.deleteSnapshots({7});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(completed.at(0).at(0).toInt(), 0);
    QCOMPARE(completed.at(0).at(1).toInt(), 1);
}

/**
 * @brief DeleteSnapshotがエラー応答を返した場合に、エラーの内容がsnapshotsDeletionFailedシグナルで通知されることを確認する
 * @note GUIは、snapshotsDeletionFailedシグナルで受け取ったエラーの内容をエラーダイアログに表示する
 */
void TestSnapshotDelete::singleDeleteErrorIsReported()
{
    m_fake->setSingleReply(FakeOperations::SingleReply::ReturnError);
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);
    QSignalSpy failed(&model, &SnapshotListModel::snapshotsDeletionFailed);

    loadList(model);
    model.deleteSnapshots({8});

    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("Snapshot not found"));
    QCOMPARE(completed.count(), 0);
}

/**
 * @brief 複数件を削除する場合に、重複を除いた番号でDeleteSnapshotsを1回だけ呼び出すことを確認する
 * @note snapshotsDeletionCompletedシグナルで、成功数2と失敗数0が通知されることも確認する
 */
void TestSnapshotDelete::multipleSelectionUsesDeleteSnapshots()
{
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);

    loadList(model);
    model.deleteSnapshots({3, 4, 3});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(completed.at(0).at(0).toInt(), 2);
    QCOMPARE(completed.at(0).at(1).toInt(), 0);
    QCOMPARE(m_fake->calls(), QStringList{QStringLiteral("DeleteSnapshots:root:3,4")});
}

/**
 * @brief 一覧を取得し終える前の削除要求を、D-Busを呼び出さずに拒否することを確認する
 * @note 取得前の一覧の番号は、どのconfigのスナップショットを指すか確定していないため、削除してはならない
 *       拒否した場合は、GUIの処理中の表示を閉じられるよう、snapshotsDeletionFailedシグナルで知らせる
 */
void TestSnapshotDelete::deletionIsRefusedBeforeListIsLoaded()
{
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);
    QSignalSpy failed(&model, &SnapshotListModel::snapshotsDeletionFailed);

    model.deleteSnapshots({5});
    model.deleteSnapshots({3, 4});

    QCOMPARE(failed.count(), 2);
    QCOMPARE(completed.count(), 0);
    QVERIFY(m_fake->calls().isEmpty());
}

/**
 * @brief configを切り替えた後は、前のconfigの一覧に基づく削除要求を拒否することを確認する
 * @note 前のconfigの行に表示されていた番号で削除すると、切り替え先のconfigにある同じ番号のスナップショットを削除してしまう
 *       一覧を切り替え先のconfigから読み込み直した後は、切り替え先のconfigに対して削除できることも確認する
 *       SnapperServiceは全テストで共有するため、最後に既定の設定名rootへ戻す
 */
void TestSnapshotDelete::deletionIsRefusedAfterConfigSwitch()
{
    SnapperService *service = SnapperService::instance();
    SnapshotListModel model;
    loadList(model);
    QSignalSpy failed(&model, &SnapshotListModel::snapshotsDeletionFailed);
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);

    // 一覧はrootから取得したままなので、home向けの番号9として扱えず拒否される
    service->setCurrentConfig(QStringLiteral("home"));
    model.deleteSnapshots({9});

    QCOMPARE(failed.count(), 1);
    QVERIFY(m_fake->calls().isEmpty());

    // homeの一覧を読み込み直せば、同じ番号9をhomeのスナップショットとして削除できる
    loadList(model);
    model.deleteSnapshots({9});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(m_fake->calls(), QStringList{QStringLiteral("DeleteSnapshot:home:9")});
    // 完了後に走る再読み込みで記録が増えるため、削除前に読み込んだ先頭2件だけを確認する
    QCOMPARE(m_fake->listedConfigs().mid(0, 2), (QStringList{QStringLiteral("root"), QStringLiteral("home")}));

    service->setCurrentConfig(QStringLiteral("root"));
}

QTEST_GUILESS_MAIN(TestSnapshotDelete)
#include "tst_snapshotdelete.moc"
