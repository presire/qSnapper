/**
 * @file snapshotoperations_configs.cpp
 * @brief configの作成と削除、スナップショットの一括削除、Btrfsの使用量とサブボリューム一覧の読み取り
 *
 * configの作成と削除、一括削除はsnapperdへ転送する
 * snapperdは同じconfigへの操作を内部の排他で直列化し、使用中のconfigやスナップショットを拒否し、configの一覧のキャッシュを更新するため、
 * 本サービスがlibsnapperで直接変更すると、snapperdが古い一覧を使い続けたり、snapperdの処理と重なったりする
 * 本サービスは外部コマンドを起動しないが、snapperdは変更の過程でプラグインやsystemctlを起動し得る
 *
 * 転送の前に、認可の後の状態を本サービスが検査し、完了の後に実際の状態を読み直して応答する
 * snapperdの応答だけでは、一部だけ変更された場合や応答が届かない場合を判断できないため
 * 転送してから完了するまでは、configを指定する全ての操作と復元の開始を拒否する (resolveConfigOrFail、共通ゲート、getSnapper)
 */

#include <QDebug>
#include <QDBusMessage>
#include <QSet>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "snapshotoperations.h"
#include "inputvalidator.h"
#include "configpathvalidation.h"
#include "btrfsinfo.h"
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/AsciiFile.h>
#include <snapper/Comparison.h>
#include <snapper/Exception.h>

using qsnapper::dbusservice::SnapperdOperation;
using qsnapper::security::TrustedPathResult;

namespace {
    using SnapshotNumberList = QList<uint>;  // 一括削除の番号 (authorizeThenのテンプレート引数を検査スクリプトが読めるよう別名にする)

    // 応答の期限
    // 期限を過ぎても変更は止めずに完了を待ち、その間は他の操作を拒否し続ける
    constexpr int CreateConfigReplyDeadlineMs = 2 * 60 * 1000;      // config作成 (subvolumeの作成だけで終わる)
    constexpr int DeleteConfigReplyDeadlineMs = 30 * 60 * 1000;     // config削除 (全てのスナップショットを削除する)
    constexpr int DeleteSnapshotsReplyDeadlineMs = 30 * 60 * 1000;  // 一括削除

    // 件数の上限
    constexpr qsizetype MaxRawSnapshotNumbers = 1000;   // 重複を除く前に受け付ける番号の件数 (除く処理そのものの負荷を抑える)
    constexpr qsizetype MaxUniqueSnapshotNumbers = 100; // 1回で削除する番号の件数 (snapperdの処理時間を抑える)
    constexpr qsizetype MaxConfigs = 256;               // 読み込むconfigの件数

    // config作成の固定値
    // FSTYPEはBtrfsだけを扱い、テンプレートはlibsnapperの既定だけを使う (利用者が任意のファイルを読ませないため)
    const QString CreateFstype = QStringLiteral("btrfs");           // 作成するconfigのFSTYPE
    const QString CreateTemplate = QStringLiteral("default");       // 作成に使うテンプレート名

    // libsnapperが読むファイルとディレクトリ
    const QString SysconfigFiles[] = {
        QStringLiteral("/etc/sysconfig/snapper"),   // openSUSE、Fedora
        QStringLiteral("/etc/default/snapper"),     // Debian、Ubuntu
    };
    const QString ConfigsDir = QStringLiteral("/etc/snapper/configs");                        // configのファイルを置くディレクトリ
    const QString EtcTemplate = QStringLiteral("/etc/snapper/config-templates/default");      // 優先して使われるテンプレート
    const QString UsrTemplate = QStringLiteral("/usr/share/snapper/config-templates/default"); // /etcに無い場合に使われるテンプレート

    // エラー本文 (config名、パス、番号、例外の本文を含めない)
    const QString InvalidConfigNameText = QStringLiteral("Invalid configName");
    const QString InvalidSubvolumeText = QStringLiteral("Invalid subvolume path");
    const QString RootConfigText = QStringLiteral("The root configuration cannot be changed here");
    const QString UntrustedPathText = QStringLiteral("The subvolume path is not acceptable");
    const QString NotSubvolumeText = QStringLiteral("The path is not a Btrfs subvolume");
    const QString ConfigExistsText = QStringLiteral("A configuration already exists for this name or subvolume");
    const QString ConfigNotFoundText = QStringLiteral("Configuration not found");
    const QString SnapshotsDirExistsText = QStringLiteral("The subvolume already has a .snapshots directory");
    const QString ConfigFilesUntrustedText = QStringLiteral("The Snapper configuration files could not be verified");
    const QString SnapperErrorText = QStringLiteral("Snapper reported an error");
    const QString SnapperdUnavailableText = QStringLiteral("The Snapper service is not available");
    const QString InterruptedText = QStringLiteral("The operation was interrupted");
    const QString VerifyFailedText = QStringLiteral("The result of the operation could not be verified");
    const QString InvalidNumbersText = QStringLiteral("Invalid snapshot numbers");
    const QString TooManyNumbersText = QStringLiteral("Too many snapshot numbers");
    const QString SnapshotNotFoundText = QStringLiteral("Snapshot not found");
    const QString SnapshotProtectedText = QStringLiteral("The current, default, or active snapshot cannot be deleted");
    const QString SnapshotDirUntrustedText = QStringLiteral("The snapshot directory could not be verified");
    const QString NotBtrfsText = QStringLiteral("The filesystem is not Btrfs");
    const QString BtrfsLimitText = QStringLiteral("The filesystem information exceeds the supported limits");
    const QString BtrfsFailedText = QStringLiteral("Failed to read the filesystem information");

