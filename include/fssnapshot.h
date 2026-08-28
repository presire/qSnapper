#ifndef FSSNAPSHOT_H
#define FSSNAPSHOT_H

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QVariantMap>

/**
 * @brief ファイルシステムスナップショットのメタデータを保持するクラス
 *
 * Snapperが管理するスナップショット1件分の情報をQObjectとしてラップし、QMLからプロパティとして参照可能にする
 * スナップショット番号、種別、タイムスタンプ、ユーザ、クリーンアップ設定、説明および任意のユーザデータを保持する
 */
class FsSnapshot : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int number READ number CONSTANT)                         // スナップショット番号
    Q_PROPERTY(SnapshotType snapshotType READ snapshotType CONSTANT)    // スナップショット種別
    Q_PROPERTY(int previousNumber READ previousNumber CONSTANT)         // 紐付け元スナップショット番号
    Q_PROPERTY(QDateTime timestamp READ timestamp CONSTANT)             // 作成日時
    Q_PROPERTY(QString user READ user CONSTANT)                         // 作成ユーザ名
    Q_PROPERTY(CleanupAlgorithm cleanupAlgo READ cleanupAlgo CONSTANT)  // クリーンアップアルゴリズム
    Q_PROPERTY(QString description READ description CONSTANT)           // 説明文
    Q_PROPERTY(QVariantMap userdata READ userdata CONSTANT)             // ユーザデータマップ

public:
    // スナップショット種別を表す列挙型
    enum class SnapshotType {
        Single,     // 単発スナップショット
        Pre,        // 変更前の事前スナップショット
        Post        // 変更後の事後スナップショット
    };
    Q_ENUM(SnapshotType)

    // クリーンアップアルゴリズムを表す列挙型
    enum class CleanupAlgorithm {
        None,       // クリーンアップなし
        Number,     // 番号ベースで古いスナップショットを削除
        Timeline    // タイムラインに基づいて削除
    };
    Q_ENUM(CleanupAlgorithm)

    // コンストラクタ
    explicit FsSnapshot(int number,
                       SnapshotType snapshotType,
                       int previousNumber,
                       const QDateTime &timestamp,
                       const QString &user,
                       CleanupAlgorithm cleanupAlgo,
                       const QString &description,
                       const QVariantMap &userdata = QVariantMap(),
                       QObject *parent = nullptr); // FsSnapshotを構築する

    // プロパティゲッター
    int number() const { return m_number; }                         // スナップショット番号を取得する
    SnapshotType snapshotType() const { return m_snapshotType; }    // スナップショット種別を取得する
    int previousNumber() const { return m_previousNumber; }         // 紐付け元スナップショット番号を取得する
    QDateTime timestamp() const { return m_timestamp; }             // 作成日時を取得する
    QString user() const { return m_user; }                         // 作成ユーザ名を取得する
    CleanupAlgorithm cleanupAlgo() const { return m_cleanupAlgo; }  // クリーンアップアルゴリズムを取得する
    QString description() const { return m_description; }           // 説明文を取得する
    QVariantMap userdata() const { return m_userdata; }             // ユーザデータマップを取得する

    // インスタンスメソッド (QML公開・判定)
    Q_INVOKABLE QString snapshotTypeString() const;                 // スナップショット種別を文字列で取得する
    Q_INVOKABLE QString cleanupAlgoString() const;                  // クリーンアップアルゴリズムを文字列で取得する
    Q_INVOKABLE bool isImportant() const;                           // 重要フラグが立っているか判定する

    // 静的変換ユーティリティ
    static QString snapshotTypeToString(SnapshotType type);                 // スナップショット種別を文字列に変換する
    static SnapshotType stringToSnapshotType(const QString &str);           // 文字列をスナップショット種別に変換する
    static QString cleanupAlgorithmToString(CleanupAlgorithm algo);         // クリーンアップアルゴリズムを文字列に変換する
    static CleanupAlgorithm stringToCleanupAlgorithm(const QString &str);   // 文字列をクリーンアップアルゴリズムに変換する

private:
    // 識別情報
    int m_number;                   // スナップショット番号
    SnapshotType m_snapshotType;    // スナップショット種別 (Single/Pre/Post)
    int m_previousNumber;           // 紐付け元スナップショット番号 (Postの場合のみ有効)

    // 作成情報
    QDateTime m_timestamp;          // スナップショット作成日時
    QString m_user;                 // スナップショット作成ユーザ名

    // メタデータ
    CleanupAlgorithm m_cleanupAlgo; // クリーンアップアルゴリズム (None/Number/Timeline)
    QString m_description;          // スナップショットの説明文
    QVariantMap m_userdata;         // カスタムユーザデータマップ
};

#endif // FSSNAPSHOT_H
