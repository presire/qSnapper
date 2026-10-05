/**
 * @file snapshotoperations_changes.cpp
 * @brief ファイル変更一覧と差分詳細の取得
 *
 * 単一スナップショットと現在との比較、2スナップショット間の比較
 * 指定ファイルのunified diffと所有者や権限などの詳細を扱う
 */

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>
#include <sys/stat.h>

/**
 * @brief ファイル変更一覧を取得
 *
 * 指定されたスナップショットと現在のシステム状態を比較し、変更されたファイルの一覧を取得する
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::GetFileChanges(const QString &configName, int snapshotNumber)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, snapshotNumber]() {
            return getFileChangesAuthorized(config, snapshotNumber);
        });
}

/**
 * @brief 認可済みのGetFileChanges本体
 *
 * @param configName 検証済みSnapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::getFileChangesAuthorized(const QString &configName,
                                                     int snapshotNumber)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // snapshot1: 比較元 (指定されたスナップショット)
        // snapshot2: 比較先 (現在のシステム状態)
        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを作成してファイル変更を取得
        // snapshot1からsnapshot2への変更を取得
        // mount=trueでComparisonを構築し、後続する詳細取得APIがmountを再利用できるようにする
        // Refresh戦略: 一覧取得時に前回キャッシュを破棄して最新状態を反映する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, snapshotNumber, std::nullopt},
            CachePolicy::Refresh,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

            // ステータスフラグを文字列に変換
            QString statusStr;
            if (status & snapper::CREATED) statusStr += "+";
            if (status & snapper::DELETED) statusStr += "-";
            if (status & snapper::TYPE) statusStr += "t";
            if (status & snapper::CONTENT) statusStr += "c";
            if (status & snapper::PERMISSIONS) statusStr += "p";
            if (status & snapper::OWNER) statusStr += "u";
            if (status & snapper::GROUP) statusStr += "g";
            if (status & snapper::XATTRS) statusStr += "x";
            if (status & snapper::ACL) statusStr += "a";

            if (statusStr.isEmpty()) statusStr = ".....";

            // パディングして出力フォーマットを整える
            statusStr = statusStr.leftJustified(5, '.');

            // 本出力は「1行 = 1エントリ」の行指向テキストである
            // パスに改行を混入できると1エントリが複数行に割れ、2行目以降が攻撃者の選んだstatusとパスを持つ独立エントリとしてクライアントに解釈される
            // 検証した文字列をそのまま連結し、再取得した値を使わないこと
            const QString fileName = QString::fromStdString(file.getName());
            if (!qsnapper::security::isRecordSafeText(fileName)) {
                // 該当エントリだけを落とすと、クライアントは欠落した一覧を完全な一覧として扱ってしまうため、メソッド全体を失敗させる
                // 攻撃者が制御するパスはログにもエラー本文にも載せない (ログ注入防止)
                qWarning() << "Rejected file change listing: snapshot" << snapshotNumber
                           << "of config" << configName
                           << "contains a path with control characters";
                replyError(QDBusError::Failed, QStringLiteral("Snapshot contains a file path with control characters"));
                return QString();
            }

            output += statusStr + " " + fileName + "\n";
        }

        return output;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間のファイル変更リストを取得
 *
 * snapshot1 -> snapshot2の差分を取得する
 * 現在のシステム状態は使用しない
 */
QString SnapshotOperations::GetFileChangesBetween(const QString &configName, int number1, int number2)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, number1, number2]() {
            return getFileChangesBetweenAuthorized(config, number1, number2);
        });
}

/**
 * @brief 認可済みのGetFileChangesBetween本体
 *
 * @param configName 検証済みSnapper設定名
 * @param number1 比較元スナップショット番号
 * @param number2 比較先スナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::getFileChangesBetweenAuthorized(
    const QString &configName, int number1, int number2)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // mount=trueでComparisonを構築し、後続する詳細取得APIがmountを再利用できるようにする
        // Refresh戦略: 一覧取得時に前回キャッシュを破棄して最新状態を反映する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, number1, number2},
            CachePolicy::Refresh,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

        QString output;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const snapper::File &file = *it;
            unsigned int status = file.getPreToPostStatus();

            QString statusStr;
            if (status & snapper::CREATED) statusStr += "+";
            if (status & snapper::DELETED) statusStr += "-";
            if (status & snapper::TYPE) statusStr += "t";
            if (status & snapper::CONTENT) statusStr += "c";
            if (status & snapper::PERMISSIONS) statusStr += "p";
            if (status & snapper::OWNER) statusStr += "u";
            if (status & snapper::GROUP) statusStr += "g";
            if (status & snapper::XATTRS) statusStr += "x";
            if (status & snapper::ACL) statusStr += "a";
            if (statusStr.isEmpty()) statusStr = ".....";
            statusStr = statusStr.leftJustified(5, '.');

            // fail-closeする理由と、検証した文字列をそのまま連結する理由は
            // getFileChangesAuthorized()側の注記を参照すること
            const QString fileName = QString::fromStdString(file.getName());
            if (!qsnapper::security::isRecordSafeText(fileName)) {
                qWarning() << "Rejected file change listing: snapshots" << number1 << "->" << number2
                           << "of config" << configName
                           << "contain a path with control characters";
                replyError(QDBusError::Failed, QStringLiteral("Snapshot contains a file path with control characters"));
                return QString();
            }

            output += statusStr + " " + fileName + "\n";
        }

        return output;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes between snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 2つのスナップショット間の個別ファイルの詳細 + diffを取得
 *
 * snapshot1側のパーミッションとsnapshot2側のパーミッションを返し、diff部も両snapshot上のファイルを比較する
 */