    /**
     * @brief libsnapperが読むsysconfigのファイルを選ぶ
     *
     * 最初に存在するファイルをlibsnapperと同じ順で選び、rootだけが書き換えられることを確かめる
     * @param path 選んだファイルのパスを受け取る
     * @return 信頼できるファイルが見つかった場合: true
     */
    bool trustedSysconfigFile(QString *path)
    {
        for (const QString &candidate : SysconfigFiles) {
            const TrustedPathResult result = qsnapper::security::checkTrustedRegularFile(candidate);
            if (result == TrustedPathResult::Missing) {
                continue;
            }
            if (result != TrustedPathResult::Ok) {
                return false;
            }
            *path = candidate;
            return true;
        }
        return false;
    }

    /**
     * @brief SNAPPER_CONFIGSに登録されたconfigを全て読み込む
     *
     * libsnapperのgetConfigs()は読み込めないconfigを黙って除くため、重複の確認から漏れないよう、登録された名前を1つずつ読み込む
     * 1つでも読み込めない場合は、判定できないものとして失敗にする
     * @param configs 読み込んだconfigの名前とSUBVOLUMEを受け取る
     * @return 全てを読み込めた場合: true
     */
    bool readAllConfigs(QMap<QString, QString> *configs)
    {
        QString sysconfig;
        if (!trustedSysconfigFile(&sysconfig)) {
            return false;
        }

        try {
            const snapper::SysconfigFile file(sysconfig.toStdString());
            std::vector<std::string> names;
            file.get_value("SNAPPER_CONFIGS", names);
            if (static_cast<qsizetype>(names.size()) > MaxConfigs) {
                return false;
            }
            configs->clear();
            for (const std::string &rawName : names) {
                const QString name = QString::fromStdString(rawName);
                if (!qsnapper::security::validateConfigName(name)) {
                    return false;
                }
                if (qsnapper::security::checkTrustedRegularFile(ConfigsDir + QLatin1Char('/') + name)
                        != TrustedPathResult::Ok) {
                    return false;
                }
                const snapper::ConfigInfo info = snapper::Snapper::getConfig(rawName, "/");
                configs->insert(name, QString::fromStdString(info.get_subvolume()));
            }
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    /**
     * @brief 2つのパスが同じsubvolumeを指すかを判定する
     *
     * 既存のconfigのSUBVOLUMEは信頼できない親を含む場合もあるため、ここではsymlinkを辿って比べる
     * 重複を見つけるための判定であり、見つけた場合は拒否するだけなので、辿っても拒否が緩くなることはない
     * @param path 比べるパス
     * @param target 作成先のfstatの結果
     * @param same 同じ場合にtrueを受け取る
     * @return 判定できた場合: true (パスが存在しない場合は同じではないと判定する)
     */
    bool sameInode(const QString &path, const struct stat &target, bool *same)
    {
        struct stat st {};
        if (::stat(path.toUtf8().constData(), &st) != 0) {
            *same = false;
            return errno == ENOENT || errno == ENOTDIR;
        }
        *same = st.st_dev == target.st_dev && st.st_ino == target.st_ino;
        return true;
    }

    /**
     * @brief config作成が読むテンプレートを、rootだけが書き換えられることを確かめる
     *
     * libsnapperは/etcのテンプレートを優先するため、/etcに存在する場合はそれだけを検査し、/usrへ代えない
     * @return 信頼できる場合: true
     */
    bool trustedTemplate()
    {
        const TrustedPathResult etc = qsnapper::security::checkTrustedRegularFile(EtcTemplate);
        if (etc == TrustedPathResult::Ok) {
            return true;
        }
        if (etc != TrustedPathResult::Missing) {
            return false;
        }
        return qsnapper::security::checkTrustedRegularFile(UsrTemplate) == TrustedPathResult::Ok;
    }

    /**
     * @brief 呼び出し元へ非同期にtrueを返す
     * @param reply 確保した応答コンテキスト
     */
    void sendAsyncTrue(const qsnapper::dbusservice::CallReply &reply)
    {
        if (reply.fromDBus) {
            reply.connection.send(reply.message.createReply(QVariant::fromValue(true)));
        }
    }

    /**
     * @brief 変更を確かめられなかった場合のエラーを、結果の種類から選ぶ
     * @param outcome snapperdとの通信の結果
     * @param stoppedByRestore 復元の開始により転送を止めた場合: true
     * @return D-Busエラー名と固定の文言
     * @note 転送していない場合 (NotForwarded、Outdated、Interrupted) は変更が無いことが分かっているため、理由を返す
     *       転送した場合は一部だけ変更された可能性があるため、呼び出し側が状態を読み直してから使う
     *       snapperdのエラー本文はconfig名やパスを含み得るため、転送しない
     */
    std::pair<QString, QString> outcomeError(SnapperdOperation::Outcome outcome, bool stoppedByRestore)
    {
        switch (outcome) {
        case SnapperdOperation::Outcome::NotForwarded:  // snapperdに接続できない
            return {QDBusError::errorString(QDBusError::Failed), SnapperdUnavailableText};
        case SnapperdOperation::Outcome::Outdated:      // snapperdの一覧が古いため転送していない
            return {QString::fromLatin1(qsnapper::dbusservice::SNAPSHOT_LIST_OUTDATED_ERROR_NAME),
                    QString::fromLatin1(qsnapper::dbusservice::SNAPSHOT_LIST_OUTDATED_ERROR_TEXT)};
        case SnapperdOperation::Outcome::Interrupted:   // 呼び出し元の切断、または復元の開始
            if (stoppedByRestore) {
                return {QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_NAME),
                        QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_TEXT)};
            }
            return {QDBusError::errorString(QDBusError::Failed), InterruptedText};
        case SnapperdOperation::Outcome::Unknown:       // 応答が届かずsnapperdが終了した
            return {QString::fromLatin1(qsnapper::dbusservice::OUTCOME_UNKNOWN_ERROR_NAME),
                    QString::fromLatin1(qsnapper::dbusservice::OUTCOME_UNKNOWN_ERROR_TEXT)};
        case SnapperdOperation::Outcome::Rejected:      // snapperdが拒否した
        case SnapperdOperation::Outcome::Settled:       // 応答は失われたが処理は終わった (状態を読み直して判断する)
        case SnapperdOperation::Outcome::Replied:
            break;
        }
        return {QDBusError::errorString(QDBusError::Failed), SnapperErrorText};
    }

