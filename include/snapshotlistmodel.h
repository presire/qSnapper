#ifndef SNAPSHOTLISTMODEL_H
#define SNAPSHOTLISTMODEL_H

/**
 * @file snapshotlistmodel.h
 * @brief スナップショット一覧をQMLへ公開するリストモデル
 *
 * SnapperServiceから取得した一覧をQAbstractListModelとしてQMLのListViewへ公開する
 * 取得は非同期で行い、取得中と操作中の状態をloadingとbusyで通知する
 *
 * 利用する側 : Main.qmlとスナップショット一覧ページ
 * 入力 : SnapperServiceのシグナルと、QMLからのQ_INVOKABLE呼び出し
 * 出力 : モデルのロール、count、loading、busy、各操作の完了シグナル
 * 依存 : SnapperService、userdataparser.h、fssnapshot.h、QtQml
 */

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include "fssnapshot.h"

class SnapperService;

/**
 * @class SnapshotListModel
 * @brief スナップショット一覧を提供するリストモデル
 *
 * SnapperServiceから取得したFsSnapshotリストをQAbstractListModelとしてQMLへ公開する
 * 作成・削除・ロールバック・変更操作をQMLから呼び出し可能にし、件数プロパティやロール定義を通じてViewとのバインディングを担う
 */
class SnapshotListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT                                                         // qmllintが型を解決できるように、ビルド時の型情報 (qmltypes) へ出力する
    Q_PROPERTY(int count READ count NOTIFY countChanged)                // スナップショット件数
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)         // 一覧を取得中かどうか
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)                  // 作成・変更・削除・ロールバックの処理中かどうか

