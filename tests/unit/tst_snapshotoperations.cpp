/**
 * @file tst_snapshotoperations.cpp
 * @brief SnapshotOperationsの復元計画とエントリ適用の特権境界を検証するテスト
 *
 * SnapshotOperationsのスロットをD-Busを介さずに直接呼ぶ (呼び出し元ownerは空文字列になる)
 * rootサブボリュームへの操作はテスト用の差し替え関数で観測し、実システムには触れない
 * 復元計画の終了状態、config操作との排他、エントリ適用の安全性、認可と例外の境界を確認する
 *
 * 実行方法 : QTEST_GUILESS_MAINから実行する単体テスト
 * 依存 : Qt Test、SnapshotOperations、RestoreManifestRegistry、snapshotoperations_internal、filesystemhelpers
 */

#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <grp.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/fsuid.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include "filesystemhelpers.h"
#include "snapshotoperations.h"
#include "restoremanifestregistry.h"
#include "snapshotoperations_internal.h"

/**
 * @class TestSnapshotOperations
 * @brief SnapshotOperationsの復元計画とエントリ適用の特権境界を検証するテスト
 *
 * D-Busを介さずにスロットを直接呼ぶ (呼び出し元ownerは空文字列になる)
 * rootサブボリュームへの操作はテスト用の差し替え関数で観測し、実システムには触れない
 * private slotsの宣言順はQTestの実行順になるため、並べ替えない
 *
 * m_readOnlyProbeCalls : rootの読み取り専用状態を確認した回数
 * m_readWriteRestoreCalls : rootをrwへ戻した回数
 */
class TestSnapshotOperations : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 各テストの前に、root操作の観測回数を0へ戻す */
    void init();
    /** @brief 各テストの終了時に、metadata適用のテスト用フックを解除する */
    void cleanup();

    /** @brief 計画の作成と取り消しだけではrootサブボリュームに触れないことを検証する */
    void beginThenCancelDoesNotTouchRootSubvolume();

    // --- config操作を止める復元計画の判定 ---
    /** @brief 作成中(Staging)の計画だけではconfig操作が止められないことを検証する */
    void stagingPlanDoesNotBlockConfigOperations();
    /** @brief config操作の直前に対象の計画だけを取り消し、他のconfigの計画は残すことを検証する */
    void configOperationsDiscardStagingPlansOfConfig();
    /** @brief 認可を得られないBeginRestorePlanが計画を作らず予算を消費しないことを検証する */
    void beginRestorePlanRequiresAuthorization();
    /** @brief 同じUIDの計画が上限に達したとき最も古いStaging計画だけを破棄することを検証する */
    void beginRestorePlanEvictsOldestStagingPlanOfSameUid();
    /** @brief 実行中の復元があるconfigだけconfig操作が止められることを検証する */
    void runningRestoreBlocksConfigOperations();
    /** @brief 同じ復元先を指すパスを含むchunkが原子的に拒否されることを検証する */
    void stageRestoreEntriesRejectsDuplicateCanonicalPaths();
    /** @brief 期限切れの計画がGetRestorePlanStatusでowner表から掃除され、有効な計画は残ることを検証する */
    void expiredPlanIsRemovedFromOwnerTableOnStatus();

    // --- 差分生成の資源上限 ---
    /** @brief 小さな差分では、従来どおりunified形式の差分を返すことを検証する */
    void diffStillProducesUnifiedOutputForSmallChanges();
    /** @brief 片方がバイナリの場合に、内容を差分へ載せずbinaryの理由で省略することを検証する */
    void diffOmitsBinaryFiles();
    /** @brief ファイルサイズの既定の上限を超える場合にtoo_largeの理由で省略することを検証する */
    void diffOmitsTooLargeFiles();
    /** @brief 行数の上限を超える場合にtoo_largeの理由で省略することを検証する */
    void diffOmitsFilesWithTooManyLines();
    /** @brief FIFOを開かずにspecial_fileの理由で省略することを検証する */
    void diffDoesNotOpenFifo();
    /** @brief 作業領域と探索ステップ数の上限を超える差分をtoo_many_changesの理由で省略することを検証する */
    void diffOmitsTooManyChanges();
    /** @brief 編集距離が最大になる入力でも、既定の上限で打ち切って短時間で戻ることを検証する */
    void diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput();

    // --- 書き込み可能な復元元の拒否 ---
    /** @brief btrfs以外の書き込み可能なディレクトリを復元元として受け入れないことを検証する */
    void writableDirectoryIsNotReadOnlyRestoreSource();
    /** @brief btrfsでは読み取り専用フラグの立ったsubvolumeだけを受け入れることを検証する */
    void btrfsSubvolumeReadOnlyFlagIsRequired();
    /** @brief btrfs以外では読み取り専用でmountされたファイルシステムを受け入れることを検証する */
    void readOnlyMountIsAcceptedForNonBtrfs();

    // --- 認可待ちの上限 ---
    void pendingAuthorizationBudgetLimitsEachUid();
    void pendingAuthorizationBudgetKeepsGlobalLimit();
    void pendingAuthorizationBudgetReleasesSlots();

    // --- エントリの適用と終了処理 ---
    /** @brief FIFOなどの特殊ファイルを、live側に触れずスキップとして扱うことを検証する */
    void specialFileSourceIsSkippedWithoutTouchingLive();
    /** @brief ディレクトリとファイルの間の種別変更を、YaST方式でも復元できることを検証する */
    void typechangedReplacesDirectoryAndFileInYastMode();
    /** @brief 既存のディレクトリと新規に作成するディレクトリの両方で、modeを適用する直前に継承ACLが削除されていることを検証する */
    void applyRestoreEntrySyncsDirectoryAclBeforeModeRestore();
    /** @brief 退避前と退避後の他者所有ディレクトリを対比するケースを提供する */
    void typechangedGuardsForeignDirectories_data();
    /** @brief 他者の木の退避を拒否し、退避後に削除を拒否した場合も失敗を報告する */
    void typechangedGuardsForeignDirectories();
    /** @brief 子エントリの適用で変わるディレクトリの時刻を、計画の終了時に復元することを検証する */
    void directoryTimesAreRestoredAfterChildEntries();
    /** @brief 復元先の同名symlinkの先へmetadataを適用しないことを検証する */
    void existingSymlinkIsNotUsedAsDirectory();
    /** @brief 他ユーザが書き換えられる親の下では、入れ替えられた可能性のある
     *        ディレクトリへmetadataを適用しないことを検証する
     */
    void untrustedParentRefusesMetadataOnUnprovenDirectories();
    /** @brief 共有親の拒否とprincipal所有親の正常復元を対比する */
    void untrustedParentRefusesMetadataOnUnprovenDirectories_data();
    /** @brief 所有権境界と復元後のfilesystem identityを検証するケースを提供する */
    void directoryMetadataFollowsChainOwnership_data();
    /** @brief 他者所有の祖先や対象を通じたroot権限の増幅を拒否する */
    void directoryMetadataFollowsChainOwnership();
    /** @brief rootをrwへ戻せなかった計画は、全エントリ適用済みでもFailedで終わることを検証する */
    void rootLeftReadOnlyMarksPlanFailed();
    /** @brief 実行中にownerが消えてもrootのrw安全網を実行することを検証する */
    void vanishedOwnerStillRunsRootSafetyNet();
    /** @brief 退避先の名前が推測できない乱数の接尾辞を持つことを検証する */
    void siblingTemporaryPathIsUnpredictable();
    /** @brief 同じ所有者のディレクトリを入れ替える攻撃と、拒否してはならない正常系を行ごとに提供する */
    void sameOwnerDirectorySwapIsRefused_data();
    /** @brief 所有者以外の利用者が名前を変更できるディレクトリの下では、rootが復元を拒否することを検証する */
    void sameOwnerDirectorySwapIsRefused();
    /** @brief 退避の後に元の名前へ戻すことも拒否された場合に、退避物を残して退避名をログに記録することを検証する */
    void refusedReattachKeepsDetachedEntry();

    // --- 例外と入力の検査 ---
    /** @brief 認可済みの本体の想定外の例外がスロットの外へ出ないことを検証する */
    void unexpectedExceptionsDoNotEscapeAuthorizedBody();
    /** @brief 未知のcleanupアルゴリズムを認可の前に拒否することを検証する */
    void modifySnapshotRejectsUnknownCleanupBeforeAuthorization();

private:
    /**
     * @brief rootサブボリュームへの操作を観測する差し替え関数を設定する
     * @param operations 対象のSnapshotOperations
     */
    void installRootSubvolumeSpies(SnapshotOperations &operations);

    int m_readOnlyProbeCalls = 0;      // rootの読み取り専用状態を確認した回数
    int m_readWriteRestoreCalls = 0;   // rootをrwへ戻した回数
};

namespace {

