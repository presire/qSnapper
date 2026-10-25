/**
 * @file filechangemodel_restore.cpp
 * @brief staged restore計画の実行と検証
 *
 * 復元対象の検証とチャンク単位のステージングと確定
 * およびD-Bus復元通知の処理を担当する
 *
 * 利用する側 : FileChangeModel
 * 依存 : Qt D-Bus、restoreplanstatus.h、FileChangeItem
 */

#include "filechangemodel.h"
#include "restoreplanstatus.h"
#include <QRegularExpression>
#include <QFileInfo>
#include <QDebug>


/**
 * @brief 復元計画の進捗スロット (staged restore用)
 *
 * サーバ側manifestが送る進捗シグナルを受信し、元の選択全体を基準に単調な進捗として再送出する
 * サーバのtotal値は契約上同じ値だが、UIへは計画開始時に固定した総数を必ず通知する
 *
 * signalはsystem busの全接続へ届くため、サーバはpathを載せない
 * サーバはstageした順にエントリを処理するため、表示するファイル名はクライアントが保持する計画 (m_planPaths) から求める
 *
 * 実行中の計画以外のマニフェストIDを持つシグナルは無視される
 *
 * @param manifestId 進捗を報告してきた計画のマニフェストID
 * @param current 処理済みエントリ数 (manifest全体基準)
 * @param total マニフェストの総エントリ数
 */
void FileChangeModel::onRestorePlanProgress(const QString &manifestId, int current, int total)
{
    // 実行中の計画以外のシグナルは無視する
    if (!m_planActive || m_planManifestId != manifestId) {
        return;
    }

    Q_UNUSED(total);

    const int boundedCurrent = qBound(0, current, m_planTotalFiles);
    m_planLastProgress = qMax(m_planLastProgress, boundedCurrent);
    const QString fileName = QFileInfo(m_planPaths.value(m_planLastProgress - 1)).fileName();
    emit restoreProgress(m_planLastProgress, m_planTotalFiles, fileName);
}

/**
 * @brief 復元計画の完了スロット (staged restore用)
 *
 * qsnapper-dbus-serviceの復元計画 (manifest) が送る終端シグナルを受信し、シグナル受信を解除してから復元完了を通知する
 * terminalStateが"completed"の場合のみ成功扱いになる
 *
 * 実行中の計画以外のマニフェストIDを持つシグナルは無視される
 * シグナルはsystem busの全接続へ届くため、messageは終端状態ごとの固定文言である
 * 計画を開始した接続だけが取得できるGetRestorePlanStatusの詳細(復元対象のパスを含み得る)は、終端状態によらずログへ出さない
 * 件数と失敗位置を結果画面へ表示するため、終端状態によらず完了後にGetRestorePlanStatusを要求する
 *
 * @note 状態は画面へ復元完了を通知するより先にリセットするが、GetRestorePlanStatusの応答は通知より後に届くことがある
 *       そのため応答はrestoreResultの更新として反映し、新しい復元の開始後に届いた古い応答は世代番号で捨てる
 *
 * @param manifestId 完了した計画のマニフェストID
 * @param terminalState 終端状態 ("completed" / "failed" / "cancelled")
 * @param message 終端状態ごとの固定文言
 */
void FileChangeModel::onRestorePlanFinished(const QString &manifestId, const QString &terminalState, const QString &message)
{
    // 実行中の計画以外のシグナルは無視する
    if (!m_planActive || m_planManifestId != manifestId) {
        return;
    }

    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    const bool completed = (terminalState == QStringLiteral("completed"));
    if (!completed) {
        qWarning() << "Restore plan finished with state:" << terminalState << message;
    }

    // 終端状態によらず、件数と失敗位置を結果画面に表示するためGetRestorePlanStatusを要求する
    // 応答はrestoreCompletedの通知より後に届くことがあり、restoreResultの更新で画面へ反映する
    // 新しい復元の開始後に古い応答が届いても上書きしないよう、世代で照合する
    const quint64 resultGeneration = ++m_restoreResultGeneration;
    m_restorePlanTransport->requestStatus(manifestId,
        [this, resultGeneration](bool statusOk, const QString &statusCsv, const QString &statusError) {
            if (resultGeneration != m_restoreResultGeneration) {
                return;
            }

            // 失敗の理由 (statusError) はログに出さず、固定の文言だけを出す
            // 認可の拒否以外では、D-Busデーモンやサービスの起動処理が返すエラー本文をそのまま受け取るため、パスを含まないとは保証できない
            if (!statusOk) {
                Q_UNUSED(statusError);
                qWarning() << "Failed to get restore plan status";
                return;
            }

            // 取得した状態 (statusCsv) は、最後のエラー・失敗したファイル・スキップしたファイルに復元先のパスを含み得るため、ログに出さない
            // 9フィールドの旧形式も受け付ける (追加の件数は0、失敗位置は-1のままになる)
            const qsnapper::restore::RestorePlanStatus status = qsnapper::restore::parseRestorePlanStatus(statusCsv);
            if (!status.valid) {
                qWarning() << "Restore plan status could not be parsed";
                return;
            }
            m_restoreResult = status.toVariantMap();
            emit restoreResultChanged();
        });

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit restoreCompleted(completed);
}

