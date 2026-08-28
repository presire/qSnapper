#ifndef FILECHANGEMODEL_H
#define FILECHANGEMODEL_H

#include <QAbstractItemModel>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QDBusInterface>
#include <functional>

/**
 * @brief ファイル変更情報を保持するアイテムクラス
 */
class FileChangeItem
{
public:
    // ファイルシステム上の変更種別
    enum ChangeType {
        Created,    // 新規作成を表す
        Modified,   // 内容または属性の変更を表す
        Deleted,    // 削除を表す
        TypeChanged // ファイル種別の変更を表す
    };

private:
    // ファイル変更情報
    QString m_path;                         // ファイル/ディレクトリのパス
    ChangeType m_changeType;                // 変更タイプ
    QString m_statusFlags;                  // 詳細ステータスフラグ (例: "cpu..")

    // ツリー構造
    QVector<FileChangeItem*> m_children;    // 子要素のリスト
    FileChangeItem *m_parent;               // 親要素へのポインタ

    // 選択状態
    bool m_checked = false;                 // チェック状態
    bool m_explicitlyUnchecked = false;     // 明示的にチェックを外されたフラグ

public:
    // ライフサイクル
    explicit FileChangeItem(const QString &path,                    // 変更アイテムを生成する
                            ChangeType type,
                            const QString &statusFlags = QString(),
                            FileChangeItem *parent = nullptr);
    ~FileChangeItem();                                              // 子アイテムを含む変更アイテムを破棄する

    // ツリー構造
    void appendChild(FileChangeItem *child);                        // 子アイテムを末尾へ追加する
    FileChangeItem *child(int row);                                 // 指定行の子アイテムを返す
    int childCount() const;                                         // 子アイテム数を返す
    int row() const;                                                // 親内での行番号を返す
    FileChangeItem *parent();                                       // 親アイテムを返す

    // 変更情報
    QString path() const { return m_path; }                         // 完全パスを返す
    QString name() const;                                           // パス末尾の名前を返す
    ChangeType changeType() const { return m_changeType; }          // 変更種別を返す
    QString statusFlags() const { return m_statusFlags; }           // 詳細ステータスフラグを返す
    bool isDirectory() const;                                       // ディレクトリかどうかを返す

    // 選択状態
    bool isChecked() const { return m_checked; }                            // 選択状態を返す
    void setChecked(bool checked) { m_checked = checked; }                  // 選択状態を設定する
    bool isExplicitlyUnchecked() const { return m_explicitlyUnchecked; }    // 明示的な選択解除状態を返す
    void setExplicitlyUnchecked(bool explicitlyUnchecked) { m_explicitlyUnchecked = explicitlyUnchecked; }  // 明示的な選択解除状態を設定する
};

/**
 * @brief 復元計画 (staged restore) のD-Bus転送を抽象化するインターフェース
 *
 * FileChangeModelとD-Bus呼び出しの間に注入可能な継ぎ目を設け、テストではモックtransportで呼び出し順序と結果を検証できるようにする
 * 各メソッドは非同期で、コールバックを必ず1回だけ (即時または後続のイベントループで) 呼び出す
 */
class RestorePlanTransport
{
public:
    /**
     * @brief transportインターフェースを破棄する
     */
    virtual ~RestorePlanTransport();                        // 派生transportを安全に破棄する

    // 復元計画操作
    /**
     * @brief staging計画の作成を要求する (この時点では認証されない)
     *
     * @param configName Snapper設定名
     * @param snapshotNumber スナップショット番号
     * @param restoreMode 復元方式 ("yast" または "direct")
     * @param done 結果コールバック (ok, manifestId, error)
     * @return なし
     */
    virtual void beginPlan(const QString &configName,       // 復元計画の作成を非同期で要求する
                           int snapshotNumber,
                           const QString &restoreMode,
                           std::function<void(bool ok, const QString &manifestId, const QString &error)> done) = 0;