    /**
     * @brief テスト用ファイルを作成する
     * @param path 作成先
     * @param content 内容
     * @return 作成できた場合true
     */
    bool writeFile(const QString &path, const QByteArray &content)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && file.write(content) == content.size();
    }

    /**
     * @brief 指定した接頭辞と連番で行を作る
     * @param prefix 行の接頭辞
     * @param count 行数
     * @return 改行区切りの内容
     */
    QByteArray numberedLines(const QByteArray &prefix, int count)
    {
        QByteArray content;
        for (int i = 0; i < count; ++i) {
            content += prefix + QByteArray::number(i) + '\n';
        }
        return content;
    }

    /**
     * @brief テスト用ファイルの内容を読む
     * @param path 読み取るファイル
     * @return 内容 (読めない場合は空)
     */
    QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    /**
     * @brief 復元元と復元先の2つのディレクトリを持つ作業領域
     *
     * 復元元はスナップショットの代わり、復元先はconfigのSUBVOLUMEの代わりとして使う
     */
    struct ApplyFixture {
        QTemporaryDir root;     // 作業領域全体 (テスト終了時に削除する)
        QString sourcePath;     // 復元元ディレクトリ
        QString livePath;       // 復元先ディレクトリ

        /**
         * @brief 復元元と復元先のディレクトリを作る
         * @return 作成できた場合true
         */
        bool setUp()
        {
            if (!root.isValid()) {
                return false;
            }
            sourcePath = root.filePath(QStringLiteral("source"));
            livePath = root.filePath(QStringLiteral("live"));
            return QDir().mkpath(sourcePath) && QDir().mkpath(livePath);
        }

        /**
         * @brief 復元元を開いて、mountせずに実行状態を作る
         * @param operations 対象のSnapshotOperations
         * @param manifestId 実行状態のID
         * @param useReflink ダイレクトコピー方式として扱う場合true
         * @return 作成できた場合true
         */
        bool begin(SnapshotOperations &operations, const QString &manifestId, bool useReflink)
        {
            const int sourceFd = ::open(QFile::encodeName(sourcePath).constData(),
                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sourceFd < 0) {
                return false;
            }
            operations.beginRestoreApplyForTesting(manifestId, sourcePath, sourceFd, livePath, useReflink,
                                                   qsnapper::restore::RootReadWriteSafetyNetState{});
            return true;
        }
    };

    /**
     * @brief ディレクトリにqsnapperの一時退避物が残っていないか調べる
     * @param directoryPath 調べるディレクトリ
     * @return 退避物や一時ファイルが無い場合true
     */
    bool hasNoQsnapperLeftovers(const QString &directoryPath)
    {
        const QStringList entries = QDir(directoryPath).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                                  | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (entry.contains(QStringLiteral("qsnapper-"))) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 名前付きユーザへ読み取りと検索を許すdefault ACL(アクセス制御リスト)のxattr(拡張属性)の値を作る
     *
     * 復元先の下にあるディレクトリが、名前付きユーザのエントリを継承する状況を再現するために使う
     * maskをr-xにして、ACLを継承したディレクトリのgroupのビットに書き込みが含まれないようにする
     *
     * @param namedUser 名前付きユーザのUID (ACLのエントリとして使うだけなので、実在しなくてよい)
     * @return system.posix_acl_defaultへ設定する値 (先頭4バイトのヘッダの後に、8バイトのエントリが並ぶ形式)
     */
    QByteArray namedUserDefaultAcl(quint32 namedUser)
    {
        /**
         * @brief ACLの1エントリ
         */
        struct AclEntry {
            quint16 tag;   // エントリの種類
            quint16 perm;  // 権限ビット (0から7)
            quint32 id;    // 名前付きユーザのUID (それ以外のエントリは未定義を表す0xffffffff)
        };

        const AclEntry entries[] = {
            {0x01, 7, 0xffffffffU},  // 所有者 (user_obj)
            {0x02, 5, namedUser},    // 名前付きユーザ (user)
            {0x04, 5, 0xffffffffU},  // 所有グループ (group_obj)
            {0x10, 5, 0xffffffffU},  // 名前付きエントリとgroupに許す権限の上限 (mask)
            {0x20, 0, 0xffffffffU},  // その他のユーザ (other)
        };

        // xattrの値は、各フィールドをlittle endianで並べる
        QByteArray value;
        const auto appendLittleEndian = [&value](quint32 number, int byteCount) {
            for (int index = 0; index < byteCount; ++index) {
                value.append(static_cast<char>((number >> (8 * index)) & 0xff));
            }
        };

        // ヘッダはACLの形式のversionで、Linuxのxattrでは2を使う
        appendLittleEndian(2, 4);
        for (const AclEntry &entry : entries) {
            appendLittleEndian(entry.tag, 2);
            appendLittleEndian(entry.perm, 2);
            appendLittleEndian(entry.id, 4);
        }
        return value;
    }

    /**
     * @brief パスのxattrが存在するかを調べる
     * @param path 対象パス (シンボリックリンクはたどらない)
     * @param name xattr名
     * @return 1: 値あり、0: 属性なし、-1: 読み出しエラー
     */
    int pathXattrState(const QString &path, const char *name)
    {
        if (::lgetxattr(QFile::encodeName(path).constData(), name, nullptr, 0) >= 0) {
            return 1;
        }
        return errno == ENODATA ? 0 : -1;
    }

    /**
     * @brief 復元元のmodeを適用する直前に観測した、ディレクトリのmodeとACLの有無
     *
     * modeを広げる前にACLの同期が終わっているかを、テスト本体で判定するために記録する
     */
    struct DirectoryAclObservation {
        mode_t mode = 0;        // 観測時点のmode (fstatに失敗した場合は全ビットを立てた値)
        int accessState = -1;   // access ACLの有無 (1: あり、0: なし、-1: 読み出し失敗)
        int defaultState = -1;  // default ACLの有無 (1: あり、0: なし、-1: 読み出し失敗)
    };

    /**
     * @brief BeforeModeRestoreの通知を受けるたびに、復元先のディレクトリのmodeとACLの有無を記録する
     *
     * 製品コードの処理の途中から呼ばれるため、QTestのマクロで判定して処理を中断せず、記録だけを行う
     *
     * @param stage 通知された段階
     * @param destinationFd 復元先のディレクトリのfd (状態を読み取るだけで、閉じない)
     * @param context 記録先のQList<DirectoryAclObservation>へのポインタ
     * @note contextが指すリストへ、観測結果を1件追加する
     */
    void recordDirectoryAclObservation(qsnapper::security::detail::MetadataStage stage, int destinationFd,
                                       void *context)
    {
        if (stage != qsnapper::security::detail::MetadataStage::BeforeModeRestore) {
            return;
        }

        const auto xattrState = [destinationFd](const char *name) {
            if (::fgetxattr(destinationFd, name, nullptr, 0) >= 0) {
                return 1;
            }
            return errno == ENODATA ? 0 : -1;
        };

        DirectoryAclObservation observation;
        struct stat destinationStat;
        observation.mode = ::fstat(destinationFd, &destinationStat) == 0 ? (destinationStat.st_mode & 07777)
                                                                          : mode_t(~0);
        observation.accessState = xattrState("system.posix_acl_access");
        observation.defaultState = xattrState("system.posix_acl_default");
        static_cast<QList<DirectoryAclObservation> *>(context)->append(observation);
    }

    /**
     * @brief 観測結果を、テストの失敗メッセージに表示する文字列へ変換する
     * @param observation 観測結果
     * @return modeを8進数で表し、ACLの有無を並べた文字列
     */
    QByteArray describeDirectoryAclObservation(const DirectoryAclObservation &observation)
    {
        return QStringLiteral("before fchmod: mode=%1 access=%2 default=%3")
                .arg(static_cast<uint>(observation.mode), 4, 8, QLatin1Char('0'))
                .arg(observation.accessState)
                .arg(observation.defaultState)
                .toUtf8();
    }

    // --- グループの他の利用者が、同じ所有者のディレクトリを入れ替える攻撃の補助 ---
    // 復元先の所有者(利用者A)と、共有グループを通じて同じディレクトリへ書き込める利用者Bを用意する
    // Bは自分では読めないAの非公開ディレクトリを、rootが復元する名前の位置へ入れ替えようとする

    constexpr uid_t DIRECTORY_OWNER_UID = 1000;                 // 復元先のディレクトリを所有する利用者AのUID
    constexpr gid_t DIRECTORY_OWNER_GID = 1000;                 // A専用のグループ(Aだけが属するグループ)のGID
    constexpr uid_t GROUP_MEMBER_UID = 2000;                    // 共有グループを通じて書き込める利用者BのUID
    constexpr gid_t SHARED_GROUP_GID = 3000;                    // AとBが共に属する共有グループのGID

    bool privateGroupCheckResult = true;                        // A専用のグループかの判定で、AとA専用のグループの組へ返す値
    QList<QPair<int, int>> privateGroupCheckIdentities;         // 判定が呼ばれた時点のfsuidとfsgidの記録

    /**
     * @brief isUserPrivateGroupの代わりに呼ばれ、所有者専用のグループかの判定を差し替えて、呼ばれた時点の識別子を記録する
     * @param owner ディレクトリの所有者のUID
     * @param group ディレクトリのグループのGID
     * @return privateGroupCheckResultが真で、AとA専用のグループの組の場合だけtrue
     * @note テストを実行するホストのpasswdとgroupの登録に結果を左右されないように、判定の結果をテストの行ごとに固定する
     *       NSS(Name Service Switch、利用者やグループの名前解決)の問い合わせが、利用者の権限へ切り替えた状態
     *       (ScopedFsIdentityの中)で行われていないことを確かめるため、fsuidとfsgidを記録する
     */
    bool recordingPrivateGroupCheck(uid_t owner, gid_t group)
    {
        const int fsuid = ::setfsuid(static_cast<uid_t>(-1));
        const int fsgid = ::setfsgid(static_cast<gid_t>(-1));
        privateGroupCheckIdentities.append(qMakePair(fsuid, fsgid));
        return privateGroupCheckResult && owner == DIRECTORY_OWNER_UID && group == DIRECTORY_OWNER_GID;
    }

    QString detachWatchedDirectory;                             // 退避名が現れたかを調べるディレクトリ

    /**
     * @brief 退避名が現れるまではA専用のグループと答え、現れた後はそうでないと答える判定の代わり
     * @param owner ディレクトリの所有者のUID
     * @param group ディレクトリのグループのGID
     * @return AとA専用のグループの組で、detachWatchedDirectoryにまだ退避名が無い場合だけtrue
     * @note 種別の変更で既存のエントリを退避した直後に、親のディレクトリが共有へ変わった状況を作る
     *       退避の後の適用と、退避物を元の名前へ戻すrenameの両方を拒否させ、退避物を残す経路を通すためである
     *       recordingPrivateGroupCheckと同じく、呼ばれた時点のfsuidとfsgidを記録する
     */
    bool privateUntilDetached(uid_t owner, gid_t group)
    {
        const int fsuid = ::setfsuid(static_cast<uid_t>(-1));
        const int fsgid = ::setfsgid(static_cast<gid_t>(-1));
        privateGroupCheckIdentities.append(qMakePair(fsuid, fsgid));
        if (owner != DIRECTORY_OWNER_UID || group != DIRECTORY_OWNER_GID) {
            return false;
        }
        const QStringList detached = QDir(detachWatchedDirectory).entryList(
            {QStringLiteral(".entry.qsnapper-old.*")},
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        return detached.isEmpty();
    }

    /**
     * @brief 子プロセスを利用者Bとして動かし、関数の戻り値を終了コードとして返す
     * @param groups Bの補助グループ
     * @param operation Bとして実行する処理 (成功時は0を返す)
     * @return 処理の戻り値、識別子の切り替えに失敗した場合は200、異常終了の場合は201
     * @note 親のテストプロセスの識別子を変えないように、fork後の子プロセスだけで切り替える
     */
    int runAsGroupMember(const std::vector<gid_t> &groups, const std::function<int()> &operation)
    {
        const pid_t child = ::fork();
        if (child < 0) {
            return 201;
        }
        if (child == 0) {
            if (::setgroups(groups.size(), groups.data()) != 0
                || ::setresgid(SHARED_GROUP_GID, SHARED_GROUP_GID, SHARED_GROUP_GID) != 0
                || ::setresuid(GROUP_MEMBER_UID, GROUP_MEMBER_UID, GROUP_MEMBER_UID) != 0) {
                ::_exit(200);
            }
            ::_exit(operation());
        }
        int status = 0;
        if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
            return 201;
        }
        return WEXITSTATUS(status);
    }

    /**
     * @brief 所有者とmodeを設定する
     * @param path 対象のパス
     * @param owner 所有者
     * @param group グループ
     * @param mode 設定するmode
     * @return 両方に成功した場合true
     */
    bool setOwnerAndMode(const QString &path, uid_t owner, gid_t group, mode_t mode)
    {
        const QByteArray encoded = QFile::encodeName(path);
        return ::chown(encoded.constData(), owner, group) == 0
               && ::chmod(encoded.constData(), mode) == 0;
    }

    /**
     * @brief ディレクトリを作成し、所有者とmodeを設定する
     * @param path 作成するディレクトリ
     * @param owner 所有者
     * @param group グループ
     * @param mode 設定するmode
     * @return 作成と設定に成功した場合true
     */
    bool createOwnedDirectory(const QString &path, uid_t owner, gid_t group, mode_t mode)
    {
        return QDir().mkpath(path) && setOwnerAndMode(path, owner, group, mode);
    }

    /**
     * @brief ファイルを作成し、所有者とmodeを設定する
     * @param path 作成するファイル
     * @param content ファイルの内容
     * @param mode 設定するmode
     * @return 作成と設定に成功した場合true
     */
    bool createOwnedFile(const QString &path, const QByteArray &content, mode_t mode)
    {
        return writeFile(path, content) && setOwnerAndMode(path, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, mode);
    }

    /**
     * @brief シンボリックリンクをたどらずにinode番号を取得する
     * @param path 対象のパス
     * @return inode番号、取得に失敗した場合は0
     */
    ino_t inodeOf(const QString &path)
    {
        struct stat info {};
        if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
            return 0;
        }
        return info.st_ino;
    }

    /**
     * @brief シンボリックリンクをたどらずにmodeの権限部分を取得する
     * @param path 対象のパス
     * @return 権限部分のmode、取得に失敗した場合は-1
     */
    int permissionBitsOf(const QString &path)
    {
        struct stat info {};
        if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
            return -1;
        }
        return static_cast<int>(info.st_mode & 07777);
    }

    /**
     * @brief 名前付き利用者を1人含むアクセスACLのxattrの値を組み立てる
     * @param namedUser 名前付き利用者のUID
     * @param namedPermission 名前付き利用者の権限 (rwxを0から7で表す)
     * @param maskPermission maskの権限 (rwxを0から7で表す)
     * @return system.posix_acl_accessへ設定するバイト列
     * @note 設定するとmodeのグループの桁がmaskの値になるため、ls上のmodeとACLの実効権限の両方を作れる
     */
    QByteArray namedUserAccessAcl(quint32 namedUser, quint16 namedPermission, quint16 maskPermission)
    {
        struct AclEntry {
            quint16 tag;                                // ACLのタグ
            quint16 permission;                         // rwxの権限
            quint32 id;                                 // UIDまたはGID
        };
        const AclEntry entries[] = {
            {0x01, 7, 0xffffffffU},
            {0x02, namedPermission, namedUser},
            {0x04, 5, 0xffffffffU},
            {0x10, maskPermission, 0xffffffffU},
            {0x20, 0, 0xffffffffU},
        };
        QByteArray value;
        const auto appendLittleEndian = [&value](quint32 number, int byteCount) {
            for (int index = 0; index < byteCount; ++index) {
                value.append(static_cast<char>((number >> (8 * index)) & 0xff));
            }
        };
        appendLittleEndian(2, 4);
        for (const AclEntry &entry : entries) {
            appendLittleEndian(entry.tag, 2);
            appendLittleEndian(entry.permission, 2);
            appendLittleEndian(entry.id, 4);
        }
        return value;
    }

    /**
     * @brief ディレクトリの中の名前を並べ替えて返す
     * @param path 対象のディレクトリ
     * @return 隠しファイルを含む名前の一覧
     */
    QStringList sortedEntryNames(const QString &path)
    {
        QStringList names = QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                 | QDir::Hidden | QDir::System);
        names.sort();
        return names;
    }

}

/** @brief 元の名前を守る拒否と、退避物を保存してFailedを返す拒否を区別する */
void TestSnapshotOperations::typechangedGuardsForeignDirectories_data()
{
    QTest::addColumn<bool>("foreignChild");
    QTest::newRow("g-foreign-top-not-moved") << false;
    QTest::newRow("h-detached-cleanup-fails") << true;
}

