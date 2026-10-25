/**
 * @file thememanager.cpp
 * @brief テーマモードの管理とテーマ依存色の提供の実装
 */

#include <QSettings>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDebug>
#include "thememanager.h"

ThemeManager* ThemeManager::s_instance = nullptr;

/**
 * @brief ThemeManagerのコンストラクタ
 *
 * 設定を読み込み、システムテーマ変更の監視を開始する
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

/**
 * @brief 単発スナップショットの色を取得する
 * @return 単発スナップショットの色
 */
QColor ThemeManager::snapshotTypeSingle() const
{
    return getColor(LightColors::snapshotSingle, DarkColors::snapshotSingle);
}

/**
 * @brief Preスナップショットの色を取得する
 * @return Preスナップショットの色
 */
QColor ThemeManager::snapshotTypePre() const
{
    return getColor(LightColors::snapshotPre, DarkColors::snapshotPre);
}

/**
 * @brief Postスナップショットの色を取得する
 * @return Postスナップショットの色
 */
QColor ThemeManager::snapshotTypePost() const
{
    return getColor(LightColors::snapshotPost, DarkColors::snapshotPost);
}

/**
 * @brief 既定スナップショットの色を取得する
 * @return 既定スナップショットの色
 */
QColor ThemeManager::snapshotTypeDefault() const
{
    return getColor(LightColors::snapshotDefault, DarkColors::snapshotDefault);
}

// ファイル変更タイプの色

/**
 * @brief 新規作成ファイルの色を取得する
 * @return 新規作成ファイルの色
 */
QColor ThemeManager::fileChangeCreated() const
{
    return getColor(LightColors::fileCreated, DarkColors::fileCreated);
}

/**
 * @brief 変更ファイルの色を取得する
 * @return 変更ファイルの色
 */
QColor ThemeManager::fileChangeModified() const
{
    return getColor(LightColors::fileModified, DarkColors::fileModified);
}

/**
 * @brief 削除ファイルの色を取得する
 * @return 削除ファイルの色
 */
QColor ThemeManager::fileChangeDeleted() const
{
    return getColor(LightColors::fileDeleted, DarkColors::fileDeleted);
}

/**
 * @brief タイプ変更ファイルの色を取得する
 * @return タイプ変更ファイルの色
 */
QColor ThemeManager::fileChangeTypeChanged() const
{
    return getColor(LightColors::fileTypeChanged, DarkColors::fileTypeChanged);
}

// 状態色

/**
 * @brief 警告色を取得する
 * @return 警告色
 */
QColor ThemeManager::warningColor() const
{
    return getColor(LightColors::warning, DarkColors::warning);
}

/**
 * @brief エラー色を取得する
 * @return エラー色
 */
QColor ThemeManager::errorColor() const
{
    return getColor(LightColors::error, DarkColors::error);
}

/**
 * @brief 重要色を取得する
 * @return 重要色
 */
QColor ThemeManager::importantColor() const
{
    return getColor(LightColors::important, DarkColors::important);
}

/**
 * @brief 成功色を取得する
 * @return 成功色
 */
QColor ThemeManager::successColor() const
{
    return getColor(LightColors::success, DarkColors::success);
}

// ダイアログ枠色

/**
 * @brief ダイアログ枠の色を取得する
 * @return ダイアログ枠の色
 */
QColor ThemeManager::dialogBorderColor() const
{
    return getColor(LightColors::dialogBorder, DarkColors::dialogBorder);
}

// グループボックスの色

/**
 * @brief グループボックス内側の背景の色を取得する
 * @return グループボックス内側の背景の色
 */
QColor ThemeManager::groupBoxBackground() const
{
    return getColor(LightColors::groupBoxBackground, DarkColors::groupBoxBackground);
}

/**
 * @brief グループボックス枠の色を取得する
 * @return グループボックス枠の色
 */
QColor ThemeManager::groupBoxBorder() const
{
    return getColor(LightColors::groupBoxBorder, DarkColors::groupBoxBorder);
}

// パレットの色
// Main.qmlのApplicationWindow.paletteへ、Light/Darkの切り替え済みの値を渡すために提供する
// 色の定義をC++側へ集約し、QML側にLight用とDark用のPaletteを重複して持たないようにするため

/**
 * @brief ウィンドウ背景の色を取得する
 * @return ウィンドウ背景の色
 */
QColor ThemeManager::paletteWindow() const
{
    return getColor(LightColors::paletteWindow, DarkColors::paletteWindow);
}

/**
 * @brief ウィンドウ文字の色を取得する
 * @return ウィンドウ文字の色
 */
QColor ThemeManager::paletteWindowText() const
{
    return getColor(LightColors::paletteWindowText, DarkColors::paletteWindowText);
}

/**
 * @brief 入力欄などの背景の色を取得する
 * @return 入力欄などの背景の色
 */
QColor ThemeManager::paletteBase() const
{
    return getColor(LightColors::paletteBase, DarkColors::paletteBase);
}

/**
 * @brief 入力欄などの文字の色を取得する
 * @return 入力欄などの文字の色
 */
QColor ThemeManager::paletteText() const
{
    return getColor(LightColors::paletteText, DarkColors::paletteText);
}

/**
 * @brief ボタン背景の色を取得する
 * @return ボタン背景の色
 */
