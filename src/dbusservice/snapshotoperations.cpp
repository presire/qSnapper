#include "snapshotoperations.h"
#include <QDebug>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusError>
#include <QDateTime>
#include <QProcess>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#include <snapper/Snapper.h>
#include <snapper/Snapshot.h>
#include <snapper/Comparison.h>
#include <snapper/File.h>
#include <snapper/Exception.h>

/**
 * @brief SnapshotOperationsクラスのコンストラクタ
 *
 * スナップショット操作を管理するクラスを初期化します。
 *
 * @param parent 親QObjectポインタ
 */
SnapshotOperations::SnapshotOperations(QObject *parent)
    : QObject(parent)
    , m_snapper(nullptr)
    , m_currentConfig("")
{
}

/**
 * @brief SnapshotOperationsクラスのデストラクタ
 *
 * リソースのクリーンアップを行います。
 */
SnapshotOperations::~SnapshotOperations()
{
}

/**
 * @brief PolicyKitによる認証チェックを実行
 *
 * 指定されたアクションIDに対してユーザーが権限を持っているかを確認します。
 * 権限がない場合はD-Busエラー応答を送信します。
 *
 * @param actionId チェックするアクションID
 * @return 認証成功時true、失敗時false
 */
bool SnapshotOperations::checkAuthorization(const QString &actionId)
{
    PolkitQt1::UnixProcessSubject subject(QDBusConnection::systemBus().interface()->servicePid(message().service()));
    PolkitQt1::Authority::Result result = PolkitQt1::Authority::instance()->checkAuthorizationSync(
        actionId, subject, PolkitQt1::Authority::AllowUserInteraction);

    if (result == PolkitQt1::Authority::Yes) {
        return true;
    }

    sendErrorReply(QDBusError::AccessDenied, "Authorization failed");
    return false;
}

/**
 * @brief Snapperインスタンスを取得
 *
 * 指定された設定名でSnapperインスタンスを取得または作成します。
 * 設定が変更された場合は新しいインスタンスを作成します。
 *
 * @param configName Snapper設定名
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 */
snapper::Snapper* SnapshotOperations::getSnapper(const QString &configName)
{
    try {
        // 設定が変更された場合、または初回の場合は新しいSnapperインスタンスを作成
        if (!m_snapper || m_currentConfig != configName) {
            m_snapper.reset(new snapper::Snapper(configName.toStdString(), "/"));
            m_currentConfig = configName;
        }
        return m_snapper.get();
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to create Snapper instance:" << e.what();
        return nullptr;
    }
}

/**
 * @brief スナップショットタイプを文字列に変換
 *
 * snapperライブラリのスナップショットタイプ列挙値を文字列表現に変換します。
 *
 * @param type スナップショットタイプ (snapper::SINGLE, PRE, POST)
 * @return タイプの文字列表現 ("single", "pre", "post")
 */
QString SnapshotOperations::snapshotTypeToString(int type)
{
    switch (type) {
        case snapper::SINGLE: return "single";
        case snapper::PRE: return "pre";
        case snapper::POST: return "post";
        default: return "single"    ;
    }
}

/**
 * @brief 文字列をスナップショットタイプに変換
 *
 * 文字列表現をsnapperライブラリのスナップショットタイプ列挙値に変換します。
 *
 * @param typeStr タイプの文字列表現 ("single", "pre", "post")
 * @return スナップショットタイプ列挙値
 */
int SnapshotOperations::stringToSnapshotType(const QString &typeStr)
{
    if (typeStr == "pre") return snapper::PRE;
    if (typeStr == "post") return snapper::POST;
    return snapper::SINGLE;
}

/**
 * @brief スナップショット一覧をCSV形式に変換
 *
 * Snapperインスタンスから取得したスナップショット一覧を
 * CSV形式の文字列に変換します。
 *
 * @param snapper Snapperインスタンスへのポインタ
 * @return CSV形式のスナップショット情報文字列
 */
