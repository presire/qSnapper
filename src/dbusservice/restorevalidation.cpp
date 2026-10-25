/**
 * @file restorevalidation.cpp
 * @brief restorevalidation.h宣言の実装
 *
 * 本ファイルの関数はシステムコールを発行しない
 * 木の観測は呼び出し側が渡す関数に委ねる
 */

#include "restorevalidation.h"

#include <QStringList>

#include <sys/stat.h>

namespace qsnapper::restore {

namespace {

// 1回のentriesSliceで検証するエントリ数の上限
// RestorePlanExecutorのchunk境界と同規模の件数上限ごとの部分列であり、凍結済み計画全体 (最大20万エントリ) を1度にメモリへ展開しない
constexpr int kEntriesPerValidationChunk = 64;

/**
 * @brief 絶対パスを空成分を除いた成分列へ分解し、安全でない成分を拒否する
 * @param absolutePath 分解対象の絶対パス
 * @param componentsOut 分解結果 ("/"のみなら空)
 * @return 先頭が"/"で、"." / ".."成分・制御文字を含まない場合true
 */
bool splitSafeAbsoluteComponents(const QString &absolutePath, QStringList *componentsOut)
{
    if (!absolutePath.startsWith(QLatin1Char('/'))) {
        return false;
    }

    const QStringList components = absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QLatin1String(".") || component == QLatin1String("..")) {
            return false;
        }
        for (const QChar ch : component) {
            if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
                return false;
            }
        }
    }
    *componentsOut = components;
    return true;
}

} // namespace

RestorePathObservation restorePathObservationFromStat(const struct stat &info)
{
    RestorePathObservation observation;
    observation.presence = RestorePathPresence::Present;
    observation.fileType = static_cast<quint32>(info.st_mode & S_IFMT);
    observation.mode = static_cast<quint32>(info.st_mode & 07777);
    observation.inode = static_cast<quint64>(info.st_ino);
    observation.uid = static_cast<quint32>(info.st_uid);
    observation.gid = static_cast<quint32>(info.st_gid);
    observation.size = static_cast<qint64>(info.st_size);
    observation.mtimeSec = static_cast<qint64>(info.st_mtim.tv_sec);
    observation.mtimeNsec = static_cast<qint64>(info.st_mtim.tv_nsec);
    observation.ctimeSec = static_cast<qint64>(info.st_ctim.tv_sec);
    observation.ctimeNsec = static_cast<qint64>(info.st_ctim.tv_nsec);
    return observation;
}

bool isRestoreEntryBackedByTrees(const QString &changeType,
                                 const RestorePathObservation &source,
                                 const RestorePathObservation &counterpart)
{
    // 片方でも存在を確定できない場合は、差分の有無を判定できないため裏付けなしとする
    if (source.presence == RestorePathPresence::Unknown
            || counterpart.presence == RestorePathPresence::Unknown) {
        return false;
    }

    const bool inSource = source.presence == RestorePathPresence::Present;
    const bool inCounterpart = counterpart.presence == RestorePathPresence::Present;

    if (changeType == QStringLiteral("created")) {
        return !inSource && inCounterpart;
    }
    if (changeType == QStringLiteral("deleted")) {
        return inSource && !inCounterpart;
    }
    // typechangedとmodifiedは両方に存在する場合だけ比較できるため、片方にしか無い組は裏付けなしとする
    if (!inSource || !inCounterpart) {
        return false;
    }
    if (changeType == QStringLiteral("typechanged")) {
        return source.fileType != counterpart.fileType;
    }
    if (changeType == QStringLiteral("modified")) {
        if (source.fileType != counterpart.fileType) {
            return false;
        }
        return source.inode != counterpart.inode
            || source.mode != counterpart.mode
            || source.uid != counterpart.uid
            || source.gid != counterpart.gid
            || source.size != counterpart.size
            || source.mtimeSec != counterpart.mtimeSec
            || source.mtimeNsec != counterpart.mtimeNsec
            || source.ctimeSec != counterpart.ctimeSec
            || source.ctimeNsec != counterpart.ctimeNsec;
    }
    // 未知のchangeTypeは実行段がどう扱うかを決められないため、裏付けなしとして拒否する
    return false;
}

