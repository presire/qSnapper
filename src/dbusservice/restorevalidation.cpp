#include "restorevalidation.h"

#include <QByteArray>

#include "inputvalidator.h"

namespace qsnapper::restore {

namespace {

// 1回のentriesSliceで検証するエントリ数の上限
// RestorePlanExecutorのchunk境界と同規模のbounded sliceであり、凍結済み計画全体 (最大20万エントリ) を1度にメモリへ展開しない
constexpr int kEntriesPerValidationChunk = 64;

} // namespace

QString restoreExpectedChangeType(unsigned int statusBits)
{
    if (statusBits & RestoreStatusCreated) {
        return QStringLiteral("created");
    }
    if (statusBits & RestoreStatusDeleted) {
        return QStringLiteral("deleted");
    }
    if (statusBits & RestoreStatusType) {
        return QStringLiteral("typechanged");
    }
    return QStringLiteral("modified");
}

QString canonicalizeRestorePlanPathKey(const QString &path)
{
    QString result = path;
    while (result.size() > 1 && result.endsWith(QLatin1Char('/'))) {
        result.chop(1);
    }

    QString collapsed;
    collapsed.reserve(result.size());
    QChar previous;
    for (const QChar current : result) {
        if (current == QLatin1Char('/') && previous == QLatin1Char('/')) {
            continue;
        }
        collapsed.append(current);
        previous = current;
    }
    return collapsed;
}

bool decodeAuthoritativeRestoreName(const std::string &rawName,
                                    QString *decodedOut)
{
    const QByteArray rawBytes(rawName.data(),
                              static_cast<qsizetype>(rawName.size()));
    const QString decoded = QString::fromUtf8(rawBytes);

    // 不正UTF-8は復号時に置換文字U+FFFDへ潰れる
    // 再符号化が元のバイト列と一致しない名前は、検証対象と実行対象がズレるため受理しない
    if (decoded.toUtf8() != rawBytes) {
        return false;
    }

    // 改行等の制御文字はlibsnapperの行指向filelistキャッシュを汚染し得る
    // (キャッシュ再読込時に改行以降が偽エントリとして混入する)
    if (!qsnapper::security::isRecordSafeText(decoded)) {
        return false;
    }

    // 比較結果のパスとして空名は意味を持たない
    if (decoded.isEmpty()) {
        return false;
    }

    if (decodedOut) {
        *decodedOut = decoded;
    }
    return true;
}

bool registerAuthoritativeRestoreEntry(QHash<QString, QString> *expectedTypes,
                                       const QString &rawName,
                                       unsigned int statusBits)
{
    if (!expectedTypes) {
        return false;
    }

    const QString key = canonicalizeRestorePlanPathKey(rawName);
    const QString expected = restoreExpectedChangeType(statusBits);

    const auto it = expectedTypes->constFind(key);
    if (it != expectedTypes->constEnd()) {
        // 同一キーへの2回目の登録は、期待型が一致する場合のみ許す
        // (snapperのfilterは同一パスを複数回報告しないため、一致はredundant登録のみ)
        return it.value() == expected;
    }
    expectedTypes->insert(key, expected);
    return true;
}

bool validateFrozenEntriesAgainstAuthoritative(
    RestoreManifestRegistry &registry,
    const QString &manifestId,
    const QString &owner,
    const QHash<QString, QString> &expectedTypes,
    ManifestError *err)
{
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
            const QString key = canonicalizeRestorePlanPathKey(entry.path);
            const auto it = expectedTypes.constFind(key);
            if (it == expectedTypes.constEnd() || it.value() != entry.changeType) {
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

} // namespace qsnapper::restore
