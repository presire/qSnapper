#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QQuickStyle>
#include <QTranslator>
#include <QLocale>
#include <QLibraryInfo>
#include <QDir>
#include <QDBusMetaType>
#include <QMap>
#include <QDebug>
#include <QQuickWindow>
#include "fssnapshot.h"
#include "snapperservice.h"
#include "snapshotlistmodel.h"
#include "filechangemodel.h"
#include "thememanager.h"
#include "snapshotgroupmodel.h"
#include "singleinstanceguard.h"

int main(int argc, char *argv[])
{
    // Qt GUIアプリケーションの初期化
    QGuiApplication app(argc, argv);

    // アプリケーション識別情報の設定 (QStandardPaths / QSettings等が参照)
    app.setOrganizationName("Presire");
    app.setOrganizationDomain("https://github.com/presire");
    app.setApplicationName("qSnapper");
    app.setApplicationVersion(QSNAPPER_VERSION);

    // 二重起動防止 (UID単位)
    // 既存インスタンスがある場合はそれを前面化して即終了する
    SingleInstanceGuard instanceGuard;
    if (!instanceGuard.tryAcquire()) {
        qInfo() << "qSnapper is already running. Activating existing instance.";
        return 0;
    }

    // QMap<QString,QString> を D-Bus a{ss} として送受信するためのメタ型登録
    qDBusRegisterMetaType<QMap<QString, QString>>();

    // アプリケーションアイコンの設定
    QIcon appIcon;
    appIcon.addFile(":/QSnapper/icons/qSnapper@64.png",  QSize(64, 64));
    appIcon.addFile(":/QSnapper/icons/qSnapper@128.png", QSize(128, 128));
    appIcon.addFile(":/QSnapper/icons/qSnapper@256.png", QSize(256, 256));
    app.setWindowIcon(appIcon);

    // 翻訳の設定
    QTranslator translator;
    QString locale = QLocale::system().name();

    // 複数のパスから翻訳ファイルを探す
    QStringList translationPaths;
    translationPaths << ":/i18n"                                    // リソース埋め込みパス (qt_add_translations使用時)
                     << QCoreApplication::applicationDirPath() +    // 相対インストールパス
                        "/../share/qsnapper/translations"
                     << "/usr/share/qsnapper/translations";         // 絶対インストールパス

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
    if (qtTranslator.load(QStringLiteral("qt_%1").arg(locale), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(&qtTranslator);
    }

    // QML型登録 (C++クラスをQMLから利用可能にする)
    qmlRegisterType<FsSnapshot>("QSnapper", 1, 0, "FsSnapshot");
    qmlRegisterType<SnapshotListModel>("QSnapper", 1, 0, "SnapshotListModel");
    qmlRegisterType<FileChangeModel>("QSnapper", 1, 0, "FileChangeModel");
    qmlRegisterType<SnapshotGroupModel>("QSnapper", 1, 0, "SnapshotGroupModel");
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

    // Fusionスタイルの強制
    // KDE Plasma 6のorg.kde.desktopスタイルがQMLのPaletteを無視する問題を回避する
    QQuickStyle::setStyle("Fusion");

    // QMLエンジンの生成とルートコンポーネントのロード
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

    // 別プロセスから二重起動要求を受けた場合にウィンドウを前面化
    QObject::connect(&instanceGuard, &SingleInstanceGuard::raiseRequested,
                     &app, [&engine]() {
        const QList<QObject *> roots = engine.rootObjects();
        if (roots.isEmpty()) {
            return;
        }
        if (auto *window = qobject_cast<QQuickWindow *>(roots.first())) {
            if (window->visibility() == QWindow::Minimized) {
                window->showNormal();
            }
            else {
                window->show();
            }
            window->raise();
            window->requestActivate();
        }
    });

    // D-Busサービスはアイドルタイマ (5分) により自律的に終了する
    // 以前は、aboutToQuit時にQuit() D-Busメソッドを呼んでいたが、
    // 無認証DoSの脆弱性 (SUSE Security Review 2026-04) を回避するため削除した

    return app.exec();
}
