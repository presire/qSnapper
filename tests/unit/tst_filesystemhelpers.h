/**
 * @file tst_filesystemhelpers.h
 * @brief filesystemhelpers単体テストの共通クラスとテストケースの宣言
 *
 * 複数のソースファイルを1つの実行体へまとめるため、クラス宣言のみを本ヘッダに配置して、
 * 各テスト本体は検証対象ごとにtst_filesystemhelpers_*.cppへ分けている
 *
 * mocの生成はAUTOMOCが本ヘッダを解析して行うため、宣言は必ずここへ集約する
 *
 * 利用する側 : tst_filesystemhelpers*.cppの各テスト本体と、QTEST_APPLESS_MAINを持つtst_filesystemhelpers.cpp
 * 依存 : filesystemhelpers.h(qsnapper::security)、Qt Test、検証に使用する実ファイルシステム
 */
#ifndef TST_FILESYSTEMHELPERS_H
#define TST_FILESYSTEMHELPERS_H

#include <QByteArray>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

/**
 * @class TestFilesystemHelpers
 * @brief qsnapper::securityの安全なファイル操作群を検証するテストクラス
 *
 * symlink差し替え攻撃を拒否することと、正規のパスを受け入れることを両面から確認する
 * 検証は実ファイルシステム上の一時ディレクトリだけで行い、実システムのファイルへは触れない
 * private slotsの宣言順はQTestの実行順になるため、並べ替えない
 *
 * isAttackRejectionErrno : 攻撃の拒否を示すerrnoかを判定する
 * isSafeHandlingErrno : 安全側の処理として扱えるerrnoかを判定する
 * fingerprintTree : 攻撃先が無変更であることの検証に使う一覧の指紋を作る
 * buildAttackFixture : rootとoutsideからなる攻撃再現用の一時構成を作る
 * swapComponentToSymlink : 解決済みの成分をoutsideへのsymlinkへ差し替える
 * verifyOutsideUntouched : outside側が削除も改変もされていないことを検証する
 */
class TestFilesystemHelpers : public QObject
{
    Q_OBJECT

private slots:
    /** @brief 各テストの終了時にmetadata適用のテスト用フックを解除し、後続のテストへ状態を持ち越さない */
    void cleanup();

    /** @brief 存在しない親ディレクトリを再帰的に作成できることを確認する */
    void safeMkpathCreatesNestedDirectories();
    /** @brief 途中のsymlink成分を辿らず、その先へディレクトリを作らないことを確認する */
    void safeMkpathRejectsSymlinkComponents();
    /** @brief 作成先そのものがsymlinkの場合に拒否し、リンク先へディレクトリを作らないことを確認する */
    void safeMkpathRejectsTargetSymlink();
    /** @brief symlinkの通常ファイルを開かず、リンク先の内容を読ませないことを確認する */
    void safeOpenRegularFileReadRejectsSymlink();
    /** @brief symlinkを含むディレクトリパスを開かないことを確認する */
    void safeOpenDirectoryRejectsSymlinkPath();
    /** @brief 2048バイトの長いリンク先も切り詰めずに取得することを確認する */
    void safeReadLinkNoFollowHandlesLargeTarget();
    /** @brief symlinkを辿らず、リンク自体の属性を返すことを確認する */
    void safeLstatReportsSymlinkItself();
    /** @brief root直下の単一ファイルを削除できることを確認する */
    void safeRemoveAllRemovesSingleFile();
    /** @brief 木を削除するときsymlinkを辿らず、リンク先を消さないことを確認する */
    void safeRemoveAllRemovesTreeWithoutFollowingSymlink();
    /** @brief 途中の成分がsymlinkの場合、その先を削除せず拒否することを確認する */
    void safeRemoveAllRejectsSymlinkIntermediateComponent();
    /** @brief mount境界を越えて、別mountの内容を削除しないことを確認する */
    void safeRemoveAllStopsAtMountBoundary();
    /** @brief 削除対象自身がmount pointの場合は拒否することを確認する */
    void safeRemoveAllRefusesMountPointTarget();
    /** @brief 同じファイルシステムを指すbind mountでも、mount境界として削除を止めることを確認する */
    void safeRemoveAllStopsAtSameFilesystemBindMount();
    /** @brief mountではないnested btrfs subvolumeの境界でも、削除を止めることを確認する */
    void safeRemoveAllStopsAtNestedBtrfsSubvolume();

