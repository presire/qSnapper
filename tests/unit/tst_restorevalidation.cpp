#include "restorevalidation.h"

#include <QTest>

using namespace qsnapper::restore;

/**
 * @brief commit時preflightの純粋関数群 (状態ビット -> changeType写像、path正規化、権威あるmap構築、凍結済みentry検証) の単体テスト
 *
 * libsnapperに依存せず、snapper::File::getPreToPostStatus()と同一のビット値を直接与えて検証する
 */
class TestRestoreValidation : public QObject
{
    Q_OBJECT

private slots:
    void createdBitWinsOverEverything();
    void deletedBitBeatsTypeAndBelow();
    void typeBitBeatsContentAndMetadata();
    void contentAndMetadataOnlyAndEmptyMapToModified();
    void canonicalizationStripsTrailingSlashes();
    void canonicalizationCollapsesInnerSlashRuns();
    void canonicalizationKeepsRootAndDoesNotFoldDotComponents();
    void authoritativeNameAcceptsWellFormedPaths();
    void authoritativeNameRejectsControlCharacters();
    void authoritativeNameRejectsInvalidUtf8();
    void authoritativeNamePreservesValidMultibyteUtf8();
    void authoritativeCollisionIsDetected();
    void authoritativeRedundantIdenticalEntryIsAccepted();
    void frozenEntriesMatchingAuthoritativeMapPass();
    void unknownPathAndMismatchedTypeAreRejected();
    void validationReadsFrozenEntriesInBoundedChunks();
    void restoreDestinationForRootConfigIsUnchanged();
    void restoreDestinationIsBasedOnConfigSubvolume();
    void restoreDestinationRejectsUnsafeNames();
    void snapshotMetadataNameIsDetectedAfterNormalization();
    void rootReadWriteSafetyNetRequiresAllConditions();
};

void TestRestoreValidation::createdBitWinsOverEverything()
{
    // CREATED (1) が他の全ビットに優先する
    QCOMPARE(restoreExpectedChangeType(RestoreStatusCreated),
             QStringLiteral("created"));
    QCOMPARE(restoreExpectedChangeType(RestoreStatusCreated
                                       | RestoreStatusDeleted
                                       | RestoreStatusType
                                       | RestoreStatusContent
                                       | RestoreStatusPermissions),
             QStringLiteral("created"));
}

void TestRestoreValidation::deletedBitBeatsTypeAndBelow()
{
    // DELETED (2) はCREATEDが無い場合に優先する
    QCOMPARE(restoreExpectedChangeType(RestoreStatusDeleted),
             QStringLiteral("deleted"));
    QCOMPARE(restoreExpectedChangeType(RestoreStatusDeleted
                                       | RestoreStatusType
                                       | RestoreStatusContent),
             QStringLiteral("deleted"));
}

void TestRestoreValidation::typeBitBeatsContentAndMetadata()
{
    QCOMPARE(restoreExpectedChangeType(RestoreStatusType),
             QStringLiteral("typechanged"));
    QCOMPARE(restoreExpectedChangeType(RestoreStatusType
                                       | RestoreStatusContent
                                       | RestoreStatusOwner
                                       | RestoreStatusXattrs
                                       | RestoreStatusAcl),
             QStringLiteral("typechanged"));
}

void TestRestoreValidation::contentAndMetadataOnlyAndEmptyMapToModified()
{
    QCOMPARE(restoreExpectedChangeType(RestoreStatusContent),
             QStringLiteral("modified"));
    QCOMPARE(restoreExpectedChangeType(RestoreStatusContent
                                       | RestoreStatusPermissions
                                       | RestoreStatusOwner
                                       | RestoreStatusGroup
                                       | RestoreStatusXattrs
                                       | RestoreStatusAcl),
             QStringLiteral("modified"));
    QCOMPARE(restoreExpectedChangeType(RestoreStatusPermissions),
             QStringLiteral("modified"));
    QCOMPARE(restoreExpectedChangeType(0),
             QStringLiteral("modified"));
}

void TestRestoreValidation::canonicalizationStripsTrailingSlashes()
{
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/etc/example.conf")),
             QStringLiteral("/etc/example.conf"));
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/etc/example.conf/")),
             QStringLiteral("/etc/example.conf"));
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/etc/conf.d///")),
             QStringLiteral("/etc/conf.d"));
}

