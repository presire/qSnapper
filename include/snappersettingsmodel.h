#ifndef SNAPPERSETTINGSMODEL_H
#define SNAPPERSETTINGSMODEL_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

/**
 * @file snappersettingsmodel.h
 * @brief snapperの設定値とsystemdタイマ状態を編集するViewModel
 */

/**
 * @class SnapperSettingsModel
 * @brief 1つのsnapper設定 (config) の設定値と、snapperのsystemdタイマの状態をQMLへ提供するViewModel
 *
 * 設定値の取得 (GetConfig) と書き込み (WriteSnapperConfig)、タイマ状態の取得 (GetTimerStates) と
 * 有効化 / 無効化 (SetTimerEnabled)、Btrfsクォータの設定 (SetupQuota) を、SnapperServiceの非同期ヘルパとQDBusPendingCallWatcherで実行する
 * GUIスレッドをブロックする同期呼び出しは使わない
 *
 * 編集可能な18個のキーだけを受け付ける
 * 値の検証 (isValidValue) は画面でのフィードバック用の写しであり、最終的な検証は特権サービスが行う
 *
 * 設定の読み込みと保存は同時に1件だけ実行し、実行中の同種の要求は無視する
 * configNameを変更すると状態を破棄し、変更前の設定に対する応答は世代番号の不一致で捨てる
 * rollbackSupportedは読み込み前と読み込み失敗時にfalseとし、不明な場合はロールバックを許可しない (fail-closed)
 *
 * タイマはすべての設定で共有されるため、configNameとは独立して扱う
 *
 * m_loaded : 最後に読み込んだ編集可能キーの値
 * m_edited : 読み込み値と異なる編集中の値 (読み込み値と同じ値に戻したキーは含めない)
 * m_generation : configNameの変更と状態の破棄ごとに増やし、古い応答を判別する
 */
class SnapperSettingsModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT                                                                                             // qmllintが型を解決できるように、ビルド時の型情報 (qmltypes) へ出力する

    Q_PROPERTY(QString configName READ configName WRITE setConfigName NOTIFY configNameChanged)             // 対象のsnapper設定名
    Q_PROPERTY(bool configBusy READ configBusy NOTIFY configBusyChanged)                                    // 設定の読み込みまたは保存の実行中
    Q_PROPERTY(bool timersBusy READ timersBusy NOTIFY timersBusyChanged)                                    // タイマ状態の取得または変更の実行中
    Q_PROPERTY(bool configLoaded READ configLoaded NOTIFY configLoadedChanged)                              // 設定値を読み込み済みか
    Q_PROPERTY(QString subvolume READ subvolume NOTIFY configLoadedChanged)                                 // 読み取り専用のSUBVOLUME
    Q_PROPERTY(QString fstype READ fstype NOTIFY configLoadedChanged)                                       // 読み取り専用のFSTYPE
    Q_PROPERTY(QString qgroup READ qgroup NOTIFY configLoadedChanged)                                       // 読み取り専用のQGROUP
    Q_PROPERTY(bool rollbackSupported READ rollbackSupported NOTIFY rollbackSupportedChanged)               // ロールバック可能な設定か (btrfsかつルート)
    Q_PROPERTY(bool modified READ hasEdits NOTIFY editsChanged)                                             // 未保存の編集があるか
    Q_PROPERTY(bool modifiedValid READ isModifiedValid NOTIFY editsChanged)                                 // 編集中の値がすべて妥当か
    Q_PROPERTY(int revision READ revision NOTIFY editsChanged)                                              // 編集状態の版番号 (QMLのバインディングの再評価用)
    Q_PROPERTY(QVariantList timers READ timers NOTIFY timersChanged)                                        // タイマ状態の一覧 (unit / unitFileState / activeState)