    /**
     * @brief 削除を確かめるまで、スナップショットのディレクトリのfdを保持する
     * @param fd 保持するfd (所有権を移す)
     * @return 最後の参照が無くなった時点でfdを閉じる共有ポインタ
     * @note 完了の処理はコピーされる関数オブジェクトに保持されるため、閉じ忘れと二重の解放を避けるよう共有ポインタにする
     */
    std::shared_ptr<const int> holdDirectoryFd(int fd)
    {
        return std::shared_ptr<const int>(new int(fd), [](const int *held) {
            if (*held >= 0) {
                ::close(*held);
            }
            delete held;
        });
    }

    /**
     * @brief 削除を要求した番号のうち、ディレクトリのエントリが実際に無くなったものを調べる
     * @param snapshotsFd 削除の前に開いたスナップショットのディレクトリ (.snapshots) のfd
     * @param snapshotsPath 同じディレクトリのパス (差し替えの確認に使う)
     * @param before 削除の前に開いた時点のディレクトリの状態
     * @param numbers 削除を要求した番号
     * @param deleted 無くなった番号を受け取る
     * @return 全ての番号について判定できた場合: true
     * @note libsnapperの一覧は、読み取りに失敗したスナップショットを黙って除くため、一覧に無いことだけでは削除されたと判断しない
     *       削除の前に開いたディレクトリの中で、番号のエントリが無い (ENOENT) ことを確かめる
     *       ENOENT以外のエラー、またはディレクトリが差し替えられた場合は、判定できないとして全体を失敗にする
     */
    bool collectDeletedEntries(int snapshotsFd, const QString &snapshotsPath, const struct stat &before,
                               const QList<uint> &numbers, QList<uint> *deleted)
    {
        int currentFd = -1;
        struct stat current {};
        const bool sameDirectory =
            qsnapper::security::openTrustedDirectory(snapshotsPath, &currentFd, &current) == TrustedPathResult::Ok
            && current.st_dev == before.st_dev && current.st_ino == before.st_ino;
        if (currentFd >= 0) {
            ::close(currentFd);
        }
        if (!sameDirectory) {
            return false;
        }

        for (const uint number : numbers) {
            const QByteArray entryName = QByteArray::number(number);
            struct stat entry {};
            if (::fstatat(snapshotsFd, entryName.constData(), &entry, AT_SYMLINK_NOFOLLOW) == 0) {
                continue;
            }
            if (errno != ENOENT) {
                return false;
            }
            deleted->append(number);
        }
        return true;
    }
}

/**
 * @brief 指定したconfigに、実行直前または実行中の復元計画があるかを判定する
 *
 * 凍結済み・実行中の計画は、configとスナップショットが残っていることを前提に進むため、それらがある間は削除しない
 * 作成中 (Staging) の計画は判定に含めない
 * 差分を閲覧できる利用者なら誰でも空の計画を作れるため、Staging計画を含めると管理者のconfig操作を止められてしまう
 * Staging計画は、認可済みのconfig操作の直前にdiscardStagingRestorePlansで取り消す
 * 同じ名前のconfigや同じ番号のスナップショットが作り直された後に、古い計画が別の内容を指して確定されないようにするため
 *
 * 計画の状態はownerを問わずregistryから直接読む
 * 他のownerの計画を読めないことを理由に判定不能とすると、それだけでconfig操作を拒否することになるため
 *
 * @param configName 検証済みの設定名
 * @return 実行直前または実行中の計画がある場合: true
 */
bool SnapshotOperations::hasUnfinishedRestorePlan(const QString &configName)
{
    for (const RestoreExecution &execution : std::as_const(m_restoreExecutions)) {
        if (execution.configName == configName) {
            return true;
        }
    }

    // 期限切れの凍結済み計画を先に終端させ、放置された計画がconfig操作を止め続けないようにする
    purgeExpiredRestorePlans();
    return m_restoreRegistry.hasActivePlanForConfig(configName);
}

/**
 * @brief 指定configのStaging計画を、ownerを問わず取り消して終端させる
 *
 * ownerを問わず取り消すため、認可済みのconfig操作の直前でだけ呼ぶ
 * finishRestorePlanで終端させるため、mountの解除と実行contextの削除も通常の終端と同じ経路で行われる
 *
 * @param configName 検証済みの設定名
 * @param snapshotNumber 参照する計画だけに絞るスナップショット番号 (負の値なら絞らない)
 */
