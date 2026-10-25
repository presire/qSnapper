/**
 * @file filechangemodel.h
 * @brief ファイル変更ツリーの中心となるFileChangeModelの宣言
 *
 * Snapperの変更出力をFileChangeItemツリーへ変換して、QML向けのロールと選択状態とファイル差分取得と復元操作を提供する
 * 変更種別の向きに関する不変条件はComparisonContextが保持する
 *
 * 利用する側 : ファイル変更ページと差分・復元ダイアログ
 * 入力 : QMLからのプロパティ設定とQ_INVOKABLE呼び出し
 * 出力 : モデルのロール、比較・読み込み・復元の状態プロパティ、各処理の完了シグナル
 * 依存 : Qt D-Bus、RestorePlanTransport、restoreplanstatus.h、FileChangeItem
 */

#ifndef FILECHANGEMODEL_H
#define FILECHANGEMODEL_H

#include <QAbstractItemModel>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>
#include <QDBusInterface>
#include <functional>
#include "filechangeitem.h"
#include "restoreplantransport.h"

/**
 * @class FileChangeModel
 * @brief ファイル変更をツリー構造で表示するためのモデル
 *
 * Snapperの変更出力をFileChangeItemツリーへ変換して、QML向けのロールと選択状態とファイル差分取得と復元操作を提供する
 * 復元はstaged restore計画でのみ行い、commit時にPolkit認証を1度だけ受ける
 *
 * 公開済みエントリのchangeTypeは比較の向きに依存するため
 * 復元時には由来の比較条件と現在の比較条件の完全一致を要求する
 * 向きを取り違えるとcreatedとdeletedが反転し、復元対象外の削除に繋がる
 */
class FileChangeModel : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QString configName READ configName WRITE setConfigName NOTIFY configNameChanged)                       // Snapper設定名を公開する
    Q_PROPERTY(int snapshotNumber READ snapshotNumber WRITE setSnapshotNumber NOTIFY snapshotNumberChanged)           // 対カレント比較用のスナップショット番号を公開する
    Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY hasChangesChanged)                                              // ファイル変更の有無を公開する
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)                                                     // 読み込み状態を公開する
    Q_PROPERTY(bool loaded READ isLoaded NOTIFY loadedChanged)                                                        // 読み込みに成功したかどうか (0件の成功も含む) を公開する
    Q_PROPERTY(bool truncated READ isTruncated NOTIFY truncatedChanged)                                               // 上限を超えて一覧を省略したかどうかを公開する
    Q_PROPERTY(int omittedCount READ omittedCount NOTIFY truncatedChanged)                                            // 上限を超えて省略した件数を公開する
    Q_PROPERTY(QVariantMap restoreResult READ restoreResult NOTIFY restoreResultChanged)                              // 直近の復元計画の結果 (件数と失敗位置) を公開する
    Q_PROPERTY(int restoreBatchSize READ restoreBatchSize WRITE setRestoreBatchSize NOTIFY restoreBatchSizeChanged)   // 復元チャンクの最大件数を公開する
    Q_PROPERTY(bool useDirectRestore READ useDirectRestore WRITE setUseDirectRestore NOTIFY useDirectRestoreChanged)  // Direct Copy方式の使用設定を公開する

public:
    /**
     * @brief 変更一覧の取り込み上限
     *
     * D-Busの応答は全文を受け取った後でないと切り詰められないが、
     * 解析とツリー構築 (エントリごとに複数のオブジェクトを確保する) がメモリと時間の大半を占めるため、ここで打ち切る
     * 上限は、通常の利用では到達しない大きさで、UIが応答しなくなる前に止まる値として選んだ
     *   エントリ数 : 500000件
     *   文字数     : 256 MiB相当 (QStringの1文字は2バイトのため、268435456文字を上限の目安とする)
     */
    static constexpr int MAX_CHANGE_ENTRIES = 500000;                       // 取り込む変更エントリ数の上限
    static constexpr qsizetype MAX_CHANGE_TEXT_CHARS = 256LL * 1024 * 1024; // 取り込む変更一覧の文字数の上限

    /**
     * @brief 文字列を返すD-Bus呼び出しの非同期結果コールバック
     * @param ok 呼び出しに成功した場合はtrue
     * @param value 成功時の応答文字列
     * @param error 失敗時のエラーメッセージ
     */
    using AsyncStringReplyHandler = std::function<void(bool ok, const QString &value, const QString &error)>;

