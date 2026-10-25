#ifndef FILESYSTEMINFOMODEL_H
#define FILESYSTEMINFOMODEL_H

#include <QObject>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class SnapperService;

/**
 * @file filesysteminfomodel.h
 * @brief Btrfsの使用量とサブボリューム一覧を表示するためのViewModel
 */

/**
 * @class FilesystemInfoModel
 * @brief Btrfsの使用量とサブボリューム一覧をQMLへ提供するViewModel
 *
 * GetFilesystemUsageとListSubvolumesをSnapperServiceの非同期ヘルパとQDBusPendingCallWatcherで実行し、GUIスレッドをブロックしない
 * 応答のCSVはRFC 4180の引用符を考慮して解析し、1行でも不正であれば部分的に表示せずエラーとする
 * configNameを変更すると状態を破棄し、変更前の設定に対する応答は世代番号の不一致で捨てる
 *
 * 使用量はbtrfs filesystem usageと同じ値を保証しない
 * サーバはBTRFS_IOC_SPACE_INFOのチャンク単位の値をそのまま返すため、TypeごとのTotalはその種別へ割り当てた容量、Usedはその中で実際に使用中の容量を表す
 *
 * m_usage : 種別ごとの割り当て容量と使用容量 (表示用のラベルを含む)
 * m_devices : デバイスごとの容量と割り当て済み容量
 * m_allSubvolumes : hideSnapperSnapshotsの絞り込み前のサブボリューム一覧
 * m_subvolumes : 絞り込み後のサブボリューム一覧
 * m_usageGeneration : 使用量の要求ごとに増やし、古い応答を捨てるための世代番号
 * m_subvolumeGeneration : サブボリューム一覧の要求ごとに増やし、古い応答を捨てるための世代番号
 */
class FilesystemInfoModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString configName READ configName WRITE setConfigName NOTIFY configNameChanged)              // 対象のsnapper設定名
    Q_PROPERTY(bool usageBusy READ usageBusy NOTIFY usageBusyChanged)                                        // 使用量の取得中
    Q_PROPERTY(bool subvolumesBusy READ subvolumesBusy NOTIFY subvolumesBusyChanged)                         // サブボリューム一覧の取得中
    Q_PROPERTY(bool usageLoaded READ usageLoaded NOTIFY usageLoadedChanged)                                  // 使用量を読み込み済みか
    Q_PROPERTY(QVariantList usage READ usage NOTIFY usageChanged)                                            // 種別ごとの割り当て容量と使用容量
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)                                      // デバイスごとの容量と割り当て済み容量
    Q_PROPERTY(qint64 deviceTotal READ deviceTotal NOTIFY deviceTotalsChanged)                               // 全デバイスの容量の合計
    Q_PROPERTY(qint64 deviceAllocated READ deviceAllocated NOTIFY deviceTotalsChanged)                       // 全デバイスでチャンクへ割り当て済みの容量の合計
    Q_PROPERTY(qint64 nodesize READ nodesize NOTIFY blockSizesChanged)                                       // Btrfsのノードサイズ
    Q_PROPERTY(qint64 sectorsize READ sectorsize NOTIFY blockSizesChanged)                                   // Btrfsのセクタサイズ
    Q_PROPERTY(QVariantList subvolumes READ subvolumes NOTIFY subvolumesChanged)                             // 絞り込み後のサブボリューム一覧
    Q_PROPERTY(bool subvolumesTruncated READ subvolumesTruncated NOTIFY subvolumesTruncatedChanged)          // 一覧が上限件数で打ち切られたか
    Q_PROPERTY(bool hideSnapperSnapshots READ hideSnapperSnapshots WRITE setHideSnapperSnapshots NOTIFY hideSnapperSnapshotsChanged) // snapperのスナップショットを隠すか

public:
    /**
     * @brief FilesystemInfoModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit FilesystemInfoModel(QObject *parent = nullptr);

    QString configName() const { return m_configName; }
    bool usageBusy() const { return m_usageBusy; }
    bool subvolumesBusy() const { return m_subvolumesBusy; }
    bool usageLoaded() const { return m_usageLoaded; }
    QVariantList usage() const { return m_usage; }
    QVariantList devices() const { return m_devices; }
    qint64 deviceTotal() const { return m_deviceTotal; }
    qint64 deviceAllocated() const { return m_deviceAllocated; }
    qint64 nodesize() const { return m_nodesize; }
    qint64 sectorsize() const { return m_sectorsize; }
    QVariantList subvolumes() const { return m_subvolumes; }
    bool subvolumesTruncated() const { return m_subvolumesTruncated; }
    bool hideSnapperSnapshots() const { return m_hideSnapperSnapshots; }

    /**
     * @brief 使用量の対象となる設定名を変更する
     * @param name 新しい設定名
     * @note 変更前の設定に対する応答を捨てるため、世代番号を進めて状態を破棄する
     */
    void setConfigName(const QString &name);

    /**
     * @brief snapperのスナップショットを一覧から隠すかを変更する
     * @param hide trueでsnapperのスナップショットを隠す
     * @note サーバから取得済みの一覧を絞り込むだけで、再取得は行わない
     */
    void setHideSnapperSnapshots(bool hide);

    /**
     * @brief 使用量を非同期に取得する (GetFilesystemUsage)
     * @note 設定名が空の場合は何もしない
     *       失敗時はusageLoadFailedを発行し、古い値を実際の状態として表示しないよう消去する
     */
    Q_INVOKABLE void refreshUsage();

    /**
     * @brief サブボリューム一覧を非同期に取得する (ListSubvolumes)
     * @note 認証を伴うため、画面から明示的に要求された場合だけ呼び出す
     */
    Q_INVOKABLE void refreshSubvolumes();

