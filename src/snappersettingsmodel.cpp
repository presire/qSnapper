#include "snappersettingsmodel.h"
#include "snapperservice.h"
#include "snappertimerstate.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRegularExpression>
#include <QVariantMap>

namespace {

// 編集可能なキーと値の種別 (画面に表示する順序)
// 特権サービスが受け付けるキーと同じ18個に限り、それ以外は設定ファイルの破壊や権限の逸脱につながるため扱わない
struct KeyDefinition
{
    const char *key;    // 設定キー
    const char *kind;   // 値の種別 (boolean / seconds / countOrRange / fraction)
};

const KeyDefinition KEY_DEFINITIONS[] = {
    {"NUMBER_CLEANUP", "boolean"},
    {"NUMBER_MIN_AGE", "seconds"},
    {"NUMBER_LIMIT", "countOrRange"},
    {"NUMBER_LIMIT_IMPORTANT", "countOrRange"},
    {"TIMELINE_CREATE", "boolean"},
    {"TIMELINE_CLEANUP", "boolean"},
    {"TIMELINE_MIN_AGE", "seconds"},
    {"TIMELINE_LIMIT_HOURLY", "countOrRange"},
    {"TIMELINE_LIMIT_DAILY", "countOrRange"},
    {"TIMELINE_LIMIT_WEEKLY", "countOrRange"},
    {"TIMELINE_LIMIT_MONTHLY", "countOrRange"},
    {"TIMELINE_LIMIT_QUARTERLY", "countOrRange"},
    {"TIMELINE_LIMIT_YEARLY", "countOrRange"},
    {"EMPTY_PRE_POST_CLEANUP", "boolean"},
    {"EMPTY_PRE_POST_MIN_AGE", "seconds"},
    {"SPACE_LIMIT", "fraction"},
    {"FREE_LIMIT", "fraction"},
    {"BACKGROUND_COMPARISON", "boolean"},
};

// 管理対象のsystemdタイマ (画面に表示する順序)
// 特権サービスが操作を許可する3種類と同じにする
const char *const TIMER_UNITS[] = {
    "snapper-timeline.timer",
    "snapper-cleanup.timer",
    "snapper-boot.timer",
};

/**
 * @brief キーの値の種別を取得する
 * @param key 設定キー
 * @return 値の種別、編集可能キー以外では空文字列
 */
QString kindOf(const QString &key)
{
    for (const KeyDefinition &def : KEY_DEFINITIONS) {
        if (key == QLatin1String(def.key)) {
            return QString::fromLatin1(def.kind);
        }
    }
    return QString();
}

/**
 * @brief 先頭が0でない0から999999999までの10進数かを判定する
 * @param text 判定する文字列
 * @param number 判定に成功した場合の数値 (nullptrの場合は格納しない)
 * @return 規則を満たす場合はtrue
 * @note 特権サービスの検証と同じく、先頭ゼロ、符号、空白を許可しない
 *       9桁までに制限するため、桁あふれは起きない
 */
bool parseDecimal(const QString &text, qint64 *number)
{
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,8})$"));
    if (!pattern.match(text).hasMatch()) {
        return false;
    }
    if (number) {
        *number = text.toLongLong();
    }
    return true;
}

} // namespace

/**
 * @brief SnapperSettingsModelを構築する
 * @param parent Qt親オブジェクト
 */
SnapperSettingsModel::SnapperSettingsModel(QObject *parent)
    : QObject(parent)
    , m_configLoaded(false)
    , m_rollbackSupported(false)
    , m_revision(0)
    , m_configBusy(false)
    , m_timersBusy(false)
    , m_generation(0)
{
}

/**
 * @brief SnapperSettingsModelを破棄する
 * @note 実行中の問い合わせの監視はQt親子関係で破棄され、応答は届かない
 */
SnapperSettingsModel::~SnapperSettingsModel() = default;

/**
 * @brief 対象のsnapper設定名を変更する
 * @param name 新しい設定名
 * @note 変更時は読み込み済みの値と編集内容を破棄するが、自動では読み込まない
 *       呼び出し側が画面の表示状態に合わせて読み込む時期を決めるため
 *       実行中の読み込みと保存は世代番号で無効化する (古い設定の値を新しい設定として表示しないため)
 */
void SnapperSettingsModel::setConfigName(const QString &name)
{
    if (m_configName == name) {
        return;
    }
    m_configName = name;
    ++m_generation;
    setConfigBusy(false);
    clearLoadedState();
    emit configNameChanged();
}

/**
 * @brief 編集中の値がすべて妥当か判定する
 * @return 編集中の値がすべてisValidValueを満たす場合はtrue、編集が無い場合もtrue
 */