void SnapshotOperations::discardStagingRestorePlans(const QString &configName, int snapshotNumber)
{
    const QStringList stagingIds = m_restoreRegistry.stagingIdsForConfig(configName, snapshotNumber);
    for (const QString &id : stagingIds) {
        // 計画は別の接続が作ったものであるため、registryにはその計画のownerとして取り消しを伝える
        qsnapper::restore::ManifestError cancelError = qsnapper::restore::ManifestError::None;
        if (!m_restoreRegistry.cancel(id, m_restorePlanOwners.value(id), &cancelError)) {
            // 既に終端したか期限切れで消えた計画は、ここで終端させる必要がない
            continue;
        }
        finishRestorePlan(id, qsnapper::restore::ManifestState::Cancelled,
                          QStringLiteral("Restore plan was discarded because its configuration changed"));
    }
}

/**
 * @brief snapperdへ変更を転送し、完了時に結果を読み直して応答する
 *
 * 転送の前に共有のSnapperインスタンスと比較キャッシュを破棄し、完了まで再び読み込ませない
 * 変更の直前に復元の実行中でないことを確かめ、実行中であれば転送しない
 * 応答の期限を過ぎた場合は、結果が分からない旨を1回だけ返し、完了の後に排他を解く
 */
void SnapshotOperations::startSnapperdOperation(
    const CallReply &reply, const QDBusMessage &mutation, const QString &listConfig,
    const QList<uint> &requiredNumbers, int replyDeadlineMs,
    std::function<void(SnapperdOperation::Outcome, bool, const CallReply &, bool)> verify)
{
    m_comparisonCache.clear();
    m_snapshotListFingerprint.clear();
    m_snapper.reset();
    m_currentConfig.clear();

    auto stoppedByRestore = std::make_shared<bool>(false);
    auto replied = std::make_shared<bool>(false);
    auto *operation = new SnapperdOperation(
        reply.connection, reply.message.service(), mutation, listConfig, requiredNumbers,
        [this, stoppedByRestore]() {
            if (!m_restoreExecutions.isEmpty()) {
                *stoppedByRestore = true;
                return false;
            }
            return true;
        },
        this);
    m_snapperdOperation = operation;
    resetIdleTimer();

    operation->start(
        replyDeadlineMs,
        [this, reply, replied]() {
            *replied = true;
            sendAsyncError(reply, QString::fromLatin1(qsnapper::dbusservice::OUTCOME_UNKNOWN_ERROR_NAME),
                           QString::fromLatin1(qsnapper::dbusservice::OUTCOME_UNKNOWN_ERROR_TEXT));
        },
        [this, operation, reply, replied, stoppedByRestore, verify](SnapperdOperation::Outcome outcome) {
            operation->deleteLater();
            m_snapperdOperation.clear();
            // 変更の途中で読み込まれた状態を使わないよう、完了の後にもう1度破棄する
            m_comparisonCache.clear();
            m_snapshotListFingerprint.clear();
            m_snapper.reset();
            m_currentConfig.clear();
            verify(outcome, *stoppedByRestore, reply, *replied);
            resetIdleTimer();
        });
}

/**
 * @brief Btrfsのsubvolumeに新しいSnapper設定を作成する
 *
 * 認可の前は文字列の形式だけを検査する
 * パスの存在や種類を認可の前に答えると、未認可の呼び出し元がroot権限でファイルの有無を調べられるため
 */
bool SnapshotOperations::CreateConfig(const QString &configName, const QString &subvolume)
{
    if (!qsnapper::security::validateNewConfigName(configName)) {
        replyError(QDBusError::InvalidArgs, InvalidConfigNameText);
        return false;
    }
    if (!qsnapper::security::validateConfigSubvolumePath(subvolume)) {
        replyError(QDBusError::InvalidArgs, InvalidSubvolumeText);
        return false;
    }
    // rootという名前は、空のconfig名の解決先として既存のAPIが参照し、DeleteConfigでも削除できないため、別のsubvolumeにも付けさせない
    if (configName == QStringLiteral("root") || subvolume == QStringLiteral("/")) {
        replyError(QDBusError::AccessDenied, RootConfigText);
        return false;
    }
    if (rejectWhileTimerOperationRunning()) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.create-config"),
        [this, configName, subvolume]() {
            return createConfigAuthorized(configName, subvolume);
        });
}

/**
 * @brief 認可済みのCreateConfig本体
 *
 * 認可を待つ間に、同じ名前のconfigが作られたり、パスが差し替えられたりし得るため、全ての検査をここでやり直す
 * snapperdはパスの文字列から作成先を開き直すため、途中の全てのディレクトリがrootだけに書き込めることを求める
 * (root以外が書き込める場所では、検査の後に別のsubvolumeへ差し替えられても気付けない)
 * 作成先がrootの設定と同じsubvolume、または既存のconfigと同じsubvolumeの場合は拒否する
 */
