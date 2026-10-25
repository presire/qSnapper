#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QPalette>

/**
 * @file thememanager.h
 * @brief アプリケーションのテーマ管理を担うクラス
 */

/**
 * @class ThemeManager
 * @brief アプリケーションのテーマ管理を担うクラス
 *
 * ライト / ダーク / システム連動のテーマモードを管理し、スナップショット種別、ファイル変更種別、状態色、ダイアログ枠色などのテーマ依存色をQMLへ提供するシングルトン
 * 設定の保存・復元およびシステムテーマ検出も担う
 */
class ThemeManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ThemeMode themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)   // テーマモード
    Q_PROPERTY(bool isDark READ isDark NOTIFY isDarkChanged)                                    // ダークテーマ適用中フラグ

    // スナップショットタイプの色
    Q_PROPERTY(QColor snapshotTypeSingle READ snapshotTypeSingle NOTIFY themeChanged)           // 単発スナップショットの色
    Q_PROPERTY(QColor snapshotTypePre READ snapshotTypePre NOTIFY themeChanged)                 // Preスナップショットの色
    Q_PROPERTY(QColor snapshotTypePost READ snapshotTypePost NOTIFY themeChanged)               // Postスナップショットの色
    Q_PROPERTY(QColor snapshotTypeDefault READ snapshotTypeDefault NOTIFY themeChanged)         // 既定スナップショットの色

    // ファイル変更タイプの色
    Q_PROPERTY(QColor fileChangeCreated READ fileChangeCreated NOTIFY themeChanged)             // 新規作成ファイルの色
    Q_PROPERTY(QColor fileChangeModified READ fileChangeModified NOTIFY themeChanged)           // 変更ファイルの色
    Q_PROPERTY(QColor fileChangeDeleted READ fileChangeDeleted NOTIFY themeChanged)             // 削除ファイルの色
    Q_PROPERTY(QColor fileChangeTypeChanged READ fileChangeTypeChanged NOTIFY themeChanged)     // タイプ変更ファイルの色

    // 状態色
    Q_PROPERTY(QColor warningColor READ warningColor NOTIFY themeChanged)                       // 警告色
    Q_PROPERTY(QColor errorColor READ errorColor NOTIFY themeChanged)                           // エラー色
    Q_PROPERTY(QColor importantColor READ importantColor NOTIFY themeChanged)                   // 重要色
    Q_PROPERTY(QColor successColor READ successColor NOTIFY themeChanged)                       // 成功色

    // ダイアログ枠色
    Q_PROPERTY(QColor dialogBorderColor READ dialogBorderColor NOTIFY themeChanged)             // ダイアログ枠の色

    // グループボックスの色
    Q_PROPERTY(QColor groupBoxBackground READ groupBoxBackground NOTIFY themeChanged)           // グループボックス内側の背景の色
    Q_PROPERTY(QColor groupBoxBorder READ groupBoxBorder NOTIFY themeChanged)                   // グループボックス枠の色

    // パレットの色 (QMLのPaletteへ割り当てる)
    Q_PROPERTY(QColor paletteWindow READ paletteWindow NOTIFY themeChanged)                                   // ウィンドウ背景の色
    Q_PROPERTY(QColor paletteWindowText READ paletteWindowText NOTIFY themeChanged)                           // ウィンドウ文字の色
    Q_PROPERTY(QColor paletteBase READ paletteBase NOTIFY themeChanged)                                       // 入力欄などの背景の色
    Q_PROPERTY(QColor paletteText READ paletteText NOTIFY themeChanged)                                       // 入力欄などの文字の色
    Q_PROPERTY(QColor paletteButton READ paletteButton NOTIFY themeChanged)                                   // ボタン背景の色
    Q_PROPERTY(QColor paletteButtonText READ paletteButtonText NOTIFY themeChanged)                           // ボタン文字の色
    Q_PROPERTY(QColor paletteHighlight READ paletteHighlight NOTIFY themeChanged)                             // 選択項目の背景の色
    Q_PROPERTY(QColor paletteHighlightedText READ paletteHighlightedText NOTIFY themeChanged)                 // 選択項目の文字の色
    Q_PROPERTY(QColor paletteLight READ paletteLight NOTIFY themeChanged)                                     // 立体表現の明るい縁の色
    Q_PROPERTY(QColor paletteMidlight READ paletteMidlight NOTIFY themeChanged)                               // 立体表現のやや明るい縁の色
    Q_PROPERTY(QColor paletteMid READ paletteMid NOTIFY themeChanged)                                         // 立体表現の中間色の色
    Q_PROPERTY(QColor paletteDark READ paletteDark NOTIFY themeChanged)                                       // 立体表現の暗い縁の色
    Q_PROPERTY(QColor paletteShadow READ paletteShadow NOTIFY themeChanged)                                   // 影の色
    Q_PROPERTY(QColor paletteAlternateBase READ paletteAlternateBase NOTIFY themeChanged)                     // 交互行の背景の色
    Q_PROPERTY(QColor paletteToolTipBase READ paletteToolTipBase NOTIFY themeChanged)                         // ツールチップ背景の色
    Q_PROPERTY(QColor paletteToolTipText READ paletteToolTipText NOTIFY themeChanged)                         // ツールチップ文字の色
    Q_PROPERTY(QColor palettePlaceholderText READ palettePlaceholderText NOTIFY themeChanged)                 // プレースホルダ文字の色
    Q_PROPERTY(QColor paletteLink READ paletteLink NOTIFY themeChanged)                                       // リンクの色
    Q_PROPERTY(QColor paletteLinkVisited READ paletteLinkVisited NOTIFY themeChanged)                         // 訪問済みリンクの色
    Q_PROPERTY(QColor paletteDisabledButtonText READ paletteDisabledButtonText NOTIFY themeChanged)           // 無効状態のボタン文字の色
    Q_PROPERTY(QColor paletteDisabledWindowText READ paletteDisabledWindowText NOTIFY themeChanged)           // 無効状態のウィンドウ文字の色
    Q_PROPERTY(QColor paletteDisabledText READ paletteDisabledText NOTIFY themeChanged)                       // 無効状態の入力欄文字の色
    Q_PROPERTY(QColor paletteDisabledButton READ paletteDisabledButton NOTIFY themeChanged)                   // 無効状態のボタン背景の色
    Q_PROPERTY(QColor paletteDisabledHighlight READ paletteDisabledHighlight NOTIFY themeChanged)             // 無効状態の選択項目背景の色
    Q_PROPERTY(QColor paletteDisabledHighlightedText READ paletteDisabledHighlightedText NOTIFY themeChanged) // 無効状態の選択項目文字の色