private:
    /**
     * @struct ComparisonContext
     * @brief モデルへ公開したエントリがどの比較で得られたかを識別する値
     *
     * エントリのchangeTypeはnumber1からnumber2への向きでしか正しく解釈できないため
     * 復元時には由来の比較条件と現在の比較条件が完全に一致している必要がある
     * number1は常に、その向きにおける復元元スナップショットを指す
     */
    struct ComparisonContext
    {
        QString configName;                         // Snapper設定名
        bool betweenMode = false;                   // 2スナップショット比較モードかどうか
        int number1 = 0;                            // 比較元スナップショット番号
        int number2 = 0;                            // 比較先スナップショット番号
        bool flatMode = false;                      // フラット表示として構築したかどうか
        bool filterUnchangedAgainstCurrent = false; // 現在のシステムと差異が無いエントリを除外したかどうか

        /** @brief 比較条件が完全に一致するかを返す */
        bool operator==(const ComparisonContext &other) const;
    };

    // スナップショット識別と比較モード
    QString m_configName;                       // Snapper設定名
    int m_snapshotNumber;                       // スナップショット番号
    int m_compareNumber1;                       // 比較元スナップショット番号
    int m_compareNumber2;                       // 比較先スナップショット番号
    bool m_betweenMode;                         // 任意2スナップショット比較モードかどうか
    bool m_filterUnchangedAgainstCurrent = false; // 現在のシステムと差異が無いエントリを表示対象から除外するかどうか

    // 公開済みエントリの由来と要求世代
    ComparisonContext m_loadedComparison;       // 現在モデルへ公開済みのエントリの比較条件
    bool m_loadedComparisonValid = false;       // 公開済みエントリが読み込み成功に由来するか
    quint64 m_loadRequestGeneration = 0;        // 変更一覧要求の世代
    quint64 m_diffRequestGeneration = 0;        // ファイル差分要求の世代

    // 表示とモデル状態
    bool m_flatMode;                            // フラット表示モード
    FileChangeItem *m_rootItem;                 // ツリーのルートアイテム
    bool m_hasChanges;                          // ファイル変更があるかどうか
    bool m_loading;                             // 読み込み中フラグ
    bool m_truncated = false;                   // 上限を超えたため一覧の後半を省略したかどうか
    int m_omittedCount = 0;                     // 上限を超えて省略した行数
    int m_maxChangeEntries = MAX_CHANGE_ENTRIES;            // 取り込む変更エントリ数の上限 (テストで差し替える)
    qsizetype m_maxChangeTextChars = MAX_CHANGE_TEXT_CHARS; // 取り込む変更一覧の文字数の上限 (テストで差し替える)
    int m_totalFilesCount;                      // 復元対象の総ファイル数
    int m_processedFilesCount;                  // 処理済みファイル数

    // D-Busとtransport
    QDBusInterface *m_dbusInterface;                     // D-Busインターフェース
    RestorePlanTransport *m_defaultRestorePlanTransport; // 所有する既定のtransport
    RestorePlanTransport *m_restorePlanTransport;        // 現在使用中のtransport

    // 復元計画
    QString m_planManifestId;               // 実行中の復元計画のマニフェストID
    QStringList m_planPaths;                // 検証済みの復元対象パス
    QStringList m_planChangeTypes;          // 検証済みの変更タイプ
    int m_planNextStageIndex;               // 次にステージングするチャンクの先頭インデックス
    int m_planTotalFiles;                   // 計画開始時に固定した総エントリ数
    int m_planLastProgress;                 // 最後に通知した計画進捗
    bool m_planActive;                      // 復元計画のフローが実行中か
    bool m_planCommitted;                   // 復元計画のcommitを送出済みか
    bool m_planSignalsSubscribed;           // 復元計画シグナルを購読中か
    bool m_planCancelRequested;             // 復元計画のキャンセルを送出済みか

    // 復元結果
    QVariantMap m_restoreResult;            // 直近の復元計画の結果 (GetRestorePlanStatusの解析結果)
    quint64 m_restoreResultGeneration = 0;  // 復元結果要求の世代 (新しい復元の開始後に届いた古い応答を捨てる)

    // 復元制御とエラー状態
    bool m_restoreHasError;                 // 復元エラーフラグ
    bool m_cancelRequested;                 // キャンセル要求フラグ

    // 復元設定
    int m_restoreBatchSize;                 // バッチサイズ
    bool m_useDirectRestore;                // Direct Copy方式を使用するか