bool SnapshotOperations::createConfigAuthorized(const QString &configName, const QString &subvolume)
{
    if (!qsnapper::security::validateNewConfigName(configName)
            || !qsnapper::security::validateConfigSubvolumePath(subvolume)
            || configName == QStringLiteral("root")
            || subvolume == QStringLiteral("/")) {
        replyError(QDBusError::InvalidArgs, InvalidSubvolumeText);
        return false;
    }
    if (rejectWhileTimerOperationRunning()) {
        return false;
    }

    QMap<QString, QString> configs;
    if (!readAllConfigs(&configs)) {
        replyError(QDBusError::Failed, ConfigFilesUntrustedText);
        return false;
    }
    if (configs.contains(configName)) {
        replyError(QDBusError::Failed, ConfigExistsText);
        return false;
    }

    int dirFd = -1;
    struct stat target {};
    const TrustedPathResult walk = qsnapper::security::openTrustedDirectory(subvolume, &dirFd, &target);
    if (walk != TrustedPathResult::Ok) {
        replyError(QDBusError::AccessDenied, UntrustedPathText);
        return false;
    }
    const bool isSubvolume = qsnapper::security::isBtrfsSubvolumeRoot(dirFd);
    ::close(dirFd);
    if (!isSubvolume) {
        replyError(QDBusError::NotSupported, NotSubvolumeText);
        return false;
    }

    // rootの設定と同じsubvolume (「/」と同じinode) は、rootの設定の作成として扱い拒否する
    bool same = false;
    if (!sameInode(QStringLiteral("/"), target, &same) || same) {
        replyError(QDBusError::AccessDenied, RootConfigText);
        return false;
    }
    for (auto it = configs.cbegin(); it != configs.cend(); ++it) {
        if (it.value() == subvolume || !sameInode(it.value(), target, &same) || same) {
            replyError(QDBusError::Failed, ConfigExistsText);
            return false;
        }
    }

    // 既に.snapshotsがある場合は、snapperdが失敗するため先に拒否する (自動で削除して再試行はしない)
    if (qsnapper::security::checkTrustedAbsent(subvolume + QStringLiteral("/.snapshots")) != TrustedPathResult::Ok) {
        replyError(QDBusError::Failed, SnapshotsDirExistsText);
        return false;
    }
    int configsFd = -1;
    if (qsnapper::security::openTrustedDirectory(ConfigsDir, &configsFd) != TrustedPathResult::Ok
            || qsnapper::security::checkTrustedAbsent(ConfigsDir + QLatin1Char('/') + configName)
                   != TrustedPathResult::Ok
            || !trustedTemplate()) {
        if (configsFd >= 0) {
            ::close(configsFd);
        }
        replyError(QDBusError::Failed, ConfigFilesUntrustedText);
        return false;
    }
    ::close(configsFd);

    // 同じ名前で実行直前または実行中の復元計画が残っている場合、作成したconfigを旧configの計画が使ってしまうため拒否する
    if (hasUnfinishedRestorePlan(configName)) {
        replyError(QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_NAME),
                   QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_TEXT));
        return false;
    }

    // 消えた名前のconfigや番号が作り直された後に、古い作成中の計画が別の内容を指して確定されないよう、変更の直前に破棄する
    discardStagingRestorePlans(configName, -1);

    const auto claimedReply = claimReplyForAsyncCompletion();
    if (!claimedReply) {
        return false;
    }

    QDBusMessage mutation = QDBusMessage::createMethodCall(
        SnapperdOperation::Service, SnapperdOperation::Path, SnapperdOperation::Interface,
        QStringLiteral("CreateConfig"));
    mutation << configName << subvolume << CreateFstype << CreateTemplate;

    startSnapperdOperation(
        *claimedReply, mutation, QString(), {}, CreateConfigReplyDeadlineMs,
        [this, configName, subvolume, target](SnapperdOperation::Outcome outcome, bool stoppedByRestore,
                                              const CallReply &reply, bool replied) {
            // 応答の種類に関わらず、作成されたconfigが要求どおりかを読み直して確かめる
            bool created = false;
            QMap<QString, QString> after;
            if (readAllConfigs(&after) && after.value(configName) == subvolume) {
                int fd = -1;
                struct stat now {};
                bool ok = qsnapper::security::openTrustedDirectory(subvolume, &fd, &now) == TrustedPathResult::Ok
                          && now.st_dev == target.st_dev && now.st_ino == target.st_ino;
                if (fd >= 0) {
                    ::close(fd);
                }
                int snapshotsFd = -1;
                ok = ok
                     && qsnapper::security::openTrustedDirectory(subvolume + QStringLiteral("/.snapshots"), &snapshotsFd)
                            == TrustedPathResult::Ok
                     && qsnapper::security::isBtrfsSubvolumeRoot(snapshotsFd);
                if (snapshotsFd >= 0) {
                    ::close(snapshotsFd);
                }
                try {
                    std::string fstype;
                    const snapper::ConfigInfo info = snapper::Snapper::getConfig(configName.toStdString(), "/");
                    ok = ok && info.get_value("FSTYPE", fstype) && QString::fromStdString(fstype) == CreateFstype;
                } catch (const std::exception &) {
                    ok = false;
                }
                created = ok;
            }

            if (replied) {
                return;
            }
            if (outcome == SnapperdOperation::Outcome::Replied && created) {
                sendAsyncTrue(reply);
                return;
            }
            if (outcome == SnapperdOperation::Outcome::Replied || created) {
                // 正常に応答したのに要求どおりでない、または失敗の応答なのに作成されている
                sendAsyncError(reply, QDBusError::errorString(QDBusError::Failed), VerifyFailedText);
                return;
            }
            const auto [name, text] = outcomeError(outcome, stoppedByRestore);
            sendAsyncError(reply, name, text);
        });
    return false;
}