    // --- 復元時のmetadata保持とrename前後の差し替え検出 ---
    /** @brief 新しいinodeの内容で置き換え、古いinodeを残さないことを確認する */
    void replaceRegularFileAtReplacesContentWithNewInode();
    /** @brief 差し替え後もsetuidとsetgidのbitを保持することを確認する */
    void replaceRegularFileAtPreservesSetuidAndSetgid();
    /** @brief 復元元のaccess ACLを復元先へ複製することを確認する */
    void replaceRegularFileAtCopiesAccessAcl();
    /** @brief 復元先から継承したdefault ACLを、復元元に対応するACLがない場合に削除することを確認する */
    void replaceRegularFileAtDropsInheritedDefaultAcl();
    /** @brief 復元先ディレクトリのdefault ACLと、その配下のACLを同期することを確認する */
    void applyRestoredMetadataSyncsDirectoryAcls();
    /** @brief 復元先ディレクトリから継承したdefault ACLの名前付きエントリが、modeを広げる前に削除されていることを確認する */
    void replaceRegularFileAtSyncsInheritedAclBeforeModeRestore();
    /** @brief 復元元にACLがある場合、modeを広げる前に、継承したACLが復元元のACLへ置き換わっていることを確認する */
    void replaceRegularFileAtSyncsSourceAclBeforeModeRestore();
    /** @brief ACLの同期をmodeの適用より先に行っても、最終的なmodeとACLが復元元と一致することを確認する */
    void replaceRegularFileAtKeepsFinalAclAndMode();
    /** @brief access ACLを同期できない場合に、作成時の0600からmodeを広げないことを確認する */
    void applyRestoredMetadataDoesNotWidenModeOnAccessAclFailure();
    /** @brief metadataの失敗時も差し替えを続ける経路(relaxed)で、ACLの同期に失敗してもmodeを広げないことを確認する */
    void replaceRegularFileAtRelaxedPathDoesNotWidenModeOnAclFailure();
    /** @brief metadataの失敗時に差し替えを中止する経路(strict)で、ACLの同期に失敗すると復元先を差し替えないことを確認する */
    void replaceRegularFileAtStrictPathDoesNotRenameOnAclFailure();
    /** @brief ディレクトリのdefault ACLを同期できない場合に、modeを広げないことを確認する */
    void applyRestoredMetadataDoesNotWidenDirectoryModeOnDefaultAclFailure();
    /** @brief 復元時に作成する親ディレクトリでも、modeを広げる前に継承ACLが削除されていることを確認する */
    void safeCreateParentDirectoriesSyncsAclBeforeModeRestore();
    /** @brief ACLとmodeの後に設定するfile capabilityが、最終状態に残ることを確認する (CAP_SETFCAPが無い環境ではスキップする) */
    void replaceRegularFileAtPreservesFileCapability();
    /** @brief 一時名から最終名へrenameするまでの差し替えを検出することを確認する */
    void replaceRegularFileAtDetectsTemporaryNameSwap();
    /** @brief 親ディレクトリの差し替え後も、解決済みの親ディレクトリを使って差し替えることを確認する */
    void replaceRegularFileAtKeepsResolvedParentAfterParentSwap();
    /** @brief symlinkの差し替えでもmetadataを適用し、リンク先の差し替えを検出することを確認する */
    void replaceSymlinkAtAppliesMetadataAndDetectsSwap();
    /** @brief metadataを要求したsymlinkの差し替えで、変更前のリンク先を書き換えないことを確認する */
    void replaceSymlinkAtRequiringMetadataLeavesLiveUntouched();
    /** @brief SELinuxのcontextを、userの部分だけ置き換えることを確認する */
    void selinuxContextUserIsReplacedOnlyInUserPart();