    /**
     * @brief staging計画へ検証済みエントリのチャンクを追加する (この時点では認証されない)
     *
     * @param manifestId 対象計画のマニフェストID
     * @param paths エントリのパスリスト
     * @param changeTypes パスに対応する変更タイプリスト
     * @param done 結果コールバック (ok, error)
     * @return なし
     */
    virtual void stageEntries(const QString &manifestId,    // 検証済みエントリのチャンクを計画へ追加する
                              const QStringList &paths,
                              const QStringList &changeTypes,
                              std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画を凍結して認証と実行を開始させる (Polkitプロンプトはこの呼び出しで1度だけ出る)
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     * @return なし
     */
    virtual void commitPlan(const QString &manifestId,      // 計画を確定して復元を開始する
                            std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画のキャンセルを要求する (次のサーバ側チャンク境界で反映される)
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     * @return なし
     */
    virtual void cancelPlan(const QString &manifestId,      // 計画のキャンセルを非同期で要求する
                            std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画の状態をCSV形式で問い合わせる
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, statusCsv, error)
     * @return なし
     */
    virtual void requestStatus(const QString &manifestId,   // 計画の状態をCSVで取得する
                               std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) = 0;

    // 復元計画シグナルの購読
    /**
     * @brief 復元計画の進捗/完了シグナルとサービス消失通知を受信側に登録する
     *
     * 復元サービスのバス名が消失した場合は、receiverのonRestorePlanServiceVanishedスロットを呼び出さなければならない
     *
     * @param receiver onRestorePlanProgress / onRestorePlanFinished / onRestorePlanServiceVanishedスロットを持つQObject
     * @return 登録に成功した場合はtrue
     */
    virtual bool subscribePlanSignals(QObject *receiver) = 0;   // 復元計画関連シグナルの受信を登録する

    /**
     * @brief 復元計画の進捗 / 完了シグナルとサービス消失通知の受信を解除する
     *
     * subscribePlanSignals()で登録したonRestorePlanServiceVanishedスロットへの
     * 通知も解除しなければならない。
     *
     * @param receiver 登録解除するQObject
     */
    virtual void unsubscribePlanSignals(QObject *receiver) = 0; // 復元計画関連シグナルの受信を解除する
};

/**
 * @brief ファイル変更をツリー構造で表示するためのモデル
 *
 * Snapperの変更出力をFileChangeItemツリーへ変換し、QML向けのロール、
 * 選択状態、ファイル差分取得および復元操作を提供する。
 */
class FileChangeModel : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QString configName READ configName WRITE setConfigName NOTIFY configNameChanged)                       // Snapper設定名を公開する
    Q_PROPERTY(int snapshotNumber READ snapshotNumber WRITE setSnapshotNumber NOTIFY snapshotNumberChanged)           // 対カレント比較用のスナップショット番号を公開する
    Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY hasChangesChanged)                                              // ファイル変更の有無を公開する
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)                                                     // 読み込み状態を公開する
    Q_PROPERTY(int restoreBatchSize READ restoreBatchSize WRITE setRestoreBatchSize NOTIFY restoreBatchSizeChanged)   // 復元チャンクの最大件数を公開する
    Q_PROPERTY(bool useDirectRestore READ useDirectRestore WRITE setUseDirectRestore NOTIFY useDirectRestoreChanged)  // Direct Copy方式の使用設定を公開する

private:
    // スナップショット識別・比較モード
    QString m_configName;                   // Snapper設定名
    int m_snapshotNumber;                   // スナップショット番号 (対カレント比較モード用)
    int m_compareNumber1;                   // 比較元スナップショット番号 (任意2スナップショット比較モード用)
    int m_compareNumber2;                   // 比較先スナップショット番号 (同上)
    bool m_betweenMode;                     // 任意2スナップショット比較モードかどうか

    // 表示・モデル状態
    bool m_flatMode;                        // フラット表示モード (ツリー構築をバイパスし、
                                            // 各変更を m_rootItem 直下に1行ずつ追加する)
                                            // 比較ダイアログ用 (ListView表示)
    FileChangeItem *m_rootItem;             // ツリーのルートアイテム
    bool m_hasChanges;                      // ファイル変更があるかどうか
    bool m_loading;                         // 読み込み中フラグ
    int m_totalFilesCount;                  // 復元対象の総ファイル数
    int m_processedFilesCount;              // 処理済みファイル数

    // D-Bus / Transport
    QDBusInterface *m_dbusInterface;                     // D-Busインターフェース
    RestorePlanTransport *m_defaultRestorePlanTransport; // 所有する既定のtransport (system bus実装)
    RestorePlanTransport *m_restorePlanTransport;        // 現在使用中のtransport (テストから差し替え可能)

    // 復元計画 (staged restore)
    QString m_planManifestId;               // 実行中の復元計画のマニフェストID
    QStringList m_planPaths;                // 検証済みの復元対象パス (選択順を維持)
    QStringList m_planChangeTypes;          // 検証済みの変更タイプ (m_planPathsと対応)
    int m_planNextStageIndex;               // 次にステージングするチャンクの先頭インデックス
    int m_planTotalFiles;                   // 計画開始時に固定した総エントリ数
    int m_planLastProgress;                 // 最後に通知した計画進捗
    bool m_planActive;                      // 復元計画のフローが実行中か
    bool m_planCommitted;                   // 復元計画のcommitを送出済みか
    bool m_planSignalsSubscribed;           // 復元計画シグナルを購読中か
    bool m_planCancelRequested;             // 復元計画のキャンセルを送出済みか

    // 復元制御・エラー状態
    bool m_restoreHasError;                 // 復元エラーフラグ
    bool m_cancelRequested;                 // キャンセル要求フラグ

    // 復元設定
    int m_restoreBatchSize;                 // バッチサイズ (1〜1000)
    bool m_useDirectRestore;                // Direct Copy方式を使用するか