private:
    /** @brief モデルデータをクリアする */
    void clearModel();

    /** @brief フィルタ付きの変更一覧を2要求で読み込む */
    void requestFilteredChangeList(const ComparisonContext &requestedContext,
                                   quint64 requestGeneration);
    /** @brief 絞り込んだ変更一覧をモデルへ公開する */
    void publishFilteredChangeList(const ComparisonContext &requestedContext,
                                   const QString &betweenOutput,
                                   const QString &currentOutput);

    /** @brief インデックスからアイテムを取得する */
    FileChangeItem *getItem(const QModelIndex &index) const;
    /** @brief パスからインデックスを検索する */
    QModelIndex findItemIndex(FileChangeItem *parent, const QString &path) const;
    /** @brief ステータス文字から変更タイプを解析する */
    FileChangeItem::ChangeType parseChangeType(const QChar &statusChar);
    /** @brief 変更タイプを文字列に変換する */
    static QString changeTypeToString(FileChangeItem::ChangeType type);
    /** @brief 復元パスを正規化する */
    static QString normalizeRestorePlanPath(const QString &path);
    /** @brief 復元エントリの妥当性を検証する */
    static bool isValidRestorePlanEntry(const QString &path, const QString &changeType);
    /** @brief 配下の実レコードが全てCreatedかを返す */
    static bool hasOnlyCreatedRecordsBeneath(FileChangeItem *item);

    /** @brief 現在の比較条件を返す */
    ComparisonContext currentComparisonContext() const;
    /** @brief 公開済みエントリとその由来を無効化する */
    void invalidateLoadedComparison();
    /** @brief 公開済みエントリから復元してよいかを返す */
    bool hasRestorableLoadedComparison() const;

    /** @brief 現在の比較条件に対応する復元元番号を返す */
    int currentRestoreSourceNumber() const;
    /** @brief 復元元と現在の比較条件の整合性を検証する */
    bool isRestoreSourceConsistent(int sourceSnapshotNumber) const;

    /** @brief チェック済みパスのみを収集する */
    void collectCheckedItems(FileChangeItem *parent, QStringList &paths) const;
    /** @brief 指定した項目の配下に、チェック済みの項目が1つでもあるかどうかを調べる */
    bool containsCheckedItem(FileChangeItem *parent) const;
    /** @brief チェック済みパスと変更タイプを収集する */
    void collectCheckedItemsWithTypes(FileChangeItem *parent, QStringList &paths, QStringList &changeTypes) const;
    /** @brief 全ファイルパスを再帰的に収集する */
    void collectAllFilesRecursive(FileChangeItem *parent, QStringList &paths) const;
    /** @brief チェック状態を再帰的に設定する */
    void setItemCheckedRecursive(FileChangeItem *item, const QModelIndex &index, bool checked);

    /** @brief 次のチャンクをステージングする */
    void stageNextPlanChunk();
    /** @brief 復元計画をコミットする */
    void commitRestorePlan();
    /** @brief 復元計画の状態をリセットする */
    void resetRestorePlanState();
    /** @brief エラーで復元計画を終了する */
    void finishRestorePlanWithError(const QString &message);

    /** @brief 復元対象を検証してstaged restore計画を開始する */
    bool startRestorePlan(int sourceSnapshotNumber,
                          const QStringList &checkedPaths,
                          const QStringList &checkedChangeTypes);
    /** @brief 計画開始完了時の処理 */
    void onPlanBeginFinished(bool ok, const QString &manifestId, const QString &error);
    /** @brief チャンクステージング完了時の処理 */
    void onPlanStageFinished(bool ok, const QString &error);
    /** @brief 計画コミット完了時の処理 */
    void onPlanCommitFinished(bool ok, const QString &error);

    /** @brief D-Busサービスへの再接続を試みる */
    bool reconnectDbus();