/** @brief 退避物を消せない場合にAppliedを返さず、復旧先のログと保存内容を確認する */
void TestSnapshotOperations::typechangedGuardsForeignDirectories()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to exercise move-aside ownership guards");
    }
    QFETCH(bool, foreignChild);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::chmod(QFile::encodeName(fixture.livePath).constData(), 0755), 0);
    const QString alice = fixture.livePath + QStringLiteral("/alice");
    const QString target = alice + QStringLiteral("/victim");
    const QString protectedPath = foreignChild ? target + QStringLiteral("/protected") : target;
    QVERIFY(QDir().mkpath(protectedPath));
    QCOMPARE(::chown(QFile::encodeName(alice).constData(), 1000, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(alice).constData(), 0755), 0);
    QCOMPARE(::chown(QFile::encodeName(target).constData(), foreignChild ? 1000 : 0, 1000), 0);
    QCOMPARE(::chmod(QFile::encodeName(protectedPath).constData(), 0700), 0);
    QVERIFY(writeFile(protectedPath + QStringLiteral("/file"), "preserved content"));
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/alice")));
    QVERIFY(writeFile(fixture.sourcePath + QStringLiteral("/alice/victim"), "restored content"));
    struct stat before {};
    QCOMPARE(::lstat(QFile::encodeName(protectedPath).constData(), &before), 0);
    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("guarded-typechanged");
    QVERIFY(fixture.begin(operations, manifestId, false));
    if (foreignChild) {
        // ランダム名の退避先もログに必須とし、警告の有無だけで成功扱いにしない
        QTest::ignoreMessage(QtCriticalMsg, QRegularExpression(
            QStringLiteral(".*preserved at:.*") + QRegularExpression::escape(alice)
            + QStringLiteral("/\\.victim\\.qsnapper-old\\.[0-9a-f]{16}.*")));
    }
    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(
        manifestId, {QStringLiteral("/alice/victim"), QStringLiteral("typechanged")});
    const int applyError = errno;
    QCOMPARE(outcome, qsnapper::restore::RestoreEntryOutcome::Failed);
    QCOMPARE(applyError, EPERM);
    QString preserved = protectedPath;
    if (foreignChild) {
        QCOMPARE(readFile(target), QByteArray("restored content"));
        const QStringList detached = QDir(alice).entryList(
            {QStringLiteral(".victim.qsnapper-old.*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(detached.size(), 1);
        preserved = alice + QLatin1Char('/') + detached.constFirst() + QStringLiteral("/protected");
    }
    else {
        QVERIFY(hasNoQsnapperLeftovers(alice));
    }
    struct stat after {};
    QCOMPARE(::lstat(QFile::encodeName(preserved).constData(), &after), 0);
    QCOMPARE(after.st_dev, before.st_dev);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_uid, before.st_uid);
    QCOMPARE(after.st_mode, before.st_mode);
    QCOMPARE(readFile(preserved + QStringLiteral("/file")), QByteArray("preserved content"));
    operations.finishRestorePlanForTesting(manifestId, qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief 各テストの前に、root操作の観測回数を0へ戻す
 *
 * テスト関数は同じインスタンスで続けて実行されるため、前のテストの観測結果を持ち越さない
 */
void TestSnapshotOperations::init()
{
    m_readOnlyProbeCalls = 0;
    m_readWriteRestoreCalls = 0;
}

/**
 * @brief 各テストの終了時に、metadata適用のテスト用フックを解除する
 *
 * フックはプロセス全体で共有されるため、解除しないと次のテストの結果が前のテストの設定に左右される
 */
void TestSnapshotOperations::cleanup()
{
    // テスト関数がQVERIFYなどの失敗で途中で戻った場合でも、フックの設定が次のテストへ残らないよう、各テストの終了時に解除する
    qsnapper::security::detail::clearMetadataTestHooksForTesting();
}

/**
 * @brief rootサブボリュームへの操作を観測する差し替え関数を設定する
 * @param operations 対象のSnapshotOperations
 *
 * 実システムのsubvolumeに触れず、読み取り専用の確認とrwへの復帰が呼ばれた回数を記録する
 */
void TestSnapshotOperations::installRootSubvolumeSpies(SnapshotOperations &operations)
{
    operations.setRootSubvolumeAccessForTesting(
        [this](bool *readOnly) {
            ++m_readOnlyProbeCalls;
            *readOnly = true;
            return true;
        },
        [this]() {
            ++m_readWriteRestoreCalls;
            return true;
        });
}

/**
 * @brief 計画の作成と取り消しだけでは、rootサブボリュームをrwへ変えないことを検証する
 *
 * 実行前は差分の閲覧だけを認めるため、view-diffの認可だけでrootの状態が変わる経路があってはならない
 */
void TestSnapshotOperations::beginThenCancelDoesNotTouchRootSubvolume()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    // 計画の作成 (view-diffの認可) とCancelだけで、復元の実行に伴うrootのrw化へ到達してはならない
    const QString manifestId = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!manifestId.isEmpty());

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    QVERIFY(operations.CancelRestorePlan(manifestId));

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), manifestId);
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);

    // system bus全体へ届く終了通知には、終端状態ごとの固定文言だけを載せる
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Restore cancelled"));

    // 終了直後も、ownerはGetRestorePlanStatusで終端状態を取得できる
    const QStringList status = operations.GetRestorePlanStatus(manifestId).split(QLatin1Char(','));
    QVERIFY(status.size() >= 2);
    QCOMPARE(status.at(0), manifestId);
    QCOMPARE(status.at(1), QStringLiteral("cancelled"));
}

/**
 * @brief 作成中(Staging)の復元計画があっても、config操作が止められないことを検証する
 *
 * Staging計画は差分を閲覧できる利用者なら誰でも作れるため、空の計画を作るだけで
 * 管理者のCreateConfig / DeleteConfig / DeleteSnapshotsを妨害できてはならない
 */
void TestSnapshotOperations::stagingPlanDoesNotBlockConfigOperations()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const QString emptyPlan = operations.BeginRestorePlan(
        QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!emptyPlan.isEmpty());
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));

    const QString stagedPlan = operations.BeginRestorePlan(
        QStringLiteral("root"), 2, 0, QStringLiteral("yast"));
    QVERIFY(!stagedPlan.isEmpty());
    QVERIFY(operations.StageRestoreEntries(stagedPlan, {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
}

/**
 * @brief 認可を得られない呼び出しでは、BeginRestorePlanが計画を作らないことを検証する
 *
 * 計画は件数とパスの予算を消費するため、view-diffの認可を得られない利用者に枠を使わせない
 *
 * 拒否された呼び出しのIDは返らず、登録簿の件数もテストから参照できないため、上限による破棄の有無で間接的に検出する
 * 拒否された呼び出しが計画を作っていた場合、続けて上限数の計画を作った時点でその計画が最も古いものとして破棄され、終了通知が出る
 * 上限の値を変えてもテストが壊れないよう、件数はkMaxManifestsPerUidから求める
 * 検出の仕組み自体が働くことを保証するため、最後に上限を超える1件を作り、そのとき初めて破棄が起きることも確認する
 */
void TestSnapshotOperations::beginRestorePlanRequiresAuthorization()
{
    using qsnapper::restore::RestoreManifestRegistry;

    // 直接呼び出しのownerは空文字列で、ownerの上限もUIDの上限と同じ値でなければ破棄の時点がずれる
    static_assert(RestoreManifestRegistry::kMaxManifestsPerUid == RestoreManifestRegistry::kMaxManifestsPerOwner);

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);

    // テスト用の認可の差し替えがない状態では、D-Bus経由でない呼び出しは本番と同じく拒否される
    QVERIFY(operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct")).isEmpty());

    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    QStringList plans;
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsPerUid; ++index) {
        const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
        QVERIFY(!plan.isEmpty());
        plans.append(plan);
    }

    // 拒否された呼び出しが枠を使っていなければ、上限数ちょうどまでは何も破棄されない
    QCOMPARE(finishedSpy.count(), 0);
    for (const QString &plan : std::as_const(plans)) {
        QCOMPARE(operations.GetRestorePlanStatus(plan).split(QLatin1Char(',')).value(1),
                 QStringLiteral("staging"));
    }

    // 上限を超える1件を作ると認可済みの最も古い計画が破棄されるため、この破棄の有無で拒否時の枠の消費を検出できる
    QVERIFY(!operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct")).isEmpty());
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), plans.first());
}

/**
 * @brief 同じUIDの計画数が上限に達したとき、最も古いStaging計画だけを破棄して新しい計画を作ることを検証する
 *
 * 空の計画を作り続けられても、同じUIDの利用者が新しい復元を始められなくなってはならない
 * 破棄した計画はcancelledで終了を通知し、元のownerが終端状態を取得できるようにする
 */
void TestSnapshotOperations::beginRestorePlanEvictsOldestStagingPlanOfSameUid()
{
    using qsnapper::restore::RestoreManifestRegistry;

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    QStringList plans;
    for (int index = 0; index < RestoreManifestRegistry::kMaxManifestsPerUid; ++index) {
        const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), index + 1, 0,
                                                         QStringLiteral("yast"));
        QVERIFY(!plan.isEmpty());
        plans.append(plan);
    }

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    const QString newest = operations.BeginRestorePlan(QStringLiteral("root"), 9, 0, QStringLiteral("yast"));
    QVERIFY(!newest.isEmpty());

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), plans.first());
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));
    QCOMPARE(operations.GetRestorePlanStatus(plans.first()).split(QLatin1Char(',')).value(1),
             QStringLiteral("cancelled"));

    // 破棄するのは最も古い1件だけで、残りの計画は引き続きエントリを追加できる
    for (qsizetype index = 1; index < plans.size(); ++index) {
        QCOMPARE(operations.GetRestorePlanStatus(plans.at(index)).split(QLatin1Char(',')).value(1),
                 QStringLiteral("staging"));
    }
    QVERIFY(operations.StageRestoreEntries(newest, {QStringLiteral("/etc/hosts")},
                                           {QStringLiteral("modified")}));
    QCOMPARE(m_readOnlyProbeCalls, 0);
    QCOMPARE(m_readWriteRestoreCalls, 0);
}

/**
 * @brief 同じ復元先を指すパスを含むchunkが原子的に拒否されることを検証する
 *
 * "/a"、"//a"、"/a/"は同じ場所への操作になるため、1回のchunkに含まれても後続のchunkでも受け付けない
 */
void TestSnapshotOperations::stageRestoreEntriesRejectsDuplicateCanonicalPaths()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const QString plan = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0,
                                                     QStringLiteral("yast"));
    QVERIFY(!plan.isEmpty());

    QVERIFY(!operations.StageRestoreEntries(plan, {QStringLiteral("/a"), QStringLiteral("//a")},
                                            {QStringLiteral("modified"), QStringLiteral("modified")}));
    QStringList status = operations.GetRestorePlanStatus(plan).split(QLatin1Char(','));
    QVERIFY(status.size() > 2);
    QCOMPARE(status.value(2), QStringLiteral("0"));

    QVERIFY(operations.StageRestoreEntries(plan, {QStringLiteral("/a")},
                                           {QStringLiteral("modified")}));
    QVERIFY(!operations.StageRestoreEntries(plan, {QStringLiteral("/a/")},
                                            {QStringLiteral("modified")}));
    status = operations.GetRestorePlanStatus(plan).split(QLatin1Char(','));
    QCOMPARE(status.value(2), QStringLiteral("1"));
}

/**
 * @brief 期限切れの計画がGetRestorePlanStatusでowner表から掃除され、有効な計画は残ることを検証する
 *
 * TTL切れの計画をowner表に残すと、破棄済みのIDがウィンドウのwatchを保持し続ける
 * 一方、まだ有効な計画を消すと、そのownerが終端状態を取得できなくなる
 */
void TestSnapshotOperations::expiredPlanIsRemovedFromOwnerTableOnStatus()
{
    using qsnapper::restore::RestoreManifestRegistry;

    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    qint64 nowMs = 1000;
    operations.setRestoreRegistryClockForTesting([&nowMs]() { return nowMs; });

    const QString expired = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0,
                                                        QStringLiteral("yast"));
    QVERIFY(!expired.isEmpty());
    QVERIFY(operations.hasRestorePlanOwnerForTesting(expired));

    nowMs += RestoreManifestRegistry::kDefaultTtlMs / 2;
    const QString live = operations.BeginRestorePlan(QStringLiteral("root"), 2, 0,
                                                     QStringLiteral("yast"));
    QVERIFY(!live.isEmpty());

    nowMs += RestoreManifestRegistry::kDefaultTtlMs / 2 + 1;

    QVERIFY(operations.GetRestorePlanStatus(expired).isEmpty());
    QVERIFY(!operations.hasRestorePlanOwnerForTesting(expired));
    QVERIFY(operations.hasRestorePlanOwnerForTesting(live));
    QCOMPARE(operations.GetRestorePlanStatus(live).split(QLatin1Char(',')).value(1),
             QStringLiteral("staging"));
}

/**
 * @brief 実行中の復元があるconfigだけ、config操作が止められることを検証する
 *
 * 実行中の復元は、configとスナップショットが残っていることを前提に進むため、その間は削除させない
 */
void TestSnapshotOperations::runningRestoreBlocksConfigOperations()
{
    SnapshotOperations operations;
    installRootSubvolumeSpies(operations);

    operations.beginRestoreExecutionForTesting(QStringLiteral("root"));
    QVERIFY(operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("home")));
    operations.endRestoreExecutionForTesting();
    QVERIFY(!operations.hasUnfinishedRestorePlanForTesting(QStringLiteral("root")));
}

/**
 * @brief 小さな差分では、従来どおりunified形式の差分を返すことを検証する
 *
 * 資源の上限を加えても、設定ファイル程度の変更は省略されずに読める
 */
void TestSnapshotOperations::diffStillProducesUnifiedOutputForSmallChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\r\nb\nc\n"));
    QVERIFY(writeFile(newPath, "a\nB\nc\n"));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QCOMPARE(result.text,
             QStringLiteral("--- %1\n+++ %2\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n").arg(oldPath, newPath));

    // 差分が無い場合は空で、省略理由も付かない
    const qsnapper::diff::UnifiedDiffResult same = qsnapper::diff::generateUnifiedDiff(newPath, newPath);
    QVERIFY(same.text.isEmpty());
    QVERIFY(same.omittedReason.isEmpty());
}

/**
 * @brief 片方がバイナリの場合、内容を差分へ載せずbinaryの理由で省略することを検証する
 *
 * 制御文字を含む内容をそのまま差分へ載せると、表示側やログが壊れる
 */
