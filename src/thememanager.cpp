#include "thememanager.h"
#include <QSettings>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDebug>

ThemeManager* ThemeManager::s_instance = nullptr;

/**
 * @brief ThemeManagerのコンストラクタ
 *
 * 設定を読み込み、システムテーマ変更の監視を開始します。
 *
 * @param parent 親オブジェクト
 */
ThemeManager::ThemeManager(QObject *parent)
    : QObject(parent)
    , m_themeMode(ThemeMode::Light)
    , m_isDark(false)
{
    // 設定を読み込む
    loadSettings();

    // システムテーマ変更の監視
    auto styleHints = QGuiApplication::styleHints();
    connect(styleHints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme colorScheme) {
        Q_UNUSED(colorScheme)
        if (m_themeMode == ThemeMode::System) {
            updateTheme();
        }
    });

    // 初期テーマを適用
    updateTheme();
}

/**
 * @brief ThemeManagerのデストラクタ
 */
ThemeManager::~ThemeManager()
{
}

/**
 * @brief シングルトンインスタンスを取得
 *
 * @return ThemeManagerのインスタンス
 */
ThemeManager* ThemeManager::instance()
{
    if (!s_instance) {
        s_instance = new ThemeManager();
    }
    return s_instance;
}

/**
 * @brief 設定ファイルから設定を読み込む
 */
void ThemeManager::loadSettings()
{
    QSettings settings("Presire", "qSnapper");
    int themeModeInt = settings.value("theme/mode", static_cast<int>(ThemeMode::Light)).toInt();
    m_themeMode = static_cast<ThemeMode>(themeModeInt);
}

/**
 * @brief 設定ファイルに設定を保存
 */
void ThemeManager::saveSettings()
{
    QSettings settings("Presire", "qSnapper");
    settings.setValue("theme/mode", static_cast<int>(m_themeMode));
}

/**
 * @brief テーマモードを設定
 *
 * @param mode 新しいテーマモード
 */
void ThemeManager::setThemeMode(ThemeMode mode)
{
    if (m_themeMode != mode) {
        m_themeMode = mode;
        saveSettings();
        updateTheme();
        emit themeModeChanged();
    }
}

/**
 * @brief テーマを更新
 */
void ThemeManager::updateTheme()
{
    bool wasDark = m_isDark;

    if (m_themeMode == ThemeMode::System) {
        detectSystemTheme();
    } else {
        m_isDark = (m_themeMode == ThemeMode::Dark);
    }

    if (wasDark != m_isDark) {
        emit isDarkChanged();
    }

    emit themeChanged();
}

/**
 * @brief システムテーマを検出
 */
void ThemeManager::detectSystemTheme()
{
    auto styleHints = QGuiApplication::styleHints();
    m_isDark = (styleHints->colorScheme() == Qt::ColorScheme::Dark);
}

/**
 * @brief ライト/ダークモードに応じた色を取得
 *
 * @param lightColor ライトモードの色
 * @param darkColor ダークモードの色
 * @return 現在のモードに応じた色
 */
QColor ThemeManager::getColor(const QString &lightColor, const QString &darkColor) const
{
    return QColor(m_isDark ? darkColor : lightColor);
}

// スナップショットタイプの色

QColor ThemeManager::snapshotTypeSingle() const
{
    return getColor(LightColors::snapshotSingle, DarkColors::snapshotSingle);
}

QColor ThemeManager::snapshotTypePre() const
{
    return getColor(LightColors::snapshotPre, DarkColors::snapshotPre);
}

QColor ThemeManager::snapshotTypePost() const
{
    return getColor(LightColors::snapshotPost, DarkColors::snapshotPost);
}

QColor ThemeManager::snapshotTypeDefault() const
{
    return getColor(LightColors::snapshotDefault, DarkColors::snapshotDefault);
}

// ファイル変更タイプの色

QColor ThemeManager::fileChangeCreated() const
{
    return getColor(LightColors::fileCreated, DarkColors::fileCreated);
}

QColor ThemeManager::fileChangeModified() const
{
    return getColor(LightColors::fileModified, DarkColors::fileModified);
}

QColor ThemeManager::fileChangeDeleted() const
{
    return getColor(LightColors::fileDeleted, DarkColors::fileDeleted);
}

QColor ThemeManager::fileChangeTypeChanged() const
{
    return getColor(LightColors::fileTypeChanged, DarkColors::fileTypeChanged);
}

// 状態色

QColor ThemeManager::warningColor() const
{
    return getColor(LightColors::warning, DarkColors::warning);
}

QColor ThemeManager::errorColor() const
{
    return getColor(LightColors::error, DarkColors::error);
}

QColor ThemeManager::importantColor() const
{
    return getColor(LightColors::important, DarkColors::important);
}

QColor ThemeManager::successColor() const
{
    return getColor(LightColors::success, DarkColors::success);
}