QString SnapshotOperations::formatSnapshotToCSV(const snapper::Snapper *snapper)
{
    if (!snapper) {
        return QString();
    }

    QString csv;
    csv += "number,type,pre-number,date,user,cleanup,description,userdata\n";

    const snapper::Snapshots &snapshots = snapper->getSnapshots();
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
        const snapper::Snapshot &snapshot = *it;

        csv += QString::number(snapshot.getNum()) + ",";
        csv += snapshotTypeToString(snapshot.getType()) + ",";
        csv += QString::number(snapshot.getPreNum()) + ",";

        // 日時をISO形式に変換
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(snapshot.getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(snapshot.getUid()) + ",";
        csv += QString::fromStdString(snapshot.getCleanup()) + ",";
        csv += QString::fromStdString(snapshot.getDescription()) + ",";

        // ユーザーデータをkey1=value1,key2=value2形式に変換
        const std::map<std::string, std::string> &userdata = snapshot.getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                               QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");
        csv += "\n";
    }

    return csv;
}

/**
 * @brief スナップショット一覧を取得
 *
 * システム上の全スナップショットをCSV形式で取得します。
 * PolicyKit認証を必要とします。
 *
 * @return CSV形式のスナップショット一覧、失敗時は空文字列
 */
QString SnapshotOperations::ListSnapshots()
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        return formatSnapshotToCSV(snapper);
    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to list snapshots:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to list snapshots: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief 新しいスナップショットを作成
 *
 * 指定されたパラメータで新しいスナップショットを作成します。
 * single、pre、postの3種類のタイプをサポートします。
 *
 * @param type スナップショットのタイプ ("single", "pre", "post")
 * @param description スナップショットの説明
 * @param preNumber postタイプの場合の対応するpreスナップショット番号
 * @param cleanup クリーンアップアルゴリズム名
 * @param important 重要フラグ
 * @return 作成されたスナップショットのCSV情報、失敗時は空文字列
 */