bool validateFrozenEntriesAgainstTrees(RestoreManifestRegistry &registry,
                                       const QString &manifestId,
                                       const QString &owner,
                                       const QString &subvolume,
                                       const RestorePathObserver &observeSource,
                                       const RestorePathObserver &observeCounterpart,
                                       ManifestError *err)
{
    // 観測関数が未設定のまま検証すると、全entryを誤って裏付けなしと判定するため、引数エラーとして拒否する
    if (!observeSource || !observeCounterpart) {
        if (err) {
            *err = ManifestError::InvalidArgument;
        }
        return false;
    }

    int offset = 0;
    for (;;) {
        ManifestError sliceError = ManifestError::None;
        const auto slice = registry.entriesSlice(manifestId, owner, offset,
                                                 kEntriesPerValidationChunk,
                                                 &sliceError);
        if (!slice.has_value()) {
            if (err) {
                *err = sliceError;
            }
            return false;
        }
        if (slice->isEmpty()) {
            break;
        }

        for (const RestoreEntry &entry : slice.value()) {
            // 実行段 (applyRestoreEntry) と同じ規則で相対パスを導出し、観測したパスと操作するパスを一致させる
            QString rootPath;
            QString destinationPath;
            QString relativePath;
            if (!buildRestoreDestination(subvolume, entry.path, &rootPath,
                                         &destinationPath, &relativePath)
                    || !isRestoreEntryBackedByTrees(entry.changeType,
                                                    observeSource(relativePath),
                                                    observeCounterpart(relativePath))) {
                if (err) {
                    *err = ManifestError::InvalidArgument;
                }
                return false;
            }
        }

        offset += static_cast<int>(slice->size());
    }

    if (err) {
        *err = ManifestError::None;
    }
    return true;
}

bool isSnapshotMetadataRestoreName(const QString &configRelativeName)
{
    const QStringList components =
        configRelativeName.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return !components.isEmpty()
        && components.first() == QLatin1String(".snapshots");
}

bool normalizeRestoreSubvolume(const QString &subvolume, QString *rootPathOut)
{
    if (!rootPathOut) {
        return false;
    }
    rootPathOut->clear();

    QStringList components;
    if (!splitSafeAbsoluteComponents(subvolume, &components)) {
        return false;
    }
    *rootPathOut = QLatin1Char('/') + components.join(QLatin1Char('/'));
    return true;
}

bool buildRestoreDestination(const QString &subvolume,
                             const QString &configRelativeName,
                             QString *rootPathOut,
                             QString *destinationOut,
                             QString *relativeOut)
{
    if (!rootPathOut || !destinationOut || !relativeOut) {
        return false;
    }
    rootPathOut->clear();
    destinationOut->clear();
    relativeOut->clear();

    QString rootPath;
    QStringList nameComponents;
    if (!normalizeRestoreSubvolume(subvolume, &rootPath)
            || !splitSafeAbsoluteComponents(configRelativeName, &nameComponents)
            || nameComponents.isEmpty()
            || nameComponents.first() == QLatin1String(".snapshots")) {
        return false;
    }

    const QString relative = nameComponents.join(QLatin1Char('/'));
    *rootPathOut = rootPath;
    *relativeOut = relative;
    *destinationOut = rootPath == QStringLiteral("/")
        ? QLatin1Char('/') + relative
        : rootPath + QLatin1Char('/') + relative;
    return true;
}

bool shouldRestoreRootReadWrite(const RootReadWriteSafetyNetState &state,
                                bool readOnlyAfterRestore)
{
    return state.executionStarted
        && state.targetsRootSubvolume
        && state.preRestoreStateKnown
        && !state.preRestoreReadOnly
        && readOnlyAfterRestore;
}

} // namespace qsnapper::restore