bool SnapperSettingsModel::isModifiedValid() const
{
    for (auto it = m_edited.constBegin(); it != m_edited.constEnd(); ++it) {
        if (!isValidValue(it.key(), it.value())) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 編集可能キーの一覧を取得する
 * @return 画面に表示する順序の18個のキー
 */
QStringList SnapperSettingsModel::editableKeys() const
{
    QStringList keys;
    for (const KeyDefinition &def : KEY_DEFINITIONS) {
        keys.append(QString::fromLatin1(def.key));
    }
    return keys;
}

/**
 * @brief キーの値の種別を取得する
 * @param key 設定キー
 * @return boolean / seconds / countOrRange / fractionのいずれか、編集可能キー以外では空文字列
 */
QString SnapperSettingsModel::valueKind(const QString &key) const
{
    return kindOf(key);
}

/**
 * @brief キーの現在値を取得する
 * @param key 設定キー
 * @return 編集中ならその値、そうでなければ読み込んだ値、無ければ空文字列
 */
QString SnapperSettingsModel::value(const QString &key) const
{
    auto edited = m_edited.constFind(key);
    if (edited != m_edited.constEnd()) {
        return edited.value();
    }
    return m_loaded.value(key);
}

/**
 * @brief キーの値を編集する
 * @param key 設定キー (編集可能な18キー以外は無視する)
 * @param value 新しい値 (不正な値も保持し、保存時に拒否する)
 * @note 未読み込みの間は編集できない (比較対象の読み込み値が無いため)
 *       読み込み値と同じ値に戻した場合は編集を取り消す
 *       変化があればeditsChangedを発行する
 */
void SnapperSettingsModel::setValue(const QString &key, const QString &value)
{
    if (!m_configLoaded || kindOf(key).isEmpty()) {
        return;
    }

    if (value == m_loaded.value(key)) {
        if (m_edited.remove(key) == 0) {
            return;
        }
    } else {
        auto edited = m_edited.constFind(key);
        if (edited != m_edited.constEnd() && edited.value() == value) {
            return;
        }
        m_edited.insert(key, value);
    }
    bumpRevision();
}

/**
 * @brief キーが読み込み値から変更されているか判定する
 * @param key 設定キー
 * @return 変更されている場合はtrue
 */
bool SnapperSettingsModel::isModified(const QString &key) const
{
    return m_edited.contains(key);
}

/**
 * @brief 値がキーの規則を満たすか判定する
 * @param key 設定キー
 * @param value 検証する値
 * @return 規則を満たす場合はtrue、編集可能キー以外ではfalse
 * @note 画面でのフィードバック専用であり、特権サービス側の検証を代替しない
 *       規則は特権サービスと同じにしている
 *       範囲指定 (MIN-MAX) は最小値が最大値以下であること
 */
bool SnapperSettingsModel::isValidValue(const QString &key, const QString &value) const
{
    const QString kind = kindOf(key);

    if (kind == QLatin1String("boolean")) {
        return value == QLatin1String("yes") || value == QLatin1String("no");
    }
    if (kind == QLatin1String("seconds")) {
        return parseDecimal(value, nullptr);
    }
    if (kind == QLatin1String("countOrRange")) {
        const int dash = value.indexOf(QLatin1Char('-'));
        if (dash < 0) {
            return parseDecimal(value, nullptr);
        }
        qint64 minimum = 0;
        qint64 maximum = 0;
        return parseDecimal(value.left(dash), &minimum)
            && parseDecimal(value.mid(dash + 1), &maximum)
            && minimum <= maximum;
    }
    if (kind == QLatin1String("fraction")) {
        static const QRegularExpression pattern(
            QStringLiteral("^(0(\\.[0-9]{1,6})?|1(\\.0{1,6})?)$"));
        return pattern.match(value).hasMatch();
    }
    return false;
}

/**
 * @brief 設定値を非同期に読み込み直す
 * @note 編集中の値は破棄される
 *       設定名が空、または読み込み / 保存の実行中は何もしない
 *       失敗時は読み込み済みの値を破棄し、loadFailedを発行する
 */
void SnapperSettingsModel::reload()
{
    if (m_configName.isEmpty() || m_configBusy) {
        return;
    }
    setConfigBusy(true);
    startLoad();
}

/**
 * @brief GetConfigを発行して応答を処理する
 * @note 応答時に世代番号が変わっていれば (設定名の変更により) 応答を捨てる
 *       失敗時は古い値を残さないよう、読み込み済みの状態を破棄する (fail-closed)
 */
void SnapperSettingsModel::startLoad()
{
    const quint64 generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(
        SnapperService::instance()->asyncGetConfig(m_configName), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation) {
            return;
        }

        QDBusPendingReply<QMap<QString, QString>> reply = *watcher;
        setConfigBusy(false);

        if (reply.isError()) {
            clearLoadedState();
            emit loadFailed(SnapperService::errorText(reply.error()));
            return;
        }

        const QMap<QString, QString> values = reply.value();
        m_loaded.clear();
        for (const KeyDefinition &def : KEY_DEFINITIONS) {
            const QString key = QString::fromLatin1(def.key);
            if (values.contains(key)) {
                m_loaded.insert(key, values.value(key));
            }
        }
        m_edited.clear();
        m_subvolume = values.value(QStringLiteral("SUBVOLUME"));
        m_fstype = values.value(QStringLiteral("FSTYPE"));
        m_qgroup = values.value(QStringLiteral("QGROUP"));
        m_configLoaded = true;

        updateRollbackSupported();
        emit configLoadedChanged();
        bumpRevision();
    });
}

