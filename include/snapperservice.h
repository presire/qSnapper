#ifndef SNAPPERSERVICE_H
#define SNAPPERSERVICE_H

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QLoggingCategory>
#include <QDBusInterface>
#include "fssnapshot.h"

Q_DECLARE_LOGGING_CATEGORY(snapperLog)

class SnapperService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ isConfigured NOTIFY configuredChanged)
    Q_PROPERTY(QStringList configs READ configs NOTIFY configsChanged)
    Q_PROPERTY(QString currentConfig READ currentConfig WRITE setCurrentConfig NOTIFY currentConfigChanged)

public:
    explicit SnapperService(QObject *parent = nullptr);
    ~SnapperService();

    static SnapperService* instance();

    bool isConfigured();
    Q_INVOKABLE void configureSnapper();

    // 設定 (config) 切替 API
    QStringList configs();
    QString currentConfig() const { return m_currentConfig; }
    Q_INVOKABLE void setCurrentConfig(const QString &name);
    Q_INVOKABLE void refreshConfigs();

    Q_INVOKABLE bool createSnapshotAllowed(const QString &snapshotType) const;

    Q_INVOKABLE FsSnapshot* createSingle(const QString &description,
                                         FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                         bool important = false,
                                         const QVariantMap &userdata = QVariantMap());

    Q_INVOKABLE FsSnapshot* createPre(const QString &description,
                                      FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                      bool important = false,
                                      const QVariantMap &userdata = QVariantMap());

    Q_INVOKABLE FsSnapshot* createPost(const QString &description,
                                       int previousNumber,
                                       FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                       bool important = false,
                                       const QVariantMap &userdata = QVariantMap());

    Q_INVOKABLE QList<FsSnapshot*> all();
    Q_INVOKABLE FsSnapshot* find(int number);
    Q_INVOKABLE bool rollback(int number);
    Q_INVOKABLE bool deleteSnapshot(int number);
    Q_INVOKABLE bool modifySnapshot(int number,
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata);

    void setConfigureOnInstall(bool value) { m_configureOnInstall = value; }
    bool configureOnInstall() const { return m_configureOnInstall; }

signals:
    void configuredChanged(bool configured);
    void configsChanged();
    void currentConfigChanged();
    void snapshotCreated(FsSnapshot *snapshot);
    void snapshotCreationFailed(const QString &error);
    void rollbackCompleted();
    void rollbackFailed(const QString &error);
    void snapshotDeleted(int number);
    void snapshotDeletionFailed(int number, const QString &error);
    void snapshotModified(int number);
    void snapshotModificationFailed(int number, const QString &error);

private:
    FsSnapshot* create(FsSnapshot::SnapshotType snapshotType,
                      const QString &description,
                      FsSnapshot *previous = nullptr,
                      FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                      bool important = false,
                      const QVariantMap &userdata = QVariantMap());

    QString targetRoot() const;
    bool nonSwitchedInstallation() const;

    void installationHelperStep4();
    void writeSnapperConfig();
    void updateEtcSysconfigYast2();
    void setupSnapperQuota();

    QList<FsSnapshot*> parseSnapshotList(const QString &csvOutput);
    QString executeCommand(const QString &program, const QStringList &arguments, bool &success);
    bool reconnect();                        // D-Busサービスへの再接続を試みる
    static SnapperService *s_instance;       // シングルトンインスタンス

    bool m_configured;                       // Snapperが設定済みかどうか
    bool m_configuredChecked;                // 設定チェック済みフラグ
    bool m_configureOnInstall;               // インストール時に設定を行うかどうか
    QDBusInterface *m_dbusInterface;         // DBus通信インターフェース
    QStringList m_configs;                   // 利用可能な Snapper config 名のキャッシュ
    bool m_configsChecked;                   // configs キャッシュ済みフラグ
    QString m_currentConfig;                 // 現在選択されている config 名
};

#endif // SNAPPERSERVICE_H
