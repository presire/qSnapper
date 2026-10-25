#ifndef CONFIGMANAGERMODEL_H
#define CONFIGMANAGERMODEL_H

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class SnapperService;

/**
 * @file configmanagermodel.h
 * @brief snapper設定の作成と削除を行うViewModel
 */

/**
 * @class ConfigManagerModel
 * @brief snapper設定の作成と削除を非同期に実行するViewModel
 *
 * CreateConfigとDeleteConfigをSnapperServiceの非同期ヘルパとQDBusPendingCallWatcherで実行し、GUIスレッドをブロックしない
 * 認可はサーバ側のpolkitが行うため、認証ダイアログの表示中は応答が遅れる
 *
 * 名前とサブボリュームの検証は画面での即時フィードバック用の写しであり、最終的な判断は特権サービスが行う
 * 結果にかかわらず設定一覧を読み直し、作成した設定は選択状態へ反映する
 * 作成と削除は同時に1件だけ実行し、実行中の要求は無視する
 *
 * m_busy : 作成または削除の実行中
 * m_generation : 要求ごとに増やし、古い応答を捨てるための世代番号
 */
class ConfigManagerModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT                                          // qmllintが型を解決できるように、ビルド時の型情報 (qmltypes) へ出力する

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)   // 作成または削除の実行中

public:
    /**
     * @brief ConfigManagerModelを構築する
     * @param parent Qt親オブジェクト
     */
    explicit ConfigManagerModel(QObject *parent = nullptr);

    /**
     * @brief 作成または削除の実行中か取得する
     * @return 実行中の場合はtrue
     */
    bool busy() const { return m_busy; }

    /**
     * @brief 設定名がサーバ側の規則に適合するか判定する
     * @param name 判定する設定名
     * @return 先頭が英数字で、以降が英数字と_と-だけで64文字以内の場合はtrue
     * @note サーバ側と同じ規則を写して、認証を求める前に画面で誤りを示すためのもの
     */
    Q_INVOKABLE bool isValidConfigName(const QString &name) const;

    /**
     * @brief サブボリュームの絶対パスがサーバ側の規則に適合するか判定する
     * @param path 判定する絶対パス
     * @return 先頭が/で、//と.と..を含まず、末尾が/でなく、/以外で、4095バイト以内の場合はtrue
     * @note サーバ側と同じ規則を写して、認証を求める前に画面で誤りを示すためのもの
     *       ルート (/) はシステムのルート設定を保護するためサーバ側が拒否する
     */
    Q_INVOKABLE bool isValidSubvolumePath(const QString &path) const;

    /**
     * @brief 設定の作成を要求する
     * @param name 作成する設定名
     * @param subvolume 設定が対象とするBtrfsサブボリュームの絶対パス
     * @note 実行中は新しい要求を無視する
     *       結果はconfigCreatedまたはconfigCreationFailedで通知する
     *       成功時は設定一覧を読み直した後に新しい設定を選択する
     */
    Q_INVOKABLE void createConfig(const QString &name, const QString &subvolume);

    /**
     * @brief 設定の削除を要求する
     * @param name 削除する設定名
     * @note 対象設定の全スナップショットも削除されるため、実行前に画面で確認する
     *       結果はconfigDeletedまたはconfigDeletionFailedで通知する
     */
    Q_INVOKABLE void deleteConfig(const QString &name);

signals:
    /**
     * @brief 実行中状態が変化したときに発行する
     */
    void busyChanged();

    /**
     * @brief 設定の作成に成功したときに発行する
     * @param name 作成された設定名
     */
    void configCreated(const QString &name);

    /**
     * @brief 設定の作成に失敗したときに発行する
     * @param error 利用者へ表示する失敗理由
     */
    void configCreationFailed(const QString &error);

    /**
     * @brief 設定の削除に成功したときに発行する
     * @param name 削除された設定名
     */
    void configDeleted(const QString &name);

    /**
     * @brief 設定の削除に失敗したときに発行する
     * @param error 利用者へ表示する失敗理由
     */
    void configDeletionFailed(const QString &error);

private:
    /**
     * @brief 実行中状態を更新する
     * @param busy 新しい状態
     */
    void setBusy(bool busy);

    SnapperService *m_snapperService;   // SnapperServiceシングルトンへの参照
    bool m_busy;                        // 作成または削除の実行中
    quint64 m_generation;               // 古い応答を捨てるための世代番号
};

#endif // CONFIGMANAGERMODEL_H