QString SnapshotOperations::GetFileDiffBetween(const QString &configName, int number1, int number2, const QString &filePath)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!qsnapper::security::validateAbsoluteFilePath(filePath)) {
        replyError(QDBusError::InvalidArgs, "Invalid file path");
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, number1, number2, filePath]() {
            return getFileDiffBetweenAuthorized(config, number1, number2,
                                                filePath);
        });
}

/**
 * @brief 認可済みのGetFileDiffBetween本体
 *
 * @param configName 検証済みSnapper設定名
 * @param number1 比較元スナップショット番号
 * @param number2 比較先スナップショット番号
 * @param filePath 検証済み対象ファイルの絶対path
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::getFileDiffBetweenAuthorized(
    const QString &configName, int number1, int number2, const QString &filePath)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(number1);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshots().find(number2);

        if (snapshot1 == snapper->getSnapshots().end() || snapshot2 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Reuse戦略: キーが一致すればリスト取得時に構築したmount有効Comparisonを再利用し、
        // ミス時は新規構築 (mount=true) してキャッシュに格納する
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, number1, number2},
            CachePolicy::Reuse,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

        // filePathはGetFileChanges*が返したconfig相対名である
        // findAbsolutePath()はSUBVOLUME付きの絶対パスを要求し非root configで一致しないため、config相対名で引く
        auto fileIt = files.find(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        unsigned int status = fileIt->getPreToPostStatus();
        QString statusStr;
        if (status & snapper::CREATED) statusStr += "+";
        if (status & snapper::DELETED) statusStr += "-";
        if (status & snapper::TYPE) statusStr += "t";
        if (status & snapper::CONTENT) statusStr += "c";
        if (status & snapper::PERMISSIONS) statusStr += "p";
        if (status & snapper::OWNER) statusStr += "u";
        if (status & snapper::GROUP) statusStr += "g";
        if (status & snapper::XATTRS) statusStr += "x";
        if (status & snapper::ACL) statusStr += "a";
        if (statusStr.isEmpty()) statusStr = ".....";
        statusStr = statusStr.leftJustified(5, '.');

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        // snapshot1をLOC_PREとして扱い、snapshot2をLOC_POSTとして扱う
        QString path1 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        struct stat info1;
        const bool hasInfo1 = qsnapper::security::safeLstat(path1, &info1);
        if (hasInfo1) {
            detailsPart += "snapshotPerms=" + qsnapper::internal::permsToOctal(info1.st_mode) + "\n";
            detailsPart += "snapshotOwner=" + qsnapper::internal::ownerName(info1.st_uid) + "\n";
            detailsPart += "snapshotGroup=" + qsnapper::internal::groupName(info1.st_gid) + "\n";
        }

        QString path2 = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_POST));
        struct stat info2;
        const bool hasInfo2 = qsnapper::security::safeLstat(path2, &info2);
        if (hasInfo2) {
            detailsPart += "currentPerms=" + qsnapper::internal::permsToOctal(info2.st_mode) + "\n";
            detailsPart += "currentOwner=" + qsnapper::internal::ownerName(info2.st_uid) + "\n";
            detailsPart += "currentGroup=" + qsnapper::internal::groupName(info2.st_gid) + "\n";
        }

        QString diffPart;
        if (hasInfo1 && hasInfo2) {
            const qsnapper::diff::UnifiedDiffResult diff = qsnapper::diff::generateUnifiedDiff(path1, path2);
            diffPart = diff.text;
            if (!diff.omittedReason.isEmpty()) {
                detailsPart += "diffOmitted=" + diff.omittedReason + "\n";
            }
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff between snapshots:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
    catch (const std::exception &) {
        qWarning() << "Failed to get file diff between snapshots: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Failed to get file diff"));
        return QString();
    }
}

/**
 * @brief ファイルの差分と詳細情報を一括取得
 *
 * 1回のComparisonオブジェクト生成で、差分 (diff) と 詳細情報 (パーミッション等) の両方を取得する
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 対象ファイルパス
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::GetFileDiffAndDetails(const QString &configName, int snapshotNumber, const QString &filePath)
{
    const auto cfg = resolveConfigOrFail(configName);
    if (!cfg) {
        return QString();
    }

    if (!qsnapper::security::validateAbsoluteFilePath(filePath)) {
        replyError(QDBusError::InvalidArgs, "Invalid file path");
        return QString();
    }

    return authorizeThen<QString>(
        QStringLiteral("com.presire.qsnapper.view-diff"),
        [this, config = *cfg, snapshotNumber, filePath]() {
            return getFileDiffAndDetailsAuthorized(config, snapshotNumber,
                                                   filePath);
        });
}

/**
 * @brief 認可済みのGetFileDiffAndDetails本体
 *
 * @param configName 検証済みSnapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 検証済み対象ファイルの絶対path
 * @return details部とdiff部をセパレータで分割した文字列
 */