void TestSnapshotOperations::diffOmitsBinaryFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString binaryPath = tempDir.filePath(QStringLiteral("binary"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(writeFile(binaryPath, QByteArray("ELF\0\1\2", 6)));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(textPath, binaryPath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("binary"));
}

/**
 * @brief ファイルサイズが既定の上限を超える場合に、too_largeの理由で省略することを検証する
 */
void TestSnapshotOperations::diffOmitsTooLargeFiles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString smallPath = tempDir.filePath(QStringLiteral("small"));
    const QString largePath = tempDir.filePath(QStringLiteral("large"));
    QVERIFY(writeFile(smallPath, "line\n"));

    const qsnapper::diff::UnifiedDiffLimits defaults;
    QVERIFY(writeFile(largePath, QByteArray(static_cast<qsizetype>(defaults.maxFileBytes + 1), 'x')));

    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(smallPath, largePath);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

/**
 * @brief 行数が上限を超える場合に、too_largeの理由で省略することを検証する
 */
void TestSnapshotOperations::diffOmitsFilesWithTooManyLines()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, "a\n"));
    QVERIFY(writeFile(newPath, QByteArray(64, '\n')));

    qsnapper::diff::UnifiedDiffLimits limits;
    limits.maxLinesPerFile = 32;
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, limits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_large"));
}

/**
 * @brief FIFOを差分の対象にしても開かず、special_fileの理由で省略することを検証する
 *
 * 書き込み側のいないFIFOを開くと、差分の生成が永久にブロックする
 */
void TestSnapshotOperations::diffDoesNotOpenFifo()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString textPath = tempDir.filePath(QStringLiteral("text"));
    const QString fifoPath = tempDir.filePath(QStringLiteral("fifo"));
    QVERIFY(writeFile(textPath, "line\n"));
    QVERIFY(::mkfifo(fifoPath.toUtf8().constData(), 0600) == 0);

    // 書き込み側のいないFIFOを開くと永久にブロックするため、開かずに省略理由を返す必要がある
    QElapsedTimer timer;
    timer.start();
    const qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(fifoPath, textPath);
    QVERIFY(timer.elapsed() < 5000);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("special_file"));

    // 読み込み用helperもFIFOを開かない
    errno = 0;
    QCOMPARE(qsnapper::security::safeOpenRegularFileRead(fifoPath), -1);
    QCOMPARE(errno, EINVAL);
}

/**
 * @brief 作業領域と探索ステップ数の上限を超える差分を、too_many_changesの理由で省略することを検証する
 *
 * どちらの上限でも打ち切られることと、既定の上限の範囲内では同じ入力の差分を生成できることを併せて確認する
 */
void TestSnapshotOperations::diffOmitsTooManyChanges()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));
    QVERIFY(writeFile(oldPath, numberedLines("old ", 2000)));
    QVERIFY(writeFile(newPath, numberedLines("new ", 2000)));

    // 作業領域の上限
    qsnapper::diff::UnifiedDiffLimits traceLimits;
    traceLimits.maxTraceBytes = 256 * 1024;
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, traceLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 探索ステップ数の上限
    qsnapper::diff::UnifiedDiffLimits stepLimits;
    stepLimits.maxSteps = 10000;
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath, stepLimits);
    QVERIFY(result.text.isEmpty());
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    // 既定の上限内であれば差分を生成する
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QVERIFY(result.omittedReason.isEmpty());
    QVERIFY(result.text.contains(QStringLiteral("@@ -1,2000 +1,2000 @@")));
}

/**
 * @brief 編集距離が最大になる入力でも、既定の上限で打ち切って短時間で戻ることを検証する
 *
 * 差分の計算量は入力の組み合わせで急増するため、上限がなければサービスが応答しなくなる
 */
void TestSnapshotOperations::diffWithDefaultLimitsReturnsQuicklyOnPathologicalInput()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString oldPath = tempDir.filePath(QStringLiteral("old"));
    const QString newPath = tempDir.filePath(QStringLiteral("new"));

    // 全行が異なる (編集距離が最大) 入力と、同一行の繰り返しに異なる行を混ぜた (全対角線のsnakeが長い) 入力
    QVERIFY(writeFile(oldPath, numberedLines("a", 200000)));
    QVERIFY(writeFile(newPath, numberedLines("b", 200000)));

    QElapsedTimer timer;
    timer.start();
    qsnapper::diff::UnifiedDiffResult result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    QByteArray repeated;
    QByteArray interleaved;
    for (int i = 0; i < 200000; ++i) {
        repeated += "x\n";
        interleaved += (i % 2 == 0) ? "x\n" : "y\n";
    }
    QVERIFY(writeFile(oldPath, repeated));
    QVERIFY(writeFile(newPath, interleaved));
    result = qsnapper::diff::generateUnifiedDiff(oldPath, newPath);
    QCOMPARE(result.omittedReason, QStringLiteral("too_many_changes"));

    qInfo("pathological diff inputs took %lld ms", static_cast<long long>(timer.elapsed()));
    QVERIFY(timer.elapsed() < 10000);
}

// ============================================================================
// 書き込み可能な復元元の拒否
// ============================================================================

namespace {

    /**
     * @brief ディレクトリをO_DIRECTORYで開く
     * @param path 対象
     * @return 開いたfd、失敗時は-1
     */
    int openDirectory(const QString &path)
    {
        return ::open(path.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

}

/**
 * @brief btrfs以外の書き込み可能なディレクトリを、復元元として受け入れないことを検証する
 *
 * 復元元が書き換わると、復元の途中で内容と所有者の前提が崩れる
 * btrfsと申告されてもsubvolumeとして確認できなければ受け入れないため、その場合も併せて確認する
 */
void TestSnapshotOperations::writableDirectoryIsNotReadOnlyRestoreSource()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const int dirFd = openDirectory(tempDir.path());
    QVERIFY(dirFd >= 0);
    // btrfs以外の場所でFSTYPE=btrfsと申告されても、subvolumeとして確認できなければ拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs"));
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs"));
    ::close(dirFd);
}

/**
 * @brief btrfsでは読み取り専用フラグの立ったsubvolumeだけを復元元として受け入れることを検証する
 *
 * 書き込み可能なsubvolumeはrollbackのwritable copy(書き込み可能な複製)に相当し、復元の途中で書き換わり得る
 * HOMEがbtrfsでない場合やsubvolumeを作れない場合は、前提を用意できないためテストをスキップする
 */
void TestSnapshotOperations::btrfsSubvolumeReadOnlyFlagIsRequired()
{
    const QString base = QDir::homePath();
    struct statfs fsInfo;
    if (::statfs(base.toUtf8().constData(), &fsInfo) < 0
            || fsInfo.f_type != BTRFS_SUPER_MAGIC) {
        QSKIP("HOME is not on btrfs");
    }

    QTemporaryDir tempDir(base + QStringLiteral("/.qsnapper-test-XXXXXX"));
    QVERIFY(tempDir.isValid());

    const int parentFd = openDirectory(tempDir.path());
    QVERIFY(parentFd >= 0);
    struct btrfs_ioctl_vol_args args;
    std::memset(&args, 0, sizeof(args));
    std::strncpy(args.name, "snapshot", sizeof(args.name) - 1);
    const int created = ::ioctl(parentFd, BTRFS_IOC_SUBVOL_CREATE, &args);
    ::close(parentFd);
    if (created < 0) {
        QSKIP("Cannot create a btrfs subvolume as this user");
    }

    const QString subvolume = tempDir.path() + QStringLiteral("/snapshot");
    const int subvolumeFd = openDirectory(subvolume);
    QVERIFY(subvolumeFd >= 0);

    // 書き込み可能なsubvolume (rollbackのwritable copy相当) は拒否する
    QVERIFY(!qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));

    quint64 flags = BTRFS_SUBVOL_RDONLY;
    const bool flagged = ::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0;
    if (flagged) {
        QVERIFY(qsnapper::restore::isPinnedRestoreSourceReadOnly(subvolumeFd, "btrfs"));
        flags = 0;
        QVERIFY(::ioctl(subvolumeFd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags) == 0);
    }
    ::close(subvolumeFd);

    // 後片付け: 空のsubvolumeは所有者がrmdirできる (kernel 4.18以降)
    if (::rmdir(subvolume.toUtf8().constData()) < 0) {
        qWarning("Could not remove test subvolume: %s", std::strerror(errno));
    }
    if (!flagged) {
        QSKIP("Cannot set the btrfs read-only flag as this user");
    }
}

/**
 * @brief btrfs以外では、読み取り専用でmountされたファイルシステムを復元元として受け入れることを検証する
 *
 * 非特権の利用者とmount名前空間の中で読み取り専用のtmpfsをmountし、実システムのmount設定に依存せず確認する
 * btrfsと申告された場合はsubvolumeの確認を通らないため、同じtmpfsでも受け入れないことを併せて確認する
 */
void TestSnapshotOperations::readOnlyMountIsAcceptedForNonBtrfs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QByteArray mountPoint = tempDir.path().toUtf8();

    // 非特権のuser + mount namespaceで、読み取り専用のtmpfsをmountして確認する
    constexpr int kNamespaceUnavailable = 77;
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        const auto writeText = [](const char *path, const QByteArray &text) {
            const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            const bool written = fd >= 0 && ::write(fd, text.constData(), text.size()) == text.size();
            if (fd >= 0) {
                ::close(fd);
            }
            return written;
        };
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0
                || !writeText("/proc/self/setgroups", "deny")
                || !writeText("/proc/self/uid_map", "0 " + QByteArray::number(uid) + " 1")
                || !writeText("/proc/self/gid_map", "0 " + QByteArray::number(gid) + " 1")
                || ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0
                || ::mount("tmpfs", mountPoint.constData(), "tmpfs", MS_RDONLY, "size=1m") < 0) {
            ::_exit(kNamespaceUnavailable);
        }
        const int dirFd = ::open(mountPoint.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd < 0) {
            ::_exit(1);
        }
        const bool accepted = qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "xfs");
        // 読み取り専用mountでも、btrfsのsubvolumeでなければbtrfsとしては受け入れない
        const bool rejectedAsBtrfs = !qsnapper::restore::isPinnedRestoreSourceReadOnly(dirFd, "btrfs");
        ::_exit(accepted && rejectedAsBtrfs ? 0 : 2);
    }

    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);
    QVERIFY(WIFEXITED(status));
    if (WEXITSTATUS(status) == kNamespaceUnavailable) {
        QSKIP("Unprivileged user and mount namespaces are unavailable");
    }
    QCOMPARE(WEXITSTATUS(status), 0);
}

// 認可待ちの上限

/**
 * @brief 1つのUIDが上限まで積んでも、別のUIDは枠を確保できることを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetLimitsEachUid()
{
    qsnapper::security::PendingAuthorizationBudget budget(4, 16);

    for (int i = 0; i < 4; ++i) {
        QVERIFY(budget.tryAcquire(1000));
    }
    QVERIFY(!budget.tryAcquire(1000));
    QCOMPARE(budget.countFor(1000), 4);

    QVERIFY(budget.tryAcquire(1001));
    QCOMPARE(budget.countFor(1001), 1);
    QCOMPARE(budget.total(), 5);
}

/**
 * @brief UIDごとの枠に余裕があっても、全体の上限を超えては確保できないことを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetKeepsGlobalLimit()
{
    qsnapper::security::PendingAuthorizationBudget budget(4, 16);

    for (uint uid = 1000; uid < 1004; ++uid) {
        for (int i = 0; i < 4; ++i) {
            QVERIFY(budget.tryAcquire(uid));
        }
    }
    QCOMPARE(budget.total(), 16);
    QVERIFY(!budget.tryAcquire(2000));
    QCOMPARE(budget.countFor(2000), 0);
    QCOMPARE(budget.total(), 16);
}

/**
 * @brief 返却した枠が同じUIDと全体の両方へ戻り、未確保のUIDの返却は件数を変えないことを確認する
 */
void TestSnapshotOperations::pendingAuthorizationBudgetReleasesSlots()
{
    qsnapper::security::PendingAuthorizationBudget budget(2, 3);

    QVERIFY(budget.tryAcquire(1000));
    QVERIFY(budget.tryAcquire(1000));
    QVERIFY(budget.tryAcquire(1001));
    QVERIFY(!budget.tryAcquire(1000));
    QVERIFY(!budget.tryAcquire(1002));

    // 確保していないUIDの返却では、他のUIDの枠も全体の件数も変わらない
    budget.release(1002);
    QCOMPARE(budget.total(), 3);
    QCOMPARE(budget.countFor(1000), 2);

    budget.release(1000);
    QCOMPARE(budget.countFor(1000), 1);
    QCOMPARE(budget.total(), 2);
    QVERIFY(budget.tryAcquire(1002));

    budget.release(1000);
    budget.release(1001);
    budget.release(1002);
    QCOMPARE(budget.total(), 0);
    QCOMPARE(budget.countFor(1000), 0);
    QVERIFY(budget.tryAcquire(1000));
}

/**
 * @brief FIFOなどの特殊ファイルは計画を失敗させず、live側に触れずにスキップされることを検証する
 */
