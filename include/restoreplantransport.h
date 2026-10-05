/**
 * @file restoreplantransport.h
 * @brief 復元計画のD-Bus転送を抽象化するRestorePlanTransportの宣言
 *
 * FileChangeModelとD-Bus呼び出しの間に注入可能な継ぎ目を設け
 * テストではモックtransportで呼び出し順序と結果を検証できるようにする
 */

#ifndef RESTOREPLANTRANSPORT_H
#define RESTOREPLANTRANSPORT_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

/**
 * @class RestorePlanTransport
 * @brief 復元計画のD-Bus転送を抽象化するインターフェース
 *
 * FileChangeModelとD-Bus呼び出しの間に注入可能な継ぎ目を設け
 * テストではモックtransportで呼び出し順序と結果を検証できるようにする
 * 各メソッドは非同期であり、コールバックを必ず1回だけ呼び出す
 * 呼び出しは即時または後続のイベントループで完了する
 */
class RestorePlanTransport
{
public:
    /**
     * @brief transportインターフェースを破棄する
     *
     * 派生transportを安全に破棄できるように仮想デストラクタとする
     */
    virtual ~RestorePlanTransport();

    /**
     * @brief staging計画の作成を要求する
     *
     * この時点では認証されない
     * Polkit認証はcommitPlanの呼び出し時にサーバ側で1度だけ行う
     *
     * @param configName Snapper設定名
     * @param snapshotNumber スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号
     * @param restoreMode 復元方式
     * @param done 結果コールバック
     */
    virtual void beginPlan(const QString &configName,
                           int snapshotNumber,
                           int counterpartSnapshotNumber,
                           const QString &restoreMode,
                           std::function<void(bool ok, const QString &manifestId, const QString &error)> done) = 0;

    /**
     * @brief staging計画へ検証済みエントリのチャンクを追加する
     *
     * この時点では認証されない
     *
     * @param manifestId 対象計画のマニフェストID
     * @param paths エントリのパスリスト
     * @param changeTypes パスに対応する変更タイプリスト
     * @param done 結果コールバック
     */
    virtual void stageEntries(const QString &manifestId,
                              const QStringList &paths,
                              const QStringList &changeTypes,
                              std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画を凍結して認証と実行を開始させる
     *
     * Polkitプロンプトはこの呼び出しで1度だけ出す
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック
     */
    virtual void commitPlan(const QString &manifestId,
                            std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画のキャンセルを要求する
     *
     * 取り消しは次のサーバ側チャンク境界で反映される
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック
     */
    virtual void cancelPlan(const QString &manifestId,
                            std::function<void(bool ok, const QString &error)> done) = 0;

    /**
     * @brief 計画の状態をCSV形式で問い合わせる
     *
     * 失敗時の所有者確認など、端末状態の確定に使う
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック
     */
    virtual void requestStatus(const QString &manifestId,
                               std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) = 0;

    /**
     * @brief 復元計画の進捗と完了シグナルおよびサービス消失通知を受信側に登録する
     *
     * 復元サービスのバス名が消失した場合は
     * receiverのonRestorePlanServiceVanishedスロットを呼び出さなければならない
     *
     * @param receiver 進捗と完了と消失通知のスロットを持つQObject
     * @return 登録に成功した場合はtrue
     */
    virtual bool subscribePlanSignals(QObject *receiver) = 0;

    /**
     * @brief 復元計画の進捗と完了シグナルおよびサービス消失通知の受信を解除する
     *
     * subscribePlanSignalsで登録したonRestorePlanServiceVanishedスロットへの通知も解除しなければならない
     *
     * @param receiver 登録解除するQObject
     */
    virtual void unsubscribePlanSignals(QObject *receiver) = 0;
};

#endif // RESTOREPLANTRANSPORT_H