    // --- beneath-root指定の宛先解決と、その回避の検証 ---
    /** @brief root配下の入れ子の宛先を、rootからの相対パスへ分割することを確認する */
    void splitDestinationBeneathRootAcceptsNestedDestination();
    /** @brief rootの外へ出る..を含むパスを分割しないことを確認する */
    void splitDestinationBeneathRootRejectsDotDotTraversal();
    /** @brief rootの外を指すパスを分割しないことを確認する */
    void splitDestinationBeneathRootRejectsOutsideRoot();
    /** @brief 空文字列やroot自身など、宛先として不正なパスを分割しないことを確認する */
    void splitDestinationBeneathRootRejectsMalformedPaths();
    /** @brief 入れ子ディレクトリを、各成分を確認しながら開くことを確認する */
    void safeOpenDirectoryBeneathRootWalksNestedPath();
    /** @brief 不足するディレクトリを、指定したmodeで作成することを確認する */
    void safeOpenDirectoryBeneathRootCreatesMissing();
    /** @brief 途中のsymlink成分を辿らず、rootの外のディレクトリを開かないことを確認する */
    void safeOpenDirectoryBeneathRootRejectsSymlinkComponent();
    /** @brief 親ディレクトリをsymlinkへ差し替える攻撃があっても、書き込みを拒否することを確認する */
    void beneathRootWriteRejectsParentSymlinkSwap();
    /** @brief 親ディレクトリのsymlink差し替え後は、renameを拒否することを確認する */
    void beneathRootRenameAsideRejectsParentSymlinkSwap();
    /** @brief 親ディレクトリのsymlink差し替え後は、削除を拒否することを確認する */
    void beneathRootRemoveRejectsParentSymlinkSwap();
    /** @brief 葉がrootの外を指すsymlinkの場合、書き込みでリンク先を変更しないことを確認する */
    void beneathRootWriteRejectsLeafSymlinkToExternalFile();
    /** @brief 存在しないパスを指すsymlink成分があっても、書き込みでリンク先を作らないことを確認する */
    void beneathRootWriteRejectsDanglingSymlinkComponent();
    /** @brief ディレクトリであるべき成分が通常ファイルの場合、書き込みを拒否することを確認する */
    void beneathRootWriteRejectsRegularFileAsDirectoryComponent();
    /** @brief 正規の入れ子パスへ、指定したmodeでファイルを書き込めることを確認する */
    void beneathRootHappyPathWritesNestedFileWithMode();
    /** @brief root配下のrenameを、rootの外へ出さずに実行できることを確認する */
    void beneathRootRenameAsideMovesWithinRoot();
    /** @brief rename先が既存のファイルやディレクトリと重なる組を提供する */
    void beneathRootRenameAsideDoesNotReplaceExisting_data();
    /** @brief renameが既存の宛先を置き換えず、失敗することを確認する */
    void beneathRootRenameAsideDoesNotReplaceExisting();
    /** @brief 不足する親ディレクトリを、復元元のowner・mode・ACLで作成することを確認する */
    void beneathRootMissingParentsFollowSnapshotMetadata();
    /** @brief 既存の親ディレクトリのowner・mode・ACLを変更しないことを確認する */
    void beneathRootMissingParentsDoNotModifyExistingParents();
    /** @brief live側の親がsymlinkへ差し替わっている場合、親ディレクトリを作成しないことを確認する */
    void beneathRootMissingParentsRejectLiveSymlinkComponent();
    /** @brief 一覧の追加・削除・改名を指紋の変化として検出することを確認する */
    void snapshotListFingerprintDetectsListChanges();
    /** @brief 先頭に0が付く番号のsnapshotでも、info.xmlの内容が変われば指紋が変わることを確認する */
    void snapshotListFingerprintDetectsLeadingZeroEntries();
    /** @brief root所有でグループと他者に書き込み権限がない場合だけ信頼する組を提供する */
    void parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode_data();
    /** @brief root所有かつグループと他者に書き込み権限がない場合だけ信頼することを確認する */
    void parentDirectoryMetadataSafetyIsJudgedFromOwnerAndMode();
    /** @brief 信頼できる親の下では、不足する親を復元元のmetadataで作成することを確認する (euid 0のときだけ実行する) */
    void beneathRootMissingParentsFollowTrustedParentPolicy();
    /** @brief 所有者以外が書き込める親の下でも、復元元の探索権限の制限を緩めないことを確認する */
    void beneathRootUntrustedParentKeepsSnapshotSearchRestriction();
    /** @brief 信頼できるディレクトリの連鎖の下では、復元元のmetadataで親を作成することを確認する */
    void beneathRootTrustedChainCreatesParentWithSourceMetadata();
    /** @brief 信頼できない連鎖の下では、親を実行ユーザーの所有として作成することを確認する */
    void beneathRootUntrustedChainCreatesParentsAsPrincipal();
    /** @brief 信頼できない連鎖で復元元が他ユーザー所有の場合、そのownerを適用しないことを確認する */
    void beneathRootUntrustedChainRejectsForeignSourceOwner();
    /** @brief 信頼できない連鎖で復元元のownerが混在する場合、単一のownerとして扱わないことを確認する */
    void beneathRootUntrustedChainRejectsMixedOwnership();
    /** @brief 解決済みの宛先が後からsymlinkへ差し替えられても、書き込みを拒否することを確認する */
    void beneathRootStaleResolutionStillRejected();
    /** @brief 差し替えを繰り返しても、解決に使うfile descriptorが漏れないことを確認する */
    void beneathRootRepeatedSwapsDoNotLeakDescriptors();
    /** @brief 空文字列やroot自身などの不正な入力で、攻撃拒否のerrnoを返すことを確認する */
    void beneathRootHandlesMalformedInputsSafely();
    /** @brief rootPathの祖先にsymlinkが含まれる場合、rootの特定を拒否することを確認する */
    void beneathRootRejectsSymlinkInRootPathAncestry();
    /** @brief 深さの上限を超える木では、再帰削除を途中で止めることを確認する */
    void beneathRootRemovalStopsAtDepthLimit();
    /** @brief 解決の途中でmount境界を越える場合、EXDEVで拒否することを確認する */
    void beneathRootResolutionRefusesMountBoundaries();
    /** @brief トップと降下先の権限境界を、正常系と同じ入口で検証する組を提供する */
    void beneathRootRemovalGuardsDirectoryOwnership_data();
    /** @brief 他者のディレクトリへ降りず、本人の木と非ディレクトリだけを削除する */
    void beneathRootRemovalGuardsDirectoryOwnership();