QString SnapshotOperations::CreateSnapshot(const QString &type, const QString &description,
                                          int preNumber, const QString &cleanup, bool important)
{
    if (!checkAuthorization("com.presire.qsnapper.create-snapshot")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::SCD scd;
        scd.description = description.toStdString();
        scd.cleanup = cleanup.toStdString();
        scd.read_only = true;

        if (important) {
            scd.userdata["important"] = "yes";
        }

        snapper::Snapshots::iterator newSnapshot;
        snapper::SnapshotType snapType = static_cast<snapper::SnapshotType>(stringToSnapshotType(type));

        if (snapType == snapper::PRE) {
            newSnapshot = snapper->createPreSnapshot(scd);
        } else if (snapType == snapper::POST && preNumber > 0) {
            snapper::Snapshots::const_iterator preSnap = snapper->getSnapshots().find(preNumber);
            if (preSnap == snapper->getSnapshots().end()) {
                sendErrorReply(QDBusError::Failed, "Pre-snapshot not found");
                return QString();
            }
            newSnapshot = snapper->createPostSnapshot(preSnap, scd);
        } else {
            newSnapshot = snapper->createSingleSnapshot(scd);
        }

        // 新しく作成されたスナップショットのCSV情報を返す
        QString csv = "number,type,pre-number,date,user,cleanup,description,userdata\n";
        csv += QString::number(newSnapshot->getNum()) + ",";
        csv += snapshotTypeToString(newSnapshot->getType()) + ",";
        csv += QString::number(newSnapshot->getPreNum()) + ",";

        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(newSnapshot->getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(newSnapshot->getUid()) + ",";
        csv += QString::fromStdString(newSnapshot->getCleanup()) + ",";
        csv += QString::fromStdString(newSnapshot->getDescription()) + ",";

        const std::map<std::string, std::string> &userdata = newSnapshot->getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            userdataPairs.append(QString::fromStdString(pair.first) + "=" +
                               QString::fromStdString(pair.second));
        }
        csv += userdataPairs.join(",");

        return csv;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to create snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to create snapshot: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief スナップショットを削除
 *
 * 指定された番号のスナップショットを削除します。
 * PolicyKit認証を必要とします。
 *
 * @param number 削除するスナップショット番号
 * @return 削除成功時true、失敗時false
 */
bool SnapshotOperations::DeleteSnapshot(int number)
{
    if (!checkAuthorization("com.presire.qsnapper.delete-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        snapper->deleteSnapshot(snapshot);
        return true;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to delete snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to delete snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief スナップショットにロールバック
 *
 * 指定されたスナップショットをデフォルトに設定し、次回起動時に
 * そのスナップショットの状態で起動するようにします。
 *
 * @param number ロールバック先のスナップショット番号
 * @return 設定成功時true、失敗時false
 */
bool SnapshotOperations::RollbackSnapshot(int number)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    try {
        snapper::Snapper *snapper = getSnapper("root");
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::iterator snapshot = snapper->getSnapshots().find(number);
        if (snapshot == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // スナップショットをデフォルトに設定 (次回起動時に適用される)
        snapshot->setDefault();
        return true;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to rollback snapshot:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to rollback snapshot: %1").arg(e.what()));
        return false;
    }
}

/**
 * @brief ファイル変更一覧を取得
 *
 * 指定されたスナップショットと現在のシステム状態を比較し、
 * 変更されたファイルの一覧を取得します。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @return ファイル変更のステータスとパスの一覧、失敗時は空文字列
 */
QString SnapshotOperations::GetFileChanges(const QString &configName, int snapshotNumber)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        // snapshot1: 比較元 (指定されたスナップショット)
        // snapshot2: 比較先 (現在のシステム状態)
        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // Comparisonオブジェクトを作成してファイル変更を取得
        // snapshot1からsnapshot2への変更を取得
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, false);
        const snapper::Files &files = comparison.getFiles();

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

            output += statusStr + " " + QString::fromStdString(file.getName()) + "\n";
        }

        return output;

    } catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file changes:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file changes: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルの差分を取得
 *
 * 指定されたスナップショット内のファイルと現在のシステムの
 * ファイルとの差分をunified diff形式で取得します。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 比較元のスナップショット番号
 * @param filePath 差分を取得するファイルパス
 * @return unified diff形式の差分、ファイルが見つからない場合は空文字列
 */
QString SnapshotOperations::GetFileDiff(const QString &configName, int snapshotNumber, const QString &filePath)
{
    if (!checkAuthorization("com.presire.qsnapper.list-snapshots")) {
        return QString();
    }

    try {
        // diffの取得には外部コマンド(diff)を使用する必要があるため、
        // libsnapperのComparisonクラスを使用してファイルパスを取得し、
        // 外部のdiffコマンドで差分を生成する
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return QString();
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return QString();
        }

        // スナップショットをマウント
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        const snapper::Files &files = comparison.getFiles();

        // 指定されたファイルを検索
        auto fileIt = files.findAbsolutePath(filePath.toStdString());
        if (fileIt == files.end()) {
            return QString(); // ファイルが見つからない場合は空文字列を返す
        }

        // ファイルの絶対パスを取得
        // LOC_PRE: スナップショット内のファイル
        // LOC_SYSTEM: 現在のシステムのファイル
        QString file1Path = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_PRE));
        QString file2Path = QString::fromStdString(fileIt->getAbsolutePath(snapper::LOC_SYSTEM));

        // diffコマンドを実行 (スナップショット -> 現在の状態)
        QProcess process;
        process.start("diff", QStringList() << "-u" << file1Path << file2Path);
        process.waitForFinished(10000);

        QString output = QString::fromUtf8(process.readAllStandardOutput());
        return output;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to get file diff:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to get file diff: %1").arg(e.what()));
        return QString();
    }
}

/**
 * @brief ファイルをスナップショットから復元
 *
 * 指定されたファイルリストを指定されたスナップショットの状態に復元します。
 * 復元の進捗はrestoreProgressシグナルで通知されます。
 *
 * @param configName Snapper設定名
 * @param snapshotNumber 復元元のスナップショット番号
 * @param filePaths 復元するファイルパスのリスト
 * @return 全ファイルの復元が成功した場合true、それ以外はfalse
 */