void TestSnapshotOperations::specialFileSourceIsSkippedWithoutTouchingLive()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::mkfifo(QFile::encodeName(fixture.sourcePath + QStringLiteral("/pipe")).constData(), 0600), 0);
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/pipe"), "live data\n"));

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-special"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-special"),
                                                    {QStringLiteral("/pipe"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Skipped);
    QCOMPARE(readFile(fixture.livePath + QStringLiteral("/pipe")), QByteArray("live data\n"));

    // 復元元に無いpathは、スキップではなく失敗として扱う
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-special"),
                                                    {QStringLiteral("/missing"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);

    operations.finishRestorePlanForTesting(QStringLiteral("apply-special"),
                                           qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief YaST方式でも、ディレクトリとファイルの間で種別が変わったエントリを復元できることを検証する
 */
void TestSnapshotOperations::typechangedReplacesDirectoryAndFileInYastMode()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元ではファイル、live側ではディレクトリ
    QVERIFY(writeFile(fixture.sourcePath + QStringLiteral("/item"), "source file\n"));
    QVERIFY(QDir().mkpath(fixture.livePath + QStringLiteral("/item/child")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/item/child/data"), "live child\n"));

    // 復元元ではディレクトリ、live側ではファイル
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/node")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/node"), "live file\n"));

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-typechanged"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-typechanged"),
                                                    {QStringLiteral("/item"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-typechanged"),
                                                    {QStringLiteral("/node"), QStringLiteral("typechanged")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);

    struct stat itemStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/item")).constData(), &itemStat), 0);
    QVERIFY(S_ISREG(itemStat.st_mode));
    QCOMPARE(readFile(fixture.livePath + QStringLiteral("/item")), QByteArray("source file\n"));

    struct stat nodeStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/node")).constData(), &nodeStat), 0);
    QVERIFY(S_ISDIR(nodeStat.st_mode));

    // 退避した元のobjectは、成功後に片付けられている
    QVERIFY(hasNoQsnapperLeftovers(fixture.livePath));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-typechanged"),
                                           qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief 既存のディレクトリと新規に作成するディレクトリの両方で、modeを適用する直前に継承ACLが削除されていることを検証する
 *
 * 復元先にdefault ACLがあると、その下のディレクトリは名前付きユーザのエントリを継承する
 * 復元元のmodeを適用してgroupのビットが広がると、継承したエントリが有効になり、名前付きユーザが復元中のディレクトリを参照できる
 * そのため、ACLの同期がmodeの適用より先に終わっていることを、applyDirectoryBeneathRootを通る両方の経路で確認する
 */
void TestSnapshotOperations::applyRestoreEntrySyncsDirectoryAclBeforeModeRestore()
{
    const QString manifestId = QStringLiteral("apply-directory-acl");
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元のディレクトリは、ACLを持たないmode 0710とする
    // live側にあるexistingは既存のディレクトリとして、live側に無いfreshは新規に作成するディレクトリとして復元する
    const QStringList names = {QStringLiteral("existing"), QStringLiteral("fresh")};
    for (const QString &name : names) {
        const QString sourceDir = fixture.sourcePath + QLatin1Char('/') + name;
        QVERIFY(QDir().mkpath(sourceDir));
        QCOMPARE(::chmod(QFile::encodeName(sourceDir).constData(), 0710), 0);
    }

    // 復元先にdefault ACLを設定してから既存のディレクトリを作成し、既存のディレクトリにもACLを継承させる
    const QByteArray defaultAcl = namedUserDefaultAcl(::getuid() + 1000);
    const int setResult = ::lsetxattr(QFile::encodeName(fixture.livePath).constData(), "system.posix_acl_default",
                                      defaultAcl.constData(), static_cast<size_t>(defaultAcl.size()), 0);
    const int setError = setResult == 0 ? 0 : errno;
    if (setError == ENOTSUP) {
        QSKIP("POSIX ACLs are not supported on this filesystem");
    }
    QCOMPARE(setError, 0);

    const QString existingDir = fixture.livePath + QStringLiteral("/existing");
    QVERIFY(QDir().mkpath(existingDir));
    QCOMPARE(pathXattrState(existingDir, "system.posix_acl_access"), 1);
    QCOMPARE(pathXattrState(existingDir, "system.posix_acl_default"), 1);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, manifestId, false));

    // 既存のディレクトリはmodifiedとして、live側に無いディレクトリはdeletedとして復元する
    // 観測結果の記録先はこの関数のローカル変数であるため、判定で途中終了する前にobserverを解除する
    QList<DirectoryAclObservation> observations;
    qsnapper::security::detail::setMetadataStageObserverForTesting(&recordDirectoryAclObservation, &observations);
    const qsnapper::restore::RestoreEntryOutcome existingOutcome =
            operations.applyRestoreEntryForTesting(manifestId, {QStringLiteral("/existing"), QStringLiteral("modified")});
    const qsnapper::restore::RestoreEntryOutcome freshOutcome =
            operations.applyRestoreEntryForTesting(manifestId, {QStringLiteral("/fresh"), QStringLiteral("deleted")});
    qsnapper::security::detail::setMetadataStageObserverForTesting(nullptr, nullptr);

    QCOMPARE(existingOutcome, qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(freshOutcome, qsnapper::restore::RestoreEntryOutcome::Applied);

    // 2つのディレクトリのどちらも、modeを適用する直前には継承したaccess ACLとdefault ACLが削除されている必要がある
    QCOMPARE(observations.size(), 2);
    for (const DirectoryAclObservation &beforeMode : std::as_const(observations)) {
        QVERIFY2(beforeMode.accessState == 0 && beforeMode.defaultState == 0,
                 describeDirectoryAclObservation(beforeMode).constData());
    }

    // 復元後のmodeとACLは、復元元と一致する
    for (const QString &name : names) {
        const QString restoredDir = fixture.livePath + QLatin1Char('/') + name;
        struct stat restoredStat;
        QCOMPARE(::lstat(QFile::encodeName(restoredDir).constData(), &restoredStat), 0);
        QCOMPARE(restoredStat.st_mode & 07777, 0710U);
        QCOMPARE(pathXattrState(restoredDir, "system.posix_acl_access"), 0);
        QCOMPARE(pathXattrState(restoredDir, "system.posix_acl_default"), 0);
    }

    operations.finishRestorePlanForTesting(manifestId, qsnapper::restore::ManifestState::Completed);
}

/**
 * @brief 子エントリの適用で変わったディレクトリの時刻が、計画の終了時に復元元へそろうことを検証する
 */
void TestSnapshotOperations::directoryTimesAreRestoredAfterChildEntries()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/dir")));
    QVERIFY(QDir().mkpath(fixture.livePath + QStringLiteral("/dir")));
    QVERIFY(writeFile(fixture.livePath + QStringLiteral("/dir/extra"), "created after snapshot\n"));

    // 復元元のディレクトリへ既知の時刻を設定し、子の削除後もその時刻が復元されることを観測できるようにする
    const struct timespec sourceTimes[2] = {{1000000000, 0}, {1000000000, 123456789}};
    QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(fixture.sourcePath + QStringLiteral("/dir")).constData(),
                         sourceTimes, AT_SYMLINK_NOFOLLOW), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-times"), false));

    // ディレクトリを先に適用し、その後に子を削除する (削除で親のmtimeが更新される)
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-times"),
                                                    {QStringLiteral("/dir"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-times"),
                                                    {QStringLiteral("/dir/extra"), QStringLiteral("created")}),
             qsnapper::restore::RestoreEntryOutcome::Applied);
    QVERIFY(!QFileInfo::exists(fixture.livePath + QStringLiteral("/dir/extra")));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-times"),
                                           qsnapper::restore::ManifestState::Completed);

    struct stat liveStat;
    QCOMPARE(::lstat(QFile::encodeName(fixture.livePath + QStringLiteral("/dir")).constData(), &liveStat), 0);
    QCOMPARE(static_cast<qint64>(liveStat.st_mtim.tv_sec), static_cast<qint64>(1000000000));
    QCOMPARE(static_cast<qint64>(liveStat.st_mtim.tv_nsec), static_cast<qint64>(123456789));
}

/**
 * @brief live側の同名objectがsymlinkの場合、その先のディレクトリへmetadataを適用しないことを検証する
 *
 * 適用中に名前をsymlinkへ差し替えられても、rootがsymlinkの先 (例えば他人のディレクトリ) の所有者やmodeを変えないようにする
 */
