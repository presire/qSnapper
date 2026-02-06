#ifndef SNAPSHOTOPERATIONS_H
#define SNAPSHOTOPERATIONS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDBusContext>
#include <QTimer>
#include <memory>

namespace snapper {
    class Snapper;
}

class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    static constexpr int IdleTimeoutMs = 5 * 60 * 1000; // 5分
    std::unique_ptr<snapper::Snapper> m_snapper;    // Snapperインスタンス
    QString m_currentConfig;                        // 現在の設定名
    QTimer m_idleTimer;                             // アイドルタイムアウト用タイマー

    void resetIdleTimer();

public:
    explicit SnapshotOperations(QObject *parent = nullptr);
    ~SnapshotOperations();

public slots:
    QString ListSnapshots();
    QString CreateSnapshot(const QString &type, const QString &description,
                          int preNumber, const QString &cleanup, bool important);
    bool DeleteSnapshot(int number);
    bool RollbackSnapshot(int number);
    QString GetFileChanges(const QString &configName, int snapshotNumber);
    QString GetFileDiff(const QString &configName, int snapshotNumber, const QString &filePath);
    bool RestoreFiles(const QString &configName, int snapshotNumber, const QStringList &filePaths);
    void Quit();

signals:
    void restoreProgress(int current, int total, const QString &filePath);

private:
    bool checkAuthorization(const QString &actionId);
    snapper::Snapper* getSnapper(const QString &configName = "root");
    QString formatSnapshotToCSV(const snapper::Snapper *snapper);
    QString snapshotTypeToString(int type);
    int stringToSnapshotType(const QString &typeStr);
};

#endif // SNAPSHOTOPERATIONS_H
