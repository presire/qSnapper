#ifndef FSSNAPSHOT_H
#define FSSNAPSHOT_H

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QVariantMap>

/**
 * @class FsSnapshot
 * @brief ファイルシステムスナップショット1件分のメタデータを保持するクラス
 *
 * Snapperが管理するスナップショット1件分の情報をQObjectとしてラップする
 * QMLからプロパティとして参照可能にするため、すべての値は構築時に確定し不変となる
 * 番号、種別、紐付け元番号、タイムスタンプ、ユーザ、クリーンアップ設定、説明および任意のユーザデータを保持する
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
    /**
     * @enum SnapshotType
     * @brief スナップショットの種別
     *
     * Postは必ずいずれかのPreと対になり、previousNumberでそのPreを指す
     */
    enum class SnapshotType {
        Single,     // 単発スナップショット
        Pre,        // 変更前の事前スナップショット
        Post        // 変更後の事後スナップショット
    };
    Q_ENUM(SnapshotType)

    /**
     * @enum CleanupAlgorithm
     * @brief 自動クリーンアップの方式
     */
    enum class CleanupAlgorithm {
        None,       // クリーンアップなし
        Number,     // 番号ベースで古いスナップショットを削除
        Timeline    // タイムラインに基づいて削除
    };
    Q_ENUM(CleanupAlgorithm)

    /**
     * @brief FsSnapshotを構築する
     * @param number スナップショット番号
     * @param snapshotType スナップショット種別
     * @param previousNumber 紐付け元スナップショット番号 (Postの場合のみ有効)
     * @param timestamp 作成日時
     * @param user 作成ユーザ名
     * @param cleanupAlgo クリーンアップアルゴリズム
     * @param description 説明文
     * @param userdata ユーザデータマップ
     * @param parent Qt親オブジェクト
     */
    explicit FsSnapshot(int number,
                       SnapshotType snapshotType,
                       int previousNumber,
                       const QDateTime &timestamp,
                       const QString &user,
                       CleanupAlgorithm cleanupAlgo,
                       const QString &description,
                       const QVariantMap &userdata = QVariantMap(),
                       QObject *parent = nullptr);

    /**
     * @brief スナップショット番号を取得する
     * @return スナップショット番号
     */
    int number() const { return m_number; }

    /**
     * @brief スナップショット種別を取得する
     * @return スナップショット種別
     */
    SnapshotType snapshotType() const { return m_snapshotType; }

    /**
     * @brief 紐付け元スナップショット番号を取得する
     * @return Postが指すPreの番号、Post以外では無効値
     */
    int previousNumber() const { return m_previousNumber; }

    /**
     * @brief 作成日時を取得する
     * @return 作成日時
     */
    QDateTime timestamp() const { return m_timestamp; }

    /**
     * @brief 作成ユーザ名を取得する
     * @return 作成ユーザ名
     */
    QString user() const { return m_user; }

    /**
     * @brief クリーンアップアルゴリズムを取得する
     * @return クリーンアップアルゴリズム
     */
    CleanupAlgorithm cleanupAlgo() const { return m_cleanupAlgo; }

    /**
     * @brief 説明文を取得する
     * @return 説明文
     */
    QString description() const { return m_description; }

    /**
     * @brief ユーザデータマップを取得する
     * @return ユーザデータマップ
     */
    QVariantMap userdata() const { return m_userdata; }

    /**
     * @brief スナップショット種別を表示用文字列で取得する
     * @return 種別を表す文字列 (QML表示用)
     */
    Q_INVOKABLE QString snapshotTypeString() const;

    /**
     * @brief クリーンアップアルゴリズムを表示用文字列で取得する
     * @return クリーンアップアルゴリズムを表す文字列 (QML表示用)
     */
    Q_INVOKABLE QString cleanupAlgoString() const;

    /**
     * @brief 重要フラグが立っているか判定する
     * @return userdataに重要フラグが含まれる場合はtrue
     */
    Q_INVOKABLE bool isImportant() const;

    /**
     * @brief スナップショット種別を文字列に変換する
     * @param type 変換対象の種別
     * @return 種別を表す文字列
     */
    static QString snapshotTypeToString(SnapshotType type);

    /**
     * @brief 文字列をスナップショット種別に変換する
     * @param str 変換対象の文字列
     * @return 対応する種別、未知の文字列ではSingle
     */
    static SnapshotType stringToSnapshotType(const QString &str);

    /**
     * @brief クリーンアップアルゴリズムを文字列に変換する
     * @param algo 変換対象のアルゴリズム
     * @return アルゴリズムを表す文字列
     */
    static QString cleanupAlgorithmToString(CleanupAlgorithm algo);

    /**
     * @brief 文字列をクリーンアップアルゴリズムに変換する
     * @param str 変換対象の文字列
     * @return 対応するアルゴリズム、未知の文字列ではNone
     */
    static CleanupAlgorithm stringToCleanupAlgorithm(const QString &str);

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
