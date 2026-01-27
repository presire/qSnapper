#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QPalette>

class ThemeManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ThemeMode themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(bool isDark READ isDark NOTIFY isDarkChanged)

    // スナップショットタイプの色
    Q_PROPERTY(QColor snapshotTypeSingle READ snapshotTypeSingle NOTIFY themeChanged)
    Q_PROPERTY(QColor snapshotTypePre READ snapshotTypePre NOTIFY themeChanged)
    Q_PROPERTY(QColor snapshotTypePost READ snapshotTypePost NOTIFY themeChanged)
    Q_PROPERTY(QColor snapshotTypeDefault READ snapshotTypeDefault NOTIFY themeChanged)

    // ファイル変更タイプの色
    Q_PROPERTY(QColor fileChangeCreated READ fileChangeCreated NOTIFY themeChanged)
    Q_PROPERTY(QColor fileChangeModified READ fileChangeModified NOTIFY themeChanged)
    Q_PROPERTY(QColor fileChangeDeleted READ fileChangeDeleted NOTIFY themeChanged)
    Q_PROPERTY(QColor fileChangeTypeChanged READ fileChangeTypeChanged NOTIFY themeChanged)

    // 状態色
    Q_PROPERTY(QColor warningColor READ warningColor NOTIFY themeChanged)
    Q_PROPERTY(QColor errorColor READ errorColor NOTIFY themeChanged)
    Q_PROPERTY(QColor importantColor READ importantColor NOTIFY themeChanged)
    Q_PROPERTY(QColor successColor READ successColor NOTIFY themeChanged)

public:
    enum ThemeMode {
        Light,
        Dark,
        System
    };
    Q_ENUM(ThemeMode)

    explicit ThemeManager(QObject *parent = nullptr);
    ~ThemeManager();

    static ThemeManager* instance();

    // テーマモード
    ThemeMode themeMode() const { return m_themeMode; }
    void setThemeMode(ThemeMode mode);
    bool isDark() const { return m_isDark; }

    // スナップショットタイプの色
    QColor snapshotTypeSingle() const;
    QColor snapshotTypePre() const;
    QColor snapshotTypePost() const;
    QColor snapshotTypeDefault() const;

    // ファイル変更タイプの色
    QColor fileChangeCreated() const;
    QColor fileChangeModified() const;
    QColor fileChangeDeleted() const;
    QColor fileChangeTypeChanged() const;

    // 状態色
    QColor warningColor() const;
    QColor errorColor() const;
    QColor importantColor() const;
    QColor successColor() const;

signals:
    void themeModeChanged();
    void isDarkChanged();
    void themeChanged();

private:
    void loadSettings();
    void saveSettings();
    void updateTheme();
    void detectSystemTheme();
    QColor getColor(const QString &lightColor, const QString &darkColor) const;

    static ThemeManager *s_instance;

    ThemeMode m_themeMode;
    bool m_isDark;

    // ライトモードのカラーパレット
    struct LightColors {
        static constexpr const char* snapshotSingle = "#4CAF50";
        static constexpr const char* snapshotPre = "#2196F3";
        static constexpr const char* snapshotPost = "#FF9800";
        static constexpr const char* snapshotDefault = "#9E9E9E";

        static constexpr const char* fileCreated = "#4CAF50";
        static constexpr const char* fileModified = "#2196F3";
        static constexpr const char* fileDeleted = "#F44336";
        static constexpr const char* fileTypeChanged = "#FF9800";

        static constexpr const char* warning = "#FF5722";
        static constexpr const char* error = "#F44336";
        static constexpr const char* important = "#FFC107";
        static constexpr const char* success = "#4CAF50";
    };

    // ダークモードのカラーパレット
    struct DarkColors {
        static constexpr const char* snapshotSingle = "#66BB6A";
        static constexpr const char* snapshotPre = "#42A5F5";
        static constexpr const char* snapshotPost = "#FFA726";
        static constexpr const char* snapshotDefault = "#BDBDBD";

        static constexpr const char* fileCreated = "#66BB6A";
        static constexpr const char* fileModified = "#42A5F5";
        static constexpr const char* fileDeleted = "#EF5350";
        static constexpr const char* fileTypeChanged = "#FFA726";

        static constexpr const char* warning = "#FF7043";
        static constexpr const char* error = "#EF5350";
        static constexpr const char* important = "#FFCA28";
        static constexpr const char* success = "#66BB6A";
    };
};

#endif // THEMEMANAGER_H
