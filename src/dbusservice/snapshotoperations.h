#ifndef SNAPSHOTOPERATIONS_H
#define SNAPSHOTOPERATIONS_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QDBusContext>
#include <QTimer>
#include <memory>
#include <optional>

namespace snapper {
    class Snapper;
}

class SnapshotOperations : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.presire.qsnapper.Operations")

private:
    /**
     * @brief アイドルタイムアウト (5分)
     *
     * 最後のD-Busメソッド呼び出しから本値を超えてアクセスが無い場合、サービスプロセスは自律的に終了する。
     */
    static constexpr int IdleTimeoutMs = 5 * 60 * 1000;
    std::unique_ptr<snapper::Snapper> m_snapper;        // 現在のSnapperインスタンス
    QString m_currentConfig;                            // 現在選択中のSnapper設定名
    QTimer m_idleTimer;                                 // アイドルタイムアウト用タイマ

private:
    /**
     * @brief アイドルタイマをリセットする
     *
     * D-Busメソッドの先頭で呼び出し、無操作5分による自動終了を先送りする。
     */
    void resetIdleTimer();

    /**
     * @brief configNameを正規化＋検証し、不正ならD-Busエラー応答を返す
     *
     * 空文字列の入力は "root" に正規化した上で、qsnapper::security::validateConfigNameで検証する
     * これにより呼び出し側の "空なら root" デフォルト割当パターンが不要になり、
     * 空入力に対する一貫した扱い (常に "root" として受理) を保証する
     *
     * 各D-Busスロットの先頭 (checkAuthorization より前) で呼び出すこと
     * Polkitプロンプトが出てから "invalid config name" で蹴られるUXを避けるため順序が重要
     *
     * @param configName 検査対象の設定名 (空文字列は "root" として扱う)
     * @return 正規化後の設定名 (有効な場合)、無効でエラー応答済みなら std::nullopt
     */
    std::optional<QString> resolveConfigOrFail(const QString &configName);

    /**
     * @brief Polkit認証を実行する
     *
     * 認証失敗時はD-Busエラー応答を送信する
     *
     * @param actionId Polkitアクションid
     * @return 認証成功時true
     */
    bool checkAuthorization(const QString &actionId);

    /**
     * @brief Snapperインスタンスを取得 (必要に応じて生成/再生成)
     * @param configName 設定名
     * @param forceReload 強制再生成フラグ
     */
    snapper::Snapper* getSnapper(const QString &configName = "root",
                                 bool forceReload = false);

    /**
     * @brief Snapperインスタンスのスナップショット一覧をCSVに整形する
     */
    QString formatSnapshotToCSV(const snapper::Snapper *snapper);

    /**
     * @brief スナップショットタイプ列挙値を文字列に変換する
     */
    QString snapshotTypeToString(int type);

    /**
     * @brief 文字列をスナップショットタイプ列挙値に変換する
     */
    int stringToSnapshotType(const QString &typeStr);

    /**
     * @brief RestoreFiles / RestoreFilesDirect 共通実装
     *
     * 両エントリポイントは差分フラグを引数にして本関数へ委譲する
     * 入力パスはqsnapper::security::isPathWithinSnapshotRootでsnapshot root内に収まっていることを検証する
     *
     * @param configName      Snapper設定名
     * @param snapshotNumber  復元元スナップショット番号
     * @param filePaths       復元対象ファイルの絶対パス
     * @param changeTypes     各ファイルのchange種別
     * @param useReflink      通常ファイルコピー時にFICLONE (btrfs CoW) を試行するか
     * @param removeOnTypechanged typechanged時に既存ファイルを先に削除するか
     * @param logTag          ログ前置詞 ("RestoreFiles"など)
     */
    bool restoreFilesImpl(const QString &configName,
                          int snapshotNumber,
                          const QStringList &filePaths,
                          const QStringList &changeTypes,
                          bool useReflink,
                          bool removeOnTypechanged,
                          const char *logTag);

    /**
     * @brief 通常ファイルをコピーする (オーナー/時刻保持)
     * @param tryReflink FICLONE (reflink) を先行試行するか
     */
    static bool copyRegularFile(const QString &src,
                                const QString &dst,
                                bool tryReflink);

    /**
     * @brief シンボリックリンクをコピーする (リンク先/オーナー/時刻保持)
     */
    static bool copySymlink(const QString &src,
                            const QString &dst);

