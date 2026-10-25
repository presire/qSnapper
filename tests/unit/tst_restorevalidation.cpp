/**
 * @file tst_restorevalidation.cpp
 * @brief commit時preflightの純粋関数群の単体テスト
 *
 * 対象は復元元と比較相手の観測結果による差分の裏付けと、凍結済みentryの検証、復元先の組み立てである
 * ファイルシステムの代わりに観測結果を返す関数を与え、libsnapperの比較結果に依存しない検証の規則を確認する
 */

#include "restorevalidation.h"

#include <QHash>
#include <QTest>

#include <sys/stat.h>

using namespace qsnapper::restore;

namespace {

/**
 * @brief 実在するパスの観測結果を作る
 * @param fileType S_IFREGなどの種別ビット
 * @param inode inode番号
 * @param mtimeSec 更新時刻 (秒)
 * @return 実在を表す観測結果
 */
RestorePathObservation presentObservation(quint32 fileType, quint64 inode, qint64 mtimeSec)
{
    RestorePathObservation observation;
    observation.presence = RestorePathPresence::Present;
    observation.fileType = fileType;
    observation.mode = 0644;
    observation.inode = inode;
    observation.uid = 0;
    observation.gid = 0;
    observation.size = 10;
    observation.mtimeSec = mtimeSec;
    observation.ctimeSec = mtimeSec;
    return observation;
}

/**
 * @brief 存在しないパスの観測結果を作る
 * @return 不在を表す観測結果
 */
RestorePathObservation absentObservation()
{
    RestorePathObservation observation;
    observation.presence = RestorePathPresence::Absent;
    return observation;
}

/**
 * @brief 相対パスと観測結果の表から、観測関数を作る
 * @param tree 相対パスを鍵とする観測結果の表 (表に無いパスは不在とする)
 * @param calls 呼び出された相対パスの記録先 (省略可)
 * @return 観測関数
 */
RestorePathObserver observerFor(const QHash<QString, RestorePathObservation> &tree,
                                QStringList *calls = nullptr)
{
    return [tree, calls](const QString &relativePath) {
        if (calls) {
            calls->append(relativePath);
        }
        return tree.value(relativePath, absentObservation());
    };
}

/**
 * @brief 指定entryを凍結した計画を作る
 * @param registry 計画を保持するregistry
 * @param paths 凍結するpath列
 * @param changeTypes pathsと対応する変更種別列
 * @return 作成した計画のID (失敗時は空文字列)
 */
QString frozenPlan(RestoreManifestRegistry &registry,
                   const QStringList &paths,
                   const QStringList &changeTypes)
{
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
        RestoreMode::YastCompatible, &error);
    if (id.isEmpty()
            || !registry.stageEntries(id, QStringLiteral(":1.1"), paths, changeTypes, &error)
            || !registry.freeze(id, QStringLiteral(":1.1"), &error)) {
        return {};
    }
    return id;
}

} // namespace

/**
 * @class TestRestoreValidation
 * @brief commit時preflightの純粋関数群を確認するテストスイート
 *
 * 観測結果による差分の裏付け、凍結済みentryの検証、復元先の組み立てを確認する
 */
class TestRestoreValidation : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 正規の各変更種別が、実際の差分に一致する場合に通ることを確認する */
    void legitimateChangeTypesAreBackedByTrees();
    /** @brief 実際の差分と食い違う変更種別が拒否されることを確認する */
    void contradictingChangeTypesAreRejected();
    /** @brief 両方の木で同一のパスはmodifiedとして拒否されることを確認する */
    void unchangedPathIsNotAcceptedAsModified();
    /** @brief 存在を確定できない観測結果と未知の変更種別が拒否されることを確認する */
    void unknownObservationAndChangeTypeAreRejected();
    /** @brief statの値が観測結果へ写されることを確認する */
    void observationFromStatCopiesIdentityFields();
    /** @brief 正規の凍結済み計画が、木の観測で検証を通ることを確認する */
    void frozenEntriesBackedByTreesPass();
    /** @brief 改行を含む名前から生まれた偽entryを模した計画が拒否されることを確認する */
    void forgedEntriesFromPoisonedComparisonAreRejected();
    /** @brief 復元先と同じ規則で組み立てた相対パスを観測することを確認する */
    void observationUsesConfigRelativePath();
    /** @brief 凍結済みentryが上限付きチャンクで漏れなく検証されることを確認する */
    void validationReadsFrozenEntriesInBoundedChunks();
    /** @brief root configの復元先が従来どおりであることを確認する */
    void restoreDestinationForRootConfigIsUnchanged();
    /** @brief 非root configの復元先がSUBVOLUME基準になることを確認する */
    void restoreDestinationIsBasedOnConfigSubvolume();
    /** @brief 安全でない名前と".snapshots"配下の名前が拒否されることを確認する */
    void restoreDestinationRejectsUnsafeNames();
    /** @brief ".snapshots"配下の判定が正規化後の先頭成分で行われることを確認する */
    void snapshotMetadataNameIsDetectedAfterNormalization();
    /** @brief rootサブボリュームのrw安全ネットが全条件を要求することを確認する */
    void rootReadWriteSafetyNetRequiresAllConditions();
};