void TestRestoreValidation::canonicalizationCollapsesInnerSlashRuns()
{
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("//etc//conf.d")),
             QStringLiteral("/etc/conf.d"));
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/a///b/c")),
             QStringLiteral("/a/b/c"));
}

void TestRestoreValidation::canonicalizationKeepsRootAndDoesNotFoldDotComponents()
{
    // root "/" はそのまま維持される
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/")),
             QStringLiteral("/"));

    // QDir::cleanPath()とは異なり '.' / '..' は折り畳まない
    // (折り畳みは既存の拒否境界を弱めるため意図的に避けている)
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/a/./b")),
             QStringLiteral("/a/./b"));
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/a/../b")),
             QStringLiteral("/a/../b"));
    QCOMPARE(canonicalizeRestorePlanPathKey(QStringLiteral("/a/./b/")),
             QStringLiteral("/a/./b"));
}

/**
 * @brief 権威名の復号検証で、通常のパスが受理されることを検証する
 *
 * カンマや空白はパスとして正当であり、拒否対象ではない
 */
void TestRestoreValidation::authoritativeNameAcceptsWellFormedPaths()
{
    QString decoded;

    QVERIFY(decodeAuthoritativeRestoreName("/etc/motd", &decoded));
    QCOMPARE(decoded, QStringLiteral("/etc/motd"));

    QVERIFY(decodeAuthoritativeRestoreName("/etc/a, b/c.txt", &decoded));
    QCOMPARE(decoded, QStringLiteral("/etc/a, b/c.txt"));

    QVERIFY(decodeAuthoritativeRestoreName("/", &decoded));
    QCOMPARE(decoded, QStringLiteral("/"));
}

/**
 * @brief 制御文字を含む権威名がfail-closedで拒否されることを検証する
 *
 * libsnapperのfilelistキャッシュは無エスケープの行指向形式であるため、
 * 改行を含む名前はキャッシュ再読込時に偽エントリとして混入し得る
 */
void TestRestoreValidation::authoritativeNameRejectsControlCharacters()
{
    // LF: 偽エントリの注入に直結する
    QVERIFY(!decodeAuthoritativeRestoreName("/etc/car\nrier", nullptr));
    QVERIFY(!decodeAuthoritativeRestoreName("\n+ /etc/shadow", nullptr));

    // CR: 行指向パーサによっては区切りとして扱われる
    QVERIFY(!decodeAuthoritativeRestoreName("/etc/car\rrier", nullptr));

    // 埋め込みNUL: syscall引数の切り詰めを招く
    const QByteArray withEmbeddedNul("/etc/a\0b", 8);
    QVERIFY(!decodeAuthoritativeRestoreName(
        std::string(withEmbeddedNul.constData(),
                    static_cast<size_t>(withEmbeddedNul.size())), nullptr));

    // DEL (U+007F)
    QVERIFY(!decodeAuthoritativeRestoreName("/etc/a\x7f" "b", nullptr));

    // C1制御文字 (U+0085 NELは、UTF-8でC2 85)
    QVERIFY(!decodeAuthoritativeRestoreName("/etc/a\xc2\x85" "b", nullptr));

    QVERIFY(!decodeAuthoritativeRestoreName("/etc/a\tb", nullptr));

    // 拒否時は出力を書き換えない
    QString untouched = QStringLiteral("sentinel");
    QVERIFY(!decodeAuthoritativeRestoreName("/etc/a\nb", &untouched));
    QCOMPARE(untouched, QStringLiteral("sentinel"));
}

/**
 * @brief 不正UTF-8の権威名が拒否されることを検証する
 *
 * 復号でU+FFFDへ潰れると、検証した名前とexecutorが使う名前がズレる
 */
void TestRestoreValidation::authoritativeNameRejectsInvalidUtf8()
{
    // 生バイト列を長さ付きで渡すための補助 (strlenでは埋め込みNUL以降を扱えない)
    const auto rawName = [](const QByteArray &bytes) {
        return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
    };

    // 単独の0xFFは、UTF-8として不正
    QVERIFY(!decodeAuthoritativeRestoreName(rawName(QByteArray("\xff", 1)), nullptr));
    QVERIFY(!decodeAuthoritativeRestoreName(
        rawName(QByteArray("/etc/\xff\xfe", 7)), nullptr));

    // 継続バイトが単独で現れる場合も不正
    QVERIFY(!decodeAuthoritativeRestoreName(
        rawName(QByteArray("/etc/\x80", 6)), nullptr));

    // 過長符号化 (0xC0 0xAF) も不正
    QVERIFY(!decodeAuthoritativeRestoreName(
        rawName(QByteArray("/etc/\xc0\xaf", 7)), nullptr));

    // 空名は比較結果のパスとして意味を持たない
    QVERIFY(!decodeAuthoritativeRestoreName(std::string(), nullptr));
}

