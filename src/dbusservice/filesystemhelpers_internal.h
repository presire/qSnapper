/**
 * @file filesystemhelpers_internal.h
 * @brief filesystemhelpers系の共有内部helper宣言
 *
 * 各filesystemhelpers_*.cppから使う内部helperを1箇所に集めた非公開ヘッダである
 * 公開APIはfilesystemhelpers.h側にだけ置き、本ヘッダの内容はsrc/dbusservice配下でのみ使う
 * 宣言するhelperは全てqsnapper::security名前空間に属し、外部リンケージを持つ
 * 呼び出し側は親dirfdの寿命と同一性を保証した上で使うこと
 */
#ifndef QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H
#define QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// qsnapper::security : symlink追従を拒否する安全なファイル操作群の名前空間
namespace qsnapper::security {

/**
 * @class UniqueFd
 * @brief close()漏れを防ぐためのfd所有ガード (RAII)
 *
 * コピー禁止
 * エラー経路を含めて全てのfd解放を保証する
 */
class UniqueFd {
public:
    explicit UniqueFd(int fd = -1)
        : m_fd(fd)
    {
    }

    ~UniqueFd()
    {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    int get() const
    {
        return m_fd;
    }

    bool isValid() const
    {
        return m_fd >= 0;
    }

    void reset()
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd; // 所有中のfd (無効時は-1)
};

/**
 * @brief 絶対パスを各成分へ分解する
 * @param path 分解対象の絶対パス
 * @param components 分解結果の格納先
 * @return 絶対パスとして分解できた場合true
 */
bool splitAbsolutePath(const QString &path, QStringList *components);

/**
 * @brief 制御文字を含むか判定する
 *
 * 埋め込みNULはsyscall引数の切り詰めを招くため、パスには一切許容しない
 * @param value 検査対象の文字列
 * @return C0 / DEL / C1のいずれかを含む場合true
 */
bool containsControlChar(const QString &value);

/**
 * @brief dirfd相対パスを検証してUTF-8へ符号化する
 *
 * 空パス・絶対パス・[.] / [..]成分・制御文字を拒否する
 * openat系は絶対パスを渡すとdirfdを無視するため、pin済みdirfdから意図しない位置へ
 * 解決されるのを入力段階で防ぐ
 * @param relativePath 検証対象の相対パス
 * @param encodedOut 符号化結果の格納先 (省略可)
 * @return 有効な相対パスの場合true
 */
bool validateRelativePathAt(const QString &relativePath, QByteArray *encodedOut);

/**
 * @brief ルートディレクトリ[/]を開く
 * @return 成功時dirfd、失敗時-1
 */
int openRootDirectory();

/**
 * @brief 基点ディレクトリrootPathを全成分O_NOFOLLOWで開く
 *
 * 単純なopen(rootPath, O_NOFOLLOW)は末尾成分にしかO_NOFOLLOWが効かず
 * rootPathの中間成分がsymlink化されていた場合に追従してしまう
 * そのため既存のsafeOpenDirectory() ([/]起点の成分単位no-follow walk) へ委譲し
 * root祖先の差し替えも拒否する
 * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
 * @return 成功時dirfd、失敗時-1
 */
int openRootPathDirectory(const QString &rootPath);

/**
 * @brief baseFd配下の成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く
 *
 * baseFdの所有は取らない (openat(baseFd, [.])で同一ディレクトリの所有fdを得る)
 * @param baseFd 走査基点のディレクトリfd
 * @param components 相対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @return 成功時最終ディレクトリのfd、失敗時-1
 */
int openDirectoryChainAtNoFollow(int baseFd, const QStringList &components,
                                 int componentCount, bool createMissing, mode_t mode);

/**
 * @brief openat2によるin-root解決で成分列を辿る
 *
 * EAGAIN時は最大3回だけ再試行する
 * @param baseFd 走査基点のディレクトリfd
 * @param components 相対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @return 成功時最終ディレクトリのfd、失敗時-1
 */
int openDirectoryChainAtOpenat2(int baseFd, const QStringList &components,
                                int componentCount, bool createMissing, mode_t mode);

/**
 * @brief openat2を試み、非対応時はフォールバックで成分列を辿る
 * @param baseFd 走査基点のディレクトリfd
 * @param components 相対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @return 成功時最終ディレクトリのfd、失敗時-1
 */
int openDirectoryChainAt(int baseFd, const QStringList &components, int componentCount,
                         bool createMissing, mode_t mode);

/**
 * @brief [/]起点で成分列をO_NOFOLLOWで辿りながらディレクトリfdを開く
 * @param components 絶対パス成分列
 * @param componentCount 辿る成分数
 * @param createMissing 存在しない成分をmkdirat()で作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @return 成功時最終ディレクトリのfd、失敗時-1
 */
int openDirectoryChainNoFollow(const QStringList &components, int componentCount,
                               bool createMissing, mode_t mode);

/**
 * @brief rootPathとabsolutePathを成分列へ分解する
 * @param rootPath 基点ルートディレクトリ (絶対パス)
 * @param absolutePath 検証対象の宛先絶対パス
 * @param components 分解結果の格納先
 * @return 両方とも絶対パスとして分解できた場合true
 */
bool splitDestinationComponents(const QString &rootPath, const QString &absolutePath,
                                QStringList *components);

/**
 * @brief rootPath配下のleaf親ディレクトリをO_NOFOLLOWで開く
 * @param rootPath 基点ルートディレクトリ
 * @param relativeComponents rootPathからの相対成分列
 * @param createMissing 存在しない中間成分を作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @param leafName 最終要素の返却先
 * @return 成功時親ディレクトリfd、失敗時-1
 */
int openLeafParentBeneathRoot(const QString &rootPath,
                              const QStringList &relativeComponents,
                              bool createMissing, mode_t mode, QByteArray *leafName);

/**
 * @brief パスの親ディレクトリをO_NOFOLLOWで開き、leaf名も返す
 * @param path 対象パス
 * @param createMissing 親ディレクトリが無い場合に作成するか
 * @param mode createMissingがtrueの場合の作成モード
 * @param leafName 最終要素の返却先
 * @return 成功時親ディレクトリfd、失敗時-1
 */
int openParentDirectoryNoFollow(const QString &path, bool createMissing, mode_t mode,
                                QByteArray *leafName);

/**
 * @brief statxの結果がmount境界を示すか判定する
 *
 * STATX_ATTR_MOUNT_ROOTはattributes_maskで対応が示された場合のみ信用する
 * btrfsのネストしたsubvolumeはmountではないが、st_devが親と異なるため境界として扱う
 * @param stx 判定対象のstatx結果
 * @param parentDev 親ディレクトリのst_dev
 * @return 境界である場合true
 */
bool isMountBoundary(const struct statx &stx, dev_t parentDev);

/**
 * @brief 親dirfd配下の1エントリをsymlink非追従かつmount境界を越えずに削除する
 *
 * 既に存在しない場合は成功扱いとする
 * @param parentFd 親ディレクトリfd
 * @param parentDev 削除範囲を閉じるための親デバイス番号
 * @param entryName 削除対象のleaf名
 * @return 削除成功時true
 */
bool removeEntryWithinDeviceAt(int parentFd, dev_t parentDev, const QByteArray &entryName);

/**
 * @brief 親dirfd配下の1エントリをsymlink非追従かつmount境界を越えずに削除する
 * @param parentFd 親ディレクトリfd
 * @param entryName 削除対象のleaf名
 * @return 削除成功時true
 */
bool removeEntryAt(int parentFd, const QByteArray &entryName);

/**
 * @brief 開いたfdがregular fileか検証する
 * @param fd 検証対象fd
 * @return regular fileの場合true
 */
bool validateRegularFileDescriptor(int fd);

/**
 * @brief 親dirfd配下のleafをregular fileとして読み取りオープンする
 *
 * leafがregular fileであることをfstatat()で確認してから開き
 * 開いた後もfstat()で再確認する
 * @param parentFd 親ディレクトリfd
 * @param leafName 対象leaf名
 * @return 成功時fd、失敗時-1
 */
int openRegularFileReadInParent(int parentFd, const QByteArray &leafName);

/**
 * @brief openat2によるin-root解決で親ディレクトリを開く
 * @param dirFd 基点ディレクトリfd
 * @param parentComponents 親までの成分列
 * @return 成功時親ディレクトリfd、失敗時-1
 */
int openParentDirectoryInRootAtOpenat2(int dirFd, const QStringList &parentComponents);

/**
 * @brief fdスタックによるフォールバックでin-root解決し親ディレクトリを開く
 *
 * カーネル5.6未満などopenat2非対応環境向けの経路である
 * 中間symlinkは40回まで辿り、超過時はELOOPで失敗する
 * @param dirFd 基点ディレクトリfd
 * @param parentComponents 親までの成分列
 * @return 成功時親ディレクトリfd、失敗時-1
 */
int openParentDirectoryInRootAtNoFollow(int dirFd, const QStringList &parentComponents);

/**
 * @brief dirfd配下の親ディレクトリをin-root解決で開く
 *
 * openat2対応時はopenat2経路を使い、ENOSYS時のみフォールバックへ切り替える
 * snapshot dirfdは[/]のbtrfs snapshotであるため、絶対symlinkの中間成分もroot内解決に収める
 * @param dirFd 基点ディレクトリfd
 * @param components leaf成分を含む相対パス成分列
 * @param leafNameOut 最終成分名の返却先 (省略可)
 * @param forceNoOpenat2 trueならopenat2を使わずフォールバック経路を強制する (単体テスト用)
 * @return 成功時親ディレクトリfd、失敗時-1
 */
int openParentDirectoryInRootAt(int dirFd, const QStringList &components,
                                QByteArray *leafNameOut,
                                bool forceNoOpenat2 = false);

/**
 * @brief 単体テスト専用の内部入口
 *
 * replaceRegularFileAt / replaceSymlinkAtが一時objectをrenameする直前に呼ぶフックを保持する
 * 空のstd::functionを渡すと解除する
 */
namespace detail {
/**
 * @brief rename直前フックの保存先
 * @return フックへの参照
 */
std::function<void(int, const QByteArray &)> &beforeReplaceRenameHook();
} // namespace detail

} // namespace qsnapper::security

#endif // QSNAPPER_FILESYSTEMHELPERS_INTERNAL_H