private:
    // モデル管理
    void clearModel();                                                                      // モデルデータをクリアする

    // アイテム取得・変換
    FileChangeItem *getItem(const QModelIndex &index) const;                                // インデックスからアイテムを取得する
    QModelIndex findItemIndex(FileChangeItem *parent, const QString &path) const;           // パスからインデックスを検索する
    FileChangeItem::ChangeType parseChangeType(const QChar &statusChar);                    // ステータス文字から変更タイプを解析する
    static QString changeTypeToString(FileChangeItem::ChangeType type);                     // 変更タイプを文字列に変換する
    static QString normalizeRestorePlanPath(const QString &path);                           // 復元パスを正規化する
    static bool isValidRestorePlanEntry(const QString &path, const QString &changeType);    // 復元エントリの妥当性を検証する

    // チェック状態・収集
    void collectCheckedItems(FileChangeItem *parent, QStringList &paths) const;                                     // チェック済みパスのみを収集する
    void collectCheckedItemsWithTypes(FileChangeItem *parent, QStringList &paths, QStringList &changeTypes) const;  // チェック済みパスと変更タイプを収集する
    void collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const;                                // 全ファイルパスを再帰的に収集する
    void setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked);                     // チェック状態を再帰的に設定する

    // 復元計画制御
    void stageNextPlanChunk();                                  // 次のチャンクをステージングする
    void commitRestorePlan();                                   // 復元計画をコミットする
    void resetRestorePlanState();                               // 復元計画の状態をリセットする
    void finishRestorePlanWithError(const QString &message);    // エラーで復元計画を終了する

    // 復元計画コールバック
    void onPlanBeginFinished(bool ok, const QString &manifestId, const QString &error); // 計画開始完了時の処理
    void onPlanStageFinished(bool ok, const QString &error);                            // チャンクステージング完了時の処理
    void onPlanCommitFinished(bool ok, const QString &error);                           // 計画コミット完了時の処理

    // D-Bus
    bool reconnectDbus();                                       // D-Busサービスへの再接続を試みる

protected:
    /**
     * @brief 変更レコードからモデルデータを構築する
     *
     * レガシーの改行区切りステータス出力を1回だけ解析し、指定した表示モードの
     * モデルツリーを構築する。テストからもD-Busを介さずに検証できるよう保護する。
     *
     * @param changeOutput Snapperの変更出力
     * @param flatMode trueの場合はフラットモデルを構築する
     */
    void setupModelData(const QString &changeOutput, bool flatMode);  // 変更出力からモデルデータを構築する