/**
 * @brief 正当なマルチバイトUTF-8名がバイト単位で保存されることを検証する
 *
 * 往復検証が正当な非ASCII名を誤って拒否しないことの確認
 */
void TestRestoreValidation::authoritativeNamePreservesValidMultibyteUtf8()
{
    QString decoded;

    const QByteArray utf8Name = QStringLiteral("/etc/日本語ファイル.txt").toUtf8();
    const std::string rawName(utf8Name.constData(),
                              static_cast<size_t>(utf8Name.size()));

    QVERIFY(decodeAuthoritativeRestoreName(rawName, &decoded));
    QCOMPARE(decoded, QStringLiteral("/etc/日本語ファイル.txt"));
    QCOMPARE(decoded.toUtf8(), utf8Name);
}

void TestRestoreValidation::authoritativeCollisionIsDetected()
{
    QHash<QString, QString> expectedTypes;

    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/dir"), RestoreStatusContent));

    // 同一raw名で異なる期待型 -> 衝突
    QVERIFY(!registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/dir"), RestoreStatusCreated));

    // 末尾スラッシュの違いは同一キーとして衝突判定される
    QVERIFY(!registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/dir/"), RestoreStatusDeleted));

    // 正規化して初めて同一になるキーも衝突とみなす
    QHash<QString, QString> second;
    QVERIFY(registerAuthoritativeRestoreEntry(
        &second, QStringLiteral("/etc/x"), RestoreStatusContent));
    QVERIFY(!registerAuthoritativeRestoreEntry(
        &second, QStringLiteral("//etc//x"), RestoreStatusType));
}

void TestRestoreValidation::authoritativeRedundantIdenticalEntryIsAccepted()
{
    QHash<QString, QString> expectedTypes;

    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/dir"), RestoreStatusContent));
    // 同一キー・同一期待型の再登録は許す (既存mapの不変性を壊さない)
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/dir"), RestoreStatusContent));
    QCOMPARE(expectedTypes.size(), 1);
}

void TestRestoreValidation::frozenEntriesMatchingAuthoritativeMapPass()
{
    RestoreManifestRegistry registry;
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
        RestoreMode::YastCompatible, &error);
    QVERIFY(!id.isEmpty());
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/etc/added.conf"),
                                   QStringLiteral("/etc/removed.conf"),
                                   QStringLiteral("/etc/changed.conf"),
                                   QStringLiteral("/etc/retyped.conf")},
                                  {QStringLiteral("created"),
                                   QStringLiteral("deleted"),
                                   QStringLiteral("modified"),
                                   QStringLiteral("typechanged")}, &error));
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    QHash<QString, QString> expectedTypes;
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/added.conf"), RestoreStatusCreated));
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/removed.conf"), RestoreStatusDeleted));
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/changed.conf"), RestoreStatusContent));
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/retyped.conf"), RestoreStatusType));

    QVERIFY(validateFrozenEntriesAgainstAuthoritative(
        registry, id, QStringLiteral(":1.1"), expectedTypes, &error));
}