bool SnapshotOperations::RestoreFiles(const QString &configName, int snapshotNumber, const QStringList &filePaths)
{
    if (!checkAuthorization("com.presire.qsnapper.rollback-snapshot")) {
        return false;
    }

    if (filePaths.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, "No files specified for restore");
        return false;
    }

    qWarning() << "RestoreFiles: Starting restore for" << filePaths.size() << "files from snapshot" << snapshotNumber;

    try {
        snapper::Snapper *snapper = getSnapper(configName);
        if (!snapper) {
            qWarning() << "Failed to get Snapper instance";
            sendErrorReply(QDBusError::Failed, "Failed to initialize Snapper");
            return false;
        }

        snapper::Snapshots::const_iterator snapshot1 = snapper->getSnapshots().find(snapshotNumber);
        snapper::Snapshots::const_iterator snapshot2 = snapper->getSnapshotCurrent();

        if (snapshot1 == snapper->getSnapshots().end()) {
            qWarning() << "Snapshot not found:" << snapshotNumber;
            sendErrorReply(QDBusError::Failed, "Snapshot not found");
            return false;
        }

        // Comparisonオブジェクトを作成 (スナップショットをマウント)
        snapper::Comparison comparison(snapper, snapshot1, snapshot2, true);
        snapper::Files &files = comparison.getFiles();

        // まず、バッチ内の全ファイルをundoフラグでマーク（差分があるファイルのみ）
        QStringList notFoundFiles;
        QStringList noDiffFiles;
        int markedCount = 0;

        for (const QString &filePath : filePaths) {
            auto fileIt = files.findAbsolutePath(filePath.toStdString());

            if (fileIt == files.end()) {
                // 代替検索：名前だけで検索
                auto fileIt2 = files.find(filePath.toStdString());
                if (fileIt2 != files.end()) {
                    fileIt2->setUndo(true);
                    markedCount++;
                }
                else {
                    // ディレクトリまたは差分のないファイルの可能性
                    notFoundFiles.append(filePath);
                }
            }
            else {
                fileIt->setUndo(true);
                markedCount++;
            }
        }

        // undoフラグが立っているファイルのUndoStepsを一度に取得
        std::vector<snapper::UndoStep> undoSteps = comparison.getUndoSteps();

        // undoStepsが空の場合は、差分がない（既に復元済みまたはディレクトリのみ）と判断
        if (undoSteps.empty()) {
            qWarning() << "No undo steps generated. Files may already be in sync or are directories. Marked files:" << markedCount;

            // undoフラグをクリア
            for (const QString &filePath : filePaths) {
                auto fileIt = files.findAbsolutePath(filePath.toStdString());
                if (fileIt != files.end()) {
                    fileIt->setUndo(false);
                }
            }

            // エラーではなく成功として扱う（差分がないため復元不要）
            return true;
        }

        // 各UndoStepを実行し、進捗を通知
        bool allSuccess = true;
        int total = undoSteps.size();
        int current = 0;
        int successCount = 0;

        for (const auto &step : undoSteps) {
            current++;
            QString fileName = QString::fromStdString(step.name);

            // 進捗を通知 (D-Busシグナル)
            emit restoreProgress(current, total, fileName);

            // ファイルを復元
            try {
                bool success = comparison.doUndoStep(step);
                if (!success) {
                    qWarning() << "Failed to restore:" << fileName;
                    allSuccess = false;
                }
                else {
                    successCount++;
                }
            }
            catch (const snapper::Exception &e) {
                qWarning() << "Exception during restore:" << fileName << "-" << e.what();
                allSuccess = false;
            }
        }

        // undoフラグをクリア
        for (const QString &filePath : filePaths) {
            auto fileIt = files.findAbsolutePath(filePath.toStdString());
            if (fileIt != files.end()) {
                fileIt->setUndo(false);
            }
        }

        qWarning() << "RestoreFiles: Completed. Successful:" << successCount << "Failed:" << (total - successCount);

        // notFoundFilesは警告のみ（ディレクトリや差分のないファイルの可能性）
        if (!notFoundFiles.isEmpty()) {
            qWarning() << "Some files were not found in comparison (may be directories or already in sync):" << notFoundFiles.size();
        }

        // 実際の復元失敗がある場合のみエラーを返す
        if (!allSuccess) {
            QString errorMsg = QString("Failed to restore %1 out of %2 files").arg(total - successCount).arg(total);
            sendErrorReply(QDBusError::Failed, errorMsg);
        }

        return allSuccess;

    }
    catch (const snapper::Exception &e) {
        qWarning() << "Failed to restore files:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Failed to restore files: %1").arg(e.what()));
        return false;
    }
    catch (const std::exception &e) {
        qWarning() << "Unexpected error during restore:" << e.what();
        sendErrorReply(QDBusError::Failed, QString("Unexpected error: %1").arg(e.what()));
        return false;
    }
}