/**
 * @brief 復元サービスの消失を失敗として処理する
 *
 * サービス消失後のcancelPlan呼び出しはD-Bus活性化で不要なサービス再起動を招くため、
 * 登録解除以外のtransport呼び出しは行わない
 */
void FileChangeModel::onRestorePlanServiceVanished()
{
    if (!m_planActive) {
        return;
    }

    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    qWarning() << "Restore service stopped unexpectedly during an active restore plan";

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit errorOccurred(tr("The restore service stopped unexpectedly"));
    emit restoreCompleted(false);
}

/**
 * @brief 指定ファイルを現在の比較条件の復元元から復元する
 *
 * @param filePath 復元対象のファイルパス
 */
void FileChangeModel::restoreSingleFile(const QString &filePath)
{
    restoreSingleFileFrom(filePath, currentRestoreSourceNumber());
}

/**
 * @brief 復元元スナップショットを明示して指定ファイルを復元する
 *
 * @param filePath 復元対象のファイルパス
 * @param sourceSnapshotNumber 復元元スナップショット番号
 */
void FileChangeModel::restoreSingleFileFrom(const QString &filePath, int sourceSnapshotNumber)
{
    if (m_configName.isEmpty() || filePath.isEmpty()) {
        qWarning() << "Rejected single file restore:"
                   << "config=" << m_configName
                   << "source=" << sourceSnapshotNumber
                   << "expected=" << currentRestoreSourceNumber();
        emit errorOccurred(tr("Invalid parameters for restore"));
        emit restoreCompleted(false);
        return;
    }

    // 読み込み中は、表示中のエントリと比較条件 (m_compareNumber1/2) が食い違う可能性があるため復元を認めない
    if (m_loading) {
        qWarning() << "Rejected single file restore while file changes are still loading";
        emit errorOccurred(tr("File changes are still loading"));
        emit restoreCompleted(false);
        return;
    }

    if (!isRestoreSourceConsistent(sourceSnapshotNumber)) {
        qWarning() << "Rejected single file restore:"
                   << "config=" << m_configName
                   << "source=" << sourceSnapshotNumber
                   << "expected=" << currentRestoreSourceNumber();
        emit errorOccurred(tr("Invalid parameters for restore"));
        emit restoreCompleted(false);
        return;
    }

    // ツリーからchangeTypeを取得する (サービス側はcommit時に権威ある比較結果と照合する)
    QString changeType = QStringLiteral("modified");
    QModelIndex idx = findItemIndex(m_rootItem, filePath);
    if (idx.isValid()) {
        FileChangeItem *item = getItem(idx);
        if (item) {
            changeType = changeTypeToString(item->changeType());
        }
    }

    // 単一ファイルも選択項目の復元と同じstaged restore (Begin / Stage / Commit) で実行する
    startRestorePlan(sourceSnapshotNumber, QStringList{filePath}, QStringList{changeType});
}

/**
 * @brief ChangeType列挙値をD-Bus送信用の文字列へ変換する
 *
 * StageRestoreEntriesが期待するlower-case文字列へ正規化する
 *
 * @param type 変換対象の変更種別
 * @return 対応するchangeType文字列
 */
QString FileChangeModel::changeTypeToString(FileChangeItem::ChangeType type)
{
    switch (type) {
    case FileChangeItem::Created:     return QStringLiteral("created");
    case FileChangeItem::Deleted:     return QStringLiteral("deleted");
    case FileChangeItem::Modified:    return QStringLiteral("modified");
    case FileChangeItem::TypeChanged: return QStringLiteral("typechanged");
    }
    return QStringLiteral("modified");
}