void TestRestoreValidation::unknownPathAndMismatchedTypeAreRejected()
{
    RestoreManifestRegistry registry;
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
        RestoreMode::YastCompatible, &error);
    QVERIFY(!id.isEmpty());
    QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                  {QStringLiteral("/etc/changed.conf")},
                                  {QStringLiteral("modified")}, &error));
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    QHash<QString, QString> expectedTypes;
    QVERIFY(registerAuthoritativeRestoreEntry(
        &expectedTypes, QStringLiteral("/etc/changed.conf"), RestoreStatusContent));

    // 権威ある比較に存在するパスは通る
    QVERIFY(validateFrozenEntriesAgainstAuthoritative(
        registry, id, QStringLiteral(":1.1"), expectedTypes, &error));

    // --- 比較に存在しないパスは1件でも計画全体をrejectする ---
    {
        QHash<QString, QString> withoutUnknown = expectedTypes;
        QVERIFY(registerAuthoritativeRestoreEntry(
            &withoutUnknown, QStringLiteral("/etc/foreign.conf"),
            RestoreStatusContent));

        // /etc/foreign.confを宣言した凍結済み計画を用意する
        RestoreManifestRegistry tamperedRegistry;
        const QString tamperedId = tamperedRegistry.createStaging(
            QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
            RestoreMode::YastCompatible, &error);
        QVERIFY(!tamperedId.isEmpty());
        QVERIFY(tamperedRegistry.stageEntries(
            tamperedId, QStringLiteral(":1.1"),
            {QStringLiteral("/etc/foreign.conf")},
            {QStringLiteral("modified")}, &error));
        QVERIFY(tamperedRegistry.freeze(tamperedId, QStringLiteral(":1.1"), &error));

        QVERIFY(!validateFrozenEntriesAgainstAuthoritative(
            tamperedRegistry, tamperedId, QStringLiteral(":1.1"),
            expectedTypes, &error));
    }

    // --- 期待型の不一致 (modified -> createdの改ざん) もrejectする ---
    {
        RestoreManifestRegistry tamperedRegistry;
        const QString tamperedId = tamperedRegistry.createStaging(
            QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
            RestoreMode::YastCompatible, &error);
        QVERIFY(!tamperedId.isEmpty());
        QVERIFY(tamperedRegistry.stageEntries(
            tamperedId, QStringLiteral(":1.1"),
            {QStringLiteral("/etc/changed.conf")},
            {QStringLiteral("created")}, &error));
        QVERIFY(tamperedRegistry.freeze(tamperedId, QStringLiteral(":1.1"), &error));

        QVERIFY(!validateFrozenEntriesAgainstAuthoritative(
            tamperedRegistry, tamperedId, QStringLiteral(":1.1"),
            expectedTypes, &error));
    }
}

void TestRestoreValidation::validationReadsFrozenEntriesInBoundedChunks()
{
    // 200件 (> 64件のslice上限) の凍結済み計画がchunk分割でも検証される
    RestoreManifestRegistry registry;
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
        RestoreMode::YastCompatible, &error);
    QVERIFY(!id.isEmpty());

    QHash<QString, QString> expectedTypes;
    for (int index = 0; index < 200; ++index) {
        const QString path = QStringLiteral("/data/file-%1").arg(index, 4, 10, QLatin1Char('0'));
        QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                      {path}, {QStringLiteral("modified")}, &error));
        QVERIFY(registerAuthoritativeRestoreEntry(
            &expectedTypes, path, RestoreStatusContent));
    }
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    QVERIFY(validateFrozenEntriesAgainstAuthoritative(
        registry, id, QStringLiteral(":1.1"), expectedTypes, &error));

    // 末尾の1件だけを含むmap (199件分が欠落) では全体としてrejectされる
    // (= 全entryが検証対象であり、末尾chunkの取りこぼしがあっても検出される)
    const auto slice = registry.entriesSlice(id, QStringLiteral(":1.1"), 199, 1, &error);
    QVERIFY(slice.has_value() && slice->size() == 1);
    QHash<QString, QString> shortMap;
    QVERIFY(registerAuthoritativeRestoreEntry(
        &shortMap, slice->first().path, RestoreStatusContent));
    QVERIFY(!validateFrozenEntriesAgainstAuthoritative(
        registry, id, QStringLiteral(":1.1"), shortMap, &error));
}

void TestRestoreValidation::restoreDestinationForRootConfigIsUnchanged()
{
    // root config (SUBVOLUME=/) では従来どおり"/"基準の宛先になる
    QString rootPath;
    QString destination;
    QString relative;
    QVERIFY(buildRestoreDestination(QStringLiteral("/"), QStringLiteral("/etc/fstab"),
                                    &rootPath, &destination, &relative));
    QCOMPARE(rootPath, QStringLiteral("/"));
    QCOMPARE(destination, QStringLiteral("/etc/fstab"));
    QCOMPARE(relative, QStringLiteral("etc/fstab"));

    QVERIFY(buildRestoreDestination(QStringLiteral("/"), QStringLiteral("/a"),
                                    &rootPath, &destination, &relative));
    QCOMPARE(destination, QStringLiteral("/a"));
    QCOMPARE(relative, QStringLiteral("a"));
}