/**
 * @brief 変更されたキーだけを特権サービスへ書き込む
 * @note 変更が無い場合と、読み込み / 保存の実行中は何もしない
 *       不正な値が含まれる場合は送信せずsaveFailedを発行する
 *       応答の成否にかかわらず、実際に保存された内容を表示するために完了後に読み込み直す
 *       (成功時も特権サービスが正規化した値を表示し、失敗時は部分的な書き込みの有無を利用者が確認できる)
 */
void SnapperSettingsModel::save()
{
    if (m_configName.isEmpty() || m_configBusy || !m_configLoaded || m_edited.isEmpty()) {
        return;
    }

    QMap<QString, QString> changes;
    for (auto it = m_edited.constBegin(); it != m_edited.constEnd(); ++it) {
        if (it.value() == m_loaded.value(it.key())) {
            continue;
        }
        if (!isValidValue(it.key(), it.value())) {
            emit saveFailed(tr("Invalid value for %1.").arg(it.key()));
            return;
        }
        changes.insert(it.key(), it.value());
    }
    if (changes.isEmpty()) {
        return;
    }

    setConfigBusy(true);
    const quint64 generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(
        SnapperService::instance()->asyncWriteSnapperConfig(m_configName, changes), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation) {
            return;
        }

        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            emit saveFailed(SnapperService::errorText(reply.error()));
        } else {
            emit saveSucceeded();
        }

        // 実行中状態は維持したまま読み込み直し、保存と読み込みの間に別の操作が割り込まないようにする
        startLoad();
    });
}

/**
 * @brief Btrfsクォータの設定を特権サービスへ要求する (SetupQuota)
 * @note 設定が未読み込み、または読み込み / 保存の実行中は何もしない
 *       成功時はquotaSetupSucceededを発行し、QGROUPを表示へ反映するために読み込み直す
 *       (未保存の編集は破棄される)
 *       応答時に世代番号が変わっていれば (設定名の変更により) 応答を捨てる
 */
void SnapperSettingsModel::setupQuota()
{
    if (m_configName.isEmpty() || m_configBusy || !m_configLoaded) {
        return;
    }

    setConfigBusy(true);
    const quint64 generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(
        SnapperService::instance()->asyncSetupQuota(m_configName), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation != m_generation) {
            return;
        }

        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            emit quotaSetupFailed(SnapperService::errorText(reply.error()));
        } else if (!reply.value()) {
            emit quotaSetupFailed(tr("Failed to set up the quota."));
        } else {
            emit quotaSetupSucceeded();
        }

        // 実行中状態は維持したまま読み込み直し、設定と読み込みの間に別の操作が割り込まないようにする
        // 失敗時も、実際のQGROUPの状態を表示するために読み込み直す
        startLoad();
    });
}

/**
 * @brief 編集内容を破棄して読み込み値へ戻す
 */
void SnapperSettingsModel::revert()
{
    if (m_edited.isEmpty()) {
        return;
    }
    m_edited.clear();
    bumpRevision();
}

/**
 * @brief タイマ状態を非同期に取得し直す
 * @note 取得 / 変更の実行中は何もしない
 */
void SnapperSettingsModel::refreshTimers()
{
    if (m_timersBusy) {
        return;
    }
    setTimersBusy(true);
    startTimersLoad();
}

/**
 * @brief GetTimerStatesを発行して応答を処理する
 * @note 失敗時は古い状態を表示し続けないよう、一覧を空にする
 *       存在しないユニットはnot-found / unknownとして一覧に残し、画面側で利用不可として表示する
 */