    // --- dirfd相対ソース解決(staged restoreのsnapshot pin) ---
    /** @brief dirfdを起点に相対パスを解決することを確認する */
    void safeLstatAtResolvesRelativeToDirFd();
    /** @brief 絶対パスと..による遡りを拒否することを確認する */
    void safeLstatAtRejectsAbsoluteAndTraversalPaths();
    /** @brief symlinkの葉と通常ファイル以外を開かないことを確認する */
    void safeOpenRegularFileReadAtRejectsSymlinkLeafAndNonRegular();
    /** @brief dirfdを起点にリンク先を読むことを確認する */
    void safeReadLinkNoFollowAtReadsRelativeToDirFd();
    /** @brief 絶対パスや..などの不正なパスを拒否することを確認する */
    void safeReadLinkNoFollowAtRejectsInvalidPaths();

    // --- created entry削除前の復元元不在確認 ---
    /** @brief 復元元に存在するパスを、不在と確認しないことを確認する */
    void isConfirmedAbsentAtRejectsPathsPresentInSource();
    /** @brief 存在しないと断定できる場合だけ、不在と確認することを確認する */
    void isConfirmedAbsentAtConfirmsOnlyDefiniteAbsence();
    /** @brief 途中のsymlinkを辿って不在を確認しないことを確認する */
    void isConfirmedAbsentAtRefusesIntermediateSymlinks();

    // --- 復元計画の検証で使う、木に含まれるかの観測 ---
    /** @brief 比較処理と同じ基準で、パスの有無を分類することを確認する */
    void observePathNoFollowAtClassifiesPresenceLikeComparison();
    /** @brief mount境界を越えるパスを拒否することを確認する */
    void observePathNoFollowAtRejectsMountBoundaries();

    // --- dirfd相対ソース解決での中間成分のin-rootハードニング(RESOLVE_IN_ROOT) ---
    /** @brief symlinkのリンク先が絶対パスでも、rootの内側へ留めて開くことを確認する */
    void safeOpenRegularFileReadAtKeepsAbsoluteIntermediateSymlinkInsideRoot();
    /** @brief symlinkのリンク先の..がrootより上へ遡らないよう、rootで頭打ちにすることを確認する */
    void safeOpenRegularFileReadAtClampsDotDotInSymlinkTargetAtRoot();
    /** @brief 正規の相対symlinkは、中間成分として引き続き辿れることを確認する */
    void safeReadHelpersKeepLegitimateRelativeIntermediateSymlinkWorking();
    /** @brief 中間のsymlinkを辿った後も、葉のsymlinkを解決しないことを確認する */
    void safeHelpersKeepLeafSymlinkUnresolvedThroughIntermediateSymlink();
    /** @brief 中間にsymlinkを含む復元元のsnapshotを、読み取りヘルパーが扱えることを確認する */
    void restoreReadHelpersHandleSnapshotWithIntermediateSymlinks();

    // --- in-root解決がsymlinkループを打ち切ることの確認 ---
    /** @brief in-root解決がsymlinkの循環を検出して打ち切ることを確認する */
    void inRootRejectsSymlinkLoop();

    // --- 復元先walkのmount境界(nested btrfs subvolume) ---
    /** @brief nested btrfs subvolumeをmount境界として拒否することを確認する */
    void nestedSubvolumeIsRejected();
    /** @brief 途中のsymlink成分を拒否することを確認する */
    void symlinkedMiddleComponentIsRejected();
    /** @brief mount境界ではない通常の入れ子ディレクトリは開けることを確認する */
    void plainNestedDirectoryIsOpened();
    /** @brief 同一mountだけに限らないwalkでは、nested subvolumeを境界として拒否しないことを確認する */
    void nestedSubvolumeAllowedWithoutSameMountOnly();

