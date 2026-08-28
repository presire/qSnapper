#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QPalette>

/**
 * @brief アプリケーションのテーマ管理を担うクラス
 *
 * ライト/ダーク/システム連動のテーマモードを管理し、スナップショット種別、ファイル変更種別、状態色、ダイアログ枠色などのテーマ依存色をQMLへ提供するシングルトン
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
    // テーマモードを表す列挙型
    enum ThemeMode {
        Light,  // ライトモード
        Dark,   // ダークモード
        System  // システム設定に連動
    };
    Q_ENUM(ThemeMode)

    // コンストラクタ/デストラクタ/シングルトン
    explicit ThemeManager(QObject *parent = nullptr);   // コンストラクタ
    ~ThemeManager();                                    // デストラクタ
    static ThemeManager* instance();                    // シングルトンインスタンスを取得する

    // テーマモード
    ThemeMode themeMode() const { return m_themeMode; } // テーマモードを取得する
    void setThemeMode(ThemeMode mode);                  // テーマモードを設定する
    bool isDark() const { return m_isDark; }            // ダークテーマ適用中か判定する

    // スナップショットタイプの色
    QColor snapshotTypeSingle() const;                  // 単発スナップショットの色を取得する
    QColor snapshotTypePre() const;                     // Preスナップショットの色を取得する
    QColor snapshotTypePost() const;                    // Postスナップショットの色を取得する
    QColor snapshotTypeDefault() const;                 // 既定スナップショットの色を取得する

    // ファイル変更タイプの色
    QColor fileChangeCreated() const;                   // 新規作成ファイルの色を取得する
    QColor fileChangeModified() const;                  // 変更ファイルの色を取得する
    QColor fileChangeDeleted() const;                   // 削除ファイルの色を取得する
    QColor fileChangeTypeChanged() const;               // タイプ変更ファイルの色を取得する

    // 状態色
    QColor warningColor() const;                        // 警告色を取得する
    QColor errorColor() const;                          // エラー色を取得する
    QColor importantColor() const;                      // 重要色を取得する
    QColor successColor() const;                        // 成功色を取得する

    // ダイアログ枠色
    QColor dialogBorderColor() const;                   // ダイアログ枠の色を取得する

signals:
    void themeModeChanged();                            // テーマモード変更時に発行する
    void isDarkChanged();                               // ダーク状態変更時に発行する
    void themeChanged();                                // テーマ色変更時に発行する

private:
    // 設定・テーマ更新
    void loadSettings();                                // 設定を読み込む
    void saveSettings();                                // 設定を保存する
    void updateTheme();                                 // テーマを更新する
    void detectSystemTheme();                           // システムテーマを検出する
    QColor getColor(const QString &lightColor,          // テーマに応じた色を取得する
                    const QString &darkColor) const;

    // シングルトン・状態
    static ThemeManager *s_instance;                    // シングルトンインスタンス
    ThemeMode m_themeMode;                              // 現在のテーマモード
    bool m_isDark;                                      // ダークテーマ適用中フラグ

    // ライトモードのカラーパレット
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

    // ダークモードのカラーパレット
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