/**
 * @brief 正規の各変更種別が、実際の差分に一致する場合に通ることを確認する
 */
void TestRestoreValidation::legitimateChangeTypesAreBackedByTrees()
{
    const RestorePathObservation file = presentObservation(S_IFREG, 10, 100);
    const RestorePathObservation changedFile = presentObservation(S_IFREG, 11, 200);
    const RestorePathObservation directory = presentObservation(S_IFDIR, 12, 100);

    // createdは比較相手だけに存在し、復元で削除される
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("created"), absentObservation(), file));
    // deletedは復元元だけに存在し、復元で作り直される
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("deleted"), file, absentObservation()));
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, changedFile));
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("typechanged"), file, directory));
}

/**
 * @brief 実際の差分と食い違う変更種別が拒否されることを確認する
 */
void TestRestoreValidation::contradictingChangeTypesAreRejected()
{
    const RestorePathObservation file = presentObservation(S_IFREG, 10, 100);
    const RestorePathObservation changedFile = presentObservation(S_IFREG, 11, 200);
    const RestorePathObservation directory = presentObservation(S_IFDIR, 12, 100);

    // 復元元に存在するパスをcreatedと偽ると、復元元の実データがあるパスを削除できてしまう
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("created"), file, changedFile));
    // 両方の木に無いパスは、復元で削除する対象にも作り直す対象にもならない
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("created"), absentObservation(), absentObservation()));
    // 比較相手に存在するパスをdeletedと偽ると、差分に無いパスを上書きできてしまう
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("deleted"), file, changedFile));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("deleted"), absentObservation(), file));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, absentObservation()));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("modified"), absentObservation(), file));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, directory));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("typechanged"), file, changedFile));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("typechanged"), file, absentObservation()));
}

/**
 * @brief 両方の木で同一のパスはmodifiedとして拒否されることを確認する
 *
 * Btrfsのスナップショットでは、変更されていないファイルはinode番号・時刻を含めて同一に見える
 */
void TestRestoreValidation::unchangedPathIsNotAcceptedAsModified()
{
    const RestorePathObservation file = presentObservation(S_IFREG, 10, 100);
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, file));

    // 1項目でも異なれば変更として扱う
    RestorePathObservation chmodded = file;
    chmodded.mode = 0600;
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, chmodded));

    RestorePathObservation touched = file;
    touched.ctimeNsec = 1;
    QVERIFY(isRestoreEntryBackedByTrees(QStringLiteral("modified"), file, touched));
}

/**
 * @brief 存在を確定できない観測結果と未知の変更種別が拒否されることを確認する
 */
void TestRestoreValidation::unknownObservationAndChangeTypeAreRejected()
{
    RestorePathObservation unknown;
    unknown.presence = RestorePathPresence::Unknown;
    const RestorePathObservation file = presentObservation(S_IFREG, 10, 100);

    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("created"), unknown, file));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("deleted"), file, unknown));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("modified"), unknown, unknown));
    QVERIFY(!isRestoreEntryBackedByTrees(QStringLiteral("renamed"), file, absentObservation()));
    QVERIFY(!isRestoreEntryBackedByTrees(QString(), absentObservation(), file));
}

/**
 * @brief statの値が観測結果へ写されることを確認する
 */
void TestRestoreValidation::observationFromStatCopiesIdentityFields()
{
    struct stat info = {};
    info.st_mode = S_IFLNK | 0777;
    info.st_ino = 42;
    info.st_uid = 1000;
    info.st_gid = 100;
    info.st_size = 7;
    info.st_mtim.tv_sec = 5;
    info.st_mtim.tv_nsec = 6;
    info.st_ctim.tv_sec = 7;
    info.st_ctim.tv_nsec = 8;

    const RestorePathObservation observation = restorePathObservationFromStat(info);
    QCOMPARE(observation.presence, RestorePathPresence::Present);
    QCOMPARE(observation.fileType, quint32(S_IFLNK));
    QCOMPARE(observation.mode, quint32(0777));
    QCOMPARE(observation.inode, quint64(42));
    QCOMPARE(observation.uid, quint32(1000));
    QCOMPARE(observation.gid, quint32(100));
    QCOMPARE(observation.size, qint64(7));
    QCOMPARE(observation.mtimeSec, qint64(5));
    QCOMPARE(observation.mtimeNsec, qint64(6));
    QCOMPARE(observation.ctimeSec, qint64(7));
    QCOMPARE(observation.ctimeNsec, qint64(8));
}