public:
    /**
     * @brief SnapperSettingsModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit SnapperSettingsModel(QObject *parent = nullptr);

    /**
     * @brief SnapperSettingsModelを破棄する
     */
    ~SnapperSettingsModel() override;

    // プロパティのアクセサ
    /**
     * @brief 対象のsnapper設定名を取得する
     * @return 設定名
     */
    QString configName() const { return m_configName; }

    /**
     * @brief 対象のsnapper設定名を変更する
     * @param name 新しい設定名
     * @note 変更時は読み込み済みの値と編集内容を破棄するが、自動では読み込まない
     *       実行中の読み込みと保存の応答は捨てる (古い設定の値を新しい設定として表示しないため)
     */
    void setConfigName(const QString &name);

    /**
     * @brief 設定の読み込みまたは保存が実行中か判定する
     * @return 実行中の場合はtrue
     */
    bool configBusy() const { return m_configBusy; }

    /**
     * @brief タイマ状態の取得または変更が実行中か判定する
     * @return 実行中の場合はtrue
     */
    bool timersBusy() const { return m_timersBusy; }

    /**
     * @brief 設定値を読み込み済みか判定する
     * @return 読み込み済みの場合はtrue
     */
    bool configLoaded() const { return m_configLoaded; }

    /**
     * @brief 読み込んだSUBVOLUMEを取得する
     * @return SUBVOLUMEの値、未読み込みの場合は空文字列
     */
    QString subvolume() const { return m_subvolume; }

    /**
     * @brief 読み込んだFSTYPEを取得する
     * @return FSTYPEの値、未読み込みの場合は空文字列
     */
    QString fstype() const { return m_fstype; }

    /**
     * @brief 読み込んだQGROUPを取得する
     * @return QGROUPの値、未読み込みまたは未設定の場合は空文字列
     */
    QString qgroup() const { return m_qgroup; }

    /**
     * @brief システムロールバックが可能な設定か判定する
     * @return FSTYPEがbtrfsかつSUBVOLUMEが/の場合だけtrue、未読み込みと読み込み失敗ではfalse
     */
    bool rollbackSupported() const { return m_rollbackSupported; }

    /**
     * @brief 未保存の編集があるか判定する
     * @return 読み込み値と異なる編集中の値がある場合はtrue
     */
    bool hasEdits() const { return !m_edited.isEmpty(); }

    /**
     * @brief 編集中の値がすべて妥当か判定する
     * @return 編集中の値がすべてisValidValueを満たす場合はtrue、編集が無い場合もtrue
     */
    bool isModifiedValid() const;

    /**
     * @brief 編集状態の版番号を取得する
     * @return 編集状態が変わるたびに増える番号
     */
    int revision() const { return m_revision; }

    /**
     * @brief タイマ状態の一覧を取得する
     * @return unit / unitFileState / activeStateを持つマップのリスト
     */
    QVariantList timers() const { return m_timers; }

    // 設定値の参照と編集
    /**
     * @brief 編集可能キーの一覧を取得する
     * @return 画面に表示する順序の18個のキー
     */
    Q_INVOKABLE QStringList editableKeys() const;

    /**
     * @brief キーの値の種別を取得する
     * @param key 設定キー
     * @return boolean / seconds / countOrRange / fractionのいずれか、編集可能キー以外では空文字列
     */
    Q_INVOKABLE QString valueKind(const QString &key) const;

    /**
     * @brief キーの現在値を取得する
     * @param key 設定キー
     * @return 編集中ならその値、そうでなければ読み込んだ値、無ければ空文字列
     */
    Q_INVOKABLE QString value(const QString &key) const;

    /**
     * @brief キーの値を編集する
     * @param key 設定キー (編集可能な18キー以外は無視する)
     * @param value 新しい値 (不正な値も保持し、保存時に拒否する)
     * @note 読み込み値と同じ値に戻した場合は編集を取り消す
     *       変化があればeditsChangedを発行する
     */
    Q_INVOKABLE void setValue(const QString &key, const QString &value);

    /**
     * @brief キーが読み込み値から変更されているか判定する
     * @param key 設定キー
     * @return 変更されている場合はtrue
     */
    Q_INVOKABLE bool isModified(const QString &key) const;

    /**
     * @brief 値がキーの規則を満たすか判定する
     * @param key 設定キー
     * @param value 検証する値
     * @return 規則を満たす場合はtrue、編集可能キー以外ではfalse
     * @note 画面でのフィードバック専用であり、特権サービス側の検証を代替しない
     */
    Q_INVOKABLE bool isValidValue(const QString &key, const QString &value) const;

    // 設定値の読み込みと保存
    /**
     * @brief 設定値を非同期に読み込み直す
     * @note 編集中の値は破棄される
     *       設定名が空、または読み込み / 保存の実行中は何もしない
     *       失敗時は読み込み済みの値を破棄し、loadFailedを発行する
     */
    Q_INVOKABLE void reload();

    /**
     * @brief 変更されたキーだけを特権サービスへ書き込む
     * @note 変更が無い場合と、読み込み / 保存の実行中は何もしない
     *       不正な値が含まれる場合は送信せずsaveFailedを発行する
     *       応答の成否にかかわらず、実際の保存内容を表示するために完了後にreloadする
     */
    Q_INVOKABLE void save();

    /**
     * @brief 編集内容を破棄して読み込み値へ戻す
     */
    Q_INVOKABLE void revert();

    // クォータの設定
    /**
     * @brief Btrfsクォータの設定を特権サービスへ要求する (SetupQuota)
     * @note 設定が未読み込み、または読み込み / 保存の実行中は何もしない
     *       成功時はquotaSetupSucceededを、失敗時はquotaSetupFailedを発行する
     *       成功時はQGROUPの反映のために完了後に読み込み直すため、未保存の編集は破棄される
     */
    Q_INVOKABLE void setupQuota();

    // タイマの状態取得と変更
    /**
     * @brief タイマ状態を非同期に取得し直す
     * @note 取得 / 変更の実行中は何もしない
     *       失敗時はtimersを空にし、timersLoadFailedを発行する
     */
    Q_INVOKABLE void refreshTimers();

    /**
     * @brief タイマの有効化 / 無効化を要求する
     * @param unit 対象のユニット名 (snapper-timeline.timerなど3種類のみ)
     * @param enabled trueで有効化して起動、falseで停止して無効化
     * @note 取得 / 変更の実行中は何もしない
     *       応答の成否にかかわらず、実際の状態を表示するために完了後にタイマ状態を取得し直す
     */
    Q_INVOKABLE void setTimerEnabled(const QString &unit, bool enabled);

