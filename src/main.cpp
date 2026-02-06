#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QTranslator>
#include <QLocale>
#include <QLibraryInfo>
#include <QDebug>
#include <QDir>
#include <QDBusInterface>
#include <QDBusConnection>
#include "fssnapshot.h"
#include "snapperservice.h"
#include "snapshotlistmodel.h"
#include "filechangemodel.h"
#include "thememanager.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    app.setOrganizationName("Presire");
    app.setOrganizationDomain("https://github.com/presire");
    app.setApplicationName("qSnapper");
    app.setApplicationVersion("1.0.1");
    app.setWindowIcon(QIcon(":QSnapper/icons/qSnapper.png"));

    // 翻訳システムの設定
    QTranslator translator;
    QString locale = QLocale::system().name();

    // 複数のパスから翻訳ファイルを探す
    QStringList translationPaths;
    translationPaths << ":/i18n"                                                    // リソース埋め込みパス (qt_add_translations使用時)
                     << QCoreApplication::applicationDirPath() + "/../share/qsnapper/translations"  // 相対インストールパス
                     << "/usr/share/qsnapper/translations";                         // 絶対インストールパス

    bool translationLoaded = false;
    for (const QString &path : std::as_const(translationPaths)) {
        QString translationFile = QString("qsnapper_%1").arg(locale);
        if (translator.load(translationFile, path)) {
            app.installTranslator(&translator);
            translationLoaded = true;
            break;
        }
    }

    if (!translationLoaded) {
        qWarning() << "Translation not found for locale:" << locale << "- using default (English)";
    }

    // Qt標準ダイアログの翻訳
    QTranslator qtTranslator;
    if (qtTranslator.load(QStringLiteral("qt_%1").arg(locale),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(&qtTranslator);
    }

    qmlRegisterType<FsSnapshot>("QSnapper", 1, 0, "FsSnapshot");
    qmlRegisterType<SnapshotListModel>("QSnapper", 1, 0, "SnapshotListModel");
    qmlRegisterType<FileChangeModel>("QSnapper", 1, 0, "FileChangeModel");
    qmlRegisterSingletonInstance("QSnapper", 1, 0, "SnapperService", SnapperService::instance());
    qmlRegisterSingletonInstance("QSnapper", 1, 0, "ThemeManager", ThemeManager::instance());

    qmlRegisterUncreatableMetaObject(
        FsSnapshot::staticMetaObject,
        "QSnapper",
        1, 0,
        "SnapshotType",
        "SnapshotType is an enum"
    );

    qmlRegisterUncreatableMetaObject(
        FsSnapshot::staticMetaObject,
        "QSnapper",
        1, 0,
        "CleanupAlgorithm",
        "CleanupAlgorithm is an enum"
    );

    QQmlApplicationEngine engine;

    const QUrl url(QStringLiteral("qrc:/qt/qml/QSnapper/qml/Main.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection
    );

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // アプリケーション終了時にD-Busサービスも終了させる
    QObject::connect(&app, &QGuiApplication::aboutToQuit, []() {
        QDBusInterface iface(
            "com.presire.qsnapper.Operations",
            "/com/presire/qsnapper/Operations",
            "com.presire.qsnapper.Operations",
            QDBusConnection::systemBus()
        );
        if (iface.isValid()) {
            iface.call(QDBus::NoBlock, "Quit");
        }
    });

    return app.exec();
}