public:
    // QMLへ公開するモデルデータのロール
    enum Roles {
        PathRole = Qt::UserRole + 1,    // 完全パスを返すロール
        NameRole,                       // 表示名を返すロール
        ChangeTypeRole,                 // 変更種別を返すロール
        IsDirectoryRole,                // ディレクトリ判定を返すロール
        IsCheckedRole,                  // 選択状態を返すロール
        StatusFlagsRole                 // 詳細ステータスフラグを返すロール
    };

    // ライフサイクル
    explicit FileChangeModel(QObject *parent = nullptr);    // ファイル変更モデルを生成する
    ~FileChangeModel();                                     // モデルと所有リソースを破棄する

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;   // 指定位置のモデルインデックスを返す
    QModelIndex parent(const QModelIndex &child) const override;                                        // 子インデックスの親を返す
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;                             // 親の子要素数を返す
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;                          // モデル列数を返す
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;                 // 指定ロールのデータを返す
    QHash<int, QByteArray> roleNames() const override;                                                  // QML向けロール名を返す

    // プロパティ
    QString configName() const { return m_configName; }             // Snapper設定名を返す
    void setConfigName(const QString &name);                        // Snapper設定名を更新する

    int snapshotNumber() const { return m_snapshotNumber; }         // 対カレント比較用の番号を返す
    void setSnapshotNumber(int number);                             // 対カレント比較用の番号を更新する

    bool hasChanges() const { return m_hasChanges; }                // ファイル変更の有無を返す
    bool isLoading() const { return m_loading; }                    // 読み込み中かどうかを返す

    int restoreBatchSize() const { return m_restoreBatchSize; }     // 復元チャンクの最大件数を返す
    void setRestoreBatchSize(int size);                             // 復元チャンクの最大件数を更新する

    bool useDirectRestore() const { return m_useDirectRestore; }    // Direct Copy方式の使用設定を返す
    void setUseDirectRestore(bool use);                             // Direct Copy方式の使用設定を更新する

    // 公開メソッド
    Q_INVOKABLE void loadChanges();                                                     // 対カレントの変更を読み込む
    Q_INVOKABLE void loadChangesBetween(int number1, int number2, bool flat = false);   // 2つのスナップショット間の変更を読み込む
    Q_INVOKABLE void getFileDiffAndDetails(const QString &filePath);                    // 指定ファイルの差分と詳細を取得する
    Q_INVOKABLE void setItemChecked(const QString &filePath, bool checked);             // 指定パスの選択状態を設定する
    Q_INVOKABLE QStringList getCheckedItems() const;                                    // 選択済みパスの一覧を返す
    Q_INVOKABLE bool restoreCheckedItems();                                             // 選択済み項目の復元を開始する
    Q_INVOKABLE void restoreSingleFile(const QString &filePath);                        // 指定ファイルの復元を開始する
    Q_INVOKABLE void cancelRestore();                                                   // 進行中の復元をキャンセルする

    /**
     * @brief テスト専用に復元計画transportを差し替える
     * @param transport 差し込むtransport (nullptrの場合は既定transportへ戻す)
     */
    void setRestorePlanTransportForTesting(RestorePlanTransport *transport);            // テスト用transportを設定する

signals:
    // 状態および操作結果の通知
    void configNameChanged();                                   // Snapper設定名の変更を通知する
    void snapshotNumberChanged();                               // スナップショット番号の変更を通知する
    void hasChangesChanged();                                   // ファイル変更の有無の更新を通知する
    void loadingChanged();                                      // 読み込み状態の更新を通知する
    void errorOccurred(const QString &message);                 // 操作エラーを通知する
    void fileDiffAndDetailsReady(const QString &filePath,       // ファイル差分と詳細の取得完了を通知する
                                 const QVariantMap &details,
                                 const QString &diff);
    void restoreProgress(int current,                           // 復元の進捗を通知する
                         int total,
                         const QString &filePath);
    void restoreCompleted(bool success);                        // 復元処理の完了を通知する
    void restoreBatchSizeChanged();                             // 復元チャンク件数の変更を通知する
    void useDirectRestoreChanged();                             // Direct Copy方式設定の変更を通知する

private slots:
    // D-Bus復元通知の処理
    void onRestoreProgress(int current, int total, const QString &filePath);                                        // 従来復元の進捗通知を処理する
    void onRestorePlanProgress(const QString &manifestId, int current, int total, const QString &filePath);         // 復元計画の進捗通知を処理する
    void onRestorePlanFinished(const QString &manifestId, const QString &terminalState, const QString &message);    // 復元計画の完了通知を処理する

    void onRestorePlanServiceVanished(); // 復元サービスの消失を失敗として処理する
};

#endif // FILECHANGEMODEL_H