/**
 * @brief Snapper設定と、その全てのスナップショットを削除する
 *
 * rootの設定は、システム全体のロールバックと復元が使うため、ここでは扱わない
 */
bool SnapshotOperations::DeleteConfig(const QString &configName)
{
    // resolveConfigOrFailは空の名前をrootとして扱うため使わず、名前そのものを検証する
    if (!qsnapper::security::validateConfigName(configName)) {
        replyError(QDBusError::InvalidArgs, InvalidConfigNameText);
        return false;
    }
    if (configName == QStringLiteral("root")) {
        replyError(QDBusError::AccessDenied, RootConfigText);
        return false;
    }
    if (rejectWhileTimerOperationRunning() || rejectWhileSnapperdOperationRunning()) {
        return false;
    }

    return authorizeThen<bool>(
        QStringLiteral("com.presire.qsnapper.delete-config"),
        [this, configName]() {
            return deleteConfigAuthorized(configName);
        });
}

/**
 * @brief 認可済みのDeleteConfig本体
 *
 * 認可を待つ間に、復元計画が作られたり、configが変わったりし得るため、全ての検査をここでやり直す
 * snapperdはconfigのSUBVOLUMEの下の.snapshotsを削除するため、SUBVOLUMEがrootの設定と同じでないこと、
 * 途中の全てのディレクトリがrootだけに書き込めることを求める
 * snapperdが古いスナップショット一覧を持っている場合は、削除の対象を取り違えないよう転送しない
 */
bool SnapshotOperations::deleteConfigAuthorized(const QString &configName)
{
    if (!qsnapper::security::validateConfigName(configName) || configName == QStringLiteral("root")) {
        replyError(QDBusError::InvalidArgs, InvalidConfigNameText);
        return false;
    }
    if (rejectWhileTimerOperationRunning()) {
        return false;
    }

    QMap<QString, QString> configs;
    if (!readAllConfigs(&configs)) {
        replyError(QDBusError::Failed, ConfigFilesUntrustedText);
        return false;
    }
    if (!configs.contains(configName)) {
        replyError(QDBusError::Failed, ConfigNotFoundText);
        return false;
    }

    const QString subvolume = configs.value(configName);
    if (!qsnapper::security::validateConfigSubvolumePath(subvolume) || subvolume == QStringLiteral("/")) {
        replyError(QDBusError::AccessDenied, RootConfigText);
        return false;
    }
    int dirFd = -1;
    struct stat target {};
    if (qsnapper::security::openTrustedDirectory(subvolume, &dirFd, &target) != TrustedPathResult::Ok) {
        replyError(QDBusError::AccessDenied, UntrustedPathText);
        return false;
    }
    ::close(dirFd);
    bool same = false;
    if (!sameInode(QStringLiteral("/"), target, &same) || same) {
        replyError(QDBusError::AccessDenied, RootConfigText);
        return false;
    }

    if (hasUnfinishedRestorePlan(configName)) {
        replyError(QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_NAME),
                   QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_TEXT));
        return false;
    }

    // snapperdの一覧と照合するため、本サービスから見えるスナップショットの番号を集める
    QList<uint> numbers;
    {
        snapper::Snapper *snapper = getSnapper(configName, true);
        if (!snapper) {
            replyError(QDBusError::Failed, SnapperErrorText);
            return false;
        }
        for (const snapper::Snapshot &snapshot : snapper->getSnapshots()) {
            if (!snapshot.isCurrent()) {
                numbers.append(snapshot.getNum());
            }
        }
    }

    // 消えた名前のconfigや番号が作り直された後に、古い作成中の計画が別の内容を指して確定されないよう、変更の直前に破棄する
    discardStagingRestorePlans(configName, -1);

    const auto claimedReply = claimReplyForAsyncCompletion();
    if (!claimedReply) {
        return false;
    }

    QDBusMessage mutation = QDBusMessage::createMethodCall(
        SnapperdOperation::Service, SnapperdOperation::Path, SnapperdOperation::Interface,
        QStringLiteral("DeleteConfig"));
    mutation << configName;

    startSnapperdOperation(
        *claimedReply, mutation, configName, numbers, DeleteConfigReplyDeadlineMs,
        [this, configName](SnapperdOperation::Outcome outcome, bool stoppedByRestore,
                           const CallReply &reply, bool replied) {
            // 一覧から消え、configのファイルも残っていない場合だけ、削除されたとみなす
            QMap<QString, QString> after;
            const bool readable = readAllConfigs(&after);
            const bool deleted = readable && !after.contains(configName)
                                 && qsnapper::security::checkTrustedAbsent(ConfigsDir + QLatin1Char('/') + configName)
                                        == TrustedPathResult::Ok;

            if (replied) {
                return;
            }
            if (outcome == SnapperdOperation::Outcome::Replied && deleted) {
                sendAsyncTrue(reply);
                return;
            }
            if (outcome == SnapperdOperation::Outcome::Replied || deleted || !readable) {
                sendAsyncError(reply, QDBusError::errorString(QDBusError::Failed), VerifyFailedText);
                return;
            }
            const auto [name, text] = outcomeError(outcome, stoppedByRestore);
            sendAsyncError(reply, name, text);
        });
    return false;
}

/**
 * @brief 複数のスナップショットを1回の認可で削除する
 *
 * 番号は1以上だけを受け付け、重複を除き、件数に上限を設ける
 * 重複を除く前の件数にも上限を設け、巨大な配列で処理を長引かせないようにする
 */
