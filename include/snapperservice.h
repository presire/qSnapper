#ifndef SNAPPERSERVICE_H
#define SNAPPERSERVICE_H

#include <QObject>
#include <QList>
#include <QString>
#include <QLoggingCategory>
#include <QDBusInterface>
#include "fssnapshot.h"

Q_DECLARE_LOGGING_CATEGORY(snapperLog)

class SnapperService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ isConfigured NOTIFY configuredChanged)

public:
    explicit SnapperService(QObject *parent = nullptr);
    ~SnapperService();

    static SnapperService* instance();

    bool isConfigured();
    Q_INVOKABLE void configureSnapper();

    Q_INVOKABLE bool createSnapshotAllowed(const QString &snapshotType) const;

    Q_INVOKABLE FsSnapshot* createSingle(const QString &description,
                                         FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                         bool important = false);

    Q_INVOKABLE FsSnapshot* createPre(const QString &description,
                                      FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                      bool important = false);

    Q_INVOKABLE FsSnapshot* createPost(const QString &description,
                                       int previousNumber,
                                       FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                                       bool important = false);

    Q_INVOKABLE QList<FsSnapshot*> all();
    Q_INVOKABLE FsSnapshot* find(int number);
    Q_INVOKABLE bool rollback(int number);
    Q_INVOKABLE bool deleteSnapshot(int number);

    void setConfigureOnInstall(bool value) { m_configureOnInstall = value; }
    bool configureOnInstall() const { return m_configureOnInstall; }

signals:
    void configuredChanged(bool configured);
    void snapshotCreated(FsSnapshot *snapshot);
    void snapshotCreationFailed(const QString &error);
    void rollbackCompleted();
    void rollbackFailed(const QString &error);
    void snapshotDeleted(int number);
    void snapshotDeletionFailed(int number, const QString &error);

private:
    FsSnapshot* create(FsSnapshot::SnapshotType snapshotType,
                      const QString &description,
                      FsSnapshot *previous = nullptr,
                      FsSnapshot::CleanupAlgorithm cleanup = FsSnapshot::CleanupAlgorithm::None,
                      bool important = false);

    QString targetRoot() const;
    bool nonSwitchedInstallation() const;

    void installationHelperStep4();
    void writeSnapperConfig();
    void updateEtcSysconfigYast2();
    void setupSnapperQuota();

    QList<FsSnapshot*> parseSnapshotList(const QString &csvOutput);
    QString executeCommand(const QString &program, const QStringList &arguments, bool &success);
    QDBusInterface* getDBusInterface();

    static SnapperService *s_instance;       // シングルトンインスタンス

    bool m_configured;                       // Snapperが設定済みかどうか
    bool m_configuredChecked;                // 設定チェック済みフラグ
    bool m_configureOnInstall;               // インストール時に設定を行うかどうか
    QDBusInterface *m_dbusInterface;         // DBus通信インターフェース
};

#endif // SNAPPERSERVICE_H
