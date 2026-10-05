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
    };
};

#endif // THEMEMANAGER_H