void TestRestoreValidation::restoreDestinationIsBasedOnConfigSubvolume()
{
    // home config ではconfig相対名"/alice/file"の宛先は"/home/alice/file"である
    QString rootPath;
    QString destination;
    QString relative;
    QVERIFY(buildRestoreDestination(QStringLiteral("/home"), QStringLiteral("/alice/file"),
                                    &rootPath, &destination, &relative));
    QCOMPARE(rootPath, QStringLiteral("/home"));
    QCOMPARE(destination, QStringLiteral("/home/alice/file"));
    QCOMPARE(relative, QStringLiteral("alice/file"));

    // SUBVOLUMEと名前の冗長な"/"は正規化される
    QVERIFY(buildRestoreDestination(QStringLiteral("//srv/data/"), QStringLiteral("//x//y/"),
                                    &rootPath, &destination, &relative));
    QCOMPARE(rootPath, QStringLiteral("/srv/data"));
    QCOMPARE(destination, QStringLiteral("/srv/data/x/y"));
    QCOMPARE(relative, QStringLiteral("x/y"));

    QString normalized;
    QVERIFY(normalizeRestoreSubvolume(QStringLiteral("/"), &normalized));
    QCOMPARE(normalized, QStringLiteral("/"));
    QVERIFY(normalizeRestoreSubvolume(QStringLiteral("/home/"), &normalized));
    QCOMPARE(normalized, QStringLiteral("/home"));
}

void TestRestoreValidation::restoreDestinationRejectsUnsafeNames()
{
    QString rootPath;
    QString destination;
    QString relative;
    const QString subvolume = QStringLiteral("/home");

    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("alice/file"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("/"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("/alice/../etc/shadow"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("/alice/./file"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("/alice/fi\nle"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("/.snapshots/1/snapshot"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(subvolume, QStringLiteral("//.snapshots/1"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(QStringLiteral("home"), QStringLiteral("/alice"),
                                     &rootPath, &destination, &relative));
    QVERIFY(!buildRestoreDestination(QStringLiteral("/home/../etc"), QStringLiteral("/alice"),
                                     &rootPath, &destination, &relative));
    QVERIFY(destination.isEmpty());
    QVERIFY(relative.isEmpty());
}

void TestRestoreValidation::snapshotMetadataNameIsDetectedAfterNormalization()
{
    QVERIFY(isSnapshotMetadataRestoreName(QStringLiteral("/.snapshots")));
    QVERIFY(isSnapshotMetadataRestoreName(QStringLiteral("/.snapshots/1/snapshot/etc")));
    QVERIFY(isSnapshotMetadataRestoreName(QStringLiteral("//.snapshots/1")));
    QVERIFY(isSnapshotMetadataRestoreName(QStringLiteral("///.snapshots/")));
    QVERIFY(!isSnapshotMetadataRestoreName(QStringLiteral("/.snapshotsx")));
    QVERIFY(!isSnapshotMetadataRestoreName(QStringLiteral("/home/.snapshots")));
    QVERIFY(!isSnapshotMetadataRestoreName(QStringLiteral("/etc/fstab")));
}

void TestRestoreValidation::rootReadWriteSafetyNetRequiresAllConditions()
{
    RootReadWriteSafetyNetState started;
    started.executionStarted = true;
    started.targetsRootSubvolume = true;
    started.preRestoreStateKnown = true;
    started.preRestoreReadOnly = false;

    // 3条件が揃い、復元後にroになった場合のみrwへ戻す
    QVERIFY(shouldRestoreRootReadWrite(started, true));
    QVERIFY(!shouldRestoreRootReadWrite(started, false));

    // 実行を開始していない計画 (Begin -> Cancel等) では何もしない
    RootReadWriteSafetyNetState notStarted = started;
    notStarted.executionStarted = false;
    QVERIFY(!shouldRestoreRootReadWrite(notStarted, true));
    QVERIFY(!shouldRestoreRootReadWrite(RootReadWriteSafetyNetState{}, true));

    // 非root config
    RootReadWriteSafetyNetState nonRoot = started;
    nonRoot.targetsRootSubvolume = false;
    QVERIFY(!shouldRestoreRootReadWrite(nonRoot, true));

    // 元からroだったroot (読み取り専用スナップショットからの起動等)
    RootReadWriteSafetyNetState wasReadOnly = started;
    wasReadOnly.preRestoreReadOnly = true;
    QVERIFY(!shouldRestoreRootReadWrite(wasReadOnly, true));

    // 復元前の状態を記録できなかった場合
    RootReadWriteSafetyNetState unknown = started;
    unknown.preRestoreStateKnown = false;
    QVERIFY(!shouldRestoreRootReadWrite(unknown, true));
}

QTEST_MAIN(TestRestoreValidation)
#include "tst_restorevalidation.moc"