/**
 * @brief 正規の凍結済み計画が、木の観測で検証を通ることを確認する
 */
void TestRestoreValidation::frozenEntriesBackedByTreesPass()
{
    RestoreManifestRegistry registry;
    const QString id = frozenPlan(registry,
                                  {QStringLiteral("/etc/added.conf"),
                                   QStringLiteral("/etc/removed.conf"),
                                   QStringLiteral("/etc/changed.conf"),
                                   QStringLiteral("/etc/retyped.conf")},
                                  {QStringLiteral("created"),
                                   QStringLiteral("deleted"),
                                   QStringLiteral("modified"),
                                   QStringLiteral("typechanged")});
    QVERIFY(!id.isEmpty());

    const QHash<QString, RestorePathObservation> source{
        {QStringLiteral("etc/removed.conf"), presentObservation(S_IFREG, 1, 100)},
        {QStringLiteral("etc/changed.conf"), presentObservation(S_IFREG, 2, 100)},
        {QStringLiteral("etc/retyped.conf"), presentObservation(S_IFREG, 3, 100)}
    };
    const QHash<QString, RestorePathObservation> counterpart{
        {QStringLiteral("etc/added.conf"), presentObservation(S_IFREG, 4, 200)},
        {QStringLiteral("etc/changed.conf"), presentObservation(S_IFREG, 5, 200)},
        {QStringLiteral("etc/retyped.conf"), presentObservation(S_IFLNK, 6, 200)}
    };

    // 成功経路がNoneを書き戻したことを確かめられるよう、失敗値を初期値にして未設定を検出する
    ManifestError error = ManifestError::InvalidArgument;
    QVERIFY(validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                              QStringLiteral("/"),
                                              observerFor(source), observerFor(counterpart),
                                              &error));
    QCOMPARE(error, ManifestError::None);
}

/**
 * @brief 改行を含む名前から生まれた偽entryを模した計画が拒否されることを確認する
 *
 * 攻撃者が"/tmp/x\n+..... /etc/passwd"のような名前を作ると、libsnapperのfilelistキャッシュからは
 * "/etc/passwd"のcreatedが読める
 * 管理者がその比較結果をそのまま復元すると、復元元に存在するパスが削除される
 * 実際の木では/etc/passwdは両方に同一で存在するため、どの変更種別でも拒否されなければならない
 */
void TestRestoreValidation::forgedEntriesFromPoisonedComparisonAreRejected()
{
    const QHash<QString, RestorePathObservation> source{
        {QStringLiteral("etc/passwd"), presentObservation(S_IFREG, 1, 100)}
    };
    const QHash<QString, RestorePathObservation> counterpart = source;

    for (const QString &changeType : {QStringLiteral("created"), QStringLiteral("deleted"),
                                      QStringLiteral("modified"), QStringLiteral("typechanged")}) {
        RestoreManifestRegistry registry;
        const QString id = frozenPlan(registry,
                                      {QStringLiteral("/etc/changed.conf"), QStringLiteral("/etc/passwd")},
                                      {QStringLiteral("deleted"), changeType});
        QVERIFY(!id.isEmpty());

        // 1件目は正当なdeletedとして通るようにし、偽の2件目だけで計画全体が拒否されることを確かめる
        QHash<QString, RestorePathObservation> sourceWithLegit = source;
        sourceWithLegit.insert(QStringLiteral("etc/changed.conf"), presentObservation(S_IFREG, 2, 100));

        ManifestError error = ManifestError::None;
        QVERIFY2(!validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                                    QStringLiteral("/"),
                                                    observerFor(sourceWithLegit),
                                                    observerFor(counterpart), &error),
                 qPrintable(changeType));
        QCOMPARE(error, ManifestError::InvalidArgument);
    }

    // 比較相手にだけ存在する偽のパスも、deletedとしては受理しない
    RestoreManifestRegistry registry;
    const QString id = frozenPlan(registry, {QStringLiteral("/root/.ssh/authorized_keys")},
                                  {QStringLiteral("deleted")});
    QVERIFY(!id.isEmpty());
    const QHash<QString, RestorePathObservation> liveOnly{
        {QStringLiteral("root/.ssh/authorized_keys"), presentObservation(S_IFREG, 9, 100)}
    };
    ManifestError error = ManifestError::None;
    QVERIFY(!validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                               QStringLiteral("/"),
                                               observerFor({}), observerFor(liveOnly), &error));
}