QList<uint> SnapshotOperations::DeleteSnapshots(const QString &configName, const QList<uint> &numbers)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return {};
    }
    if (numbers.size() > MaxRawSnapshotNumbers) {
        replyError(QDBusError::LimitsExceeded, TooManyNumbersText);
        return {};
    }
    QList<uint> unique;
    for (const uint number : numbers) {
        if (number == 0) {
            replyError(QDBusError::InvalidArgs, InvalidNumbersText);
            return {};
        }
        if (!unique.contains(number)) {
            unique.append(number);
        }
    }
    if (unique.isEmpty()) {
        replyError(QDBusError::InvalidArgs, InvalidNumbersText);
        return {};
    }
    if (unique.size() > MaxUniqueSnapshotNumbers) {
        replyError(QDBusError::LimitsExceeded, TooManyNumbersText);
        return {};
    }
    std::sort(unique.begin(), unique.end());

    return authorizeThen<SnapshotNumberList>(
        QStringLiteral("com.presire.qsnapper.delete-snapshots"),
        [this, config = *cfg, unique]() {
            return deleteSnapshotsAuthorized(config, unique);
        });
}

/**
 * @brief 認可済みのDeleteSnapshots本体
 *
 * 認可を待つ間に、復元計画が作られたり、スナップショットが削除されたりし得るため、全ての検査をここでやり直す
 * 1つでも存在しない番号、または削除できない番号 (現在・既定・使用中) がある場合は、何も削除せずに拒否する
 * 削除の後は、削除の前に開いたスナップショットのディレクトリで、実際に消えた番号だけを返す (要求より少なければ一部だけが削除された)
 * 消えたかどうかを判定できない番号がある場合は、削除した番号を返さずにエラーとする
 */
QList<uint> SnapshotOperations::deleteSnapshotsAuthorized(const QString &configName, const QList<uint> &numbers)
{
    if (!qsnapper::security::validateConfigName(configName) || numbers.isEmpty()
            || numbers.size() > MaxUniqueSnapshotNumbers
            || QSet<uint>(numbers.cbegin(), numbers.cend()).size() != numbers.size()
            || numbers.contains(0U)) {
        replyError(QDBusError::InvalidArgs, InvalidNumbersText);
        return {};
    }

    // 実行直前または実行中の復元計画は、計画を作った時点のスナップショットが残っていることを前提に進むため、同じconfigでは削除しない
    if (hasUnfinishedRestorePlan(configName)) {
        replyError(QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_NAME),
                   QString::fromLatin1(qsnapper::dbusservice::RESTORE_PLAN_PENDING_ERROR_TEXT));
        return {};
    }

    QString subvolume;
    try {
        snapper::Snapper *snapper = getSnapper(configName, true);
        if (!snapper) {
            replyError(QDBusError::Failed, SnapperErrorText);
            return {};
        }
        const snapper::Snapshots &snapshots = snapper->getSnapshots();
        for (const uint number : numbers) {
            const auto it = snapshots.find(number);
            if (it == snapshots.end()) {
                replyError(QDBusError::Failed, SnapshotNotFoundText);
                return {};
            }
            if (it->isCurrent() || it->isDefault() || it->isActive()) {
                replyError(QDBusError::AccessDenied, SnapshotProtectedText);
                return {};
            }
        }
        subvolume = QString::fromStdString(snapper->subvolumeDir());
    } catch (const std::exception &) {
        replyError(QDBusError::Failed, SnapperErrorText);
        return {};
    }

    // 削除の結果は、この時点で開いたスナップショットのディレクトリの中で確かめる
    // 削除の後にパスから開き直すだけでは、その間にディレクトリが差し替えられても気付けないため
    if (!qsnapper::security::validateConfigSubvolumePath(subvolume)) {
        replyError(QDBusError::Failed, SnapshotDirUntrustedText);
        return {};
    }
    const QString snapshotsPath = subvolume == QStringLiteral("/")
                                      ? QStringLiteral("/.snapshots")
                                      : subvolume + QStringLiteral("/.snapshots");
    int snapshotsFd = -1;
    struct stat snapshotsStat {};
    const TrustedPathResult opened = qsnapper::security::openTrustedDirectory(snapshotsPath, &snapshotsFd,
                                                                              &snapshotsStat);
    const std::shared_ptr<const int> heldSnapshotsFd = holdDirectoryFd(snapshotsFd);
    if (opened != TrustedPathResult::Ok) {
        replyError(QDBusError::Failed, SnapshotDirUntrustedText);
        return {};
    }

    // 消えた名前のconfigや番号が作り直された後に、古い作成中の計画が別の内容を指して確定されないよう、変更の直前に破棄する
    discardStagingRestorePlans(configName, -1);

    const auto claimedReply = claimReplyForAsyncCompletion();
    if (!claimedReply) {
        return {};
    }

    QDBusMessage mutation = QDBusMessage::createMethodCall(
        SnapperdOperation::Service, SnapperdOperation::Path, SnapperdOperation::Interface,
        QStringLiteral("DeleteSnapshots"));
    mutation << configName << QVariant::fromValue(numbers);

    startSnapperdOperation(
        *claimedReply, mutation, configName, numbers, DeleteSnapshotsReplyDeadlineMs,
        [this, numbers, snapshotsPath, snapshotsStat, heldSnapshotsFd](SnapperdOperation::Outcome outcome,
                                                                       bool stoppedByRestore, const CallReply &reply,
                                                                       bool replied) {
            QList<uint> deleted;
            const bool readable = collectDeletedEntries(*heldSnapshotsFd, snapshotsPath, snapshotsStat,
                                                        numbers, &deleted);

            if (replied) {
                return;
            }
            if (!readable) {
                sendAsyncError(reply, QDBusError::errorString(QDBusError::Failed), VerifyFailedText);
                return;
            }
            // 1件でも削除された場合は、削除された番号を返し、呼び出し元が一部だけの削除かを判断する
            if (outcome == SnapperdOperation::Outcome::Replied || !deleted.isEmpty()) {
                if (reply.fromDBus) {
                    reply.connection.send(reply.message.createReply(QVariant::fromValue(deleted)));
                }
                return;
            }
            const auto [name, text] = outcomeError(outcome, stoppedByRestore);
            sendAsyncError(reply, name, text);
        });
    return {};
}

