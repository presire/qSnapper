/**
 * @file restoremanifest.h
 * @brief 単一復元計画のD-Bus非依存な状態機械
 *
 * データ型は restoremanifesttypes.h に分離した
 * 所有と上限の管理は restoremanifestregistry.h を参照すること
 * 本ヘッダは registry までまとめて取り込むため既存の取り込み元は変更不要である
 */

#ifndef QSNAPPER_RESTOREMANIFEST_H
#define QSNAPPER_RESTOREMANIFEST_H

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <memory>
#include <optional>

#include "restoremanifesttypes.h"

// 単一復元計画の状態機械を扱う名前空間
namespace qsnapper::restore {

/**
 * @class RestoreManifest
 * @brief 単一復元計画のD-Bus非依存な状態機械
 *
 * owner、設定、スナップショット、mode、id、TTLは構築後不変である
 * freeze後のエントリ列はconstなコンテナへ移され、型として変更できない
 *
 * m_owner : 構築時に固定されたD-Bus呼び出し元のユニーク名
 * m_configName : 構築時に固定されたSnapper設定名
 * m_snapshotNumber : 構築時に固定された復元元スナップショット番号
 * m_counterpartSnapshotNumber : 構築時に固定された比較相手番号 (0は現在のシステム)
 * m_mode : 構築時に固定された復元方式
 * m_id : 構築時に固定された推測困難なマニフェスト識別子
 * m_creationTimeMs : 作成時刻 (ミリ秒)
 * m_ttlMs : 最終活動から失効するまでの時間 (ミリ秒)
 * m_state : 現在のライフサイクル状態
 * m_stagingEntries : Staging中に末尾へ追加する可変のエントリ列
 * m_frozenEntries : freeze後に移される変更不能なエントリ列
 * m_cursor : 次に処理する0始まりの位置
 * m_lastActivityMs : TTL判定の起点となる最終活動時刻 (ミリ秒)
 * m_lastError : 人間向けの失敗理由
 */
class RestoreManifest
{
public:
    /**
     * @brief Staging状態の復元計画を構築する
     * @param owner D-Bus呼び出し元のユニーク名
     * @param configName Snapper設定名
     * @param snapshotNumber 復元元スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号 (0は現在のシステム)
     * @param mode 復元方式
     * @param id 推測困難なマニフェスト識別子
     * @param creationTimeMs 作成時刻 (ミリ秒)
     * @param ttlMs 最終活動から失効するまでの時間 (ミリ秒)
     */
    RestoreManifest(QString owner, QString configName, int snapshotNumber,
                    int counterpartSnapshotNumber, RestoreMode mode,
                    QString id, qint64 creationTimeMs, qint64 ttlMs);

    /**
     * @brief Staging中の末尾へエントリ列を原子的に追加する
     * @param entries 追加する順序付きエントリ列
     * @param err 結果エラーの格納先 (省略可)
     * @return 追加成功時true
     */
    bool appendEntries(const QVector<RestoreEntry> &entries, ManifestError *err);

    /**
     * @brief 非空のStaging計画を変更不能なFrozen状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool freeze(ManifestError *err);

    /**
     * @brief Frozen計画を1度だけRunning状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markRunning(ManifestError *err);

    /**
     * @brief Running計画のcursorを進め、終端到達時にCompletedへ遷移する
     * @param count 進める正のエントリ数
     * @param err 結果エラーの格納先 (省略可)
     * @return 更新成功時true
     */
    bool advanceCursor(int count, ManifestError *err);

    /**
     * @brief 非終端計画をFailed状態へ遷移する
     * @param reason 人間向けの失敗理由
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool markFailed(const QString &reason, ManifestError *err);

    /**
     * @brief 非終端計画をCancelled状態へ遷移する
     * @param err 結果エラーの格納先 (省略可)
     * @return 遷移成功時true
     */
    bool cancel(ManifestError *err);

    /**
     * @brief 計画がCompleted、Failed、Cancelledのいずれかを返す
     * @return 終端状態ならtrue
     */
    bool isTerminal() const;

    /**
     * @brief 指定時刻でTTLを超過しているか判定する
     *
     * Running状態の計画は認可済みかつexecutorが駆動中であり、TTL回収の対象外とする
     * (owner消失時はregistryのremoveByOwnerが回収する)
     *
     * @param nowMs 判定時刻 (ミリ秒)
     * @return 非Runningかつ最終活動からTTLを超過していればtrue
     */
    bool isExpired(qint64 nowMs) const;