QString SnapshotOperations::getFileDiffAndDetailsAuthorized(
    const QString &configName, int snapshotNumber, const QString &filePath)
{
    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            replyError(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            replyError(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Reuse戦略: キーが一致すればリスト取得時に構築したmount有効Comparisonを再利用し、
        // ミス時は新規構築 (mount=true) してキャッシュに格納する
        // Comparisonオブジェクトは1回だけ作成 (スナップショットマウントも1回のみ)
        using CachePolicy = ComparisonCache<snapper::Comparison>::Policy;
        auto *comparison = m_comparisonCache.get(
            {configName, snapshotNumber, std::nullopt},
            CachePolicy::Reuse,
            [&](const ComparisonCache<snapper::Comparison>::Key &) {
                return std::unique_ptr<snapper::Comparison>(
                    new snapper::Comparison(snapper, snapshot1, snapshot2, true));
            });
        const snapper::Files &files = comparison->getFiles();

        // filePathはGetFileChanges*が返したconfig相対名である
        // findAbsolutePath()はSUBVOLUME付きの絶対パスを要求し非root configで一致しないため、config相対名で引く
        auto fileIt = files.find(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString();
        }

        // Details部の構築
        unsigned int status = fileIt->getPreToPostStatus();
        QString statusStr;
        if (status & snapper::CREATED) statusStr += "+";
        if (status & snapper::DELETED) statusStr += "-";
        if (status & snapper::TYPE) statusStr += "t";
        if (status & snapper::CONTENT) statusStr += "c";
        if (status & snapper::PERMISSIONS) statusStr += "p";
        if (status & snapper::OWNER) statusStr += "u";
        if (status & snapper::GROUP) statusStr += "g";
        if (status & snapper::XATTRS) statusStr += "x";
        if (status & snapper::ACL) statusStr += "a";
        if (statusStr.isEmpty()) statusStr = ".....";
        statusStr = statusStr.leftJustified(5, '.');

        QString detailsPart;
        detailsPart += "status=" + statusStr + "\n";

        QString snapshotPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        struct stat snapshotInfo;
        const bool hasSnapshotInfo = qsnapper::security::safeLstat(snapshotPath, &snapshotInfo);
        if (hasSnapshotInfo) {
            detailsPart += "snapshotPerms=" + qsnapper::internal::permsToOctal(snapshotInfo.st_mode) + "\n";
            detailsPart += "snapshotOwner=" + qsnapper::internal::ownerName(snapshotInfo.st_uid) + "\n";
            detailsPart += "snapshotGroup=" + qsnapper::internal::groupName(snapshotInfo.st_gid) + "\n";
        }

        QString currentPath = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_SYSTEM));
        struct stat currentInfo;
        const bool hasCurrentInfo = qsnapper::security::safeLstat(currentPath, &currentInfo);
        if (hasCurrentInfo) {
            detailsPart += "currentPerms=" + qsnapper::internal::permsToOctal(currentInfo.st_mode) + "\n";
            detailsPart += "currentOwner=" + qsnapper::internal::ownerName(currentInfo.st_uid) + "\n";
            detailsPart += "currentGroup=" + qsnapper::internal::groupName(currentInfo.st_gid) + "\n";
        }

        // Diff部の取得
        QString diffPart;
        if (hasSnapshotInfo && hasCurrentInfo) {
            const qsnapper::diff::UnifiedDiffResult diff =
                qsnapper::diff::generateUnifiedDiff(snapshotPath, currentPath);
            diffPart = diff.text;
            if (!diff.omittedReason.isEmpty()) {
                detailsPart += "diffOmitted=" + diff.omittedReason + "\n";
            }
        }

        return detailsPart + "---DIFF_SEPARATOR---\n" + diffPart;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff and details:" << e.what();
        replyError(QDBusError::Failed, QString("Failed to get file diff and details: %1").arg(e.what()));
        return QString();
    }
    catch (const std::exception &) {
        qWarning() << "Failed to get file diff and details: unexpected exception";
        replyError(QDBusError::Failed, QStringLiteral("Failed to get file diff and details"));
        return QString();
    }
}