void TestSnapshotOperations::existingSymlinkIsNotUsedAsDirectory()
{
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QVERIFY(QDir().mkpath(fixture.sourcePath + QStringLiteral("/conf")));
    QCOMPARE(::chmod(QFile::encodeName(fixture.sourcePath + QStringLiteral("/conf")).constData(), 0700), 0);

    // symlinkの先にrootが変えてはならないディレクトリを置き、modeが変わらないことを観測できるようにする
    const QString victimPath = fixture.root.filePath(QStringLiteral("victim"));
    QVERIFY(QDir().mkpath(victimPath));
    QCOMPARE(::chmod(QFile::encodeName(victimPath).constData(), 0755), 0);
    QCOMPARE(::symlink(QFile::encodeName(victimPath).constData(),
                       QFile::encodeName(fixture.livePath + QStringLiteral("/conf")).constData()), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-symlink"), false));

    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-symlink"),
                                                    {QStringLiteral("/conf"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);

    struct stat victimStat;
    QCOMPARE(::stat(QFile::encodeName(victimPath).constData(), &victimStat), 0);
    QCOMPARE(victimStat.st_mode & 07777, static_cast<mode_t>(0755));

    operations.finishRestorePlanForTesting(QStringLiteral("apply-symlink"),
                                           qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief root共有親と単一principalの親で旧ポリシーの適用範囲を区別する
 */
void TestSnapshotOperations::untrustedParentRefusesMetadataOnUnprovenDirectories_data()
{
    QTest::addColumn<int>("parentUid");
    QTest::addColumn<int>("parentMode");
    QTest::addColumn<bool>("allowed");
    QTest::newRow("root-shared-refused") << 0 << 0777 << false;
    QTest::newRow("principal-parent-applied") << 1000 << 0755 << true;
}

/**
 * @brief 他ユーザの書き換えられる親の下では、入れ替えられた可能性のあるディレクトリへrootがmetadataを適用しないことを検証する
 *
 * 所有者の変更を伴うため、rootで実行した場合だけ確認する
 */
void TestSnapshotOperations::untrustedParentRefusesMetadataOnUnprovenDirectories()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to change directory owners");
    }

    constexpr uid_t kUserUid = 1000;    // 親ディレクトリを書き換えられる一般ユーザ
    QFETCH(int, parentUid);
    QFETCH(int, parentMode);
    QFETCH(bool, allowed);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());

    // 復元元の3件を一般ユーザ所有にし、復元先の所有者とmodeを変えて3つの場合分けを作る
    const QString sourceShared = fixture.sourcePath + QStringLiteral("/shared");
    const QString liveShared = fixture.livePath + QStringLiteral("/shared");
    for (const QString &name : {QStringLiteral("swapped"), QStringLiteral("owned"), QStringLiteral("created")}) {
        const QString path = sourceShared + QLatin1Char('/') + name;
        QVERIFY(QDir().mkpath(path));
        QCOMPARE(::chown(QFile::encodeName(path).constData(), kUserUid, kUserUid), 0);
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0700), 0);
    }
    QVERIFY(QDir().mkpath(liveShared + QStringLiteral("/swapped")));
    QVERIFY(QDir().mkpath(liveShared + QStringLiteral("/owned")));
    QCOMPARE(::chown(QFile::encodeName(liveShared).constData(), parentUid, parentUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared).constData(), parentMode), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared + QStringLiteral("/swapped")).constData(), 0755), 0);
    QCOMPARE(::chown(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), kUserUid, kUserUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), 0755), 0);

    SnapshotOperations operations;
    QVERIFY(fixture.begin(operations, QStringLiteral("apply-untrusted"), false));

    // rootが所有する既存のディレクトリは、入れ替えられた機密のディレクトリかもしれないため変更しない
    QCOMPARE(operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/swapped"), QStringLiteral("modified")}),
             qsnapper::restore::RestoreEntryOutcome::Failed);
    struct stat swappedStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/swapped")).constData(), &swappedStat), 0);
    QCOMPARE(swappedStat.st_uid, static_cast<uid_t>(0));
    QCOMPARE(swappedStat.st_mode & 07777, static_cast<mode_t>(0755));

    // sourceとの所有者一致だけでは不十分で、親の単一principalとも一致する場合だけ復元する
    errno = 0;
    const auto ownedOutcome = operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/owned"), QStringLiteral("modified")});
    const int ownedError = errno;
    QCOMPARE(ownedOutcome, allowed ? qsnapper::restore::RestoreEntryOutcome::Applied
                                  : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (!allowed) {
        QCOMPARE(ownedError, EPERM);
    }
    struct stat ownedStat;
    QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/owned")).constData(), &ownedStat), 0);
    QCOMPARE(ownedStat.st_uid, kUserUid);
    QCOMPARE(ownedStat.st_mode & 07777, static_cast<mode_t>(allowed ? 0700 : 0755));

    // 作成時からprincipal所有にできる親だけに限定し、root共有親では何も作らない
    errno = 0;
    const auto createdOutcome = operations.applyRestoreEntryForTesting(QStringLiteral("apply-untrusted"),
                                                    {QStringLiteral("/shared/created"), QStringLiteral("deleted")});
    const int createdError = errno;
    QCOMPARE(createdOutcome, allowed ? qsnapper::restore::RestoreEntryOutcome::Applied
                                    : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (allowed) {
        struct stat createdStat;
        QCOMPARE(::stat(QFile::encodeName(liveShared + QStringLiteral("/created")).constData(), &createdStat), 0);
        QCOMPARE(createdStat.st_uid, kUserUid);
        QCOMPARE(createdStat.st_mode & 07777, static_cast<mode_t>(0700));
    }
    else {
        QCOMPARE(createdError, EPERM);
        QVERIFY(!QFileInfo::exists(liveShared + QStringLiteral("/created")));
    }

    operations.finishRestorePlanForTesting(QStringLiteral("apply-untrusted"),
                                           qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief trusted-chain規則の拒否と正常系を独立した行にしてSKIPによる見かけの成功を防ぐ
 */
void TestSnapshotOperations::directoryMetadataFollowsChainOwnership_data()
{
    QTest::addColumn<QString>("relative");
    QTest::addColumn<int>("parentUid");
    QTest::addColumn<int>("parentMode");
    QTest::addColumn<int>("targetUid");
    QTest::addColumn<int>("sourceUid");
    QTest::addColumn<int>("sourceMode");
    QTest::addColumn<int>("expectedError");
    QTest::addColumn<bool>("regularFile");
    QTest::newRow("i-root-victim") << QStringLiteral("home/alice/victim") << 1000 << 0755 << 0 << 1000 << 0750 << EPERM << false;
    QTest::newRow("ii-foreign-owner-match") << QStringLiteral("home/alice/bobdir") << 1000 << 0755 << 2000 << 2000 << 0750 << EPERM << false;
    QTest::newRow("iii-principal-existing") << QStringLiteral("home/alice/own") << 1000 << 0755 << 1000 << 1000 << 0750 << 0 << false;
    QTest::newRow("iv-principal-missing") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 1000 << 0750 << 0 << false;
    QTest::newRow("v-root-shared") << QStringLiteral("shared/d") << 0 << 0777 << 1000 << 1000 << 0750 << EPERM << false;
    QTest::newRow("vi-root-sticky-shared") << QStringLiteral("shared/d") << 0 << 01777 << 1000 << 1000 << 0750 << EPERM << false;
    QTest::newRow("vii-trusted-chain") << QStringLiteral("home/d") << 0 << 0755 << 0 << 1000 << 0750 << 0 << false;
    QTest::newRow("viii-protected-ancestor") << QStringLiteral("home/alice/protected/file") << 1000 << 0755 << 0 << 1000 << 0640 << EPERM << true;
    QTest::newRow("ix-readonly-principal-parent") << QStringLiteral("home/alice/new") << 1000 << 0555 << -1 << 1000 << 0750 << EACCES << false;
    QTest::newRow("x-identity-restored") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 1000 << 0700 << 0 << false;
    QTest::newRow("xi-principal-group-writable") << QStringLiteral("home/alice/grp/x") << 1000 << 0755 << -1 << 1000 << 0775 << 0 << false;
    QTest::newRow("xi-principal-group-writable-non-upg") << QStringLiteral("home/alice/grp/x") << 1000 << 0755 << -1 << 1000 << 0775 << EPERM << false;
    QTest::newRow("source-foreign-refused-before-mkdir") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 2000 << 02775 << EPERM << false;
    QTest::newRow("principal-root-source-refused") << QStringLiteral("home/alice/new") << 1000 << 0755 << -1 << 0 << 04755 << EPERM << false;
}

/**
 * @brief 攻撃者が同じ親内で他者のディレクトリをrenameできてもmetadataや内容が変わらないことを確認する
 */
void TestSnapshotOperations::directoryMetadataFollowsChainOwnership()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to exercise directory ownership and filesystem identity");
    }
    QFETCH(QString, relative);
    QFETCH(int, parentUid);
    QFETCH(int, parentMode);
    QFETCH(int, targetUid);
    QFETCH(int, sourceUid);
    QFETCH(int, sourceMode);
    QFETCH(int, expectedError);
    QFETCH(bool, regularFile);
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QCOMPARE(::chmod(QFile::encodeName(fixture.livePath).constData(), 0755), 0);
    const QString target = fixture.livePath + QLatin1Char('/') + relative;
    const QString source = fixture.sourcePath + QLatin1Char('/') + relative;
    const QString parentRelative = relative.left(relative.lastIndexOf(QLatin1Char('/')));
    const QString parent = fixture.livePath + QLatin1Char('/') + parentRelative;
    QVERIFY(QDir().mkpath(parent));
    if (relative.startsWith(QStringLiteral("home/alice/"))) {
        const QByteArray home = QFile::encodeName(fixture.livePath + QStringLiteral("/home"));
        const QByteArray alice = QFile::encodeName(fixture.livePath + QStringLiteral("/home/alice"));
        QCOMPARE(::chmod(home.constData(), 0755), 0);
        QCOMPARE(::chown(alice.constData(), parentUid, parentUid), 0);
        QCOMPARE(::chmod(alice.constData(), parentMode), 0);
    }
    else {
        QCOMPARE(::chown(QFile::encodeName(parent).constData(), parentUid, parentUid), 0);
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), parentMode), 0);
    }
    if (relative.contains(QStringLiteral("/grp/"))) {
        QCOMPARE(::chown(QFile::encodeName(parent).constData(), 1000, 1000), 0);
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), 0775), 0);
    }
    if (regularFile) {
        QCOMPARE(::chmod(QFile::encodeName(parent).constData(), 0700), 0);
        QVERIFY(writeFile(target, "protected live content\n"));
        QVERIFY(QDir().mkpath(source.left(source.lastIndexOf(QLatin1Char('/')))));
        QVERIFY(writeFile(source, "snapshot content\n"));
    }
    else {
        QVERIFY(QDir().mkpath(source));
        if (targetUid >= 0) {
            QVERIFY(QDir().mkpath(target));
            QCOMPARE(::chown(QFile::encodeName(target).constData(), targetUid, targetUid), 0);
            QCOMPARE(::chmod(QFile::encodeName(target).constData(), 0711), 0);
        }
    }
    QCOMPARE(::chown(QFile::encodeName(source).constData(), sourceUid, sourceUid), 0);
    QCOMPARE(::chmod(QFile::encodeName(source).constData(), sourceMode), 0);
    struct stat before {};
    if (targetUid >= 0) {
        QCOMPARE(::lstat(QFile::encodeName(target).constData(), &before), 0);
    }
    // grpのグループ1000を、xiでは所有者専用のグループ(UPG、user private group)、non-upgではそうでないとフックで固定し、ホストのNSSに結果を左右させない
    if (relative.contains(QStringLiteral("/grp/"))) {
        privateGroupCheckResult = QByteArray(QTest::currentDataTag()) == "xi-principal-group-writable";
        privateGroupCheckIdentities.clear();
        qsnapper::security::detail::setUserPrivateGroupCheckForTesting(&recordingPrivateGroupCheck);
    }
    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("chain-policy");
    QVERIFY(fixture.begin(operations, manifestId, false));
    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(
        manifestId, {QLatin1Char('/') + relative, QStringLiteral("modified")});
    const int applyError = errno;
    // 成功と失敗のどちらでもidentityを戻し、次のroot操作に権限状態を持ち越さない
    QCOMPARE(::setfsuid(static_cast<uid_t>(-1)), 0);
    QCOMPARE(::setfsgid(static_cast<gid_t>(-1)), 0);
    QCOMPARE(outcome, expectedError == 0 ? qsnapper::restore::RestoreEntryOutcome::Applied
                                        : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (expectedError != 0) {
        QCOMPARE(applyError, expectedError);
        if (targetUid < 0) {
            QVERIFY(!QFileInfo::exists(target));
        }
        else {
            struct stat after {};
            QCOMPARE(::lstat(QFile::encodeName(target).constData(), &after), 0);
            QCOMPARE(after.st_ino, before.st_ino);
            QCOMPARE(after.st_uid, before.st_uid);
            QCOMPARE(after.st_mode, before.st_mode);
        }
        if (regularFile) {
            QCOMPARE(readFile(target), QByteArray("protected live content\n"));
            struct stat protectedStat {};
            QCOMPARE(::stat(QFile::encodeName(parent).constData(), &protectedStat), 0);
            QCOMPARE(protectedStat.st_uid, uid_t(0));
            QCOMPARE(protectedStat.st_mode & 07777, mode_t(0700));
        }
    }
    else {
        struct stat after {};
        QCOMPARE(::stat(QFile::encodeName(target).constData(), &after), 0);
        QCOMPARE(after.st_uid, static_cast<uid_t>(sourceUid));
        QCOMPARE(after.st_gid, static_cast<gid_t>(sourceUid));
        QCOMPARE(after.st_mode & 07777, static_cast<mode_t>(sourceMode));
    }
    QVERIFY(hasNoQsnapperLeftovers(parent));
    operations.finishRestorePlanForTesting(manifestId,
        expectedError == 0 ? qsnapper::restore::ManifestState::Completed : qsnapper::restore::ManifestState::Failed);
}

/**
 * @brief rootサブボリュームをrwへ戻せなかった計画は、全エントリを適用できていてもFailedで終わることを検証する
 */
void TestSnapshotOperations::rootLeftReadOnlyMarksPlanFailed()
{
    SnapshotOperations operations;
    int restoreCalls = 0;
    operations.setRootSubvolumeAccessForTesting(
        [](bool *readOnly) {
            *readOnly = true;
            return true;
        },
        [&restoreCalls]() {
            ++restoreCalls;
            return false;
        });

    qsnapper::restore::RootReadWriteSafetyNetState state;
    state.executionStarted = true;
    state.targetsRootSubvolume = true;
    state.preRestoreStateKnown = true;
    state.preRestoreReadOnly = false;
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const int sourceFd = ::open(QFile::encodeName(sourceDir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(sourceFd >= 0);
    operations.beginRestoreApplyForTesting(QStringLiteral("root-left-ro"), sourceDir.path(), sourceFd,
                                           QStringLiteral("/"), false, state);

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);
    operations.finishRestorePlanForTesting(QStringLiteral("root-left-ro"),
                                           qsnapper::restore::ManifestState::Completed);

    QCOMPARE(restoreCalls, 1);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("failed"));
    QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Restore failed"));

    const QString status = operations.GetRestorePlanStatus(QStringLiteral("root-left-ro"));
    QVERIFY(status.startsWith(QStringLiteral("root-left-ro,failed,")));
    QVERIFY(status.contains(QStringLiteral("could not be confirmed writable")));
}

/**
 * @brief 退避先の名前が推測できない乱数の接尾辞を持つことを検証する
 *
 * 書き込み可能な親ディレクトリでは、他のユーザが名前を先回りして作成し、退避や差し替えを妨害できる
 * そのため、接尾辞は推測できない値でなければならない
 * ただし、推測できないことはテストでは証明できないため、同じ元パスから8回生成した名前について
 * 形式 (16桁の16進数の接尾辞) と重複の有無だけを確認する
 * 推測できないことの根拠は、実装がQRandomGenerator::system()->generate64()で接尾辞を生成していることにある
 *
 * @note 接尾辞にPIDの数字列が含まれないことは確認しない
 *       16桁の乱数にPIDの数字列が偶然現れる場合があり、正しい実装でもテストが失敗するため
 */
void TestSnapshotOperations::siblingTemporaryPathIsUnpredictable()
{
    constexpr int kGenerationCount = 8;  // 形式と重複の有無を確かめる生成回数

    const QRegularExpression pattern(QStringLiteral("^/srv/data/\\.file\\.conf\\.qsnapper-old\\.([0-9a-f]{16})$"));

    QSet<QString> names;
    QSet<QString> suffixes;
    for (int index = 0; index < kGenerationCount; ++index) {
        const QString name = qsnapper::internal::siblingTemporaryPath(QStringLiteral("/srv/data/file.conf"),
                                                                      QStringLiteral("qsnapper-old"));
        const QRegularExpressionMatch match = pattern.match(name);
        QVERIFY2(match.hasMatch(), qPrintable(name));

        names.insert(name);
        suffixes.insert(match.captured(1));
    }

    // 接尾辞が固定値であれば、8回の生成のどこかで名前が重複する
    QCOMPARE(names.size(), kGenerationCount);
    QCOMPARE(suffixes.size(), kGenerationCount);

    // 長い名前でもNAME_MAXを超えず、接尾辞は保たれる
    const QString longName = QStringLiteral("/tmp/") + QString(300, QLatin1Char('a'));
    const QString trimmed = qsnapper::internal::siblingTemporaryPath(longName, QStringLiteral("qsnapper-old"));
    QVERIFY(trimmed.mid(5).toUtf8().size() <= NAME_MAX);
    QVERIFY(trimmed.contains(QStringLiteral(".qsnapper-old.")));
}

/**
 * @brief 認可済みの本体で起きた想定外の例外が、スロットの外へ出ないことを確認する
 *
 * 例外がQtのイベントループまで届くとサービスが異常終了するため、bad_allocとstd::exception以外の例外も止める
 */
void TestSnapshotOperations::unexpectedExceptionsDoNotEscapeAuthorizedBody()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    bool result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw std::bad_alloc(); });
    }
    catch (...) {
        QFAIL("bad_alloc escaped from the authorized body");
    }
    QVERIFY(!result);

    result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw std::runtime_error("secret path"); });
    }
    catch (...) {
        QFAIL("std::exception escaped from the authorized body");
    }
    QVERIFY(!result);

    result = true;
    try {
        result = operations.authorizeThrowingBodyForTesting([]() -> bool { throw 42; });
    }
    catch (...) {
        QFAIL("a non-standard exception escaped from the authorized body");
    }
    QVERIFY(!result);

    // 例外を止めた後も、通常の本体はそのまま実行できる
    QVERIFY(operations.authorizeThrowingBodyForTesting([]() { return true; }));
}

/**
 * @brief ModifySnapshotが未知のcleanupアルゴリズムを認可の前に拒否することを確認する
 *
 * 未知の名前を保存すると、そのスナップショットがcleanupの対象から黙って外れる
 * また、通らない値のために利用者へ認可のプロンプトを出さないよう、認可の要求そのものが起きないことを確かめる
 * 許可する名前でも、説明文に制御文字があれば本体の先頭で拒否し、実在のスナップショットに触れずに認可だけが要求されることを確かめる
 */
