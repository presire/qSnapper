/**
 * @file tst_filesystemhelpers.h
 * @brief filesystemhelpers単体テストの共通クラス宣言
 *
 * 1テストクラス1実行体の既存構成を保つため、クラス宣言だけを本ヘッダに置き
 * 各テスト本体はtst_filesystemhelpers_*.cppへ機能別に分散する
 * mocはAUTOMOCが本ヘッダから生成する
 */
#ifndef TST_FILESYSTEMHELPERS_H
#define TST_FILESYSTEMHELPERS_H

#include <QByteArray>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

/**
 * @class TestFilesystemHelpers
 * @brief qsnapper::securityの安全ファイル操作群を検証するテストクラス
 *
 * symlink差し替え攻撃に対する拒否と、正規パスの正常受付を両面から確認する
 * テストは実ファイルシステム上の一時ディレクトリでのみ行い、実システムへは触れない
 *
 * isAttackRejectionErrno : 攻撃拒否を示すerrnoかの判定
 * isSafeHandlingErrno : 安全側処理を示すerrnoかの判定
 * fingerprintTree : outside側の無変更検証に使う一覧指紋
 * buildAttackFixture : root / outsideの攻撃再現用一時構成を作る
 * swapComponentToSymlink : 解決済み成分を外部へのsymlinkへ差し替える
 * verifyOutsideUntouched : outside側が無変更であることを検証する
 */
class TestFilesystemHelpers : public QObject
{
    Q_OBJECT

private slots:
    void safeMkpathCreatesNestedDirectories();
    void safeMkpathRejectsSymlinkComponents();
    void safeMkpathRejectsTargetSymlink();
    void safeOpenRegularFileReadRejectsSymlink();
    void safeOpenDirectoryRejectsSymlinkPath();
    void safeReadLinkNoFollowHandlesLargeTarget();
    void safeLstatReportsSymlinkItself();
    void safeRemoveAllRemovesSingleFile();
    void safeRemoveAllRemovesTreeWithoutFollowingSymlink();
    void safeRemoveAllRejectsSymlinkIntermediateComponent();
    void safeRemoveAllStopsAtMountBoundary();
    void safeRemoveAllRefusesMountPointTarget();
    void safeRemoveAllStopsAtSameFilesystemBindMount();
    void safeRemoveAllStopsAtNestedBtrfsSubvolume();

    // --- 復元時のmetadata保持とrename前後の差し替え検出 ---
    void replaceRegularFileAtReplacesContentWithNewInode();
    void replaceRegularFileAtPreservesSetuidAndSetgid();
    void replaceRegularFileAtCopiesAccessAcl();
    void replaceRegularFileAtDropsInheritedDefaultAcl();
    void applyRestoredMetadataSyncsDirectoryAcls();
    void replaceRegularFileAtDetectsTemporaryNameSwap();
    void replaceRegularFileAtKeepsResolvedParentAfterParentSwap();
    void replaceSymlinkAtAppliesMetadataAndDetectsSwap();
    void replaceSymlinkAtRequiringMetadataLeavesLiveUntouched();
    void selinuxContextUserIsReplacedOnlyInUserPart();