/**
 * @brief 復元計画に送出するパスを正規化する
 *
 * 末尾のスラッシュを除去し、連続するスラッシュを単一にまとめる
 * サーバ側はこれらを許容するが、正規化して送ることでmanifestの内容を安定させる
 *
 * @param path 正規化対象のパス
 * @return 正規化済みのパス
 */
QString FileChangeModel::normalizeRestorePlanPath(const QString &path)
{
    QString normalized = path;
    while (normalized.size() > 1 && normalized.endsWith('/')) {
        normalized.chop(1);
    }
    static const QRegularExpression repeatedSlashes(QStringLiteral("/{2,}"));
    normalized.replace(repeatedSlashes, QStringLiteral("/"));
    return normalized;
}

/**
 * @brief 復元計画に送出できるエントリかどうかを判定する
 *
 * サーバ側 (StageRestoreEntries) は1件でも不正なエントリがあるとチャンク全体を拒否するため、
 * クライアント側で同じ規則を先に検査して不正エントリを除外する
 * (レガシーのRestoreFiles*が不正エントリを黙ってスキップしていた挙動に合わせる)
 *
 * @param path 正規化済みのパス
 * @param changeType 変更タイプ文字列
 * @return 送出可能な場合はtrue
 */
bool FileChangeModel::isValidRestorePlanEntry(const QString &path, const QString &changeType)
{
    if (!path.startsWith('/')) {
        return false;
    }
    if (path == QStringLiteral("/.snapshots") || path.startsWith(QStringLiteral("/.snapshots/"))) {
        return false;
    }
    if (changeType != QStringLiteral("created") && changeType != QStringLiteral("deleted")
            && changeType != QStringLiteral("modified") && changeType != QStringLiteral("typechanged")) {
        return false;
    }

    // '.' / '..' 成分を含まないこと
    const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QLatin1String(".") || component == QLatin1String("..")) {
            return false;
        }
    }

    // 制御文字 (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含まないこと
    for (const QChar &ch : path) {
        const ushort ucs = ch.unicode();
        if (ucs <= 0x1F || ucs == 0x7F || (ucs >= 0x80 && ucs <= 0x9F)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 配下の実レコードが全てCreatedかどうかを返す
 *
 * 復元元に存在しないディレクトリ (= Created) の配下にあるパスは、復元元にも存在し得ないため必ずCreatedになる
 * Created配下にModified / Deleted / TypeChangedの実レコードが現れるのは自己矛盾であり、
 * 行指向出力への改行注入によって偽の親ディレクトリが差し込まれた場合に起きる
 * Createdディレクトリは復元時に現在のファイルシステムでの再帰削除として扱われるため、矛盾する親をそのまま復元計画へ載せると、意図しない範囲が削除され得る
 *
 * @note 本判定はベストエフォートの多層防御であって、完全な防御ではない
 *       偽の親の配下が全てCreatedである変種 (新規ファイルだけが追加された正規のディレクトリと同じ形) は、
 *       行形式だけを受け取るクライアントからは正規のレコードと区別できない
 *       本欠陥に対する実質的な境界は、サーバ側のシリアライズfail-closedと、created削除前の復元元存在確認である
 *
 * ツリー構築時に中間ディレクトリとして合成されたノードはstatusFlagsが空であり、比較結果に由来しないため判定から除外する
 * 合成ノードは復元元での状態が不明なだけで矛盾の証拠にはならず、Pre/Post表示フィルタが実レコードを間引いた結果としても生じ得る
 *
 * @param item 判定対象のノード
 * @return 配下の実レコードが全てCreatedの場合はtrue
 */
bool FileChangeModel::hasOnlyCreatedRecordsBeneath(FileChangeItem *item)
{
    if (!item) {
        return true;
    }

    for (int i = 0; i < item->childCount(); ++i) {
        FileChangeItem *child = item->child(i);
        if (!child) {
            continue;
        }

        const bool isParsedRecord = !child->statusFlags().isEmpty();
        if (isParsedRecord && child->changeType() != FileChangeItem::Created) {
            return false;
        }
        if (!hasOnlyCreatedRecordsBeneath(child)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief チェック済みアイテムをchangeType付きで再帰収集する
 *
 * ディレクトリは自身の変更と配下の変更を分けて扱い、
 * 最終的にStageRestoreEntriesへ渡せるpath / changeType配列を構築する
 *
 * @param parent 走査開始ノード
 * @param paths 収集したパスの格納先
 * @param changeTypes 収集したchangeType文字列の格納先
 */
void FileChangeModel::collectCheckedItemsWithTypes(FileChangeItem *parent, QStringList &paths, QStringList &changeTypes) const
{
    for (int i = 0; i < parent->childCount(); ++i) {
        FileChangeItem *child = parent->child(i);

        if (child->isChecked()) {
            // チェックされたノードは、自身と必要なら配下の両方を収集対象にする
            QString itemPath = child->path();

            // パスを正規化 (末尾のスラッシュを削除)
            if (itemPath.endsWith('/') && itemPath.length() > 1) {
                itemPath = itemPath.left(itemPath.length() - 1);
            }

            bool hasChildren = (child->childCount() > 0);
            // Modified は「親ディレクトリとして存在するだけ」の場合があるため、
            // ディレクトリエントリ自体を送るかどうかは別途判定する
            bool isActualChange = (child->changeType() != FileChangeItem::Modified);

            if (hasChildren) {
                // 子要素があるアイテム = ディレクトリ
                // Createdディレクトリは現在のファイルシステムでの再帰削除になるため、配下の実レコードが全てCreatedであるという整合性を満たす場合のみ計画へ載せる
                // 矛盾する場合も配下の収集は必ず継続し、本物の選択パスは従来どおり載せる
                const bool createdSubtreeIsConsistent =
                        child->changeType() != FileChangeItem::Created
                        || hasOnlyCreatedRecordsBeneath(child);
                if (isActualChange && createdSubtreeIsConsistent
                        && !itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                    changeTypes.append(changeTypeToString(child->changeType()));
                }
                else if (isActualChange && !createdSubtreeIsConsistent) {
                    qWarning() << "FileChangeModel: Dropped an inconsistent created directory"
                               << "from the restore plan (non-created records beneath it)";
                }
                // 配下を再帰的に収集 (collectAllFilesRecursiveと同等の処理)
                collectCheckedItemsWithTypes(child, paths, changeTypes);
            }
            else {
                if (!itemPath.isEmpty() && itemPath != "/") {
                    paths.append(itemPath);
                    changeTypes.append(changeTypeToString(child->changeType()));
                }
            }
        }
        else {
            // 親が未チェックでも、配下に個別選択された項目があれば拾う
            collectCheckedItemsWithTypes(child, paths, changeTypes);
        }
    }
}

/**
 * @brief チェック済みの項目のうち、復元で現在のシステムから削除されるパスを返す
 *
 * 変更種別がcreatedのエントリは、復元元のスナップショットに存在しないパスであり、復元すると現在のシステムから削除される
 * 復元計画へ載せるときと同じ収集と正規化の規則を使うため、確認ダイアログの表示と実際の削除対象が食い違わない
 *
 * @param maxPaths 返すパスの最大件数 (ダイアログの描画負荷を抑えるため、負の値は0件として扱う)
 * @return count : 削除されるパスの総数、paths : 先頭から最大maxPaths件のパス (QStringList)
 */
QVariantMap FileChangeModel::checkedDeletePreview(int maxPaths) const
{
    QStringList checkedPaths;
    QStringList checkedChangeTypes;
    if (m_rootItem != nullptr) {
        collectCheckedItemsWithTypes(m_rootItem, checkedPaths, checkedChangeTypes);
    }

    QStringList deletePaths;
    QSet<QString> seenPaths;
    const int entryCount = qMin(checkedPaths.size(), checkedChangeTypes.size());
    for (int i = 0; i < entryCount; ++i) {
        const QString normalized = normalizeRestorePlanPath(checkedPaths.at(i));
        if (!isValidRestorePlanEntry(normalized, checkedChangeTypes.at(i))
                || seenPaths.contains(normalized)) {
            continue;
        }
        seenPaths.insert(normalized);
        if (checkedChangeTypes.at(i) == QStringLiteral("created")) {
            deletePaths.append(normalized);
        }
    }

    const int total = static_cast<int>(deletePaths.size());
    const int limit = qBound(0, maxPaths, total);
    QVariantMap preview;
    preview.insert(QStringLiteral("count"), total);
    preview.insert(QStringLiteral("paths"), QVariant::fromValue(deletePaths.mid(0, limit)));
    return preview;
}

/**
 * @brief チェックされたアイテムを復元
 *
 * チェックされた全てのアイテムを復元する (staged restore)
 * 事前にエントリを上限付きチャンクへ分割して認証前にstagingし、全チャンクのstaging完了後にcommitを1度だけ呼び出す
 * Polkitプロンプトはcommit時に1度だけ表示され、以降の進捗/完了はサーバ側manifestのシグナルで駆動される
 *
 * @return 復元処理が開始された場合: true、エラーの場合: false
 */
bool FileChangeModel::restoreCheckedItems()
{
    return restoreCheckedItemsFrom(currentRestoreSourceNumber());
}

/**
 * @brief 復元元スナップショットを明示してチェックされたアイテムを復元
 *
 * 復元元を引数で受け取ることで、比較モード (m_betweenMode) を維持したまま復元できる
 * 指定された復元元は、現在読み込まれているエントリの解釈と一致する必要がある
 *
 * @param sourceSnapshotNumber 復元元スナップショット番号
 * @return 復元処理が開始された場合: true、エラーの場合: false
 */
bool FileChangeModel::restoreCheckedItemsFrom(int sourceSnapshotNumber)
{
    if (m_configName.isEmpty()) {
        qWarning() << "Rejected restore plan: config name is empty";
        emit errorOccurred("Invalid config name or snapshot number");
        emit restoreCompleted(false);
        return false;
    }

    // 読み込み中は、表示中のエントリと比較条件 (m_compareNumber1/2) が食い違う可能性があるため復元を認めない
    if (m_loading) {
        qWarning() << "Rejected restore plan while file changes are still loading";
        emit errorOccurred(tr("File changes are still loading"));
        emit restoreCompleted(false);
        return false;
    }

    if (!isRestoreSourceConsistent(sourceSnapshotNumber)) {
        qWarning() << "Rejected restore plan:"
                   << "config=" << m_configName
                   << "source=" << sourceSnapshotNumber
                   << "expected=" << currentRestoreSourceNumber();
        emit errorOccurred("Invalid config name or snapshot number");
        emit restoreCompleted(false);
        return false;
    }

    // 両方のモードでchangeTypeを収集する (StageRestoreEntriesでchangeTypes必須)
    QStringList checkedPaths;
    QStringList checkedChangeTypes;

    collectCheckedItemsWithTypes(m_rootItem, checkedPaths, checkedChangeTypes);

    return startRestorePlan(sourceSnapshotNumber, checkedPaths, checkedChangeTypes);
}

/**
 * @brief 復元対象を検証・正規化し、staged restore計画を開始する
 *
 * 選択項目の復元と単一ファイルの復元の共通処理
 * 呼び出し側で設定名・読み込み状態・復元元の整合性を検証済みであること
 *
 * @param sourceSnapshotNumber 復元元スナップショット番号
 * @param checkedPaths 復元対象のパス (config相対名)
 * @param checkedChangeTypes 各パスの変更種別
 * @return 復元処理が開始された場合: true、エラーの場合: false
 */
bool FileChangeModel::startRestorePlan(int sourceSnapshotNumber,
                                       const QStringList &checkedPaths,
                                       const QStringList &checkedChangeTypes)
{
    // 送信前にクライアント側で検証・正規化し、不正エントリを除外する
    // 正規化後のパスで重複を除き、最初の出現順を維持する
    QStringList planPaths;
    QStringList planChangeTypes;
    QSet<QString> seenPaths;
    const int entryCount = qMin(checkedPaths.size(), checkedChangeTypes.size());
    for (int i = 0; i < entryCount; ++i) {
        const QString normalized = normalizeRestorePlanPath(checkedPaths.at(i));
        if (!isValidRestorePlanEntry(normalized, checkedChangeTypes.at(i))) {
            qWarning() << "Skipping invalid restore entry:" << checkedPaths.at(i);
            continue;
        }
        if (seenPaths.contains(normalized)) {
            continue;
        }
        seenPaths.insert(normalized);
        planPaths.append(normalized);
        planChangeTypes.append(checkedChangeTypes.at(i));
    }

    if (planPaths.isEmpty()) {
        emit errorOccurred(tr("No files selected for restoration"));
        emit restoreCompleted(false);
        return false;
    }

    if (!m_restorePlanTransport) {
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }

    // System Bus transportは、従来の復元経路と同じくサービス再接続を試みる
    // テストtransportはD-Bus接続なしで実行できるよう、この確認を適用しない
    if (m_restorePlanTransport == m_defaultRestorePlanTransport
            && (!m_dbusInterface || !m_dbusInterface->isValid())
            && !reconnectDbus()) {
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }

    // 前回の復元結果を消し、まだ届いていない前回の状態応答も無効にする
    ++m_restoreResultGeneration;
    if (!m_restoreResult.isEmpty()) {
        m_restoreResult.clear();
        emit restoreResultChanged();
    }

    // 復元計画の状態を初期化する (前回実行の残留状態を消す)
    m_planManifestId.clear();
    m_planPaths = planPaths;
    m_planChangeTypes = planChangeTypes;
    m_planNextStageIndex = 0;
    m_planCommitted = false;
    m_planActive = true;
    m_planSignalsSubscribed = false;
    m_planCancelRequested = false;
    m_planTotalFiles = planPaths.size();
    m_planLastProgress = 0;
    m_cancelRequested = false;
    m_totalFilesCount = planPaths.size();
    m_processedFilesCount = 0;

    // 進捗/完了はサーバ側manifestのシグナルで駆動する
    if (!m_restorePlanTransport->subscribePlanSignals(this)) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        resetRestorePlanState();
        emit errorOccurred("D-Bus connection failed");
        emit restoreCompleted(false);
        return false;
    }
    m_planSignalsSubscribed = true;

    // 認証なしでstaging計画を開始する (Polkitプロンプトはcommit時に1度だけ出る)
    const QString restoreMode = m_useDirectRestore ? QStringLiteral("direct") : QStringLiteral("yast");
    // counterpartは表示中のエントリを生んだ比較条件 (m_loadedComparison) から取る
    // 後から変更され得るUIプロパティを使うと、検証対象と実行対象の比較が食い違う
    // (対カレント比較では0 = 「現在のシステム」のsentinelになる)
    m_restorePlanTransport->beginPlan(m_configName, sourceSnapshotNumber,
                                      m_loadedComparison.number2, restoreMode,
                                      [this](bool ok, const QString &manifestId, const QString &error) {
                                          onPlanBeginFinished(ok, manifestId, error);
                                      });

    return true;
}

/**
 * @brief staging計画の作成結果を処理する
 *
 * beginPlanの結果を受け、成功した場合は先頭チャンクのstagingを開始する
 *
 * @param ok 計画作成に成功した場合: true
 * @param manifestId 作成された計画のマニフェストID
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanBeginFinished(bool ok, const QString &manifestId, const QString &error)
{
    // beginの応答がキャンセル後に届いた場合も、発行済みmanifestをベストエフォートで破棄する
    if (!m_planActive) {
        if (m_cancelRequested && !manifestId.isEmpty()) {
            m_restorePlanTransport->cancelPlan(manifestId,
                                               [](bool cancelOk, const QString &cancelError) {
                                                   Q_UNUSED(cancelOk);
                                                   Q_UNUSED(cancelError);
                                               });
        }
        return;
    }

    if (m_cancelRequested) {
        if (!manifestId.isEmpty()) {
            m_planManifestId = manifestId;
            m_planCancelRequested = true;
            m_planActive = false;
            if (m_planSignalsSubscribed) {
                m_restorePlanTransport->unsubscribePlanSignals(this);
                m_planSignalsSubscribed = false;
            }
            m_restorePlanTransport->cancelPlan(m_planManifestId,
                                               [](bool cancelOk, const QString &cancelError) {
                                                   Q_UNUSED(cancelOk);
                                                   Q_UNUSED(cancelError);
                                               });
        }
        else {
            m_planActive = false;
            if (m_planSignalsSubscribed) {
                m_restorePlanTransport->unsubscribePlanSignals(this);
                m_planSignalsSubscribed = false;
            }
        }
        resetRestorePlanState();
        emit restoreCompleted(false);
        return;
    }

    if (!ok || manifestId.isEmpty()) {
        if (!manifestId.isEmpty()) {
            m_planManifestId = manifestId;
        }
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to begin restore plan") : error);
        return;
    }

    m_planManifestId = manifestId;
    stageNextPlanChunk();
}

/**
 * @brief 次のチャンクをstagingする
 *
 * チャンクは必ず1件ずつ順次送出する (サーバ側は計画ごとに単一スレッドで処理するため、並列送出は行わない)
 * 全チャンクのstagingが完了したらcommitを1度だけ呼び出して、計画を凍結する
 */
void FileChangeModel::stageNextPlanChunk()
{
    if (!m_planActive || m_cancelRequested || m_planCommitted) {
        return;
    }

    // サーバ側の1チャンク上限に合わせたチャンクサイズ
    // (m_restoreBatchSizeは1..1000に丸め済みのため常に収まるが、念のためクランプする)
    static constexpr int kMaxEntriesPerStageChunk = 5000;  // src/dbusservice/restoremanifest.hのkMaxEntriesPerStageChunkと同期
    const int chunkSize = qBound(1, m_restoreBatchSize, kMaxEntriesPerStageChunk);

    if (m_planManifestId.isEmpty()) {
        finishRestorePlanWithError(tr("Restore plan has no manifest id"));
        return;
    }

    if (m_planNextStageIndex >= m_planPaths.size()) {
        // 全チャンクのstaging完了 -> 計画を凍結して認証と実行を開始する
        commitRestorePlan();
        return;
    }

    const int chunkEnd = qMin(m_planNextStageIndex + chunkSize, m_planPaths.size());
    QStringList chunkPaths;
    QStringList chunkTypes;
    for (int i = m_planNextStageIndex; i < chunkEnd; ++i) {
        chunkPaths.append(m_planPaths.at(i));
        chunkTypes.append(m_planChangeTypes.at(i));
    }

    // チャンクの送出前にインデックスを進める (失敗時はフロー自体を中断するため巻き戻し不要)
    m_planNextStageIndex = chunkEnd;

    m_restorePlanTransport->stageEntries(m_planManifestId, chunkPaths, chunkTypes,
                                         [this](bool ok, const QString &error) {
                                             onPlanStageFinished(ok, error);
                                         });
}

/**
 * @brief チャンクのstaging結果を処理する
 *
 * 成功した場合は次のチャンクをstagingし、失敗した場合はフローを中断する
 *
 * @param ok stagingに成功した場合: true
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanStageFinished(bool ok, const QString &error)
{
    if (!m_planActive || m_cancelRequested || m_planCommitted) {
        return;
    }

    if (!ok) {
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to stage restore entries") : error);
        return;
    }

    stageNextPlanChunk();
}

/**
 * @brief 復元計画をcommit (凍結) する
 *
 * この呼び出しでサーバ側のPolkit認証が1度だけ行われ、認証後は実行がサーバ側で非同期に継続される
 * 以降のUI更新はシグナル駆動になる
 */
void FileChangeModel::commitRestorePlan()
{
    if (!m_planActive || m_cancelRequested || m_planCommitted || m_planManifestId.isEmpty()) {
        return;
    }

    // commit呼び出しを発行した時点で計画は凍結処理中とみなし、その間のcancelRestore()も実行中計画として扱う
    m_planCommitted = true;
    m_restorePlanTransport->commitPlan(m_planManifestId,
                                       [this](bool ok, const QString &error) {
                                           onPlanCommitFinished(ok, error);
                                       });
}

/**
 * @brief 復元計画のcommit結果を処理する
 *
 * commitに成功したらフローはシグナル待ちに移行する
 * 失敗した場合は、フローを中断する
 *
 * @param ok commitに成功した場合: true
 * @param error 失敗時のエラーメッセージ
 */
void FileChangeModel::onPlanCommitFinished(bool ok, const QString &error)
{
    if (!m_planActive) {
        return;
    }

    if (!ok) {
        finishRestorePlanWithError(error.isEmpty() ? tr("Failed to commit restore plan") : error);
        return;
    }

    // 計画は凍結済み
    // 以降はrestorePlanProgress / restorePlanFinishedで駆動される
}

/**
 * @brief 復元計画をエラー完了させる
 *
 * エラー発生時は以降のtransport呼び出しを行わず (サーバ側に残ったstaging計画の破棄を促すベストエフォートのcancelPlanのみ例外)、
 * シグナル受信を解除してエラーと完了を通知し、状態をリセットする
 *
 * @param message ユーザーに表示するエラーメッセージ
 */
void FileChangeModel::finishRestorePlanWithError(const QString &message)
{
    if (!m_planActive) {
        return;
    }

    const QString manifestId = m_planManifestId;
    RestorePlanTransport *transport = m_restorePlanTransport;
    m_planActive = false;

    // 以降の遅延コールバックやserver signalが完了通知を重ねないよう、transport呼び出しの前に購読を解除する
    if (m_planSignalsSubscribed) {
        transport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    // マニフェストIDが既に発行されている場合は、サーバ側に残った計画をエージング任せにせず破棄させる (ベストエフォート)
    if (!manifestId.isEmpty() && !m_planCancelRequested) {
        m_planCancelRequested = true;
        transport->cancelPlan(manifestId,
                              [](bool cancelOk, const QString &cancelError) {
                                  Q_UNUSED(cancelOk);
                                  Q_UNUSED(cancelError);
                              });
    }

    qWarning() << "Restore plan failed:" << message;

    // 状態を先にリセットしてから完了を通知する (完了ハンドラからの再開始に備える)
    resetRestorePlanState();
    emit errorOccurred(message);
    emit restoreCompleted(false);
}

/**
 * @brief 復元計画の実行状態をリセットする
 *
 * 次回のrestoreCheckedItems()がクリーンな状態から開始できるようにする
 */
void FileChangeModel::resetRestorePlanState()
{
    m_planManifestId.clear();
    m_planPaths.clear();
    m_planChangeTypes.clear();
    m_planNextStageIndex = 0;
    m_planCommitted = false;
    m_planActive = false;
    m_planSignalsSubscribed = false;
    m_planCancelRequested = false;
    m_planTotalFiles = 0;
    m_planLastProgress = 0;
}

/**
 * @brief 復元処理をキャンセル
 *
 * 復元処理のキャンセルを要求する
 *
 * キャンセルはサーバ側の次のチャンク境界で反映されるため、既に復元されたファイルやディレクトリはそのまま残る
 *
 * 復元計画がcommit済みの場合はcancelPlanを送った後、終端シグナル (restorePlanFinished) を待ってから完了を通知する
 * staging中 (commit前) の場合は以降のチャンク送出を停止し、ベストエフォートで計画を破棄してローカルで完了扱いにする
 * 実行中の計画がない場合は従来どおりフラグの設定のみ行う
 */
void FileChangeModel::cancelRestore()
{
    m_cancelRequested = true;
    qWarning() << "Restore operation cancel requested";

    if (!m_planActive) {
        // 復元計画が実行中でない場合はフラグのみ設定する
        return;
    }

    if (m_planCommitted) {
        // commit済み:
        // サーバ側のチャンク境界でキャンセルが効く
        // 完了はrestorePlanFinishedシグナルで判定するため、ここでは楽観的に完了しない
        if (!m_planCancelRequested && !m_planManifestId.isEmpty()) {
            m_planCancelRequested = true;
            m_restorePlanTransport->cancelPlan(m_planManifestId,
                                               [](bool ok, const QString &error) {
                                                   Q_UNUSED(ok);
                                                   Q_UNUSED(error);
                                                   // キャンセル要求の結果は終端シグナルで扱う
                                               });
        }
        return;
    }

    // staging中 (commit前):
    // 以降のチャンクは送出せず、ベストエフォートで計画を破棄してローカルで完了扱いにする
    const QString manifestId = m_planManifestId;
    m_planActive = false;
    if (m_planSignalsSubscribed) {
        m_restorePlanTransport->unsubscribePlanSignals(this);
        m_planSignalsSubscribed = false;
    }

    if (!manifestId.isEmpty() && !m_planCancelRequested) {
        m_planCancelRequested = true;
        m_restorePlanTransport->cancelPlan(manifestId,
                                           [](bool ok, const QString &error) {
                                               Q_UNUSED(ok);
                                               Q_UNUSED(error);
                                               // 破棄結果はUI状態に影響しない (ローカル完了扱いのため)
                                           });
    }
    resetRestorePlanState();
    emit restoreCompleted(false);
}

/**
 * @brief テスト専用に復元計画transportを差し替える
 *
 * 所有権は移らないため、差し込んだtransportは呼び出し側が管理する
 * 既定のSystem Bus実装は破棄せず、ポインタのみ差し替える
 *
 * @param transport 差し込むtransport (nullptrの場合は既定のtransportへ戻す)
 */
void FileChangeModel::setRestorePlanTransportForTesting(RestorePlanTransport *transport)
{
    m_restorePlanTransport = transport ? transport : m_defaultRestorePlanTransport;
}

/**
 * @brief テスト専用に変更一覧の取り込み上限を差し替える
 *
 * 実際の上限(50万件と256 MiB)は大きすぎてテストで超えられないため、小さい値へ差し替えて省略の動作を確認する
 *
 * @param maxEntries 取り込む変更エントリ数の上限
 * @param maxTextChars 取り込む変更一覧の文字数の上限
 */
void FileChangeModel::setChangeLimitsForTesting(int maxEntries, qsizetype maxTextChars)
{
    m_maxChangeEntries = maxEntries;
    m_maxChangeTextChars = maxTextChars;
}
