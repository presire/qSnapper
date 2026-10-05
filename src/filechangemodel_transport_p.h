/**
 * @file filechangemodel_transport_p.h
 * @brief 復元計画D-Bus転送のシステムバス実装の宣言
 *
 * FileChangeModelが既定で使うSystemBusRestorePlanTransportを宣言する
 * 公開APIではないため、src配下の実装ファイルからのみ取り込む
 * 実装はfilechangemodel_transport.cppに置く
 */

#ifndef FILECHANGEMODEL_TRANSPORT_P_H
#define FILECHANGEMODEL_TRANSPORT_P_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDBusMessage>
#include <functional>
#include "restoreplantransport.h"

class QDBusServiceWatcher;
class QDBusPendingCallWatcher;

/**
 * @class SystemBusRestorePlanTransport
 * @brief RestorePlanTransportのシステムバス実装
 *
 * com.presire.qsnapper.Operationsのstaged restoreメソッド群を非同期で呼び出す
 * Polkit認証はcommitPlan()の呼び出し時にサーバ側で1度だけ行われるため、
 * beginPlan() / stageEntries() は認証なしで呼び出せる
 */
class SystemBusRestorePlanTransport : public QObject, public RestorePlanTransport
{
public:
    /**
     * @brief SystemBusRestorePlanTransportを構築する
     *
     * @param parent 親QObject (未指定の場合は呼び出し側がdeleteする)
     */
    explicit SystemBusRestorePlanTransport(QObject *parent = nullptr);

    /**
     * @brief staging計画の作成を要求する
     *
     * @param configName Snapper設定名
     * @param snapshotNumber スナップショット番号
     * @param counterpartSnapshotNumber 比較相手のスナップショット番号
     * @param restoreMode 復元方式 ("yast" または "direct")
     * @param done 結果コールバック (ok, マニフェストID, error)
     */
    void beginPlan(const QString &configName, int snapshotNumber,
                   int counterpartSnapshotNumber, const QString &restoreMode,
                   std::function<void(bool ok, const QString &manifestId, const QString &error)> done) override;

    /**
     * @brief staging計画へ検証済みエントリのチャンクを追加する
     *
     * @param manifestId 対象計画のマニフェストID
     * @param paths エントリのパスリスト
     * @param changeTypes パスに対応する変更タイプリスト
     * @param done 結果コールバック (ok, error)
     */
    void stageEntries(const QString &manifestId, const QStringList &paths, const QStringList &changeTypes,
                      std::function<void(bool ok, const QString &error)> done) override;

    /**
     * @brief 計画を凍結して認証と実行を開始させる
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     */
    void commitPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override;

    /**
     * @brief 計画のキャンセルを要求する
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, error)
     */
    void cancelPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override;

    /**
     * @brief 計画の状態をCSV形式で問い合わせる
     *
     * @param manifestId 対象計画のマニフェストID
     * @param done 結果コールバック (ok, statusCsv, error)
     */
    void requestStatus(const QString &manifestId,
                       std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) override;

    /**
     * @brief 復元計画の進捗/完了シグナルとサービス消失通知を受信側に登録する
     *
     * @param receiver 復元計画スロットとonRestorePlanServiceVanishedスロットを持つQObject
     * @return すべてのシグナル登録に成功した場合はtrue
     */
    bool subscribePlanSignals(QObject *receiver) override;

    /**
     * @brief 復元計画の進捗/完了シグナルとサービス消失通知の受信を解除する
     *
     * @param receiver 登録解除するQObject
     */
    void unsubscribePlanSignals(QObject *receiver) override;

private:
    QDBusServiceWatcher *m_serviceWatcher = nullptr;  // サービス消失を監視するwatcher (初回登録時に生成)

    /**
     * @brief 復元計画メソッドを非同期で呼び出す (タイムアウトなし)
     *
     * watcherはthisの子として管理されるため、transportの破棄時に未完了のコールバックが発火することはない
     *
     * @param message 送信するメソッドコール
     * @param handler watcherの完了時に呼び出すハンドラ
     */
    void callPlanMethod(const QDBusMessage &message,
                        std::function<void(QDBusPendingCallWatcher *watcher)> handler);
};

#endif // FILECHANGEMODEL_TRANSPORT_P_H