protected:
    /**
     * @brief 変更レコードからモデルデータを構築する
     * @param changeOutput Snapperの変更出力
     * @param flatMode trueの場合はフラットモデルを構築する
     */
    void setupModelData(const QString &changeOutput, bool flatMode);

    /**
     * @brief 2スナップショット比較の条件を設定する
     * @param number1 比較元スナップショット番号
     * @param number2 比較先スナップショット番号
     * @param flat trueの場合はフラットモデルを構築する
     * @param filterUnchangedAgainstCurrent trueの場合、現在のシステムと差異が無いエントリを除外する
     */
    void setComparisonRange(int number1, int number2, bool flat,
                            bool filterUnchangedAgainstCurrent = false);

    /**
     * @brief 変更レコード出力から正規化済みパスの集合を抽出する
     * @param changeOutput Snapperの変更出力
     * @return 出力に含まれる正規化済みパスの集合
     */
    static QSet<QString> collectChangedPaths(const QString &changeOutput);

    /**
     * @brief 変更レコード出力を、許可パス集合に含まれる行だけへ絞り込む
     * @param changeOutput 絞り込み対象のSnapperの変更出力
     * @param allowedPaths 表示を許可する正規化済みパスの集合
     * @return 絞り込み後の変更出力
     */
    static QString filterChangeOutputByPaths(const QString &changeOutput,
                                             const QSet<QString> &allowedPaths);

    /**
     * @brief 文字列応答のD-Bus呼び出しを非同期で発行する
     * @param methodName 呼び出すD-Busメソッド名
     * @param arguments メソッドへ渡す引数
     * @param done 結果コールバック
     */
    virtual void requestStringReply(const QString &methodName,
                                    const QVariantList &arguments,
                                    AsyncStringReplyHandler done);

public:
    /**
     * @brief QMLへ公開するモデルデータのロール
     *
     * QML側のdelegateがrole名で参照するため、追加や削除や採番変更はQMLと同時に更新する必要がある
     */
    enum Roles {
        PathRole = Qt::UserRole + 1, // 完全パスを返すロール
        NameRole,                    // 表示名を返すロール
        ChangeTypeRole,              // 変更種別を返すロール
        IsDirectoryRole,             // ディレクトリ判定を返すロール
        IsCheckedRole,               // 選択状態を返すロール
        StatusFlagsRole              // 詳細ステータスフラグを返すロール
    };

    /** @brief ファイル変更モデルを生成する */
    explicit FileChangeModel(QObject *parent = nullptr);
    /** @brief モデルと所有リソースを破棄する */
    ~FileChangeModel();

    /** @brief 指定位置のモデルインデックスを返す */
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    /** @brief 子インデックスの親を返す */
    QModelIndex parent(const QModelIndex &child) const override;
    /** @brief 親の子要素数を返す */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    /** @brief モデル列数を返す */
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    /** @brief 指定ロールのデータを返す */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    /** @brief QML向けロール名を返す */
    QHash<int, QByteArray> roleNames() const override;

    /** @brief Snapper設定名を返す */
    QString configName() const { return m_configName; }
    /** @brief Snapper設定名を更新する */
    void setConfigName(const QString &name);

    /** @brief 対カレント比較用の番号を返す */
    int snapshotNumber() const { return m_snapshotNumber; }
    /** @brief 対カレント比較用の番号を更新する */
    void setSnapshotNumber(int number);

    /** @brief ファイル変更の有無を返す */
    bool hasChanges() const { return m_hasChanges; }
    /** @brief 読み込み中かどうかを返す */
    bool isLoading() const { return m_loading; }
    /** @brief 読み込みに成功した結果を公開しているかどうかを返す (0件の成功も含む) */
    bool isLoaded() const { return m_loadedComparisonValid; }
    /** @brief 上限を超えて一覧を省略したかどうかを返す */
    bool isTruncated() const { return m_truncated; }
    /** @brief 上限を超えて省略した行数を返す */
    int omittedCount() const { return m_omittedCount; }
    /** @brief 直近の復元計画の結果を返す */
    QVariantMap restoreResult() const { return m_restoreResult; }

    /** @brief 復元チャンクの最大件数を返す */
    int restoreBatchSize() const { return m_restoreBatchSize; }
    /** @brief 復元チャンクの最大件数を更新する */
    void setRestoreBatchSize(int size);

    /** @brief Direct Copy方式の使用設定を返す */
    bool useDirectRestore() const { return m_useDirectRestore; }
    /** @brief Direct Copy方式の使用設定を更新する */
    void setUseDirectRestore(bool use);

    /** @brief 対カレントの変更を読み込む */
    Q_INVOKABLE void loadChanges();

    /**
     * @brief 2つのスナップショット間の変更を読み込む
     * @param number1 比較元スナップショット番号
     * @param number2 比較先スナップショット番号
     * @param flat trueの場合はフラットモデルを構築する
     * @param filterUnchangedAgainstCurrent trueの場合、現在のシステムと差異が無いエントリを除外する
     */
    Q_INVOKABLE void loadChangesBetween(int number1, int number2,
                                        bool flat = false,
                                        bool filterUnchangedAgainstCurrent = false);
    /** @brief 指定ファイルの差分と詳細を取得する */
    Q_INVOKABLE void getFileDiffAndDetails(const QString &filePath);
    /** @brief 指定パスの選択状態を設定する */
    Q_INVOKABLE void setItemChecked(const QString &filePath, bool checked);
    /** @brief 選択済みパスの一覧を返す */
    Q_INVOKABLE QStringList getCheckedItems() const;
    /** @brief チェック済みの項目が1つでもあるかどうかを返す (RestorePreviewDialogが復元ボタンの有効状態の判定に使う) */
    Q_INVOKABLE bool hasCheckedItems() const;
    /**
     * @brief チェック済みの項目のうち、復元で現在のシステムから削除されるパスを返す
     *
     * 変更種別がcreatedのエントリは、復元元のスナップショットに存在しないパスであり、復元すると現在のシステムから削除される
     * 復元計画へ載せる対象と同じ規則で収集するため、確認ダイアログの表示と実際の削除対象が食い違わない
     *
     * @param maxPaths 返すパスの最大件数 (ダイアログの描画負荷を抑えるための上限、負の値は0件として扱う)
     * @return 削除されるパスの総数 (count) と、先頭から最大maxPaths件のパス (paths) を持つマップ
     */
    Q_INVOKABLE QVariantMap checkedDeletePreview(int maxPaths) const;
    /** @brief 選択済み項目の復元を開始する */
    Q_INVOKABLE bool restoreCheckedItems();
    /** @brief 指定ファイルの復元を開始する */
    Q_INVOKABLE void restoreSingleFile(const QString &filePath);
    /** @brief 進行中の復元をキャンセルする */
    Q_INVOKABLE void cancelRestore();

    /**
     * @brief 復元元スナップショットを明示して選択済み項目の復元を開始する
     * @param sourceSnapshotNumber 復元元スナップショット番号
     * @return 復元処理が開始された場合にtrue、検証に失敗した場合にfalse
     */
    Q_INVOKABLE bool restoreCheckedItemsFrom(int sourceSnapshotNumber);

    /**
     * @brief 復元元スナップショットを明示して指定ファイルの復元を開始する
     * @param filePath 復元対象のファイルパス
     * @param sourceSnapshotNumber 復元元スナップショット番号
     */
    Q_INVOKABLE void restoreSingleFileFrom(const QString &filePath,
                                           int sourceSnapshotNumber);

    /** @brief テスト専用に復元計画transportを差し替える */
    void setRestorePlanTransportForTesting(RestorePlanTransport *transport);

    /**
     * @brief テスト専用に変更一覧の取り込み上限を差し替える
     * @param maxEntries 取り込む変更エントリ数の上限
     * @param maxTextChars 取り込む変更一覧の文字数の上限
     */
    void setChangeLimitsForTesting(int maxEntries, qsizetype maxTextChars);