/**
 * @brief configのsubvolumeがあるBtrfsの割当量と使用量を読み取る
 */
QString SnapshotOperations::GetFilesystemUsage(const QString &configName)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return {};
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-filesystem"),
        [this, config = *cfg]() {
            return getFilesystemUsageAuthorized(config);
        });
}

/**
 * @brief configのsubvolumeがあるBtrfsのサブボリュームの一覧を読み取る
 */
QString SnapshotOperations::ListSubvolumes(const QString &configName)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return {};
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.list-subvolumes"),
        [this, config = *cfg]() {
            return listSubvolumesAuthorized(config);
        });
}

namespace {
    /**
     * @brief configのSUBVOLUMEを、symlinkを辿らずに開く
     * @param configName 検証済みの設定名
     * @param error 失敗時の固定の文言を受け取る
     * @return 開いたディレクトリのfd、失敗時は-1
     */
    int openConfigFilesystem(const QString &configName, QString *error)
    {
        QString subvolume;
        try {
            const snapper::ConfigInfo info = snapper::Snapper::getConfig(configName.toStdString(), "/");
            subvolume = QString::fromStdString(info.get_subvolume());
        } catch (const std::exception &) {
            *error = ConfigNotFoundText;
            return -1;
        }
        const int fd = qsnapper::btrfs::openBtrfsDirectory(subvolume);
        if (fd < 0) {
            *error = NotBtrfsText;
        }
        return fd;
    }

    /**
     * @brief Btrfsの情報の読み取りの失敗を、D-Busエラーの種類と固定の文言へ変換する
     * @param error 読み取りの結果
     * @param type D-Busエラーの種類を受け取る
     * @return 固定の文言
     */
    QString btrfsErrorText(qsnapper::btrfs::InfoError error, QDBusError::ErrorType *type)
    {
        switch (error) {
        case qsnapper::btrfs::InfoError::NotBtrfs:      // Btrfs以外では読み取れない
            *type = QDBusError::NotSupported;
            return NotBtrfsText;
        case qsnapper::btrfs::InfoError::LimitExceeded: // 応答の大きさを抑えるため、上限を超えたら打ち切らずに失敗にする
            *type = QDBusError::LimitsExceeded;
            return BtrfsLimitText;
        case qsnapper::btrfs::InfoError::None:
        case qsnapper::btrfs::InfoError::Failed:
            break;
        }
        *type = QDBusError::Failed;
        return BtrfsFailedText;
    }
}

/**
 * @brief 認可済みのGetFilesystemUsage本体
 *
 * 結果の件数は読み取り側で上限を設けており、上限を超えた場合は一部だけを返さずに失敗にする
 */
QString SnapshotOperations::getFilesystemUsageAuthorized(const QString &configName)
{
    QString openError;
    const int fd = openConfigFilesystem(configName, &openError);
    if (fd < 0) {
        replyError(QDBusError::NotSupported, openError);
        return {};
    }
    qsnapper::btrfs::FilesystemUsage usage;
    const qsnapper::btrfs::InfoError error = qsnapper::btrfs::readFilesystemUsage(fd, &usage);
    ::close(fd);
    if (error != qsnapper::btrfs::InfoError::None) {
        QDBusError::ErrorType type = QDBusError::Failed;
        const QString text = btrfsErrorText(error, &type);
        replyError(type, text);
        return {};
    }
    return qsnapper::btrfs::formatFilesystemUsageCsv(usage);
}

/**
 * @brief 認可済みのListSubvolumes本体
 *
 * 本サービスは単一スレッドで動作するため、列挙は件数・サイズ・時間の上限で打ち切り、他の要求を長く止めない
 * 打ち切った場合は、そのことを結果の先頭で示し、完全な一覧と取り違えないようにする
 */
QString SnapshotOperations::listSubvolumesAuthorized(const QString &configName)
{
    QString openError;
    const int fd = openConfigFilesystem(configName, &openError);
    if (fd < 0) {
        replyError(QDBusError::NotSupported, openError);
        return {};
    }
    QVector<qsnapper::btrfs::SubvolumeEntry> entries;
    bool truncated = false;
    const qsnapper::btrfs::InfoError error =
        qsnapper::btrfs::listSubvolumes(fd, qsnapper::btrfs::SubvolumeListLimits{}, &entries, &truncated);
    ::close(fd);
    if (error != qsnapper::btrfs::InfoError::None) {
        QDBusError::ErrorType type = QDBusError::Failed;
        const QString text = btrfsErrorText(error, &type);
        replyError(type, text);
        return {};
    }
    return qsnapper::btrfs::formatSubvolumeListCsv(entries, truncated);
}