/**
 * @brief 復元先と同じ規則で組み立てた相対パスを観測することを確認する
 *
 * 非root configでも、計画に載る名前はconfig相対名であり、両方の木の根からの相対で観測する
 * 空成分は畳み、実行段のapplyRestoreEntryと同じパスを検証する
 */
void TestRestoreValidation::observationUsesConfigRelativePath()
{
    RestoreManifestRegistry registry;
    const QString id = frozenPlan(registry, {QStringLiteral("//alice//notes.txt")},
                                  {QStringLiteral("created")});
    QVERIFY(!id.isEmpty());

    QStringList sourceCalls;
    QStringList counterpartCalls;
    const QHash<QString, RestorePathObservation> counterpart{
        {QStringLiteral("alice/notes.txt"), presentObservation(S_IFREG, 4, 200)}
    };
    ManifestError error = ManifestError::None;
    QVERIFY(validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                              QStringLiteral("/home"),
                                              observerFor({}, &sourceCalls),
                                              observerFor(counterpart, &counterpartCalls),
                                              &error));
    QCOMPARE(sourceCalls, QStringList{QStringLiteral("alice/notes.txt")});
    QCOMPARE(counterpartCalls, QStringList{QStringLiteral("alice/notes.txt")});

    // 観測関数が無い場合は検証できないため拒否する
    QVERIFY(!validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                               QStringLiteral("/home"),
                                               RestorePathObserver(), observerFor(counterpart),
                                               &error));
    QCOMPARE(error, ManifestError::InvalidArgument);
}

/**
 * @brief 凍結済みentryが上限付きチャンクで漏れなく検証されることを確認する
 *
 * 200件 (> 64件のslice上限) を与え、末尾1件だけが差分に裏付けられない場合も全体が拒否されることを確認する
 */
void TestRestoreValidation::validationReadsFrozenEntriesInBoundedChunks()
{
    RestoreManifestRegistry registry;
    ManifestError error = ManifestError::None;
    const QString id = registry.createStaging(
        QStringLiteral(":1.1"), 1000, QStringLiteral("root"), 100, 101,
        RestoreMode::YastCompatible, &error);
    QVERIFY(!id.isEmpty());

    QHash<QString, RestorePathObservation> source;
    QHash<QString, RestorePathObservation> counterpart;
    for (int index = 0; index < 200; ++index) {
        const QString relative = QStringLiteral("data/file-%1").arg(index, 4, 10, QLatin1Char('0'));
        QVERIFY(registry.stageEntries(id, QStringLiteral(":1.1"),
                                      {QLatin1Char('/') + relative},
                                      {QStringLiteral("modified")}, &error));
        source.insert(relative, presentObservation(S_IFREG, 1000 + index, 100));
        counterpart.insert(relative, presentObservation(S_IFREG, 5000 + index, 200));
    }
    QVERIFY(registry.freeze(id, QStringLiteral(":1.1"), &error));

    QStringList calls;
    QVERIFY(validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                              QStringLiteral("/"),
                                              observerFor(source, &calls),
                                              observerFor(counterpart), &error));
    QCOMPARE(calls.size(), 200);

    // 末尾のentryだけを両方の木で同一にすると、末尾のchunkまで検証されていれば全体が拒否される
    counterpart.insert(QStringLiteral("data/file-0199"),
                       source.value(QStringLiteral("data/file-0199")));
    QVERIFY(!validateFrozenEntriesAgainstTrees(registry, id, QStringLiteral(":1.1"),
                                               QStringLiteral("/"),
                                               observerFor(source), observerFor(counterpart),
                                               &error));
}

/**
 * @brief root configの復元先が従来どおりであることを確認する
 */
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

/**
 * @brief 復元先がconfigのSUBVOLUME基準で決まることを確認する
 */
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

/**
 * @brief 復元先の構築が安全でない名前を拒否することを確認する
 *
 * 相対名、root、親参照、カレント参照、改行、スナップショットメタデータ
 * 不正なSUBVOLUMEを確認する
 */
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

/**
 * @brief 正規化後にスナップショットメタデータ名が検出されることを確認する
 */
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

/**
 * @brief root復元後のrw復帰セーフティネットが全条件を要求することを確認する
 *
 * 実行開始済み、root subvolume対象、復元前状態が既知の3条件が揃い、
 * 復元後にroになった場合のみrwへ戻すことを確認する
 */
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
