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
 * SnapshotListModel::refreshは、ListSnapshotsを同期で呼び出し、応答が届くまでテストのスレッドを止める
 * 本オブジェクトがテストと同じスレッドにあると応答を返せなくなるため、本オブジェクトは別のスレッドで動かす
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
     * @brief 削除後にSnapshotListModel::refreshが行う一覧の再読み込みに応答する偽のListSnapshots
     * @param configName Snapperの設定名 (本テストでは使用しない)
     * @return スナップショットが1つもないことを表す空文字列 (CSV形式の一覧が空)
     * @note 本メソッドがないと、refreshの同期呼び出しがタイムアウトするまでテストが止まるため、空の一覧を返す
     */
    QString ListSnapshots(const QString &configName)
    {
        Q_UNUSED(configName);
        return QString();
    }

private:
    mutable QMutex m_mutex;                               // m_callsとm_singleReplyを2つのスレッドから読み書きするための排他
    QStringList m_calls;                                  // 受け取った削除の呼び出しの記録
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

private:
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
 * @brief 1件だけ削除する場合に、DeleteSnapshotを呼び出すことを確認する
 * @note 成功した場合は、snapshotsDeletionCompletedシグナルで成功数1と失敗数0が通知されることも確認する
 *       設定名のrootは、SnapperServiceの既定の設定名である
 */
void TestSnapshotDelete::singleSelectionUsesDeleteSnapshot()
{
    SnapshotListModel model;
    QSignalSpy completed(&model, &SnapshotListModel::snapshotsDeletionCompleted);
    QSignalSpy failed(&model, &SnapshotListModel::snapshotsDeletionFailed);

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

    model.deleteSnapshots({3, 4, 3});

    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(completed.at(0).at(0).toInt(), 2);
    QCOMPARE(completed.at(0).at(1).toInt(), 0);
    QCOMPARE(m_fake->calls(), QStringList{QStringLiteral("DeleteSnapshots:root:3,4")});
}

QTEST_GUILESS_MAIN(TestSnapshotDelete)
#include "tst_snapshotdelete.moc"
