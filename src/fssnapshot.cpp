#include "fssnapshot.h"

/**
 * @brief FsSnapshotオブジェクトを構築
 *
 * スナップショットのメタデータを保持するデータクラスのコンストラクタ。
 * Snapperから取得したスナップショット情報をQtオブジェクトとして管理する。
 *
 * @param number スナップショット番号
 * @param snapshotType スナップショットタイプ (Single/Pre/Post)
 * @param previousNumber 前のスナップショット番号 (Postスナップショットの場合)
 * @param timestamp スナップショット作成日時
 * @param user スナップショット作成ユーザー名
 * @param cleanupAlgo クリーンアップアルゴリズム (None/Number/Timeline)
 * @param description スナップショットの説明文
 * @param userdata カスタムユーザーデータマップ
 * @param parent 親QObjectポインタ
 */
FsSnapshot::FsSnapshot(int number,
                       SnapshotType snapshotType,
                       int previousNumber,
                       const QDateTime &timestamp,
                       const QString &user,
                       CleanupAlgorithm cleanupAlgo,
                       const QString &description,
                       const QVariantMap &userdata,
                       QObject *parent)
    : QObject(parent)
    , m_number(number)
    , m_snapshotType(snapshotType)
    , m_previousNumber(previousNumber)
    , m_timestamp(timestamp)
    , m_user(user)
    , m_cleanupAlgo(cleanupAlgo)
    , m_description(description)
    , m_userdata(userdata)
{
}

/**
 * @brief スナップショットタイプを文字列で取得
 *
 * スナップショットタイプの列挙値を人間が読める文字列形式に変換する。
 * QMLバインディングで使用される。
 *
 * @return スナップショットタイプ文字列 ("single", "pre", "post")
 */
QString FsSnapshot::snapshotTypeString() const
{
    return snapshotTypeToString(m_snapshotType);
}

/**
 * @brief クリーンアップアルゴリズムを文字列で取得
 *
 * クリーンアップアルゴリズムの列挙値を文字列形式に変換する。
 * QMLバインディングで使用される。
 *
 * @return クリーンアップアルゴリズム文字列 ("number", "timeline", または空文字列)
 */
QString FsSnapshot::cleanupAlgoString() const
{
    return cleanupAlgorithmToString(m_cleanupAlgo);
}

/**
 * @brief スナップショットが重要とマークされているかチェック
 *
 * ユーザーデータマップ内の"important"キーの値が"yes"かどうかを判定する。
 * 重要なスナップショットはUI上で特別な表示がされる。
 *
 * @return 重要なスナップショットの場合true、それ以外はfalse
 */
bool FsSnapshot::isImportant() const
{
    return m_userdata.value("important").toString() == QStringLiteral("yes");
}

/**
 * @brief スナップショットタイプ列挙値を文字列に変換
 *
 * SnapshotType列挙値をSnapperが使用する文字列形式に変換する静的ヘルパー関数。
 *
 * @param type 変換するスナップショットタイプ
 *
 * @return スナップショットタイプ文字列 ("single", "pre", "post")、
 *         不明な値の場合は空文字列
 */
QString FsSnapshot::snapshotTypeToString(SnapshotType type)
{
    switch (type) {
    case SnapshotType::Single:
        return QStringLiteral("single");
    case SnapshotType::Pre:
        return QStringLiteral("pre");
    case SnapshotType::Post:
        return QStringLiteral("post");
    }
    return QString();
}

/**
 * @brief 文字列をスナップショットタイプ列挙値に変換
 *
 * Snapperから取得した文字列をSnapshotType列挙値に変換する静的ヘルパー関数。
 * 不明な文字列が渡された場合はデフォルトでSingleを返す。
 *
 * @param str 変換する文字列 ("single", "pre", "post")
 *
 * @return 対応するSnapshotType列挙値、
 *         不明な文字列の場合はSnapshotType::Single
 */
FsSnapshot::SnapshotType FsSnapshot::stringToSnapshotType(const QString &str)
{
    if (str == QStringLiteral("single"))
        return SnapshotType::Single;
    else if (str == QStringLiteral("pre"))
        return SnapshotType::Pre;
    else if (str == QStringLiteral("post"))
        return SnapshotType::Post;

    return SnapshotType::Single;
}

/**
 * @brief クリーンアップアルゴリズム列挙値を文字列に変換
 *
 * CleanupAlgorithm列挙値をSnapperが使用する文字列形式に変換する静的ヘルパー関数。
 * None値の場合は空文字列を返す。
 *
 * @param algo 変換するクリーンアップアルゴリズム
 *
 * @return クリーンアップアルゴリズム文字列 ("number", "timeline")、
 *         Noneの場合は空文字列
 */
QString FsSnapshot::cleanupAlgorithmToString(CleanupAlgorithm algo)
{
    switch (algo) {
    case CleanupAlgorithm::None:
        return QString();
    case CleanupAlgorithm::Number:
        return QStringLiteral("number");
    case CleanupAlgorithm::Timeline:
        return QStringLiteral("timeline");
    }
    return QString();
}

/**
 * @brief 文字列をクリーンアップアルゴリズム列挙値に変換
 *
 * Snapperから取得した文字列をCleanupAlgorithm列挙値に変換する静的ヘルパー関数。
 * 不明な文字列または空文字列が渡された場合はNoneを返す。
 *
 * @param str 変換する文字列 ("number", "timeline")
 *
 * @return 対応するCleanupAlgorithm列挙値、
 *         不明な文字列の場合はCleanupAlgorithm::None
 */
FsSnapshot::CleanupAlgorithm FsSnapshot::stringToCleanupAlgorithm(const QString &str)
{
    if (str == QStringLiteral("number"))
        return CleanupAlgorithm::Number;
    else if (str == QStringLiteral("timeline"))
        return CleanupAlgorithm::Timeline;

    return CleanupAlgorithm::None;
}