    /**
     * @brief 最終活動時刻を更新する
     * @param nowMs 新しい最終活動時刻 (ミリ秒)
     */
    void touch(qint64 nowMs);

    // 構築時に固定された値を返す参照系
    /**
     * @brief 所有者ユニーク名を返す
     * @return 構築時に固定された所有者
     */
    const QString &owner() const;

    /**
     * @brief マニフェスト識別子を返す
     * @return 構築時に固定された識別子
     */
    const QString &id() const;

    /**
     * @brief 現在の状態を返す
     * @return マニフェスト状態
     */
    ManifestState state() const;

    /**
     * @brief Snapper設定名を返す
     * @return 構築時に固定された設定名
     */
    const QString &configName() const;

    /**
     * @brief 復元元スナップショット番号を返す
     * @return 構築時に固定されたスナップショット番号
     */
    int snapshotNumber() const;

    /**
     * @brief 比較相手のスナップショット番号を返す
     * @return 構築時に固定された比較相手番号 (0は現在のシステム)
     */
    int counterpartSnapshotNumber() const;

    /**
     * @brief 復元方式を返す
     * @return 構築時に固定された復元方式
     */
    RestoreMode mode() const;

    // エントリ列の参照系
    /**
     * @brief 保存済みエントリ総数を返す
     * @return エントリ総数
     */
    int totalEntries() const;

    /**
     * @brief 次に処理するエントリ位置を返す
     * @return 0始まりのcursor
     */
    int cursor() const;

    /**
     * @brief 指定位置のエントリを値として返す
     * @param index 0始まりの位置
     * @return 範囲内ならエントリ、範囲外ならstd::nullopt
     */
    std::optional<RestoreEntry> entryAt(int index) const;

    /**
     * @brief 指定範囲のエントリを順序通り値として返す
     * @param offset 0始まりの開始位置
     * @param count 最大取得数
     * @return 不正範囲では空、それ以外では切り出したエントリ列
     */
    QVector<RestoreEntry> entriesSlice(int offset, int count) const;

    /**
     * @brief 外部公開用の状態スナップショットを返す
     * @return 現在状態を反映したマニフェスト状態
     */
    ManifestStatus status() const;

private:
    // 内部参照系
    /**
     * @brief 現在有効なエントリ列を返す
     * @return 凍結済みなら凍結列、それ以外はStaging列
     */
    const QVector<RestoreEntry> &entries() const;

    /**
     * @brief 任意のエラー格納先へ値を設定する
     * @param err 結果エラーの格納先 (空でもよい)
     * @param value 設定する値
     */
    static void setError(ManifestError *err, ManifestError value);

    // 構築時に固定される値
    const QString m_owner;                  // D-Bus呼び出し元のユニーク名
    const QString m_configName;             // Snapper設定名
    const int m_snapshotNumber;             // 復元元スナップショット番号
    const int m_counterpartSnapshotNumber;  // 比較相手番号 (0は現在のシステム)
    const RestoreMode m_mode;               // 復元方式
    const QString m_id;                     // 推測困難なマニフェスト識別子
    const qint64 m_creationTimeMs;          // 作成時刻 (ミリ秒)
    const qint64 m_ttlMs;                   // 最終活動から失効するまでの時間 (ミリ秒)
    // 可変の実行状態
    ManifestState m_state = ManifestState::Staging;              // 現在のライフサイクル状態
    QVector<RestoreEntry> m_stagingEntries;                      // Staging中の可変エントリ列
    std::unique_ptr<const QVector<RestoreEntry>> m_frozenEntries; // freeze後に移される変更不能列
    int m_cursor = 0;                                            // 次に処理する0始まりの位置
    qint64 m_lastActivityMs;                                     // TTL判定の起点となる最終活動時刻
    QString m_lastError;                                         // 人間向けの失敗理由

    Q_DISABLE_COPY(RestoreManifest)
};

} // namespace qsnapper::restore

// 既存の取り込み元が変更なしで使えるよう所有管理までまとめて取り込む
#include "restoremanifestregistry.h"

#endif // QSNAPPER_RESTOREMANIFEST_H