    // --- beneath-root 宛先解決ハードニング (Todo 2) ---
    void splitDestinationBeneathRootAcceptsNestedDestination();
    void splitDestinationBeneathRootRejectsDotDotTraversal();
    void splitDestinationBeneathRootRejectsOutsideRoot();
    void splitDestinationBeneathRootRejectsMalformedPaths();
    void safeOpenDirectoryBeneathRootWalksNestedPath();
    void safeOpenDirectoryBeneathRootCreatesMissing();
    void safeOpenDirectoryBeneathRootRejectsSymlinkComponent();
    void beneathRootWriteRejectsParentSymlinkSwap();
    void beneathRootRenameAsideRejectsParentSymlinkSwap();
    void beneathRootRemoveRejectsParentSymlinkSwap();
    void beneathRootWriteRejectsLeafSymlinkToExternalFile();
    void beneathRootWriteRejectsDanglingSymlinkComponent();
    void beneathRootWriteRejectsRegularFileAsDirectoryComponent();
    void beneathRootHappyPathWritesNestedFileWithMode();
    void beneathRootRenameAsideMovesWithinRoot();
    void beneathRootRenameAsideDoesNotReplaceExisting_data();
    void beneathRootRenameAsideDoesNotReplaceExisting();
    void beneathRootMissingParentsFollowSnapshotMetadata();
    void beneathRootMissingParentsDoNotModifyExistingParents();
    void beneathRootMissingParentsRejectLiveSymlinkComponent();
    void snapshotListFingerprintDetectsListChanges();
    void snapshotListFingerprintDetectsLeadingZeroEntries();
    /** @brief 所有者がmodeを戻せる場合も非信頼とする判定表を提供する */
    void parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode_data();
    void parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode();
    void beneathRootMissingParentsFollowTrustedParentPolicy();
    void beneathRootUntrustedParentKeepsSnapshotSearchRestriction();
    void beneathRootTrustedChainCreatesParentWithSourceMetadata();
    void beneathRootUntrustedChainCreatesParentsAsPrincipal();
    void beneathRootUntrustedChainRejectsForeignSourceOwner();
    void beneathRootUntrustedChainRejectsMixedOwnership();
    void beneathRootStaleResolutionStillRejected();
    void beneathRootRepeatedSwapsDoNotLeakDescriptors();
    void beneathRootHandlesMalformedInputsSafely();
    void beneathRootRejectsSymlinkInRootPathAncestry();
    void beneathRootRemovalStopsAtDepthLimit();
    void beneathRootResolutionRefusesMountBoundaries();
    /** @brief 所有権付き再帰削除の拒否と既存動作を対比する行を提供する */
    void beneathRootRemovalGuardsDirectoryOwnership_data();
    /** @brief 他者のディレクトリへ降りず、本人の木と非ディレクトリだけを削除する */
    void beneathRootRemovalGuardsDirectoryOwnership();

    // --- dirfd相対ソース解決 (staged restore のsnapshot pin) ---
    void safeLstatAtResolvesRelativeToDirFd();
    void safeLstatAtRejectsAbsoluteAndTraversalPaths();
    void safeOpenRegularFileReadAtRejectsSymlinkLeafAndNonRegular();
    void safeReadLinkNoFollowAtReadsRelativeToDirFd();
    void safeReadLinkNoFollowAtRejectsInvalidPaths();

    // --- created entry削除前の復元元不在確認 ---
    void isConfirmedAbsentAtRejectsPathsPresentInSource();
    void isConfirmedAbsentAtConfirmsOnlyDefiniteAbsence();
    void isConfirmedAbsentAtRefusesIntermediateSymlinks();

    // --- 復元計画の検証で使う、木に含まれるかの観測 ---
    void observePathNoFollowAtClassifiesPresenceLikeComparison();
    void observePathNoFollowAtRejectsMountBoundaries();

    // --- dirfd相対ソース解決の中間成分in-rootハードニング (RESOLVE_IN_ROOT) ---
    void safeOpenRegularFileReadAtKeepsAbsoluteIntermediateSymlinkInsideRoot();
    void safeOpenRegularFileReadAtClampsDotDotInSymlinkTargetAtRoot();
    void safeReadHelpersKeepLegitimateRelativeIntermediateSymlinkWorking();
    void safeHelpersKeepLeafSymlinkUnresolvedThroughIntermediateSymlink();
    void restoreReadHelpersHandleSnapshotWithIntermediateSymlinks();

    // --- in-root解決がsymlinkループを打ち切ることの確認 ---
    void inRootRejectsSymlinkLoop();

    // --- 復元先walkのmount境界 (nested btrfs subvolume) ---
    void nestedSubvolumeIsRejected();
    void symlinkedMiddleComponentIsRejected();
    void plainNestedDirectoryIsOpened();
    void nestedSubvolumeAllowedWithoutSameMountOnly();

    // --- 復元元snapshotのmount前後の解決 (.snapshots/<番号>/snapshot) ---
    /** @brief 正しい木と差し替え・所有者・種別の異常を対比する行を提供する */
    void snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths_data();
    /** @brief symlinkを辿らず、rootが管理する .snapshots と番号ディレクトリの下だけを開く */
    void snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths();

private:
    // --- errno判定と指紋 ---
    static bool isAttackRejectionErrno(int err);
    static bool isSafeHandlingErrno(int err);
    static QStringList fingerprintTree(const QString &dirPath);
    // --- 攻撃再現fixture ---
    void buildAttackFixture(QTemporaryDir *rootDir, QTemporaryDir *outsideDir,
                            QString *victimPath, QByteArray *sentinel) const;
    static void swapComponentToSymlink(const QTemporaryDir &rootDir,
                                       const QTemporaryDir &outsideDir,
                                       const QStringList &components);
    void verifyOutsideUntouched(const QTemporaryDir &outsideDir, const QByteArray &sentinel,
                                const QStringList &fingerprintBefore) const;
};

#endif // TST_FILESYSTEMHELPERS_H