void SnapperSettingsModel::startTimersLoad()
{
    auto *watcher = new QDBusPendingCallWatcher(
        SnapperService::instance()->asyncGetTimerStates(), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        watcher->deleteLater();

        QDBusPendingReply<SnapperTimerStateMap> reply = *watcher;
        QVariantList list;
        if (!reply.isError()) {
            const SnapperTimerStateMap states = reply.value();
            for (const char *unit : TIMER_UNITS) {
                const QString name = QString::fromLatin1(unit);
                const auto it = states.constFind(name);
                QVariantMap entry;
                entry.insert(QStringLiteral("unit"), name);
                entry.insert(QStringLiteral("unitFileState"),
                             it != states.constEnd() ? it->unitFileState : QStringLiteral("not-found"));
                entry.insert(QStringLiteral("activeState"),
                             it != states.constEnd() ? it->activeState : QStringLiteral("unknown"));
                list.append(entry);
            }
        }

        m_timers = list;
        emit timersChanged();
        setTimersBusy(false);

        if (reply.isError()) {
            emit timersLoadFailed(SnapperService::errorText(reply.error()));
        }
    });
}

/**
 * @brief タイマの有効化 / 無効化を要求する
 * @param unit 対象のユニット名
 * @param enabled trueで有効化して起動、falseで停止して無効化
 * @note 取得 / 変更の実行中は何もしない
 *       管理対象外のユニット名は特権サービスへ送らず失敗として扱う
 *       応答の成否にかかわらず、実際の状態を表示するために完了後にタイマ状態を取得し直す
 */
void SnapperSettingsModel::setTimerEnabled(const QString &unit, bool enabled)
{
    if (m_timersBusy) {
        return;
    }

    bool known = false;
    for (const char *candidate : TIMER_UNITS) {
        if (unit == QLatin1String(candidate)) {
            known = true;
            break;
        }
    }
    if (!known) {
        emit timerChangeFailed(unit, tr("Unknown timer unit."));
        return;
    }

    setTimersBusy(true);
    auto *watcher = new QDBusPendingCallWatcher(
        SnapperService::instance()->asyncSetTimerEnabled(unit, enabled), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, unit, enabled]() {
        watcher->deleteLater();

        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            emit timerChangeFailed(unit, SnapperService::errorText(reply.error()));
        } else {
            emit timerChangeSucceeded(unit, enabled);
        }

        // 実行中状態は維持したまま状態を取得し直す
        startTimersLoad();
    });
}

/**
 * @brief 設定の実行中状態を更新する
 * @param busy 新しい状態
 */
void SnapperSettingsModel::setConfigBusy(bool busy)
{
    if (m_configBusy == busy) {
        return;
    }
    m_configBusy = busy;
    emit configBusyChanged();
}

/**
 * @brief タイマの実行中状態を更新する
 * @param busy 新しい状態
 */
void SnapperSettingsModel::setTimersBusy(bool busy)
{
    if (m_timersBusy == busy) {
        return;
    }
    m_timersBusy = busy;
    emit timersBusyChanged();
}

/**
 * @brief 読み込み済みの値と編集内容を破棄する
 * @note ロールバック可否もfalseへ戻す (fail-closed)
 *       変化した項目のシグナルを発行する
 */
void SnapperSettingsModel::clearLoadedState()
{
    const bool changed = m_configLoaded
        || !m_loaded.isEmpty()
        || !m_edited.isEmpty()
        || !m_subvolume.isEmpty()
        || !m_fstype.isEmpty()
        || !m_qgroup.isEmpty();

    m_loaded.clear();
    m_edited.clear();
    m_subvolume.clear();
    m_fstype.clear();
    m_qgroup.clear();
    m_configLoaded = false;

    updateRollbackSupported();
    if (changed) {
        emit configLoadedChanged();
        bumpRevision();
    }
}

/**
 * @brief ロールバック可否を再計算する
 * @note 値が変わった場合だけrollbackSupportedChangedを発行する
 *       btrfsのルート (SUBVOLUMEが/) 以外は、システムのロールバック対象にならない
 */
void SnapperSettingsModel::updateRollbackSupported()
{
    const bool supported = m_configLoaded
        && m_fstype == QLatin1String("btrfs")
        && m_subvolume == QLatin1String("/");
    if (m_rollbackSupported == supported) {
        return;
    }
    m_rollbackSupported = supported;
    emit rollbackSupportedChanged();
}

/**
 * @brief 編集状態の版番号を進めてeditsChangedを発行する
 */
void SnapperSettingsModel::bumpRevision()
{
    ++m_revision;
    emit editsChanged();
}