public:
    /**
     * @enum ThemeMode
     * @brief テーマモードを表す列挙型
     */
    enum ThemeMode {
        Light,  // ライトモード
        Dark,   // ダークモード
        System  // システム設定に連動
    };
    Q_ENUM(ThemeMode)

    // コンストラクタ/デストラクタ/シングルトン
    /**
     * @brief ThemeManagerを構築する
     * @param parent Qt親オブジェクト
     */
    explicit ThemeManager(QObject *parent = nullptr);

    /**
     * @brief ThemeManagerを破棄する
     */
    ~ThemeManager();

    /**
     * @brief シングルトンインスタンスを取得する
     * @return ThemeManagerの唯一のインスタンス
     */
    static ThemeManager* instance();

    // テーマモード
    /**
     * @brief テーマモードを取得する
     * @return 現在のテーマモード
     */
    ThemeMode themeMode() const { return m_themeMode; }

    /**
     * @brief テーマモードを設定する
     * @param mode 設定するテーマモード
     */
    void setThemeMode(ThemeMode mode);

    /**
     * @brief ダークテーマ適用中か判定する
     * @return ダークテーマ適用中の場合はtrue
     */
    bool isDark() const { return m_isDark; }

    // スナップショットタイプの色
    /**
     * @brief 単発スナップショットの色を取得する
     * @return 単発スナップショットの色
     */
    QColor snapshotTypeSingle() const;

    /**
     * @brief Preスナップショットの色を取得する
     * @return Preスナップショットの色
     */
    QColor snapshotTypePre() const;

    /**
     * @brief Postスナップショットの色を取得する
     * @return Postスナップショットの色
     */
    QColor snapshotTypePost() const;

    /**
     * @brief 既定スナップショットの色を取得する
     * @return 既定スナップショットの色
     */
    QColor snapshotTypeDefault() const;

    // ファイル変更タイプの色
    /**
     * @brief 新規作成ファイルの色を取得する
     * @return 新規作成ファイルの色
     */
    QColor fileChangeCreated() const;

    /**
     * @brief 変更ファイルの色を取得する
     * @return 変更ファイルの色
     */
    QColor fileChangeModified() const;

    /**
     * @brief 削除ファイルの色を取得する
     * @return 削除ファイルの色
     */
    QColor fileChangeDeleted() const;

    /**
     * @brief タイプ変更ファイルの色を取得する
     * @return タイプ変更ファイルの色
     */
    QColor fileChangeTypeChanged() const;

    // 状態色
    /**
     * @brief 警告色を取得する
     * @return 警告色
     */
    QColor warningColor() const;

    /**
     * @brief エラー色を取得する
     * @return エラー色
     */
    QColor errorColor() const;

    /**
     * @brief 重要色を取得する
     * @return 重要色
     */
    QColor importantColor() const;

    /**
     * @brief 成功色を取得する
     * @return 成功色
     */
    QColor successColor() const;

    // ダイアログ枠色
    /**
     * @brief ダイアログ枠の色を取得する
     * @return ダイアログ枠の色
     */
    QColor dialogBorderColor() const;

    // グループボックスの色
    /**
     * @brief グループボックス内側の背景の色を取得する
     * @return グループボックス内側の背景の色
     * @note Fusionスタイル既定の描画は、ダークモードでウィンドウ背景とほぼ同じ色になり、グループの範囲が分からないため専用の色を持つ
     */
    QColor groupBoxBackground() const;

    /**
     * @brief グループボックス枠の色を取得する
     * @return グループボックス枠の色
     * @note ダークモードでは、Fusion既定の枠 (ウィンドウ背景より暗い色) が背景に埋もれるため、背景より明るい色にする
     */
    QColor groupBoxBorder() const;

    // パレットの色
    /**
     * @brief ウィンドウ背景の色を取得する
     * @return ウィンドウ背景の色
     */
    QColor paletteWindow() const;

    /**
     * @brief ウィンドウ文字の色を取得する
     * @return ウィンドウ文字の色
     */
    QColor paletteWindowText() const;

    /**
     * @brief 入力欄などの背景の色を取得する
     * @return 入力欄などの背景の色
     */
    QColor paletteBase() const;

    /**
     * @brief 入力欄などの文字の色を取得する
     * @return 入力欄などの文字の色
     */
    QColor paletteText() const;

    /**
     * @brief ボタン背景の色を取得する
     * @return ボタン背景の色
     */
    QColor paletteButton() const;

    /**
     * @brief ボタン文字の色を取得する
     * @return ボタン文字の色
     */
    QColor paletteButtonText() const;

    /**
     * @brief 選択項目の背景の色を取得する
     * @return 選択項目の背景の色
     */
    QColor paletteHighlight() const;

    /**
     * @brief 選択項目の文字の色を取得する
     * @return 選択項目の文字の色
     */
    QColor paletteHighlightedText() const;

    /**
     * @brief 立体表現の明るい縁の色を取得する
     * @return 立体表現の明るい縁の色
     */
    QColor paletteLight() const;

    /**
     * @brief 立体表現のやや明るい縁の色を取得する
     * @return 立体表現のやや明るい縁の色
     */
    QColor paletteMidlight() const;

    /**
     * @brief 立体表現の中間色の色を取得する
     * @return 立体表現の中間色の色
     */
    QColor paletteMid() const;

    /**
     * @brief 立体表現の暗い縁の色を取得する
     * @return 立体表現の暗い縁の色
     */
    QColor paletteDark() const;

    /**
     * @brief 影の色を取得する
     * @return 影の色
     */
    QColor paletteShadow() const;

    /**
     * @brief 交互行の背景の色を取得する
     * @return 交互行の背景の色
     */
    QColor paletteAlternateBase() const;

    /**
     * @brief ツールチップ背景の色を取得する
     * @return ツールチップ背景の色
     */
    QColor paletteToolTipBase() const;

    /**
     * @brief ツールチップ文字の色を取得する
     * @return ツールチップ文字の色
     */
    QColor paletteToolTipText() const;

    /**
     * @brief プレースホルダ文字の色を取得する
     * @return プレースホルダ文字の色
     */
    QColor palettePlaceholderText() const;

    /**
     * @brief リンクの色を取得する
     * @return リンクの色
     */
    QColor paletteLink() const;

    /**
     * @brief 訪問済みリンクの色を取得する
     * @return 訪問済みリンクの色
     */
    QColor paletteLinkVisited() const;

    /**
     * @brief 無効状態のボタン文字の色を取得する
     * @return 無効状態のボタン文字の色
     */
    QColor paletteDisabledButtonText() const;

    /**
     * @brief 無効状態のウィンドウ文字の色を取得する
     * @return 無効状態のウィンドウ文字の色
     */
    QColor paletteDisabledWindowText() const;

    /**
     * @brief 無効状態の入力欄文字の色を取得する
     * @return 無効状態の入力欄文字の色
     */
    QColor paletteDisabledText() const;

    /**
     * @brief 無効状態のボタン背景の色を取得する
     * @return 無効状態のボタン背景の色
     */
    QColor paletteDisabledButton() const;

    /**
     * @brief 無効状態の選択項目背景の色を取得する
     * @return 無効状態の選択項目背景の色
     */
    QColor paletteDisabledHighlight() const;

    /**
     * @brief 無効状態の選択項目文字の色を取得する
     * @return 無効状態の選択項目文字の色
     */
    QColor paletteDisabledHighlightedText() const;

