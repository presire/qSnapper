/**
 * @file filesystemhelpers_internal.h
 * @brief filesystemhelpersの実装で共有する内部ヘルパーの宣言
 *
 * filesystemhelpers.cpp、filesystemhelpers_beneathroot.cpp、
 * filesystemhelpers_replace.cpp、snapshotoperations_restore_apply.cppが
 * 同じ検証処理を使うため、内部ヘルパーの宣言を1箇所に集めた非公開ヘッダである
 *
 * 公開APIはfilesystemhelpers.h側にだけ置き、本ヘッダはsrc/dbusservice配下からのみ使う
 * 宣言するヘルパーは全てqsnapper::security名前空間に属し、外部リンケージを持つ
 *
 * 呼び出し側は、fdの寿命と同一性を保証した上で使うこと
 *
 * 依存 : Qt Core (QByteArray、QString)、LinuxとPOSIXのAPI (openat2、statx、fstat、setfsuid)
 * QSNAPPER_ENABLE_TEST_METADATA_HOOKSが定義されている場合だけ、filesystemhelpers.hのテスト用フック型を使う
 */
#ifndef QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H
#define QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H

#include <QByteArray>
#include <cerrno>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <functional>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
#include "filesystemhelpers.h"
#endif

// qsnapper::security : symlink追従を拒否する安全なファイル操作群の名前空間
namespace qsnapper::security {

/**
 * @class UniqueFd
 * @brief close()の呼び忘れを防ぐfd (ファイルディスクリプタ) 所有ガード
 *
 * RAII (Resource Acquisition Is Initialization、資源の取得と解放をスコープへ結び付ける手法) により、
 * エラー経路を含む全ての脱出経路でfdを解放する
 * fdを2重にcloseしないよう、コピーとコピー代入を禁止する
 */
class UniqueFd {
public:
    /**
     * @brief 所有権を受け取ったfdを保持する
     * @param fd 所有するfd、所有しない場合は-1
     */
    explicit UniqueFd(int fd = -1)
        : m_fd(fd)
    {
    }

    /** @brief スコープを抜けるときに所有するfdを解放する */
    ~UniqueFd()
    {
        reset();
    }

    /** @brief fdの二重closeを防ぐためコピーを禁止する */
    UniqueFd(const UniqueFd &) = delete;
    /** @brief 所有権を複製できないためコピー代入を禁止する */
    UniqueFd &operator=(const UniqueFd &) = delete;

    /** @brief 所有しているfdを返す (所有権は移さない) */
    int get() const
    {
        return m_fd;
    }

    /** @brief 有効なfdを所有しているか返す */
    bool isValid() const
    {
        return m_fd >= 0;
    }

    /**
     * @brief fdの所有権を呼び出し元へ渡すためガードから切り離す
     * @return 呼び出し元がcloseするfd
     */
    int release()
    {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    /**
     * @brief 所有するfdを閉じて無効に戻す
     *
     * close()がerrnoを上書きしても呼び出し元のエラー処理を乱さないよう、呼び出し前の値へ戻す
     */
    void reset()
    {
        if (m_fd >= 0) {
            const int savedErrno = errno;
            ::close(m_fd);
            m_fd = -1;
            errno = savedErrno;
        }
    }

private:
    int m_fd; // 所有中のfd (無効時は-1)
};

/**
 * @brief 開いたディレクトリを順に取り込み、rootが変更を委ねてよい相手を単一の所有者へ限定できるか表す状態
 *
 * 信頼できると判定できなくなった最初のディレクトリ (非信頼点) 以降では、
 * 全てのディレクトリの所有者が一致し、所有者以外の利用者が中の名前を変更できないことを要求する
 * 所有者以外が名前を変更できるディレクトリでは、検査した名前の先を同じ所有者の別のディレクトリへ入れ替えられるためである
 * グループに書き込みを許すディレクトリ (Fedoraのumask 002による0775など) は、所有者がroot以外で、グループが所有者専用かつ拡張ACLも無い場合だけ許容する
 */
struct DirectoryChainOwnership {
    bool trusted = true;        // ここまで全てroot所有かつグループとその他の書き込み不可
    bool conflict = false;      // 単一の変更主体に限定できないディレクトリを検出済み
    bool hasPrincipal = false;  // 信頼できないと判定した最初のディレクトリの所有者がroot以外
    uid_t principal = 0;        // 非信頼点以降の変更を許す唯一の所有者