QColor ThemeManager::paletteButton() const
{
    return getColor(LightColors::paletteButton, DarkColors::paletteButton);
}

/**
 * @brief ボタン文字の色を取得する
 * @return ボタン文字の色
 */
QColor ThemeManager::paletteButtonText() const
{
    return getColor(LightColors::paletteButtonText, DarkColors::paletteButtonText);
}

/**
 * @brief 選択項目の背景の色を取得する
 * @return 選択項目の背景の色
 */
QColor ThemeManager::paletteHighlight() const
{
    return getColor(LightColors::paletteHighlight, DarkColors::paletteHighlight);
}

/**
 * @brief 選択項目の文字の色を取得する
 * @return 選択項目の文字の色
 */
QColor ThemeManager::paletteHighlightedText() const
{
    return getColor(LightColors::paletteHighlightedText, DarkColors::paletteHighlightedText);
}

/**
 * @brief 立体表現の明るい縁の色を取得する
 * @return 立体表現の明るい縁の色
 */
QColor ThemeManager::paletteLight() const
{
    return getColor(LightColors::paletteLight, DarkColors::paletteLight);
}

/**
 * @brief 立体表現のやや明るい縁の色を取得する
 * @return 立体表現のやや明るい縁の色
 */
QColor ThemeManager::paletteMidlight() const
{
    return getColor(LightColors::paletteMidlight, DarkColors::paletteMidlight);
}

/**
 * @brief 立体表現の中間色の色を取得する
 * @return 立体表現の中間色の色
 */
QColor ThemeManager::paletteMid() const
{
    return getColor(LightColors::paletteMid, DarkColors::paletteMid);
}

/**
 * @brief 立体表現の暗い縁の色を取得する
 * @return 立体表現の暗い縁の色
 */
QColor ThemeManager::paletteDark() const
{
    return getColor(LightColors::paletteDark, DarkColors::paletteDark);
}

/**
 * @brief 影の色を取得する
 * @return 影の色
 */
QColor ThemeManager::paletteShadow() const
{
    return getColor(LightColors::paletteShadow, DarkColors::paletteShadow);
}

/**
 * @brief 交互行の背景の色を取得する
 * @return 交互行の背景の色
 */
QColor ThemeManager::paletteAlternateBase() const
{
    return getColor(LightColors::paletteAlternateBase, DarkColors::paletteAlternateBase);
}

/**
 * @brief ツールチップ背景の色を取得する
 * @return ツールチップ背景の色
 */
QColor ThemeManager::paletteToolTipBase() const
{
    return getColor(LightColors::paletteToolTipBase, DarkColors::paletteToolTipBase);
}

/**
 * @brief ツールチップ文字の色を取得する
 * @return ツールチップ文字の色
 */
QColor ThemeManager::paletteToolTipText() const
{
    return getColor(LightColors::paletteToolTipText, DarkColors::paletteToolTipText);
}

/**
 * @brief プレースホルダ文字の色を取得する
 * @return プレースホルダ文字の色
 */
QColor ThemeManager::palettePlaceholderText() const
{
    return getColor(LightColors::palettePlaceholderText, DarkColors::palettePlaceholderText);
}

/**
 * @brief リンクの色を取得する
 * @return リンクの色
 */
QColor ThemeManager::paletteLink() const
{
    return getColor(LightColors::paletteLink, DarkColors::paletteLink);
}

/**
 * @brief 訪問済みリンクの色を取得する
 * @return 訪問済みリンクの色
 */
QColor ThemeManager::paletteLinkVisited() const
{
    return getColor(LightColors::paletteLinkVisited, DarkColors::paletteLinkVisited);
}

/**
 * @brief 無効状態のボタン文字の色を取得する
 * @return 無効状態のボタン文字の色
 */
QColor ThemeManager::paletteDisabledButtonText() const
{
    return getColor(LightColors::paletteDisabledButtonText, DarkColors::paletteDisabledButtonText);
}

/**
 * @brief 無効状態のウィンドウ文字の色を取得する
 * @return 無効状態のウィンドウ文字の色
 */
QColor ThemeManager::paletteDisabledWindowText() const
{
    return getColor(LightColors::paletteDisabledWindowText, DarkColors::paletteDisabledWindowText);
}

/**
 * @brief 無効状態の入力欄文字の色を取得する
 * @return 無効状態の入力欄文字の色
 */
QColor ThemeManager::paletteDisabledText() const
{
    return getColor(LightColors::paletteDisabledText, DarkColors::paletteDisabledText);
}

/**
 * @brief 無効状態のボタン背景の色を取得する
 * @return 無効状態のボタン背景の色
 */
QColor ThemeManager::paletteDisabledButton() const
{
    return getColor(LightColors::paletteDisabledButton, DarkColors::paletteDisabledButton);
}

/**
 * @brief 無効状態の選択項目背景の色を取得する
 * @return 無効状態の選択項目背景の色
 */
QColor ThemeManager::paletteDisabledHighlight() const
{
    return getColor(LightColors::paletteDisabledHighlight, DarkColors::paletteDisabledHighlight);
}

/**
 * @brief 無効状態の選択項目文字の色を取得する
 * @return 無効状態の選択項目文字の色
 */
QColor ThemeManager::paletteDisabledHighlightedText() const
{
    return getColor(LightColors::paletteDisabledHighlightedText, DarkColors::paletteDisabledHighlightedText);
}