signals:
    /**
     * @brief 対象の設定名の変更時に発行する
     */
    void configNameChanged();

    /**
     * @brief 設定の実行中状態の変更時に発行する
     */
    void configBusyChanged();

    /**
     * @brief タイマの実行中状態の変更時に発行する
     */
    void timersBusyChanged();

    /**
     * @brief 読み込み状態または読み取り専用項目の変更時に発行する
     */
    void configLoadedChanged();

    /**
     * @brief ロールバック可否の変更時に発行する
     */
    void rollbackSupportedChanged();

    /**
     * @brief 編集状態 (編集中の値の有無と妥当性) の変更時に発行する
     * @note 読み込み値の更新時にも発行する
     *       入力欄は自身の編集で上書きされないよう、版番号 (revision) を介して値を取得し直す
     */
    void editsChanged();

    /**
     * @brief タイマ状態の一覧の変更時に発行する
     */
    void timersChanged();

    /**
     * @brief 設定の読み込み失敗時に発行する
     * @param error 表示用のエラー文言
     */
    void loadFailed(const QString &error);

    /**
     * @brief 設定の保存成功時に発行する
     */
    void saveSucceeded();

    /**
     * @brief 設定の保存失敗時に発行する
     * @param error 表示用のエラー文言
     */
    void saveFailed(const QString &error);

    /**
     * @brief クォータの設定成功時に発行する
     * @note QGROUPの反映のため、この後に設定を読み込み直す
     */
    void quotaSetupSucceeded();

    /**
     * @brief クォータの設定失敗時に発行する
     * @param error 表示用のエラー文言
     */
    void quotaSetupFailed(const QString &error);

    /**
     * @brief タイマ状態の取得失敗時に発行する
     * @param error 表示用のエラー文言
     */
    void timersLoadFailed(const QString &error);

    /**
     * @brief タイマの変更成功時に発行する
     * @param unit 変更したユニット名
     * @param enabled 要求した状態
     */
    void timerChangeSucceeded(const QString &unit, bool enabled);

    /**
     * @brief タイマの変更失敗時に発行する
     * @param unit 変更を要求したユニット名
     * @param error 表示用のエラー文言
     */
    void timerChangeFailed(const QString &unit, const QString &error);

private:
    // 内部状態の更新
    /**
     * @brief 設定の実行中状態を更新する
     * @param busy 新しい状態
     */
    void setConfigBusy(bool busy);

    /**
     * @brief タイマの実行中状態を更新する
     * @param busy 新しい状態
     */
    void setTimersBusy(bool busy);

    /**
     * @brief 読み込み済みの値と編集内容を破棄する
     * @note ロールバック可否もfalseへ戻す (fail-closed)
     *       変化した項目のシグナルを発行する
     */
    void clearLoadedState();

    /**
     * @brief ロールバック可否を再計算する
     * @note 値が変わった場合だけrollbackSupportedChangedを発行する
     */
    void updateRollbackSupported();

    /**
     * @brief 編集状態の版番号を進めてeditsChangedを発行する
     */
    void bumpRevision();

    // 非同期呼び出しの開始
    /**
     * @brief GetConfigを発行して応答を処理する
     * @note 呼び出し前にm_configBusyをtrueにしておくこと
     *       応答時にm_generationが変わっていれば応答を捨てる
     */
    void startLoad();

    /**
     * @brief GetTimerStatesを発行して応答を処理する
     * @note 呼び出し前にm_timersBusyをtrueにしておくこと
     */
    void startTimersLoad();

    // 設定値
    QString m_configName;                   // 対象のsnapper設定名
    QMap<QString, QString> m_loaded;        // 最後に読み込んだ編集可能キーの値
    QMap<QString, QString> m_edited;        // 読み込み値と異なる編集中の値
    QString m_subvolume;                    // 読み込んだSUBVOLUME
    QString m_fstype;                       // 読み込んだFSTYPE
    QString m_qgroup;                       // 読み込んだQGROUP
    bool m_configLoaded;                    // 設定値を読み込み済みか
    bool m_rollbackSupported;               // ロールバック可否のキャッシュ
    int m_revision;                         // 編集状態の版番号

    // 実行状態
    bool m_configBusy;                      // 設定の読み込みまたは保存の実行中
    bool m_timersBusy;                      // タイマ状態の取得または変更の実行中
    quint64 m_generation;                   // 古い応答を判別する世代番号

    // タイマ
    QVariantList m_timers;                  // タイマ状態の一覧
};

#endif // SNAPPERSETTINGSMODEL_H