    /**
     * @brief 取り込んだディレクトリ全体で、矛盾しない変更主体が得られたかを返す
     * @return 単一の変更主体 (principal) が確定している場合true
     */
    bool principalKnown() const { return hasPrincipal && !conflict; }
};

/**
 * @brief 走査で開いたディレクトリの所有者を順に取り込み、変更主体の混在を検出する
 *
 * 所有者の混在に加えて、所有者以外の利用者が名前を変更できるディレクトリ (directoryDelegatesNamesを参照) もconflictとする
 *
 * @param ownership 基点から累積している状態
 * @param directoryFd 現在開いているディレクトリのfd (拡張ACLの有無を同じinodeで確かめるために使う)
 * @param directoryStat 現在開いているディレクトリのfstat結果
 * @note ScopedFsIdentityでfilesystem identity (fsuidとfsgid) を利用者へ落としている間には呼び出さない
 *       xattr (拡張属性) と/etc/passwd、/etc/groupの読み出し結果が、利用者の権限で変わらないようにするためである
 */
void accumulateChainOwnership(DirectoryChainOwnership &ownership, int directoryFd,
                              const struct stat &directoryStat);

/**
 * @brief rootが変更を委ねられる信頼チェーンまたは単一の変更主体かを判定する
 * @param ownership 走査で集積した状態
 * @return 信頼チェーン全体が安全な場合、または単一の変更主体 (principal) が確定している場合true
 */
bool chainAllowsMutation(const DirectoryChainOwnership &ownership);

/**
 * @brief ローカルのpasswdとgroupを走査した結果を表す
 */
enum class LocalScanResult {
    Exclusive, // 所有者以外にこのgidを主グループとする利用者も、同じgidの重複レコードも無い
    Shared,    // 所有者以外にこのgidを主グループとする利用者がいるか、同じgidの別レコードがある
    Failed,    // 読み取りに失敗したか、判定に必要なレコードが無い
};

/**
 * @brief 所有者とグループのエントリだけから、所有者専用のグループの条件を満たすか判定する
 *
 * 所有者の主グループであること、グループ名が所有者名と一致すること、補助メンバに所有者以外が含まれないことを確かめる
 * usersのような共有の主グループは、名前が一致しないため除く
 * @param owner 所有者のpasswdエントリ
 * @param group 所有グループのgroupエントリ
 * @return 3つの条件を全て満たす場合true
 */
bool isUserPrivateGroupEntry(const struct passwd &owner, const struct group &group);

/**
 * @brief passwd形式のストリームを走査し、所有者以外に指定したgidを主グループとする利用者がいるか調べる
 *
 * gr_memに現れず、主グループとしてだけ属する利用者は、groupのエントリからは分からないため走査する
 * @param stream passwd形式のストリーム (呼び出し側が開いて閉じる)
 * @param owner 所有者のuid
 * @param group 調べるgid
 * @return 他にいなければExclusive、いればShared、読み取りに失敗した場合はFailed
 */
LocalScanResult scanPasswdForOtherPrimaryMember(FILE *stream, uid_t owner, gid_t group);

/**
 * @brief group形式のストリームを走査し、同じgidのレコードがちょうど1件だけか調べる
 *
 * getgrgid_rは同じgidのレコードのうち1件しか返さないため、
 * 別のレコードのメンバがinitgroupsでそのgidを補助グループとして得ることを見落とす
 * そのため、メンバの無いレコードが返された場合でも、同じgidのレコードが他に無いことを確かめる
 * @param stream group形式のストリーム (呼び出し側が開いて閉じる)
 * @param group 調べるgid
 * @return 1件ならExclusive、2件以上ならShared、0件か読み取りに失敗した場合はFailed
 */
LocalScanResult scanGroupForDuplicateGid(FILE *stream, gid_t group);

/**
 * @brief 指定したgroupがownerの所有者専用グループか判定する
 *
 * 所有者専用のグループとは、所有者の主グループで、名前が所有者と同じで、所有者だけが属するグループである
 * グループに書き込みを許すディレクトリ (Fedoraのumask 002による0775など) でも、グループの権限だけを見れば、メンバが所有者だけならグループ経由では所有者以外は名前を変更できない
 * NSSの取得と/etc/passwd、/etc/groupの走査が全て成功し、いずれも所有者専用と示す場合だけtrueを返す
 * どれかが失敗した場合は、共有されている可能性を否定できないためfalseを返す
 * NSSの内容は実行中に変わり得るため、結果は保持せず呼び出すたびに取得し直す
 * @param owner ディレクトリの所有者のuid
 * @param group ディレクトリの所有グループのgid
 * @return 所有者専用のグループと確かめられた場合true
 * @note テスト用フックが設定されている場合は、その結果を返す
 */
bool isUserPrivateGroup(uid_t owner, gid_t group);

/**
 * @brief ディレクトリの中の名前を、所有者以外にも変えさせているか判定する
 *
 * グループだけに書き込みを許すディレクトリで拡張ACL (Access Control List、ファイルごとの利用者やグループの権限) があれば、
 * 名前付きの利用者やグループへ書き込みを許している可能性があるため委譲ありとする
 * 拡張ACLが無い場合は、グループが所有者専用でなければ委譲ありとする
 * グループの権限ビットに書き込みが無ければ、拡張ACLのmaskにも書き込みが含まれず、名前付きの利用者やグループの書き込みは効かない
 * stickyビットは委譲の判定に使わない
 * stickyでも、書き込み権限を持つ利用者は別の場所からディレクトリを空いた名前へ移し入れられるためである
 * @param directoryFd 判定するディレクトリのfd (拡張ACLのxattrを読む)
 * @param directoryStat directoryFdのfstat結果
 * @return 所有者以外が名前を変えられる可能性がある場合true
 * @note errnoは呼び出し前の値に戻す
 *       呼び出し元のエラー処理をxattr取得の失敗で乱さないためである
 */
bool directoryDelegatesNames(int directoryFd, const struct stat &directoryStat);

/**
 * @brief 開いたディレクトリのfdとfstat結果を組にして、所有権の集計へ渡す
 */
struct ObservedDirectory {
    int fd;           // 開いたディレクトリのfd (所有しない)
    struct stat info; // fdのfstat結果
};

/**
 * @brief 「/」から対象ディレクトリ自身までの経路を、1つの所有権の集計へ順に取り込む
 *
 * 対象の親と対象を別々に集計すると、親の所有者が対象の名前を入れ替えられることを見落とすため、1つにまとめる
 * @param ancestors 「/」から対象ディレクトリ自身まで順に並べたもの
 * @return 集計した状態 (対象より下の集計の初期値に使う)
 */
DirectoryChainOwnership accumulateAncestorChainOwnership(const std::vector<ObservedDirectory> &ancestors);

/**
 * @class ScopedFsIdentity
 * @brief mkdiratの間だけfilesystem identityをprincipalへ落とし、作成物を最初からその所有にする
 *
 * Linuxではfsuidが0以外になると、rootの特権 (CAP_CHOWN、CAP_DAC_OVERRIDE、CAP_FOWNER、CAP_FSETIDなど) がeffective集合から外れる
 * rootの権限で保護されたディレクトリを作成したり、差し替えられたroot所有物へ介入したりしないために使う
 * qsnapper_dbus_tにはCAP_SETUIDとCAP_SETGIDが許可されているため、fsuidとfsgidを切り替えられる
 * 復元失敗後に続行すると以後の権限判定が誤るため、identityを0へ戻せなければqFatalで終了する
 *
 * m_ok : fsuidとfsgidの切り替えと照合に成功した場合だけ操作を許可する
 */
class ScopedFsIdentity {
public:
    /**
     * @brief groupを先に切り替えてからuidを切り替え、切り替え結果を検証する
     *
     * fsuidを非rootへ切り替えるとfsgidを変更できなくなるため、groupを先に切り替える
     * @param uid 作成時の所有者
     * @param gid 作成時のグループ
     * @note rootのfilesystem identityからのみ使用し、入れ子にしない
     *       入れ子にすると内側の破棄でidentityが0へ戻り、外側の前提が崩れるためである
     */
    ScopedFsIdentity(uid_t uid, gid_t gid);
    /** @brief 次のroot操作へ影響させないようfsuid、fsgidの順で0へ戻して検証する */
    ~ScopedFsIdentity();
    /** @brief スコープの終了時にidentityを二重に復元しないようコピーを禁止する */
    ScopedFsIdentity(const ScopedFsIdentity &) = delete;
    /** @brief コピーと同じ理由でコピー代入も禁止する */
    ScopedFsIdentity &operator=(const ScopedFsIdentity &) = delete;
    /**
     * @brief 切り替えに失敗した状態でmkdiratしないよう、検証結果を返す
     * @return 指定したuidとgidへ切り替わった場合true
     */
    bool ok() const;

private:
    bool m_ok = false; // fsuidとfsgidの切り替えと照合に成功したか
};

/**
 * @brief 絶対パスを、空の成分を除いた成分列へ分解する
 *
 * 呼び出し側が1成分ずつopenat2へ渡せるようにするため、[/]で区切って分解する
 * 先頭と末尾の[/]、連続する[/]は空の成分として取り除く
 * @param path 分解対象の絶対パス
 * @param components 分解結果の格納先 (呼び出しごとにクリアされる)
 * @return 絶対パスとして分解できた場合true、それ以外はerrno=EINVALでfalse
 */
bool splitAbsolutePath(const QString &path, QStringList *components);

/**
 * @brief 制御文字を含むか判定する
 *
 * 埋め込みNULはsyscall引数の切り詰めを招き、検証したパスと実際に操作するパスがずれるため、パスには一切許容しない
 * C0制御文字とC1制御文字は端末の表示を乱すため、ログやエラーメッセージへそのまま出さないよう拒否する
 * @param value 検査対象の文字列
 * @return U+0000..U+001F、DEL (U+007F)、C1 (U+0080..U+009F) のいずれかを含む場合true
 */
bool containsControlChar(const QString &value);

/**
 * @brief dirfd基点の相対パスを検証し、UTF-8へ符号化する
 *
 * 空パス、絶対パス、[.]/[..]成分、制御文字を拒否する
 * openat系は絶対パスを渡すとdirfdを無視するため、固定したdirfdから意図しない位置へ
 * 解決されるのを入力段階で防ぐ
 * @param relativePath 検証対象の相対パス
 * @param encodedOut 符号化結果の格納先 (nullptrを渡すと符号化しない)
 * @return 有効な相対パスの場合true、それ以外はerrno=EINVALでfalse
 */
bool validateRelativePathAt(const QString &relativePath, QByteArray *encodedOut);

/**
 * @brief ルートディレクトリ[/]を開く
 *
 * プロセスの作業ディレクトリに依存せず絶対パスを解決するため、常に[/]から開く
 * @return 成功時dirfd、失敗時-1 (errno設定、呼び出し側がcloseする)
 */
int openRootDirectory();

/**
 * @brief 基点ディレクトリrootPathをsymlink非追従で開く
 *
 * 単純なopen(rootPath, O_NOFOLLOW)は末尾の成分にしかO_NOFOLLOWが効かず、
 * rootPathの中間成分がsymlinkへ差し替えられていた場合に追従してしまう
 * そのため[/]から1成分ずつsymlink非追従で開き、rootPath自身とその祖先がsymlinkへ差し替えられていても追従しない
 *
 * root実行時は、ancestorOwnershipOutの有無にかかわらず、[/]からrootPath自身までの経路を1つの所有権の集計へ取り込む
 * 経路上の所有者が混在する場合や、経路上で所有者以外の利用者が名前を変更できる場合は、rootPath自体を入れ替えられるためEPERMで拒否する
 * 返すfdは検査に使ったfdそのものであり、判定の後に名前を付け替えられても検査済みのinodeを使い続ける
 * 非rootの実行では、カーネルの権限検査が利用者の権限で働くため、経路を集計せず拒否もしない
 * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
 * @param ancestorOwnershipOut rootPath自身まで検査した所有権の返却先 (root実行時だけ書き込み、rootPathより下の集計の初期値に使う)
 * @return 成功時dirfd、失敗時-1 (root実行時に、経路上の所有者が混在するか、経路上で所有者以外の利用者が名前を変更できる場合はEPERM)
 */
int openRootPathDirectory(const QString &rootPath, DirectoryChainOwnership *ancestorOwnershipOut = nullptr);

/**
 * @brief baseFd配下の成分列を1成分ずつopenat2で辿りながらディレクトリfdを開く
 *
 * 1成分ごとに開いてstatxするため、途中のmount境界とnested btrfs subvolumeのst_dev変化を検出できる
 * nested btrfs subvolumeはmountではないのでRESOLVE_NO_XDEVだけでは止まらない
 * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSのまま失敗する
 *
 * @param baseFd 走査基点のディレクトリfd
 * @param components 相対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @param sameMountOnly mount境界を越える成分をEXDEVで拒否する場合true (復元先の木を辿り、別mountの下へ書き込ませないために使う)
 * @param ownershipOut 同じ走査の基点と各成分から得る所有者状態の返却先 (省略時は集積しない)
 * @param initialOwnership baseFdまで検査済みの所有権 (ownershipOutと一緒に渡した場合はownershipOutへ複写し、baseFdを再集計しない)
 * @return 成功時最終ディレクトリのfd、失敗時-1 (errno設定)
 * @note 同じディレクトリを2回集計しないのは、ACL、passwdとgroupの応答、modeが取得の間に変わり得るためである
 */
int openDirectoryChainAt(int baseFd, const QStringList &components, int componentCount,
                         bool createMissing, mode_t mode, bool sameMountOnly,
                         DirectoryChainOwnership *ownershipOut = nullptr,
                         const DirectoryChainOwnership *initialOwnership = nullptr);

/**
 * @brief [/]起点で成分列をsymlink非追従で辿りながらディレクトリfdを開く
 *
 * 絶対パスを現在の作業ディレクトリに依存せず解決するために使う
 * mount境界は制限しない
 *
 * @param components 絶対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @return 成功時最終ディレクトリのfd、失敗時-1 (errno設定)
 */
int openDirectoryChainNoFollow(const QStringList &components, int componentCount,
                               bool createMissing, mode_t mode);

/**
 * @brief rootPath配下の絶対パスを、rootPathからの相対成分列へ分解する
 *
 * rootPathの外を指すパス、[.]/[..]成分を含むパス、rootPath自身を指して相対部分が空になるパスは拒否する
 *
 * @param rootPath 基点ルートディレクトリ (絶対パス)
 * @param absolutePath 検証対象の宛先絶対パス
 * @param components 相対成分列の格納先 (空の成分は取り除かれ、非空になる)
 * @return rootPath配下の絶対パスとして分解できた場合true、それ以外はerrno=EINVALでfalse
 */
bool splitDestinationComponents(const QString &rootPath, const QString &absolutePath,
                                QStringList *components);

/**
 * @brief rootPath配下で、末尾の成分 (leaf) の親ディレクトリをO_NOFOLLOWで開く
 *
 * root実行時は、[/]からrootPath自身までの経路を検査した所有権を初期値にし、leafの親まで同じ集計を続ける
 * 経路とrootPathより下を別々に集計すると、経路上の所有者による入れ替えを見落とすためである
 *
 * root実行時は、ownershipOutの有無にかかわらずrootPathまでの経路を検査し、ownershipOutを渡した場合はleafの親までの集計も検査する
 * 所有者が混在するか、途中に所有者以外の利用者が名前を変更できるディレクトリがあればEPERMで拒否する
 *
 * @param rootPath 基点ルートディレクトリ
 * @param relativeComponents rootPathからの相対成分列
 * @param createMissing 存在しない中間成分を作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @param leafName 最終要素の返却先
 * @param ownershipOut 所有者状態の返却先 (指定時はrootが変更主体を確定できなければEPERM)
 * @return 成功時親ディレクトリfd、失敗時-1 (errno設定)
 */
int openLeafParentBeneathRoot(const QString &rootPath,
                              const QStringList &relativeComponents,
                              bool createMissing, mode_t mode, QByteArray *leafName,
                              DirectoryChainOwnership *ownershipOut = nullptr);

/**
 * @brief パスの親ディレクトリをO_NOFOLLOWで開き、leaf名も返す
 *
 * パス文字列をそのままopenすると中間のsymlinkに追従するため、絶対パスを成分ごとに分けて開く
 * 存在しない中間ディレクトリはcreateMissingに従って作成する
 *
 * @param path 対象パス
 * @param createMissing 存在しない中間ディレクトリを作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @param leafName 最終要素の返却先
 * @return 成功時親ディレクトリfd、失敗時-1 (errno設定)
 */
int openParentDirectoryNoFollow(const QString &path, bool createMissing, mode_t mode,
                                QByteArray *leafName);

/**
 * @brief statxの結果がmount境界を示すか判定する
 *
 * STATX_ATTR_MOUNT_ROOTはattributes_maskで対応が示された場合のみ信用する
 * btrfsのネストしたsubvolumeはmountではないが、st_devが親と異なるため境界として扱う
 *
 * @param stx 判定対象のstatx結果
 * @param parentDev 親ディレクトリのst_dev
 * @return 境界である場合true
 */
bool isMountBoundary(const struct statx &stx, dev_t parentDev);

/**
 * @brief 親dirfd配下の1エントリをsymlink非追従かつmount境界を越えずに削除する
 *
 * 既に存在しない場合は成功扱いとする
 *
 * 深い階層でスタックとfdを使い切らないよう明示的なスタックで降り、
 * 深さの上限を超えた場合はerrno=ELOOPで失敗する (それまでに削除したエントリは戻らない)
 *
 * root実行時はトップと降下先のfdごとにaccumulateChainOwnershipで判定し、
 * chainAllowsMutationを満たさないディレクトリはerrno=EPERMで拒否して中身を削除しない
 *
 * 所有者以外の利用者が名前を変更できるディレクトリも拒否するのは、削除の途中で中身を同じ所有者の別のディレクトリと入れ替えられるためである
 * 最後の名前によるAT_REMOVEDIRは空ディレクトリだけを削除できるため、非空のディレクトリへ差し替えられていればENOTEMPTYで失敗する
 *
 * @param parentFd 親ディレクトリfd
 * @param parentDev 削除をこのデバイス内に限定するための親のデバイス番号
 * @param entryName 削除対象のleaf名
 * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
 * @return 削除成功時 (対象が既に存在しない場合を含む) true、失敗時はerrno設定済みでfalse
 */
bool removeEntryWithinDeviceAt(int parentFd, dev_t parentDev, const QByteArray &entryName,
                               const DirectoryChainOwnership &parentOwnership);

/**
 * @brief 親dirfdのデバイスを基準に、配下の1エントリをsymlink非追従かつmount境界を越えずに削除する
 *
 * 親ディレクトリのst_devを取得し、配下の削除を同じデバイス内に限定する
 * 実際の削除はremoveEntryWithinDeviceAtへ委譲する
 *
 * @param parentFd 親ディレクトリfd
 * @param entryName 削除対象のleaf名
 * @param parentOwnership 基点から親ディレクトリまでの所有権の状態
 * @return 削除成功時 (対象が既に存在しない場合を含む) true、失敗時はerrno設定済みでfalse
 */
bool removeEntryAt(int parentFd, const QByteArray &entryName,
                   const DirectoryChainOwnership &parentOwnership);

/**
 * @brief 開いたfdがregular fileか検証する
 *
 * 通常ファイル以外を読み書きの対象にしないため、開いたfdの種別を確認する
 *
 * @param fd 検証対象fd
 * @return 通常ファイル (regular file) の場合true、失敗時はerrno設定済みでfalse (regular fileでなければEINVAL)
 */
bool validateRegularFileDescriptor(int fd);

/**
 * @brief 親dirfd配下のleafをregular fileとして読み取りオープンする
 *
 * leafがregular fileであることをfstatat()で確認してから開き、開いた後もfstat()で再確認する
 * O_NONBLOCKでFIFOを開いても待ち状態にならないようにし、O_NOCTTYで端末を制御端末にしない
 *
 * @param parentFd 親ディレクトリfd
 * @param leafName 対象leaf名
 * @return 成功時fd、失敗時は-1 (errno設定)
 */
int openRegularFileReadInParent(int parentFd, const QByteArray &leafName);

/**
 * @brief openat2によるin-root解決で親ディレクトリを開く
 *
 * dirFdを[/]とみなしたin-root解決で、絶対symlinkもdirFdの内側へ収めるため、RESOLVE_IN_ROOTを使用する
 * EAGAINが続く場合は最大3回まで試行する
 *
 * @param dirFd 基点ディレクトリfd
 * @param parentComponents 親までの成分列
 * @return 成功時親ディレクトリfd、失敗時-1 (errno設定)
 */
int openParentDirectoryInRootAtOpenat2(int dirFd, const QStringList &parentComponents);

/**
 * @brief dirfd配下の親ディレクトリをin-root解決で開く
 *
 * dirFdを[/]とみなして解決するため、絶対symlinkの中間成分もdirFdの内側へ収める
 * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSのまま失敗する
 * leafが1成分だけの場合はdirFdと同じディレクトリを指すfdを返す
 *
 * @param dirFd 基点ディレクトリfd
 * @param components leaf成分を含む相対パス成分列
 * @param leafNameOut 最終成分名の返却先 (nullptrを渡すと返さない)
 * @return 成功時親ディレクトリfd、失敗時は-1 (errno設定、呼び出し側がcloseする)
 */
int openParentDirectoryInRootAt(int dirFd, const QStringList &components,
                                QByteArray *leafNameOut);

/**
 * @brief 単体テスト専用の内部入口
 *
 * replaceRegularFileAtとreplaceSymlinkAtが一時objectをrenameする直前に呼ぶフックを保持する
 * フックが空の場合、呼び出し側はrenameをそのまま行う
 */
namespace detail {
/**
 * @brief rename直前フックの保存先
 * @return フックへの参照
 */
std::function<void(int, const QByteArray &)> &beforeReplaceRenameHook();

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
/**
 * @brief 所有者専用グループの判定を置き換えるテスト用関数を返す
 * @return 置き換え先の関数、設定されていない場合はnullptr
 */
UserPrivateGroupCheck userPrivateGroupCheck();
#endif
} // namespace detail

} // namespace qsnapper::security

#endif // QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H