public:
    /**
     * @enum SnapshotRoles
     * @brief QMLへ公開するスナップショットのロール
     */
    enum SnapshotRoles {
        NumberRole = Qt::UserRole + 1,  // 番号ロール
        SnapshotTypeRole,               // スナップショット種別ロール
        PreviousNumberRole,             // 紐付け元番号ロール
        TimestampRole,                  // タイムスタンプロール
        UserRole,                       // ユーザ名ロール
        CleanupAlgoRole,                // クリーンアップアルゴリズムロール
        DescriptionRole,                // 説明文ロール
        SnapshotTypeStringRole,         // スナップショット種別文字列ロール
        CleanupAlgoStringRole,          // クリーンアップアルゴリズム文字列ロール
        UserdataRole                    // ユーザデータロール
    };

    /**
     * @brief SnapshotListModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit SnapshotListModel(QObject *parent = nullptr);

    /**
     * @brief SnapshotListModelを破棄する
     */
    ~SnapshotListModel();

    /**
     * @brief 行数を取得する
     * @param parent 親インデックス (リストモデルでは未使用)
     * @return スナップショット件数
     */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    /**
     * @brief 指定インデックスのデータを取得する
     * @param index 対象インデックス
     * @param role 取得するロール
     * @return ロールに対応する値、範囲外の場合は無効なQVariant
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    /**
     * @brief ロール名マップを取得する
     * @return ロール番号からロール名へのマップ
     */
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief スナップショット件数を取得する
     * @return スナップショット件数
     */
    int count() const { return m_snapshots.count(); }

    /**
     * @brief 一覧を取得中かどうかを取得する
     * @return 最新のrefresh()の応答を待っている場合はtrue
     */
    bool loading() const { return m_loading; }

    /**
     * @brief 作成・変更・削除・ロールバックの処理中かどうかを取得する
     * @return SnapperServiceが処理中の操作を持つ場合はtrue
     */
    bool busy() const;

    /**
     * @brief 一覧を非同期に更新する
     *
     * SnapperServiceへ全スナップショットの取得を要求し、応答を受け取ってからモデルへ反映する
     * 連続して呼ばれた場合は、最後の要求の応答だけを反映する
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief 指定番号のスナップショットの編集用の情報を、現在の一覧から取得する
     * @param number スナップショット番号
     * @return number、description、cleanup、userdataを持つマップ、一覧に無い場合は空のマップ
     * @note 同期のD-Bus呼び出しでGUIを止めないよう、取得済みの一覧だけを参照する
     */
    Q_INVOKABLE QVariantMap snapshotInfo(int number) const;

    /**
     * @brief ユーザデータの入力文字列を検証し、キーと値のマップへ変換する
     * @param text 改行区切りで key=value を並べた文字列
     * @return ok (成功したか)、userdata (キーと値のマップ)、error (失敗理由の翻訳済み文言) を持つマップ
     * @note 解析の規則はqsnapper::userdata::parse()を参照
     */
    Q_INVOKABLE QVariantMap parseUserdata(const QString &text) const;

    /**
     * @brief 単発スナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createSingleSnapshot(const QString &description,
                                          const QString &cleanup,
                                          bool important,
                                          const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Preスナップショットを作成する
     * @param description 説明文
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createPreSnapshot(const QString &description,
                                       const QString &cleanup,
                                       bool important,
                                       const QVariantMap &userdata = QVariantMap());

    /**
     * @brief Postスナップショットを作成する
     * @param description 説明文
     * @param previousNumber 紐付け元のPreスナップショット番号
     * @param cleanup クリーンアップアルゴリズム ("" / "number" / "timeline"、未知の値は"")
     * @param important 重要フラグ
     * @param userdata ユーザデータマップ
     */
    Q_INVOKABLE void createPostSnapshot(const QString &description,
                                        int previousNumber,
                                        const QString &cleanup,
                                        bool important,
                                        const QVariantMap &userdata = QVariantMap());

    /**
     * @brief 選択中のスナップショットから、Postを作成できるPreスナップショットの番号を求める
     * @param selectedNumbers 選択中のスナップショット番号の一覧
     * @return 選択がPreスナップショット1件だけで、そのPreに対応するPostが一覧にまだ無い場合はPreの番号、それ以外は-1
     */
    Q_INVOKABLE int pendingPreNumber(const QVariantList &selectedNumbers) const;

    /**
     * @brief 指定番号へロールバックする
     * @param number ロールバック先のスナップショット番号
     * @note 一覧が現在のconfigから取得し終えたものでない場合は要求せず、失敗のシグナルで知らせる
     */
    Q_INVOKABLE void rollbackSnapshot(int number);

    /**
     * @brief 単一スナップショットを削除する
     * @param number 削除するスナップショット番号
     * @note 一覧が現在のconfigから取得し終えたものでない場合は要求せず、失敗のシグナルで知らせる
     */
    Q_INVOKABLE void deleteSnapshot(int number);

    /**
     * @brief 複数スナップショットを一括削除する
     * @param numbers 削除するスナップショット番号の一覧
     * @note 複数件の場合は、認証を1回で済ませるため、番号ごとに削除せずDeleteSnapshotsを1回だけ呼び出す
     *       1件だけの場合は、DeleteSnapshotを呼び出す
     *       DeleteSnapshotは、Polkitが認可を一時保持する間は再認証を求めない
     *       1未満の番号と重複した番号は、D-Busを呼び出す前に除外する
     *       一覧が現在のconfigから取得し終えたものでない場合は要求せず、snapshotsDeletionFailedで知らせる
     */
    Q_INVOKABLE void deleteSnapshots(const QVariantList &numbers);

    /**
     * @brief スナップショット情報を変更する
     * @param number 変更対象のスナップショット番号
     * @param description 新しい説明文
     * @param cleanup 新しいクリーンアップアルゴリズム文字列
     * @param userdata 新しいユーザデータマップ
     * @note 一覧が現在のconfigから取得し終えたものでない場合は要求せず、失敗のシグナルで知らせる
     */
    Q_INVOKABLE void modifySnapshot(int number,
                                    const QString &description,
                                    const QString &cleanup,
                                    const QVariantMap &userdata);