void TestSnapshotOperations::modifySnapshotRejectsUnknownCleanupBeforeAuthorization()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);

    const int before = operations.testAuthorizationRequestCount();
    const QStringList rejected = {
        QStringLiteral("bogus"),
        QStringLiteral("Number"),
        QStringLiteral("number "),
        QStringLiteral("timeline,number"),
    };
    for (const QString &cleanup : rejected) {
        QVERIFY(!operations.ModifySnapshot(QStringLiteral("root"), 1, QStringLiteral("description"), cleanup, {}));
    }
    QCOMPARE(operations.testAuthorizationRequestCount(), before);

    const QStringList allowed = {
        QString(),
        QStringLiteral("number"),
        QStringLiteral("timeline"),
        QStringLiteral("empty-pre-post"),
    };
    for (const QString &cleanup : allowed) {
        QVERIFY(!operations.ModifySnapshot(QStringLiteral("root"), 1, QStringLiteral("line\nbreak"), cleanup, {}));
    }
    QCOMPARE(operations.testAuthorizationRequestCount(), before + static_cast<int>(allowed.size()));
}

/**
 * @brief 認可済みのconfig操作の直前に、対象configのStaging計画をownerを問わず取り消すことを検証する
 *
 * 取り消した計画は作成者へcancelledで通知し、別のconfigの計画や番号の異なる計画は残す
 */
void TestSnapshotOperations::configOperationsDiscardStagingPlansOfConfig()
{
    SnapshotOperations operations;
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    // rootの計画を2件と、同じ復元元番号2を持つhomeの計画を用意し、取り消す範囲を区別できるようにする
    const QString first = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    const QString second = operations.BeginRestorePlan(QStringLiteral("root"), 2, 0, QStringLiteral("direct"));
    const QString other = operations.BeginRestorePlan(QStringLiteral("home"), 2, 0, QStringLiteral("direct"));
    QVERIFY(!first.isEmpty() && !second.isEmpty() && !other.isEmpty());

    QSignalSpy finishedSpy(&operations, &SnapshotOperations::restorePlanFinished);

    // 単体削除では、削除するスナップショットを参照する計画だけを取り消す
    operations.discardStagingRestorePlansForTesting(QStringLiteral("root"), 2);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), second);
    QCOMPARE(finishedSpy.at(0).at(1).toString(), QStringLiteral("cancelled"));

    // config単位の操作では、そのconfigのStaging計画を全て取り消す
    operations.discardStagingRestorePlansForTesting(QStringLiteral("root"), -1);
    QCOMPARE(finishedSpy.count(), 2);
    QCOMPARE(finishedSpy.at(1).at(0).toString(), first);
    QVERIFY(operations.GetRestorePlanStatus(first).startsWith(first + QStringLiteral(",cancelled,")));
    QVERIFY(operations.GetRestorePlanStatus(other).startsWith(other + QStringLiteral(",staging,")));
}

/**
 * @brief 実行中にownerの接続が切れても、rootサブボリュームをrwへ戻す安全ネットを実行することを検証する
 */
