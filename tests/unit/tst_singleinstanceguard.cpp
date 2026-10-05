// tst_singleinstanceguard.cpp
//
// SingleInstanceGuardの単体テスト
// ソケットとlock fileがユーザ専用のruntimeディレクトリに置かれること、
// runtimeディレクトリを使えない場合に共有の/tmpへフォールバックしないことを確認する

#include <QtTest/QtTest>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "singleinstanceguard.h"

/**
 * @brief SingleInstanceGuardの単体テスト
 */
class TestSingleInstanceGuard : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void secondInstanceRaisesPrimaryInRuntimeDirectory();
    void staleSocketIsReplaced();
    void guardIsDisabledWithoutPrivateRuntimeDirectory();

private:
    QByteArray m_savedRuntimeDir;
    bool m_hadRuntimeDir = false;
    QByteArray m_savedTmpDir;
    bool m_hadTmpDir = false;
};

/**
 * @brief 各テストの前にXDG_RUNTIME_DIRを退避する
 */
void TestSingleInstanceGuard::init()
{
    m_hadRuntimeDir = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
    m_savedRuntimeDir = qgetenv("XDG_RUNTIME_DIR");
    m_hadTmpDir = qEnvironmentVariableIsSet("TMPDIR");
    m_savedTmpDir = qgetenv("TMPDIR");
}

/**
 * @brief 各テストの後にXDG_RUNTIME_DIRを元に戻す
 */
void TestSingleInstanceGuard::cleanup()
{
    if (m_hadRuntimeDir) {
        qputenv("XDG_RUNTIME_DIR", m_savedRuntimeDir);
    }
    else {
        qunsetenv("XDG_RUNTIME_DIR");
    }
    if (m_hadTmpDir) {
        qputenv("TMPDIR", m_savedTmpDir);
    }
    else {
        qunsetenv("TMPDIR");
    }
}

/**
 * @brief 2つ目のインスタンスはプライマリへraise要求を送って終了して、ソケットはruntimeディレクトリ配下にあることを確認する
 */
void TestSingleInstanceGuard::secondInstanceRaisesPrimaryInRuntimeDirectory()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    QVERIFY(QFile::setPermissions(runtimeDir.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtimeDir.path()));

    SingleInstanceGuard primary;
    QSignalSpy raiseSpy(&primary, &SingleInstanceGuard::raiseRequested);
    QVERIFY(primary.tryAcquire());
    QVERIFY(QFileInfo(runtimeDir.path() + QStringLiteral("/qsnapper.socket")).exists());
    QVERIFY(QFileInfo::exists(runtimeDir.path() + QStringLiteral("/qsnapper.lock")));

    SingleInstanceGuard secondary;
    QVERIFY(!secondary.tryAcquire());
    QTRY_COMPARE(raiseSpy.count(), 1);
}

/**
 * @brief 前回の異常終了で残ったソケットは、lockを取得した上で置き換えられることを確認する
 */
void TestSingleInstanceGuard::staleSocketIsReplaced()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    QVERIFY(QFile::setPermissions(runtimeDir.path(),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtimeDir.path()));

    // 待ち受けていないソケットファイルを残す
    const QByteArray socketPath = QFile::encodeName(runtimeDir.path() + QStringLiteral("/qsnapper.socket"));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    QVERIFY(fd >= 0);
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    QVERIFY(socketPath.size() < int(sizeof(address.sun_path)));
    ::memcpy(address.sun_path, socketPath.constData(), size_t(socketPath.size()));
    QCOMPARE(::bind(fd, reinterpret_cast<const struct sockaddr *>(&address), sizeof(address)), 0);
    ::close(fd);
    QVERIFY(QFileInfo::exists(runtimeDir.path() + QStringLiteral("/qsnapper.socket")));

    SingleInstanceGuard primary;
    QSignalSpy raiseSpy(&primary, &SingleInstanceGuard::raiseRequested);
    QVERIFY(primary.tryAcquire());

    SingleInstanceGuard secondary;
    QVERIFY(!secondary.tryAcquire());
    QTRY_COMPARE(raiseSpy.count(), 1);
}

/**
 * @brief 自分が所有していないruntimeディレクトリを指定された場合、2重起動防止を無効にして起動を許容することを確認する
 */
void TestSingleInstanceGuard::guardIsDisabledWithoutPrivateRuntimeDirectory()
{
    if (::geteuid() == 0) {
        QSKIP("root owns the directory used as an untrusted runtime directory");
    }

    // QStandardPathsは、XDG_RUNTIME_DIRを採用できない場合、"<TMPDIR>/runtime-<ユーザ名>"へフォールバックすることがある
    // フォールバック先をディレクトリ以外にして、そちらも採用できない状態を作る
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const struct passwd *user = ::getpwuid(::geteuid());
    QVERIFY(user != nullptr);
    QFile blocker(tmpDir.path() + QStringLiteral("/runtime-") + QString::fromLocal8Bit(user->pw_name));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    qputenv("TMPDIR", QFile::encodeName(tmpDir.path()));

    // ルートディレクトリはrootが所有するため、QStandardPathsはruntimeディレクトリとして採用しない
    qputenv("XDG_RUNTIME_DIR", QByteArrayLiteral("/"));
    if (!QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation).isEmpty()) {
        QSKIP("this Qt version found another private runtime directory");
    }

    SingleInstanceGuard first;
    SingleInstanceGuard second;
    QVERIFY(first.tryAcquire());
    QVERIFY(second.tryAcquire());

    // 共有の一時ディレクトリにソケットやlockを作っていない
    QCOMPARE(QDir(tmpDir.path()).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).size(), 1);
}

QTEST_GUILESS_MAIN(TestSingleInstanceGuard)
#include "tst_singleinstanceguard.moc"