signals:
    /**
     * @brief 件数が変更されたときに発行する
     */
    void countChanged();

    /**
     * @brief 一覧の取得中かどうかが変わったときに発行する
     */
    void loadingChanged();

    /**
     * @brief 一覧の取得がD-Busエラーで失敗したときに発行する
     * @param error 利用者へ表示する失敗理由 (SnapperService::errorTextで変換した文言、本文が空のエラーでは汎用の文言)
     * @note 取得に失敗すると一覧を空にするため、通知しなければ利用者には0件と区別できない
     *       再読み込みを続けて要求した場合は、最後の要求に対する失敗だけを通知し、古い要求の失敗は通知しない
     */
    void listLoadFailed(const QString &error);

    /**
     * @brief 処理中かどうかが変わったときに発行する
     */
    void busyChanged();

    /**
     * @brief スナップショット作成成功時に発行する
     */
    void snapshotCreated();

    /**
     * @brief スナップショット作成失敗時に発行する
     * @param error 失敗理由
     */
    void snapshotCreationFailed(const QString &error);

    /**
     * @brief ロールバック完了時に発行する
     */
    void rollbackCompleted();

    /**
     * @brief ロールバック失敗時に発行する
     * @param error 失敗理由
     */
    void rollbackFailed(const QString &error);

    /**
     * @brief 単一削除成功時に発行する
     * @param number 削除されたスナップショット番号
     */
    void snapshotDeleted(int number);

    /**
     * @brief 単一削除失敗時に発行する
     * @param number 削除対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotDeletionFailed(int number, const QString &error);

    /**
     * @brief 複数削除完了時に発行する
     * @param successCount 削除を確認できた件数
     * @param failureCount 削除を確認できなかった件数
     * @note failureCountが0より大きい場合は一部失敗であり、成功として扱ってはならない
     */
    void snapshotsDeletionCompleted(int successCount, int failureCount);

    /**
     * @brief 複数削除を開始できなかった、または応答がエラーのときに発行する
     * @param error 利用者へ表示する失敗理由
     * @note この場合はsnapshotsDeletionCompletedを発行しない (成功件数を知らせる手段がないため)
     */
    void snapshotsDeletionFailed(const QString &error);

    /**
     * @brief 変更成功時に発行する
     * @param number 変更されたスナップショット番号
     */
    void snapshotModified(int number);

    /**
     * @brief 変更失敗時に発行する
     * @param number 変更対象のスナップショット番号
     * @param error 失敗理由
     */
    void snapshotModificationFailed(int number, const QString &error);

private slots:
    /**
     * @brief 作成成功通知を処理する
     * @param snapshot 作成されたスナップショット
     */
    void onSnapshotCreated(FsSnapshot *snapshot);

    /**
     * @brief 作成失敗通知を処理する
     * @param error 失敗理由
     */
    void onSnapshotCreationFailed(const QString &error);

    /**
     * @brief ロールバック完了通知を処理する
     */
    void onRollbackCompleted();

    /**
     * @brief ロールバック失敗通知を処理する
     * @param error 失敗理由
     */
    void onRollbackFailed(const QString &error);

    /**
     * @brief 削除成功通知を処理する
     * @param number 削除されたスナップショット番号
     */
    void onSnapshotDeleted(int number);

    /**
     * @brief 削除失敗通知を処理する
     * @param number 削除対象のスナップショット番号
     * @param error 失敗理由
     */
    void onSnapshotDeletionFailed(int number, const QString &error);

    /**
     * @brief 変更成功通知を処理する
     * @param number 変更されたスナップショット番号
     * @note 変更内容を一覧へ反映するため、一覧を取得し直してからsnapshotModifiedを転送する
     */
    void onSnapshotModified(int number);

    /**
     * @brief 変更失敗通知を処理する
     * @param number 変更対象のスナップショット番号
     * @param error 失敗理由
     */
    void onSnapshotModificationFailed(int number, const QString &error);

private:
    /**
     * @brief 表示中の一覧が、現在のconfigから取得し終えたものかを判定する
     * @return 取得が完了しており、取得元のconfigが現在のconfigと同じ場合true
     * @note 番号はconfigごとに振られるため、別のconfigの一覧の番号で操作すると、同じ番号の別のスナップショットを変更・削除してしまう
     */
    bool isListForCurrentConfig() const;

    /**
     * @brief 一覧が現在のconfigのものではない、または取得中のため操作を受け付けない旨の文言を返す
     * @return 翻訳済みの文言
     */
    QString listOutdatedText() const;

    // モデルデータ
    QList<FsSnapshot*> m_snapshots;     // スナップショットオブジェクトのリスト
    SnapperService *m_snapperService;   // SnapperServiceシングルトンインスタンスへのポインタ
    quint64 m_deletionGeneration;       // 一括削除の世代番号 (古い応答を捨てるため)
    quint64 m_refreshGeneration;        // 一覧取得の世代番号 (古い応答を捨てるため)
    bool m_loading;                     // 最新の一覧取得の応答を待っているかどうか
    QString m_loadedConfig;             // m_snapshotsを取得したconfig名 (取得前や切り替え中は空)
};

#endif // SNAPSHOTLISTMODEL_H