signals:
    /** @brief Snapper設定名の変更を通知する */
    void configNameChanged();
    /** @brief スナップショット番号の変更を通知する */
    void snapshotNumberChanged();
    /** @brief ファイル変更の有無の更新を通知する */
    void hasChangesChanged();
    /** @brief 読み込み状態の更新を通知する */
    void loadingChanged();
    /** @brief 読み込み成功状態の更新を通知する */
    void loadedChanged();
    /** @brief 一覧の省略状態の更新を通知する */
    void truncatedChanged();
    /** @brief 復元結果の更新を通知する */
    void restoreResultChanged();
    /** @brief 操作エラーを通知する */
    void errorOccurred(const QString &message);
    /** @brief ファイル差分と詳細の取得完了を通知する */
    void fileDiffAndDetailsReady(const QString &filePath,
                                 const QVariantMap &details,
                                 const QString &diff);
    /** @brief 復元の進捗を通知する */
    void restoreProgress(int current,
                         int total,
                         const QString &filePath);
    /** @brief 復元処理の完了を通知する */
    void restoreCompleted(bool success);
    /** @brief 復元チャンク件数の変更を通知する */
    void restoreBatchSizeChanged();
    /** @brief Direct Copy方式設定の変更を通知する */
    void useDirectRestoreChanged();

private slots:
    /** @brief 復元計画の進捗通知を処理する */
    void onRestorePlanProgress(const QString &manifestId, int current, int total);
    /** @brief 復元計画の完了通知を処理する */
    void onRestorePlanFinished(const QString &manifestId, const QString &terminalState, const QString &message);

    /** @brief 復元サービスの消失を失敗として処理する */
    void onRestorePlanServiceVanished();
};

#endif // FILECHANGEMODEL_H
