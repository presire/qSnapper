/**
 * @file filechangeitem.h
 * @brief ファイル変更ツリーの1ノードを表すFileChangeItemの宣言
 *
 * FileChangeModelが保持するツリーの要素であり、パス・変更種別・選択状態を保持する
 */

#ifndef FILECHANGEITEM_H
#define FILECHANGEITEM_H

#include <QString>
#include <QVector>

/**
 * @class FileChangeItem
 * @brief ファイル変更情報を保持するツリーの1ノード
 *
 * 完全パスと変更種別を持ち、親子関係でツリーを構成する
 * ディレクトリの中間ノードは合成であり、実レコードを持たない場合がある
 * 選択状態は復元対象の収集に使い、明示的な選択解除は親の再選択から除外するために保持する
 *
 * m_path              : 完全パス
 * m_changeType        : 変更種別
 * m_statusFlags       : 詳細ステータスフラグ
 * m_children          : 子要素の所有リスト
 * m_parent            : 親要素への非所有ポインタ
 * m_checked           : 選択状態
 * m_explicitlyUnchecked : 明示的な選択解除の記録
 */
class FileChangeItem
{
public:
    /**
     * @brief ファイルシステム上の変更種別
     *
     * 向きは比較条件に依存し、number1からnumber2への差分として解釈する
     * 復元時に向きを取り違えるとcreatedとdeletedが反転するため、ComparisonContextとの一致検証が必須になる
     */
    enum ChangeType {
        Created,    // 新規作成を表す
        Modified,   // 内容または属性の変更を表す
        Deleted,    // 削除を表す
        TypeChanged // ファイル種別の変更を表す
    };

    // ライフサイクル
    /**
     * @brief 変更アイテムを生成する
     * @param path 完全パス
     * @param type 変更種別
     * @param statusFlags 詳細ステータスフラグ
     * @param parent 親アイテムへのポインタ
     */
    explicit FileChangeItem(const QString &path,
                            ChangeType type,
                            const QString &statusFlags = QString(),
                            FileChangeItem *parent = nullptr);
    /**
     * @brief 子アイテムを含む変更アイテムを破棄する
     */
    ~FileChangeItem();

    // ツリー構造
    /**
     * @brief 子アイテムを末尾へ追加する
     * @param child 追加する子アイテム
     */
    void appendChild(FileChangeItem *child);
    /**
     * @brief 指定行の子アイテムを返す
     * @param row 子アイテムの行番号
     * @return 子アイテムへのポインタ
     */
    FileChangeItem *child(int row);
    /**
     * @brief 子アイテム数を返す
     * @return 子アイテム数
     */
    int childCount() const;
    /**
     * @brief 親内での行番号を返す
     * @return 行番号
     */
    int row() const;
    /**
     * @brief 親アイテムを返す
     * @return 親アイテムへのポインタ
     */
    FileChangeItem *parent();

    // 変更情報
    /**
     * @brief 完全パスを返す
     * @return 完全パス
     */
    QString path() const { return m_path; }
    /**
     * @brief パス末尾の名前を返す
     * @return 表示用の名前
     */
    QString name() const;
    /**
     * @brief 変更種別を返す
     * @return 変更種別
     */
    ChangeType changeType() const { return m_changeType; }
    /**
     * @brief 詳細ステータスフラグを返す
     * @return 詳細ステータスフラグ
     */
    QString statusFlags() const { return m_statusFlags; }
    /**
     * @brief ディレクトリかどうかを返す
     * @return ディレクトリの場合はtrue
     */
    bool isDirectory() const;

    // 選択状態
    /**
     * @brief 選択状態を返す
     * @return 選択されている場合はtrue
     */
    bool isChecked() const { return m_checked; }
    /**
     * @brief 選択状態を設定する
     * @param checked 新しい選択状態
     */
    void setChecked(bool checked) { m_checked = checked; }
    /**
     * @brief 明示的な選択解除状態を返す
     * @return 明示的に選択解除された場合はtrue
     */
    bool isExplicitlyUnchecked() const { return m_explicitlyUnchecked; }
    /**
     * @brief 明示的な選択解除状態を設定する
     * @param explicitlyUnchecked 新しい明示的選択解除状態
     */
    void setExplicitlyUnchecked(bool explicitlyUnchecked) { m_explicitlyUnchecked = explicitlyUnchecked; }

private:
    // ファイル変更情報
    QString m_path;                         // ファイルやディレクトリのパス
    ChangeType m_changeType;                // 変更タイプ
    QString m_statusFlags;                  // 詳細ステータスフラグ

    // ツリー構造
    QVector<FileChangeItem*> m_children;    // 子要素のリスト
    FileChangeItem *m_parent;               // 親要素へのポインタ

    // 選択状態
    bool m_checked = false;                 // チェック状態
    bool m_explicitlyUnchecked = false;     // 明示的にチェックを外されたフラグ
};

#endif // FILECHANGEITEM_H
