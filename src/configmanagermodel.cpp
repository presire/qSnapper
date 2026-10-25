/**
 * @file configmanagermodel.cpp
 * @brief snapper設定の作成と削除を行うViewModelの実装
 */

#include "configmanagermodel.h"
#include "snapperservice.h"
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRegularExpression>
#include <QStringList>

namespace {
    // 設定名の長さ上限 (サーバ側の規則と一致させる)
    constexpr int MAX_CONFIG_NAME_LENGTH = 64;

    // サブボリュームパスの長さ上限 (サーバ側の規則と一致させる)
    constexpr int MAX_SUBVOLUME_PATH_BYTES = 4095;
}

/**
 * @brief ConfigManagerModelを構築する
 * @param parent Qt親オブジェクト
 */
ConfigManagerModel::ConfigManagerModel(QObject *parent)
    : QObject(parent)
    , m_snapperService(SnapperService::instance())
    , m_busy(false)
    , m_generation(0)
{
}

/**
 * @brief 設定名がサーバ側の規則に適合するか判定する
 * @param name 判定する設定名
 * @return 適合する場合はtrue
 * @note サーバ側と同じ規則を写して、認証を求める前に画面で誤りを示すため
 *       名前の全体が一致することを要求し、末尾の改行などで規則をすり抜けないようにする
 */
bool ConfigManagerModel::isValidConfigName(const QString &name) const
{
    if (name.size() > MAX_CONFIG_NAME_LENGTH) {
        return false;
    }

    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")));
    return pattern.match(name).hasMatch();
}

/**
 * @brief サブボリュームの絶対パスがサーバ側の規則に適合するか判定する
 * @param path 判定する絶対パス
 * @return 適合する場合はtrue
 * @note サーバ側と同じ規則を写して、認証を求める前に画面で誤りを示すため
 *       ルート (/) と、.や..を含むパスは、設定の対象として曖昧になるため拒否する
 */
bool ConfigManagerModel::isValidSubvolumePath(const QString &path) const
{
    if (path.isEmpty()
            || path == QLatin1String("/")
            || path.endsWith(QLatin1Char('/'))
            || !path.startsWith(QLatin1Char('/'))
            || path.contains(QLatin1String("//"))
            || path.toUtf8().size() > MAX_SUBVOLUME_PATH_BYTES) {
        return false;
    }

    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("/[A-Za-z0-9._@+/-]*")));
    if (!pattern.match(path).hasMatch()) {
        return false;
    }

    const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QLatin1String(".") || component == QLatin1String("..")) {
            return false;
        }
    }

    return !components.isEmpty();
}

/**
 * @brief 設定の作成を要求する
 * @param name 作成する設定名
 * @param subvolume 設定が対象とするBtrfsサブボリュームの絶対パス
 * @note 実行中は新しい要求を無視する
 *       画面の検証を通過しない値は送らず、認証ダイアログを無駄に表示しない
 *       成功時は設定一覧を読み直した後に新しい設定を選択する
 */
void ConfigManagerModel::createConfig(const QString &name, const QString &subvolume)
{
    if (m_busy) {
        return;
    }

    if (!isValidConfigName(name) || !isValidSubvolumePath(subvolume)) {
        // サーバへ要求していない場合も、一覧が別の操作で変わっている可能性があるため読み直す
        m_snapperService->refreshConfigs();
        emit configCreationFailed(tr("The configuration name or subvolume is not valid."));
        return;
    }

    const quint64 generation = ++m_generation;
    setBusy(true);

    auto *watcher = new QDBusPendingCallWatcher(
        m_snapperService->asyncCreateConfig(name, subvolume), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation, name]() {
        watcher->deleteLater();

        // 新しい要求が始まっている場合は、古い応答で表示を乱さない
        if (generation != m_generation) {
            return;
        }

        QDBusPendingReply<bool> reply = *watcher;
        const bool created = !reply.isError() && reply.value();

        setBusy(false);

        // 成否にかかわらず一覧を読み直し、コンボボックスを実際の状態に合わせる
        m_snapperService->refreshConfigs();

        if (reply.isError()) {
            emit configCreationFailed(SnapperService::errorText(reply.error()));
            return;
        }
        if (!created) {
            emit configCreationFailed(tr("The service did not create the configuration."));
            return;
        }

        // 一覧を読み直した後に選択する (読み直しの前に選ぶと、一覧に無い設定として先頭へ戻されるため)
        m_snapperService->setCurrentConfig(name);
        emit configCreated(name);
    });
}

/**
 * @brief 設定の削除を要求する
 * @param name 削除する設定名
 * @note 実行中は新しい要求を無視する
 *       削除した設定が選択中だった場合は、一覧の読み直しで先頭の設定へ切り替わる
 */
void ConfigManagerModel::deleteConfig(const QString &name)
{
    if (m_busy) {
        return;
    }

    if (!isValidConfigName(name)) {
        // サーバへ要求していない場合も、一覧が別の操作で変わっている可能性があるため読み直す
        m_snapperService->refreshConfigs();
        emit configDeletionFailed(tr("The configuration name is not valid."));
        return;
    }

    const quint64 generation = ++m_generation;
    setBusy(true);

    auto *watcher = new QDBusPendingCallWatcher(
        m_snapperService->asyncDeleteConfig(name), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation, name]() {
        watcher->deleteLater();

        // 新しい要求が始まっている場合は、古い応答で表示を乱さない
        if (generation != m_generation) {
            return;
        }

        QDBusPendingReply<bool> reply = *watcher;
        const bool deleted = !reply.isError() && reply.value();

        setBusy(false);

        // 成否にかかわらず一覧を読み直し、選択中の設定が消えていれば先頭へ戻す
        m_snapperService->refreshConfigs();

        if (reply.isError()) {
            emit configDeletionFailed(SnapperService::errorText(reply.error()));
            return;
        }
        if (!deleted) {
            emit configDeletionFailed(tr("The service did not delete the configuration."));
            return;
        }

        emit configDeleted(name);
    });
}

/**
 * @brief 実行中状態を更新する
 * @param busy 新しい状態
 */
void ConfigManagerModel::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}