    // --- 復元元snapshotのmount前後の解決(.snapshots/<番号>/snapshot) ---
    /** @brief 正しい木と、差し替え・所有者・種別の異常を対比する行を提供する */
    void snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths_data();
    /** @brief symlinkを辿らず、rootが管理する.snapshotsと番号ディレクトリの下だけを開く */
    void snapshotDirectoryBeneathSubvolumeRefusesUntrustedPaths();

    // --- 所有者以外の利用者が名前を変更できるディレクトリの下での変更の拒否 ---
    /** @brief 所有者専用のグループの条件を満たす組と満たさない組を対比する行を提供する */
    void userPrivateGroupEntryRequiresExclusiveMembership_data();
    /** @brief 所有者の主グループ、名前の一致、補助メンバが所有者だけという条件を全て満たす場合だけ
     *         所有者専用のグループとみなす
     */
    void userPrivateGroupEntryRequiresExclusiveMembership();
    /** @brief 共有の有無と読み取りの失敗を含むpasswdとgroupの内容を提供する */
    void localAccountScanDetectsSharing_data();
    /** @brief 走査が共有を見落とさず、読めない場合は失敗を返すことを確認する */
    void localAccountScanDetectsSharing();
    /** @brief 祖先の所有者とmodeとACLの並びを提供する */
    void chainOwnershipRefusesDelegatedNames_data();
    /** @brief 所有者以外の利用者が名前を変更できるディレクトリを含む祖先を、単一の所有者の管理下とみなさない */
    void chainOwnershipRefusesDelegatedNames();
    /** @brief NSS(ユーザー情報の検索元を切り替える仕組み)に登録されていないuidとgidを、所有者専用のグループとして扱わない */
    void userPrivateGroupRejectsUnresolvableIds();
    /** @brief rootPathを「/」まで遡る祖先の並びを提供する */
    void ancestorChainOwnershipCoversRootPath_data();
    /** @brief rootPath自身とその祖先を、1つの集計でまとめて判定することを確認する */
    void ancestorChainOwnershipCoversRootPath();
    /** @brief 移動元と移動先の親ディレクトリの組を提供する */
    void renameBeneathRootRefusesSharedParents_data();
    /** @brief 移動元と移動先のどちらかの親で所有者以外の利用者が名前を変更できる場合、復元先のrootとしてrenameしない */
    void renameBeneathRootRefusesSharedParents();

private:
    // --- errno判定と指紋 ---
    /** @brief 攻撃を拒否した結果を示すerrnoかを判定する
     * @param err 判定するerrno
     * @return 攻撃の拒否を示すerrnoの場合true
     */
    static bool isAttackRejectionErrno(int err);
    /** @brief 安全側の処理として扱えるerrnoかを判定する
     * @param err 判定するerrno
     * @return 攻撃を拒否しつつ安全に処理できたerrnoの場合true
     */
    static bool isSafeHandlingErrno(int err);
    /** @brief ディレクトリ木の内容を比較可能な一覧へ変換する
     * @param dirPath 一覧を作るディレクトリ
     * @return symlinkを辿らずに集めた相対パスの並び(比較用の指紋)
     */
    static QStringList fingerprintTree(const QString &dirPath);
    // --- 攻撃再現fixture ---
    /** @brief rootとoutsideからなる攻撃再現用の構成を作る
     * @param rootDir 復元先に見立てたroot
     * @param outsideDir rootの外に置く攻撃先
     * @param victimPath root配下の攻撃対象パスを受け取る
     * @param sentinel outside側へ書き込むsentinelの内容を受け取る
     */
    void buildAttackFixture(QTemporaryDir *rootDir, QTemporaryDir *outsideDir,
                            QString *victimPath, QByteArray *sentinel) const;
    /** @brief 解決済みのディレクトリ成分を、outsideを指すsymlinkへ差し替える
     * @param rootDir 差し替える成分を含むroot
     * @param outsideDir symlinkのリンク先にするroot外のディレクトリ
     * @param components rootからの相対パスを成分ごとに並べたもの
     */
    static void swapComponentToSymlink(const QTemporaryDir &rootDir,
                                       const QTemporaryDir &outsideDir,
                                       const QStringList &components);
    /** @brief outside側が削除も改変もされていないことを検証する
     * @param outsideDir 攻撃先のディレクトリ
     * @param sentinel 事前に書き込んだsentinelの内容
     * @param fingerprintBefore 攻撃前のfingerprintTreeの結果
     */
    void verifyOutsideUntouched(const QTemporaryDir &outsideDir, const QByteArray &sentinel,
                                const QStringList &fingerprintBefore) const;
};

#endif // TST_FILESYSTEMHELPERS_H