void TestSnapshotOperations::vanishedOwnerStillRunsRootSafetyNet()
{
    SnapshotOperations operations;
    int restoreCalls = 0;
    operations.setRootSubvolumeAccessForTesting(
        [](bool *readOnly) {
            *readOnly = true;
            return true;
        },
        [&restoreCalls]() {
            ++restoreCalls;
            return true;
        });
    operations.setAuthorizationForTesting(SnapshotOperations::TestAuthorization::Grant);
    const QString planId = operations.BeginRestorePlan(QStringLiteral("root"), 1, 0, QStringLiteral("direct"));
    QVERIFY(!planId.isEmpty());

    qsnapper::restore::RootReadWriteSafetyNetState state;
    state.executionStarted = true;
    state.targetsRootSubvolume = true;
    state.preRestoreStateKnown = true;
    state.preRestoreReadOnly = false;
    QTemporaryDir sourceDir;
    QVERIFY(sourceDir.isValid());
    const int sourceFd = ::open(QFile::encodeName(sourceDir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(sourceFd >= 0);
    operations.beginRestoreApplyForTesting(planId, sourceDir.path(), sourceFd, QStringLiteral("/"), false, state);

    // D-Busを介さない呼び出しでは、ownerは空文字列として記録される
    operations.handleRestoreOwnerUnregisteredForTesting(QString());

    QCOMPARE(restoreCalls, 1);
    QVERIFY(operations.GetRestorePlanStatus(planId).isEmpty());
}

/**
 * @brief 同じ所有者のディレクトリを入れ替える攻撃と、拒否してはならない正常系を行ごとに提供する
 *
 * expectAppliedが偽の行は、修正前はApplied、修正後はEPERMによるFailedになる攻撃の行である
 * expectAppliedが真の行は、修正の前後でAppliedのままでなければならない正常系の行である
 */
void TestSnapshotOperations::sameOwnerDirectorySwapIsRefused_data()
{
    QTest::addColumn<QString>("entryPath");
    QTest::addColumn<QString>("changeType");
    QTest::addColumn<bool>("ownerGroupIsPrivate");
    QTest::addColumn<bool>("expectApplied");

    QTest::newRow("s1-exchange-0770") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << false;
    QTest::newRow("s2-file-0770") << QStringLiteral("/team/public/index.html") << QStringLiteral("modified") << true << false;
    QTest::newRow("s3-created-0770") << QStringLiteral("/team/cache") << QStringLiteral("created") << true << false;
    QTest::newRow("s1-acl") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << false;
    QTest::newRow("s1-upg-0775-with-member") << QStringLiteral("/team/public") << QStringLiteral("modified") << false << false;
    QTest::newRow("sticky-smuggle") << QStringLiteral("/team/public/private") << QStringLiteral("modified") << true << false;
    QTest::newRow("symlink-0770") << QStringLiteral("/team/public/link") << QStringLiteral("deleted") << true << false;
    QTest::newRow("typechanged-0770") << QStringLiteral("/team/public/entry") << QStringLiteral("typechanged") << true << false;
    QTest::newRow("parent-create-shared") << QStringLiteral("/team/public/sub/file") << QStringLiteral("deleted") << true << false;
    QTest::newRow("delete-mid-tree") << QStringLiteral("/team/cache") << QStringLiteral("created") << true << false;
    QTest::newRow("base-ancestor") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << false;
    QTest::newRow("base-owner-break") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << false;
    QTest::newRow("dir-times-swap") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << true;
    QTest::newRow("leaf-shared-mode") << QStringLiteral("/team/shared") << QStringLiteral("modified") << true << true;
    QTest::newRow("acl-masked") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << true;
    QTest::newRow("upg-0775") << QStringLiteral("/team/public") << QStringLiteral("modified") << true << true;
}

/**
 * @brief 所有者以外の利用者が名前を変更できるディレクトリの下では、rootが復元を拒否することを検証する
 *
 * 攻撃の行では、Bが復元の対象を直接変更できないことと、Bの入れ替えが成功したことを先に確かめる
 * その上で、復元がEPERMで失敗し、入れ替えで差し込まれたAの非公開ディレクトリが変わらないことを確かめる
 * 復元元の木は全ての行でAが所有し、rootが所有する復元元を拒否する既存の規則に結果を左右させない
 */
void TestSnapshotOperations::sameOwnerDirectorySwapIsRefused()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to change directory owners and to act as another user");
    }
    QFETCH(QString, entryPath);
    QFETCH(QString, changeType);
    QFETCH(bool, ownerGroupIsPrivate);
    QFETCH(bool, expectApplied);
    const QByteArray scenario(QTest::currentDataTag());

    // ホストのNSSに左右されないように、所有者専用のグループかどうかの判定をフックで固定する
    privateGroupCheckResult = ownerGroupIsPrivate;
    privateGroupCheckIdentities.clear();
    qsnapper::security::detail::setUserPrivateGroupCheckForTesting(&recordingPrivateGroupCheck);

    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    const QString base = fixture.root.path();
    QVERIFY(setOwnerAndMode(base, 0, 0, 0755));
    QVERIFY(setOwnerAndMode(fixture.livePath, 0, 0, 0755));
    const QString liveTeam = fixture.livePath + QStringLiteral("/team");
    const QString sourceTeam = fixture.sourcePath + QStringLiteral("/team");
    const QString sourcePublic = sourceTeam + QStringLiteral("/public");

    // 復元元の木は全てAが所有する
    QVERIFY(createOwnedDirectory(sourceTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
    QVERIFY(createOwnedDirectory(sourcePublic, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
    QVERIFY(createOwnedFile(sourcePublic + QStringLiteral("/index.html"), "PUBLIC-SNAPSHOT\n", 0644));

    const auto makeVictim = [](const QString &path) {
        return createOwnedDirectory(path, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0700)
               && createOwnedFile(path + QStringLiteral("/secret"), "SECRET\n", 0600)
               && createOwnedFile(path + QStringLiteral("/index.html"), "PRIVATE-INDEX\n", 0600);
    };
    const auto exchangeNames = [](const QString &first, const QString &second) {
        const QByteArray firstName = QFile::encodeName(first);
        const QByteArray secondName = QFile::encodeName(second);
        return std::function<int()>([firstName, secondName]() {
            return ::renameat2(AT_FDCWD, firstName.constData(), AT_FDCWD, secondName.constData(),
                               RENAME_EXCHANGE) == 0 ? 0 : errno;
        });
    };
    const auto renameName = [](const QString &from, const QString &to) {
        const QByteArray fromName = QFile::encodeName(from);
        const QByteArray toName = QFile::encodeName(to);
        return [fromName, toName]() {
            return ::rename(fromName.constData(), toName.constData()) == 0 ? 0 : errno;
        };
    };

    QString victim;                                     // 攻撃の前のAの非公開ディレクトリ
    QString victimAfter;                                // 攻撃の後に非公開ディレクトリが現れる名前
    QStringList victimEntries = {QStringLiteral("index.html"), QStringLiteral("secret")};
    std::vector<gid_t> memberGroups = {SHARED_GROUP_GID};
    std::function<int()> attack;

    const QSet<QByteArray> exchangeScenarios = {
        "s1-exchange-0770", "s2-file-0770", "s3-created-0770", "s1-acl", "s1-upg-0775-with-member",
        "symlink-0770", "typechanged-0770", "parent-create-shared",
    };
    if (exchangeScenarios.contains(scenario)) {
        // Bがpublicとprivateを入れ替え、Aの非公開ディレクトリを復元の経路へ差し込む
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QVERIFY(createOwnedDirectory(liveTeam + QStringLiteral("/public"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0750));
        QVERIFY(createOwnedFile(liveTeam + QStringLiteral("/public/index.html"), "PUBLIC-LIVE\n", 0644));
        victim = liveTeam + QStringLiteral("/private");
        QVERIFY(makeVictim(victim));
        victimAfter = liveTeam + QStringLiteral("/public");
        attack = exchangeNames(liveTeam + QStringLiteral("/public"), victim);

        if (scenario == "s3-created-0770") {
            // cacheを退かしてから非公開ディレクトリをcacheへ移し、復元によるcacheの削除を非公開ディレクトリへ向ける
            const QString cache = liveTeam + QStringLiteral("/cache");
            QVERIFY(createOwnedDirectory(cache, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0700));
            victimAfter = cache;
            const auto moveCache = renameName(cache, cache + QStringLiteral(".moved"));
            const auto moveVictim = renameName(victim, cache);
            attack = [moveCache, moveVictim]() {
                const int result = moveCache();
                return result != 0 ? result : moveVictim();
            };
        }
        else if (scenario == "symlink-0770") {
            const QByteArray link = QFile::encodeName(sourcePublic + QStringLiteral("/link"));
            QCOMPARE(::symlink("index.html", link.constData()), 0);
            QCOMPARE(::lchown(link.constData(), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID), 0);
        }
        else if (scenario == "typechanged-0770") {
            QVERIFY(createOwnedFile(liveTeam + QStringLiteral("/public/entry"), "PUBLIC-ENTRY\n", 0644));
            QVERIFY(createOwnedFile(victim + QStringLiteral("/entry"), "PRIVATE-ENTRY\n", 0600));
            QVERIFY(createOwnedDirectory(sourcePublic + QStringLiteral("/entry"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
            victimEntries = {QStringLiteral("entry"), QStringLiteral("index.html"), QStringLiteral("secret")};
        }
        else if (scenario == "parent-create-shared") {
            QVERIFY(createOwnedDirectory(sourcePublic + QStringLiteral("/sub"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
            QVERIFY(createOwnedFile(sourcePublic + QStringLiteral("/sub/file"), "SUB-FILE\n", 0644));
        }

        if (scenario == "s1-acl") {
            // modeは0750のまま、名前付き利用者BへACLでrwxを与え、modeのグループの桁をmaskの0770にする
            QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0750));
            const QByteArray acl = namedUserAccessAcl(GROUP_MEMBER_UID, 7, 7);
            QCOMPARE(::setxattr(QFile::encodeName(liveTeam).constData(), "system.posix_acl_access",
                                acl.constData(), static_cast<size_t>(acl.size()), 0), 0);
            QCOMPARE(permissionBitsOf(liveTeam), 0770);
        }
        else if (scenario == "s1-upg-0775-with-member") {
            // 所有者専用のグループに見えるがBも属するグループのため、フックは所有者専用ではないと答える
            QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0775));
            memberGroups = {SHARED_GROUP_GID, DIRECTORY_OWNER_GID};
        }
        else {
            QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 0770));
        }
    }
    else if (scenario == "sticky-smuggle") {
        // stickyのteamでは同じ親での入れ替えが止まるため、別の共有ディレクトリから非公開ディレクトリを箱ごと持ち込む
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        const QString staging = fixture.livePath + QStringLiteral("/staging");
        QVERIFY(createOwnedDirectory(staging, DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 0770));
        QVERIFY(createOwnedDirectory(staging + QStringLiteral("/box"), DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 01770));
        victim = staging + QStringLiteral("/box/private");
        QVERIFY(makeVictim(victim));
        QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 01770));
        victimAfter = liveTeam + QStringLiteral("/public/private");
        attack = renameName(staging + QStringLiteral("/box"), liveTeam + QStringLiteral("/public"));
        QVERIFY(createOwnedDirectory(sourcePublic + QStringLiteral("/private"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
    }
    else if (scenario == "base-ancestor" || scenario == "base-owner-break") {
        // 復元先の根であるliveそのものを、Bが非公開ディレクトリと入れ替える
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QVERIFY(createOwnedDirectory(liveTeam + QStringLiteral("/public"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0750));
        victim = base + QStringLiteral("/private");
        QVERIFY(makeVictim(victim));
        victimAfter = fixture.livePath;
        attack = exchangeNames(fixture.livePath, victim);
        if (scenario == "base-ancestor") {
            QVERIFY(setOwnerAndMode(fixture.livePath, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
            QVERIFY(setOwnerAndMode(base, DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 0770));
        }
        else {
            QVERIFY(setOwnerAndMode(fixture.livePath, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0700));
            QVERIFY(setOwnerAndMode(base, GROUP_MEMBER_UID, GROUP_MEMBER_UID, 0755));
        }
    }
    else if (scenario == "delete-mid-tree") {
        // 削除の途中で降りるbだけが、共有グループの利用者にも名前の変更を許している
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        const QString cache = liveTeam + QStringLiteral("/cache");
        QVERIFY(createOwnedDirectory(cache, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0700));
        QVERIFY(createOwnedDirectory(cache + QStringLiteral("/a"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0700));
        QVERIFY(createOwnedFile(cache + QStringLiteral("/a/file"), "A\n", 0600));
        QVERIFY(createOwnedDirectory(cache + QStringLiteral("/b"), DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 0770));
        QVERIFY(createOwnedFile(cache + QStringLiteral("/b/file"), "B\n", 0600));
    }
    else if (scenario == "leaf-shared-mode") {
        // 復元の対象そのものが共有のmodeになるだけで、親は安全なので拒否しない
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QVERIFY(createOwnedDirectory(liveTeam + QStringLiteral("/shared"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QVERIFY(createOwnedDirectory(sourceTeam + QStringLiteral("/shared"), DIRECTORY_OWNER_UID, SHARED_GROUP_GID, 0770));
    }
    else {
        QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QVERIFY(createOwnedDirectory(liveTeam + QStringLiteral("/public"), DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0750));
        if (scenario == "acl-masked") {
            // 名前付き利用者Bのrwxをmaskのr-xで打ち消し、modeのグループの桁を書き込み不可の0750にする
            QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0750));
            const QByteArray acl = namedUserAccessAcl(GROUP_MEMBER_UID, 7, 5);
            QCOMPARE(::setxattr(QFile::encodeName(liveTeam).constData(), "system.posix_acl_access",
                                acl.constData(), static_cast<size_t>(acl.size()), 0), 0);
            QCOMPARE(permissionBitsOf(liveTeam), 0750);
        }
        else if (scenario == "upg-0775") {
            QVERIFY(setOwnerAndMode(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0775));
        }
    }

    // 復元元のディレクトリの時刻は、中身を作り終えた後に固定する
    const struct timespec sourceTimes[2] = {{1000000000, 0}, {1000000000, 0}};
    QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(sourcePublic).constData(), sourceTimes,
                         AT_SYMLINK_NOFOLLOW), 0);

    ino_t victimInode = 0;
    if (attack) {
        // Bが非公開ディレクトリを直接は読めず、変更もできないことを先に確かめる
        const QByteArray victimName = QFile::encodeName(victim);
        const QByteArray victimIndex = QFile::encodeName(victim + QStringLiteral("/index.html"));
        QCOMPARE(runAsGroupMember(memberGroups, [victimName, victimIndex]() {
            if (::open(victimName.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) >= 0) {
                return 1;
            }
            if (::chmod(victimName.constData(), 0777) == 0) {
                return 2;
            }
            if (::open(victimIndex.constData(), O_WRONLY | O_CLOEXEC) >= 0) {
                return 3;
            }
            return 0;
        }), 0);
        victimInode = inodeOf(victim);
        QVERIFY(victimInode != 0);
    }

    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("same-owner-swap");
    QVERIFY(fixture.begin(operations, manifestId, false));
    if (attack) {
        QCOMPARE(runAsGroupMember(memberGroups, attack), 0);
        QCOMPARE(inodeOf(victimAfter), victimInode);
    }

    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(manifestId, {entryPath, changeType});
    const int applyError = errno;
    // 成功と失敗のどちらでもidentityを戻し、次のroot操作に権限状態を持ち越さない
    QCOMPARE(::setfsuid(static_cast<uid_t>(-1)), 0);
    QCOMPARE(::setfsgid(static_cast<gid_t>(-1)), 0);

    ino_t swappedInode = 0;
    const struct timespec swappedTimes[2] = {{1100000000, 0}, {1100000000, 0}};
    if (scenario == "dir-times-swap") {
        // 時刻の反映の前に、復元したディレクトリを別のinodeへ差し替える
        const QString restored = liveTeam + QStringLiteral("/public");
        QCOMPARE(::rename(QFile::encodeName(restored).constData(),
                          QFile::encodeName(restored + QStringLiteral(".orig")).constData()), 0);
        QVERIFY(createOwnedDirectory(restored, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(restored).constData(), swappedTimes,
                             AT_SYMLINK_NOFOLLOW), 0);
        swappedInode = inodeOf(restored);
    }
    operations.finishRestorePlanForTesting(
        manifestId, outcome == qsnapper::restore::RestoreEntryOutcome::Applied
                        ? qsnapper::restore::ManifestState::Completed
                        : qsnapper::restore::ManifestState::Failed);

    // 所有者専用のグループかどうかの判定は、ScopedFsIdentityの外でrootの識別子のまま行われなければならない
    qInfo("user private group checks: %lld", static_cast<long long>(privateGroupCheckIdentities.size()));
    // groupが書き込めてACLの無い行だけは判定まで必ず到達するため、記録が空のまま下のループを素通りさせない
    // ACLで先に拒否する行や、groupが書き込めない行では判定が呼ばれないので要求しない
    if (scenario == "upg-0775" || scenario == "s1-upg-0775-with-member") {
        QVERIFY(!privateGroupCheckIdentities.isEmpty());
    }
    for (const auto &identity : std::as_const(privateGroupCheckIdentities)) {
        QCOMPARE(identity.first, 0);
        QCOMPARE(identity.second, 0);
    }

    QCOMPARE(outcome, expectApplied ? qsnapper::restore::RestoreEntryOutcome::Applied
                                    : qsnapper::restore::RestoreEntryOutcome::Failed);
    if (!expectApplied) {
        QCOMPARE(applyError, EPERM);
    }

    if (attack) {
        QCOMPARE(inodeOf(victimAfter), victimInode);
        QCOMPARE(permissionBitsOf(victimAfter), 0700);
        QCOMPARE(sortedEntryNames(victimAfter), victimEntries);
        QCOMPARE(readFile(victimAfter + QStringLiteral("/secret")), QByteArray("SECRET\n"));
        QCOMPARE(readFile(victimAfter + QStringLiteral("/index.html")), QByteArray("PRIVATE-INDEX\n"));
        if (scenario == "typechanged-0770") {
            const QFileInfo entry(victimAfter + QStringLiteral("/entry"));
            QVERIFY(entry.isFile() && !entry.isSymLink());
            QCOMPARE(readFile(entry.filePath()), QByteArray("PRIVATE-ENTRY\n"));
        }
    }
    else if (scenario == "delete-mid-tree") {
        const QString cache = liveTeam + QStringLiteral("/cache");
        QVERIFY(QFileInfo::exists(cache + QStringLiteral("/b/file")));
        qInfo("delete-mid-tree: a %s", QFileInfo::exists(cache + QStringLiteral("/a")) ? "remains" : "was removed");
    }
    else if (scenario == "dir-times-swap") {
        struct stat swapped {};
        QCOMPARE(::lstat(QFile::encodeName(liveTeam + QStringLiteral("/public")).constData(), &swapped), 0);
        QCOMPARE(swapped.st_ino, swappedInode);
        QCOMPARE(static_cast<long long>(swapped.st_mtim.tv_sec), 1100000000LL);
    }
    else if (scenario == "leaf-shared-mode") {
        struct stat shared {};
        QCOMPARE(::lstat(QFile::encodeName(liveTeam + QStringLiteral("/shared")).constData(), &shared), 0);
        QCOMPARE(static_cast<int>(shared.st_mode & 07777), 0770);
        QCOMPARE(shared.st_gid, SHARED_GROUP_GID);
    }
    else {
        QCOMPARE(permissionBitsOf(liveTeam + QStringLiteral("/public")), 0755);
    }
}

/**
 * @brief 種別の変更で退避した後に、適用と元の名前へ戻す処理の両方が拒否された場合の後始末を検証する
 *
 * 退避の後に親のディレクトリが、所有者以外の利用者も名前を変更できる状態へ変わった状況を、判定のフックで作る
 * 戻すrenameも拒否されるため、退避物は退避名のまま残り、管理者が復旧できるようにその名前がログに記録されなければならない
 */
void TestSnapshotOperations::refusedReattachKeepsDetachedEntry()
{
    if (::geteuid() != 0) {
        QSKIP("Requires root to change directory owners");
    }
    ApplyFixture fixture;
    QVERIFY(fixture.setUp());
    QVERIFY(setOwnerAndMode(fixture.root.path(), 0, 0, 0755));
    QVERIFY(setOwnerAndMode(fixture.livePath, 0, 0, 0755));

    // liveでは通常ファイル、スナップショットではディレクトリとし、適用の前に既存のファイルを退避させる
    const QString liveTeam = fixture.livePath + QStringLiteral("/team");
    const QString liveEntry = liveTeam + QStringLiteral("/entry");
    QVERIFY(createOwnedDirectory(liveTeam, DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0775));
    QVERIFY(createOwnedFile(liveEntry, "LIVE-ENTRY", 0644));
    const ino_t entryInode = inodeOf(liveEntry);
    QVERIFY(entryInode != 0);
    QVERIFY(createOwnedDirectory(fixture.sourcePath + QStringLiteral("/team"),
                                 DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));
    QVERIFY(createOwnedDirectory(fixture.sourcePath + QStringLiteral("/team/entry"),
                                 DIRECTORY_OWNER_UID, DIRECTORY_OWNER_GID, 0755));

    // 退避名が現れるまでは0775をA専用のグループとして許可し、現れた後は共有として拒否させる
    privateGroupCheckIdentities.clear();
    detachWatchedDirectory = liveTeam;
    qsnapper::security::detail::setUserPrivateGroupCheckForTesting(&privateUntilDetached);

    SnapshotOperations operations;
    const QString manifestId = QStringLiteral("refused-reattach");
    QVERIFY(fixture.begin(operations, manifestId, false));
    // 退避名はランダムな接尾辞を持つため形式で照合し、ログが出なければ失敗させる
    QTest::ignoreMessage(QtCriticalMsg, QRegularExpression(
        QStringLiteral(".*preserved at:.*") + QRegularExpression::escape(liveTeam)
        + QStringLiteral("/\\.entry\\.qsnapper-old\\.[0-9a-f]{16}.*")));
    errno = 0;
    const auto outcome = operations.applyRestoreEntryForTesting(
        manifestId, {QStringLiteral("/team/entry"), QStringLiteral("typechanged")});
    const int applyError = errno;
    QCOMPARE(::setfsuid(static_cast<uid_t>(-1)), 0);
    QCOMPARE(::setfsgid(static_cast<gid_t>(-1)), 0);
    operations.finishRestorePlanForTesting(manifestId, qsnapper::restore::ManifestState::Failed);

    QCOMPARE(outcome, qsnapper::restore::RestoreEntryOutcome::Failed);
    QCOMPARE(applyError, EPERM);
    // 判定はScopedFsIdentityの外でrootの識別子のまま行われなければならない
    QVERIFY(!privateGroupCheckIdentities.isEmpty());
    for (const auto &identity : std::as_const(privateGroupCheckIdentities)) {
        QCOMPARE(identity.first, 0);
        QCOMPARE(identity.second, 0);
    }

    // 元の名前には何も作られず、退避物は同じinodeと内容のまま退避名で残る
    QCOMPARE(inodeOf(liveEntry), static_cast<ino_t>(0));
    const QStringList names = sortedEntryNames(liveTeam);
    QCOMPARE(names.size(), 1);
    QVERIFY2(QRegularExpression(QStringLiteral("^\\.entry\\.qsnapper-old\\.[0-9a-f]{16}$"))
                 .match(names.first()).hasMatch(),
             qPrintable(names.first()));
    const QString detachedPath = liveTeam + QLatin1Char('/') + names.first();
    QCOMPARE(inodeOf(detachedPath), entryInode);
    QCOMPARE(readFile(detachedPath), QByteArray("LIVE-ENTRY"));
}

QTEST_GUILESS_MAIN(TestSnapshotOperations)
#include "tst_snapshotoperations.moc"
