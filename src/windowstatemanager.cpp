#include <QSettings>
#include <QGuiApplication>
#include <QScreen>
#include <QQuickWindow>
#include <QWindow>
#include <QCoreApplication>
#include "windowstatemanager.h"

namespace {
    /// @brief 幅の既定値 (初回起動時)
    constexpr int kDefaultWidth = 1280;

    /// @brief 高さの既定値 (初回起動時)
    constexpr int kDefaultHeight = 900;
}

/**
 * @brief WindowStateManagerのコンストラクタ
 *
 * @param parent 親オブジェクト
 */
WindowStateManager::WindowStateManager(QObject *parent)
    : QObject(parent)
    , m_restored(false)
    , m_normalWidth(kDefaultWidth)
    , m_normalHeight(kDefaultHeight)
{
}

/**
 * @brief WindowStateManagerのデストラクタ
 */
WindowStateManager::~WindowStateManager()
{
}

/**
 * @brief 保存済みのウィンドウ状態を復元する
 *
 * 設定を読み込み、画面の利用可能領域に収まるようクランプしてからウィンドウへ適用する
 * 適用後は通常状態のサイズ追跡と、終了時の保存フックの接続を開始する
 * 二重接続を防ぐため、2回目以降の呼び出しは無視する
 *
 * @param window 対象のウィンドウ
 */
void WindowStateManager::restore(QQuickWindow *window)
{
    if (!window) {
        return;
    }

    // 二重接続防止
    if (m_restored) {
        return;
    }
    m_restored = true;

    QSettings settings("Presire", "qSnapper");
    const int savedWidth = settings.value("window/width", kDefaultWidth).toInt();
    const int savedHeight = settings.value("window/height", kDefaultHeight).toInt();
    const bool maximized = settings.value("window/maximized", false).toBool();

    // 画面の利用可能領域に収まるようクランプする
    const int width = clampWidth(window, savedWidth);
    const int height = clampHeight(window, savedHeight);

    // 通常状態のサイズを記録する (最大化中でも復元できるよう保持する)
    m_normalWidth = width;
    m_normalHeight = height;

    // 通常サイズを先にウィンドウへ適用する
    // 最大化時もこのサイズが最大化解除後の復帰先となるため、必ず適用してから最大化する
    window->resize(width, height);

    if (maximized) {
        window->showMaximized();
    }
    else {
        window->showNormal();
    }

    // 通常状態のときだけサイズ変更を追跡する
    // 最大化・最小化・全画面ではこのハンドラを素通りさせることで、通常サイズを保持する
    connect(window, &QQuickWindow::widthChanged, this, [this, window]() {
        if (window->visibility() == QWindow::Windowed) {
            m_normalWidth = window->width();
        }
    });
    connect(window, &QQuickWindow::heightChanged, this, [this, window]() {
        if (window->visibility() == QWindow::Windowed) {
            m_normalHeight = window->height();
        }
    });

    // 終了時の保存フック (double saveは許容する)
    connect(window, &QQuickWindow::closing, this, [this, window](QQuickCloseEvent *) {
        save(window);
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this, window]() {
        save(window);
    });
}

/**
 * @brief 現在のウィンドウ状態を保存する
 *
 * 最大化中は追跡している通常サイズを、通常状態では現在のサイズを保存する
 * 冪等であり、複数回呼び出しても安全
 *
 * @param window 対象のウィンドウ
 */
void WindowStateManager::save(QQuickWindow *window)
{
    if (!window) {
        return;
    }

    QSettings settings("Presire", "qSnapper");

    const bool maximized = window->windowStates().testFlag(Qt::WindowMaximized)
        || window->visibility() == QWindow::Maximized;

    if (maximized) {
        // 最大化中は追跡している通常サイズを保存する
        settings.setValue("window/width", m_normalWidth);
        settings.setValue("window/height", m_normalHeight);
        settings.setValue("window/maximized", true);
    }
    else {
        settings.setValue("window/width", window->width());
        settings.setValue("window/height", window->height());
        settings.setValue("window/maximized", false);
    }

    settings.sync();
}

/**
 * @brief 幅をウィンドウの最小幅と画面の利用可能幅の範囲にクランプする
 *
 * @param window 対象のウィンドウ
 * @param requestedWidth 要求された幅
 * @return クランプ後の幅
 */
int WindowStateManager::clampWidth(QQuickWindow *window, int requestedWidth) const
{
    QScreen *screen = window->screen();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    int availableWidth = requestedWidth;
    if (screen) {
        availableWidth = screen->availableGeometry().width();
    }

    return qMax(window->minimumWidth(), qMin(requestedWidth, availableWidth));
}

/**
 * @brief 高さをウィンドウの最小高と画面の利用可能高の範囲にクランプする
 *
 * @param window 対象のウィンドウ
 * @param requestedHeight 要求された高さ
 * @return クランプ後の高さ
 */
int WindowStateManager::clampHeight(QQuickWindow *window, int requestedHeight) const
{
    QScreen *screen = window->screen();
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    int availableHeight = requestedHeight;
    if (screen) {
        availableHeight = screen->availableGeometry().height();
    }

    return qMax(window->minimumHeight(), qMin(requestedHeight, availableHeight));
}