public:
    /**
     * @brief コンストラクタ
     * @param parent 親QObject
     */
    explicit SnapshotOperations(QObject *parent = nullptr);

    /**
     * @brief デストラクタ
     */
    ~SnapshotOperations();

public slots:
    /**
     * @brief 既存の Snapper 設定名の一覧を返す
     * @return 設定名の配列
     */
    QStringList ListConfigs();

    /**
     * @brief 指定設定のスナップショット一覧をCSVで返す
     * @param configName 設定名 (空文字列時は "root")
     */
    QString ListSnapshots(const QString &configName);

    /**
     * @brief 新しいスナップショットを作成する
     * @param configName 設定名
     * @param type "single" / "pre" / "post"
     * @param description 説明
     * @param preNumber postタイプ時の対応pre番号
     * @param cleanup cleanup アルゴリズム名
     * @param userdata 追加メタデータ
     * @param important 重要フラグ
     */
    QString CreateSnapshot(const QString &configName,
                           const QString &type,
                           const QString &description,
                           int preNumber,
                           const QString &cleanup,
                           const QMap<QString, QString> &userdata,
                           bool important);

    /**
     * @brief 既存スナップショットの属性を更新する
     */
    bool ModifySnapshot(const QString &configName,
                        int number,
                        const QString &description,
                        const QString &cleanup,
                        const QMap<QString, QString> &userdata);

    /**
     * @brief 指定スナップショットを削除する
     */
    bool DeleteSnapshot(const QString &configName,
                        int number);

    /**
     * @brief 指定スナップショットへロールバックする
     */
    bool RollbackSnapshot(const QString &configName,
                          int number);

    /**
     * @brief 現在のシステムと単一スナップショット間の変更ファイル一覧を返す
     */
    QString GetFileChanges(const QString &configName,
                           int snapshotNumber);

    /**
     * @brief 2つのスナップショット間の変更ファイル一覧を返す
     */
    QString GetFileChangesBetween(const QString &configName,
                                  int number1,
                                  int number2);

    /**
     * @brief 指定ファイルの現在とスナップショット間のunified diff + 詳細を返す
     */
    QString GetFileDiffAndDetails(const QString &configName,
                                  int snapshotNumber,
                                  const QString &filePath);

    /**
     * @brief 指定ファイルの2スナップショット間のunified diff + 詳細を返す
     */
    QString GetFileDiffBetween(const QString &configName,
                               int number1,
                               int number2,
                               const QString &filePath);

    /**
     * @brief ファイルをスナップショットから復元する (YaST互換コピー経路)
     *
     * reflinkを用いず、typechangedの事前削除も行わない。
     */
    bool RestoreFiles(const QString &configName,
                      int snapshotNumber,
                      const QStringList &filePaths,
                      const QStringList &changeTypes);

    /**
     * @brief ファイルをスナップショットから復元する (高速経路)
     *
     * btrfs reflinkを優先、typechangedは既存ファイル削除後にコピー。
     */
    bool RestoreFilesDirect(const QString &configName,
                            int snapshotNumber,
                            const QStringList &filePaths,
                            const QStringList &changeTypes);

    /**
     * @brief Snapperが1つ以上設定されているかを返す
     */
    bool IsConfigured();

    /**
     * @brief Snapper設定に値を書き込む
     */
    bool WriteSnapperConfig(const QString &configName,
                            const QMap<QString, QString> &settings);

    /**
     * @brief Snapperクォータ機能をセットアップする
     */
    bool SetupQuota(const QString &configName);


signals:
    /**
     * @brief 復元処理の進捗通知
     * @param current 現在処理中のファイル番号 (1から始まる)
     * @param total 総ファイル数
     * @param filePath 処理中のファイルパス
     */
    void restoreProgress(int current,
                         int total,
                         const QString &filePath);
};

#endif // SNAPSHOTOPERATIONS_H
