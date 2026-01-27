#ifndef FSSNAPSHOT_H
#define FSSNAPSHOT_H

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QVariantMap>

class FsSnapshot : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int number READ number CONSTANT)
    Q_PROPERTY(SnapshotType snapshotType READ snapshotType CONSTANT)
    Q_PROPERTY(int previousNumber READ previousNumber CONSTANT)
    Q_PROPERTY(QDateTime timestamp READ timestamp CONSTANT)
    Q_PROPERTY(QString user READ user CONSTANT)
    Q_PROPERTY(CleanupAlgorithm cleanupAlgo READ cleanupAlgo CONSTANT)
    Q_PROPERTY(QString description READ description CONSTANT)
    Q_PROPERTY(QVariantMap userdata READ userdata CONSTANT)

public:
    enum class SnapshotType {
        Single,
        Pre,
        Post
    };
    Q_ENUM(SnapshotType)

    enum class CleanupAlgorithm {
        None,
        Number,
        Timeline
    };
    Q_ENUM(CleanupAlgorithm)

    explicit FsSnapshot(int number,
                       SnapshotType snapshotType,
                       int previousNumber,
                       const QDateTime &timestamp,
                       const QString &user,
                       CleanupAlgorithm cleanupAlgo,
                       const QString &description,
                       const QVariantMap &userdata = QVariantMap(),
                       QObject *parent = nullptr);

    int number() const { return m_number; }
    SnapshotType snapshotType() const { return m_snapshotType; }
    int previousNumber() const { return m_previousNumber; }
    QDateTime timestamp() const { return m_timestamp; }
    QString user() const { return m_user; }
    CleanupAlgorithm cleanupAlgo() const { return m_cleanupAlgo; }
    QString description() const { return m_description; }
    QVariantMap userdata() const { return m_userdata; }

    Q_INVOKABLE QString snapshotTypeString() const;
    Q_INVOKABLE QString cleanupAlgoString() const;
    Q_INVOKABLE bool isImportant() const;

    static QString snapshotTypeToString(SnapshotType type);
    static SnapshotType stringToSnapshotType(const QString &str);
    static QString cleanupAlgorithmToString(CleanupAlgorithm algo);
    static CleanupAlgorithm stringToCleanupAlgorithm(const QString &str);

private:
    int m_number;                        // スナップショット番号
    SnapshotType m_snapshotType;         // スナップショットタイプ (Single/Pre/Post)
    int m_previousNumber;                // 前のスナップショット番号 (Postの場合のみ有効)
    QDateTime m_timestamp;               // スナップショット作成日時
    QString m_user;                      // スナップショット作成ユーザー名
    CleanupAlgorithm m_cleanupAlgo;      // クリーンアップアルゴリズム (None/Number/Timeline)
    QString m_description;               // スナップショットの説明文
    QVariantMap m_userdata;              // カスタムユーザーデータマップ
};

#endif // FSSNAPSHOT_H