signals:
    /**
     * @brief 対象の設定名が変化したときに発行する
     */
    void configNameChanged();

    /**
     * @brief 使用量の取得中状態が変化したときに発行する
     */
    void usageBusyChanged();

    /**
     * @brief サブボリューム一覧の取得中状態が変化したときに発行する
     */
    void subvolumesBusyChanged();

    /**
     * @brief 使用量の読み込み済み状態が変化したときに発行する
     */
    void usageLoadedChanged();

    /**
     * @brief 種別ごとの使用量が変化したときに発行する
     */
    void usageChanged();

    /**
     * @brief デバイス一覧が変化したときに発行する
     */
    void devicesChanged();

    /**
     * @brief デバイス容量の合計と割り当て済み容量の合計が変化したときに発行する
     */
    void deviceTotalsChanged();

    /**
     * @brief ノードサイズとセクタサイズが変化したときに発行する
     */
    void blockSizesChanged();

    /**
     * @brief サブボリューム一覧が変化したときに発行する
     */
    void subvolumesChanged();

    /**
     * @brief サブボリューム一覧の打ち切り状態が変化したときに発行する
     */
    void subvolumesTruncatedChanged();

    /**
     * @brief snapperのスナップショットを隠すかの設定が変化したときに発行する
     */
    void hideSnapperSnapshotsChanged();

    /**
     * @brief 使用量の取得に失敗したときに発行する
     * @param error 利用者へ表示する失敗理由
     */
    void usageLoadFailed(const QString &error);

    /**
     * @brief サブボリューム一覧の取得に失敗したときに発行する
     * @param error 利用者へ表示する失敗理由
     */
    void subvolumesLoadFailed(const QString &error);

private:
    /**
     * @brief GetFilesystemUsageのCSVを解析してメンバへ反映する
     * @param csv サーバが返したCSV
     * @param error 解析に失敗した理由の格納先
     * @return すべての行を解析できた場合はtrue
     * @note 1行でも不正であれば部分的に反映せずfalseを返し、呼び出し側が古い値を消去する
     */
    bool parseUsageCsv(const QString &csv, QString *error);

    /**
     * @brief ListSubvolumesのCSVを解析してメンバへ反映する
     * @param csv サーバが返したCSV
     * @param error 解析に失敗した理由の格納先
     * @return すべての行を解析できた場合はtrue
     * @note 1行でも不正であれば部分的に反映せずfalseを返し、呼び出し側が古い値を消去する
     */
    bool parseSubvolumesCsv(const QString &csv, QString *error);

    /**
     * @brief 絞り込み後のサブボリューム一覧を再構築する
     */
    void rebuildVisibleSubvolumes();

    /**
     * @brief 使用量の表示状態を破棄する
     */
    void clearUsage();

    /**
     * @brief サブボリューム一覧の表示状態を破棄する
     */
    void clearSubvolumes();

    /**
     * @brief 使用量の取得中状態を更新する
     * @param busy 新しい状態
     */
    void setUsageBusy(bool busy);

    /**
     * @brief サブボリューム一覧の取得中状態を更新する
     * @param busy 新しい状態
     */
    void setSubvolumesBusy(bool busy);

    SnapperService *m_snapperService;   // SnapperServiceシングルトンへの参照
    QString m_configName;               // 対象のsnapper設定名

    bool m_usageBusy;                   // 使用量の取得中
    bool m_subvolumesBusy;              // サブボリューム一覧の取得中
    bool m_usageLoaded;                 // 使用量を読み込み済みか
    bool m_subvolumesTruncated;         // 一覧が上限件数で打ち切られたか
    bool m_hideSnapperSnapshots;        // snapperのスナップショットを隠すか

    QVariantList m_usage;               // 種別ごとの割り当て容量と使用容量
    QVariantList m_devices;             // デバイスごとの容量と割り当て済み容量
    QVariantList m_allSubvolumes;       // 絞り込み前のサブボリューム一覧
    QVariantList m_subvolumes;          // 絞り込み後のサブボリューム一覧
    qint64 m_deviceTotal;               // 全デバイスの容量の合計
    qint64 m_deviceAllocated;           // 全デバイスでチャンクへ割り当て済みの容量の合計
    qint64 m_nodesize;                  // Btrfsのノードサイズ
    qint64 m_sectorsize;                // Btrfsのセクタサイズ

    quint64 m_usageGeneration;          // 古い使用量の応答を捨てるための世代番号
    quint64 m_subvolumeGeneration;      // 古いサブボリューム一覧の応答を捨てるための世代番号
};

#endif // FILESYSTEMINFOMODEL_H