signals:
    /**
     * @brief テーマモード変更時に発行する
     */
    void themeModeChanged();

    /**
     * @brief ダーク状態変更時に発行する
     */
    void isDarkChanged();

    /**
     * @brief テーマ色変更時に発行する
     */
    void themeChanged();

private:
    // 設定・テーマ更新
    /**
     * @brief 設定を読み込む
     */
    void loadSettings();

    /**
     * @brief 設定を保存する
     */
    void saveSettings();

    /**
     * @brief テーマを更新する
     */
    void updateTheme();

    /**
     * @brief システムテーマを検出する
     */
    void detectSystemTheme();

    /**
     * @brief テーマに応じた色を取得する
     * @param lightColor ライトモード時の色 (16進表現)
     * @param darkColor ダークモード時の色 (16進表現)
     * @return 現在のテーマに対応する色
     */
    QColor getColor(const QString &lightColor,
                    const QString &darkColor) const;

    // シングルトン・状態
    static ThemeManager *s_instance;    // シングルトンインスタンス
    ThemeMode m_themeMode;              // 現在のテーマモード
    bool m_isDark;                      // ダークテーマ適用中フラグ

    /**
     * @struct LightColors
     * @brief ライトモードのカラーパレット
     */
    struct LightColors {
        static constexpr const char* snapshotSingle = "#4CAF50";    // 単発スナップショット色
        static constexpr const char* snapshotPre = "#2196F3";       // Preスナップショット色
        static constexpr const char* snapshotPost = "#FF9800";      // Postスナップショット色
        static constexpr const char* snapshotDefault = "#9E9E9E";   // 既定スナップショット色

        static constexpr const char* fileCreated = "#4CAF50";       // 新規作成色
        static constexpr const char* fileModified = "#2196F3";      // 変更色
        static constexpr const char* fileDeleted = "#F44336";       // 削除色
        static constexpr const char* fileTypeChanged = "#FF9800";   // タイプ変更色

        static constexpr const char* warning = "#FF5722";           // 警告色
        static constexpr const char* error = "#F44336";             // エラー色
        static constexpr const char* important = "#FFC107";         // 重要色
        static constexpr const char* success = "#4CAF50";           // 成功色

        static constexpr const char* dialogBorder = "#9E9E9E";      // ダイアログ枠色

        static constexpr const char* groupBoxBackground = "#FCFCFC";// グループボックス内側の背景 (Fusion既定の描画結果と同じ値)
        static constexpr const char* groupBoxBorder = "#C5C5C5";    // グループボックス枠 (Fusion既定の描画結果と同じ値)

        // QMLのPaletteへ割り当てる色
        static constexpr const char* paletteWindow = "#FFFFFF";     // ウィンドウ背景
        static constexpr const char* paletteWindowText = "#000000"; // ウィンドウ文字
        static constexpr const char* paletteBase = "#FFFFFF";       // 入力欄などの背景
        static constexpr const char* paletteText = "#000000";       // 入力欄などの文字
        static constexpr const char* paletteButton = "#F5F5F5";     // ボタン背景
        static constexpr const char* paletteButtonText = "#000000"; // ボタン文字
        static constexpr const char* paletteHighlight = "#2196F3";  // 選択項目の背景
        static constexpr const char* paletteHighlightedText = "#FFFFFF";// 選択項目の文字
        static constexpr const char* paletteLight = "#FAFAFA";      // 立体表現の明るい縁
        static constexpr const char* paletteMidlight = "#F5F5F5";   // 立体表現のやや明るい縁
        static constexpr const char* paletteMid = "#EEEEEE";        // 立体表現の中間色
        static constexpr const char* paletteDark = "#E0E0E0";       // 立体表現の暗い縁
        static constexpr const char* paletteShadow = "#BDBDBD";     // 影
        static constexpr const char* paletteAlternateBase = "#FAFAFA";// 交互行の背景
        static constexpr const char* paletteToolTipBase = "#FFFFCC";// ツールチップ背景
        static constexpr const char* paletteToolTipText = "#000000";// ツールチップ文字
        static constexpr const char* palettePlaceholderText = "#9E9E9E";// プレースホルダ文字
        static constexpr const char* paletteLink = "#2196F3";       // リンク
        static constexpr const char* paletteLinkVisited = "#9C27B0";// 訪問済みリンク
        static constexpr const char* paletteDisabledButtonText = "#BDBDBD";// 無効状態のボタン文字
        static constexpr const char* paletteDisabledWindowText = "#BDBDBD";// 無効状態のウィンドウ文字
        static constexpr const char* paletteDisabledText = "#BDBDBD";// 無効状態の入力欄文字
        static constexpr const char* paletteDisabledButton = "#EEEEEE";// 無効状態のボタン背景
        static constexpr const char* paletteDisabledHighlight = "#BDBDBD";// 無効状態の選択項目背景
        static constexpr const char* paletteDisabledHighlightedText = "#FFFFFF";// 無効状態の選択項目文字
    };

    /**
     * @struct DarkColors
     * @brief ダークモードのカラーパレット
     */
    struct DarkColors {
        static constexpr const char* snapshotSingle = "#66BB6A";    // 単発スナップショット色
        static constexpr const char* snapshotPre = "#42A5F5";       // Preスナップショット色
        static constexpr const char* snapshotPost = "#FFA726";      // Postスナップショット色
        static constexpr const char* snapshotDefault = "#BDBDBD";   // 既定スナップショット色

        static constexpr const char* fileCreated = "#66BB6A";       // 新規作成色
        static constexpr const char* fileModified = "#42A5F5";      // 変更色
        static constexpr const char* fileDeleted = "#EF5350";       // 削除色
        static constexpr const char* fileTypeChanged = "#FFA726";   // タイプ変更色

        static constexpr const char* warning = "#FF7043";           // 警告色
        static constexpr const char* error = "#EF5350";             // エラー色
        static constexpr const char* important = "#FFCA28";         // 重要色
        static constexpr const char* success = "#66BB6A";           // 成功色

        static constexpr const char* dialogBorder = "#616161";      // ダイアログ枠色

        static constexpr const char* groupBoxBackground = "#2A2A2A";// グループボックス内側の背景 (ウィンドウ背景#212121より明るくする)
        static constexpr const char* groupBoxBorder = "#5A5A5A";    // グループボックス枠 (背景に対してコントラスト比2以上を確保する)

        // QMLのPaletteへ割り当てる色
        static constexpr const char* paletteWindow = "#212121";     // ウィンドウ背景
        static constexpr const char* paletteWindowText = "#FFFFFF"; // ウィンドウ文字
        static constexpr const char* paletteBase = "#303030";       // 入力欄などの背景
        static constexpr const char* paletteText = "#FFFFFF";       // 入力欄などの文字
        static constexpr const char* paletteButton = "#424242";     // ボタン背景
        static constexpr const char* paletteButtonText = "#FFFFFF"; // ボタン文字
        static constexpr const char* paletteHighlight = "#42A5F5";  // 選択項目の背景
        static constexpr const char* paletteHighlightedText = "#000000";// 選択項目の文字
        static constexpr const char* paletteLight = "#424242";      // 立体表現の明るい縁
        static constexpr const char* paletteMidlight = "#383838";   // 立体表現のやや明るい縁
        static constexpr const char* paletteMid = "#2C2C2C";        // 立体表現の中間色
        static constexpr const char* paletteDark = "#1E1E1E";       // 立体表現の暗い縁
        static constexpr const char* paletteShadow = "#000000";     // 影
        static constexpr const char* paletteAlternateBase = "#2C2C2C";// 交互行の背景
        static constexpr const char* paletteToolTipBase = "#616161";// ツールチップ背景
        static constexpr const char* paletteToolTipText = "#FFFFFF";// ツールチップ文字
        static constexpr const char* palettePlaceholderText = "#757575";// プレースホルダ文字
        static constexpr const char* paletteLink = "#42A5F5";       // リンク
        static constexpr const char* paletteLinkVisited = "#BA68C8";// 訪問済みリンク
        static constexpr const char* paletteDisabledButtonText = "#6E6E6E";// 無効状態のボタン文字
        static constexpr const char* paletteDisabledWindowText = "#6E6E6E";// 無効状態のウィンドウ文字
        static constexpr const char* paletteDisabledText = "#6E6E6E";// 無効状態の入力欄文字
        static constexpr const char* paletteDisabledButton = "#383838";// 無効状態のボタン背景
        static constexpr const char* paletteDisabledHighlight = "#6E6E6E";// 無効状態の選択項目背景
        static constexpr const char* paletteDisabledHighlightedText = "#1E1E1E";// 無効状態の選択項目文字
    };
};

#endif // THEMEMANAGER_H
