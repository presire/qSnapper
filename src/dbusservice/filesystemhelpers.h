#ifndef QSNAPPER_FILESYSTEMHELPERS_H
#define QSNAPPER_FILESYSTEMHELPERS_H

#include <QString>
#include <QByteArray>
#include <sys/stat.h>
#include <sys/types.h>

namespace qsnapper::security {

    /**
     * @brief シンボリックリンクを辿らずにディレクトリ階層を作成する
     *
     * 各パス成分を openat(..., O_DIRECTORY | O_NOFOLLOW) で辿るため、中間成分のsymlink差し替えに対して強い
     */
    bool safeMkpath(const QString &path, mode_t mode = 0755);

    /**
     * @brief シンボリックリンクを辿らずに既存ディレクトリを開く
     *
     * 各パス成分を openat(..., O_DIRECTORY | O_NOFOLLOW) で辿り、最終ディレクトリのfdを返す。
     *
     * @return 成功時は file descriptor、失敗時は -1。
     */
    int safeOpenDirectory(const QString &path);

    /**
     * @brief 通常ファイルを安全に新規/上書きオープンする
     *
     * 親ディレクトリをO_NOFOLLOWで開いた後、leafを openat() で開き、fstat() でregular fileであることを再確認する
     *
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileWrite(const QString &path, mode_t mode);

    /**
     * @brief 通常ファイルを安全に読み取りオープンする
     *
     * 親ディレクトリをO_NOFOLLOWで開いた後、leafを openat() で開き、fstat() でregular fileであることを再確認する
     *
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileRead(const QString &path);

    /**
     * @brief シンボリックリンクを辿らずにパスを再帰削除する
     *
     * fstatat(..., AT_SYMLINK_NOFOLLOW) / unlinkat() を使い、tree walk中にリンク先へ逸脱しない
     */
    bool safeRemoveAll(const QString &path);

    /**
     * @brief 親dirfdを固定した renameat() でパスを移動/改名する
     *
     * source / destination の親ディレクトリをO_NOFOLLOWで開いた上で renameat() を実行する
     * 中間親ディレクトリのsymlink置換を避けるために使う
     */
    bool safeRenamePathNoFollow(const QString &sourcePath, const QString &destinationPath);

    /**
     * @brief 親dirfdを固定した readlinkat() でsymlink targetを取得する
     *
     * @return 成功時: true (targetOutにsymlink targetの生バイト列を格納する)
     */
    bool safeReadLinkNoFollow(const QString &path, QByteArray *targetOut);

    /**
     * @brief 親dirfdを固定した symlinkat() でsymlinkを作成する
     */
    bool safeCreateSymlinkNoFollow(const QByteArray &target, const QString &path);

    /**
     * @brief 親dirfdを固定した AT_SYMLINK_NOFOLLOW 操作でsymlinkメタデータを更新する
     *
     * owner/group更新とタイムスタンプ更新の両方を試み、個別結果を返す
     */
    bool safeSetSymlinkMetadataNoFollow(const QString &path,
                                        uid_t owner,
                                        gid_t group,
                                        const struct timespec times[2],
                                        bool *ownerUpdated,
                                        bool *timesUpdated);

    /**
     * @brief read-onlyなメタデータ取得用の lstat() ラッパー
     *
     * fdベースhelper群とは異なり、path stringをそのまま lstat() に渡す
     * セキュリティ判断後に同一路径へ書き込む用途では使わず、信頼済み親配下や表示用メタデータ取得に限定して使う
     */
    bool safeLstat(const QString &path, struct stat *out);

    /**
     * @brief 指定dirfd相対で leafをsymlink非追従の fstatat() によりstatする
     *
     * pin済みのsnapshot dirfd等、寿命と同一性が呼び出し側で保証されたdirfdを基点に
     * メタデータを取得する。relativePathは絶対パス・"." / ".." 成分・制御文字を含んではならない
     * (絶対パスはopenat系と同じくdirfdを無視してしまうため入力段階で拒否する)
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param out stat構造体の出力先
     * @return 成功時: true
     */
    bool safeLstatAt(int dirFd, const QString &relativePath, struct stat *out);

    /**
     * @brief 指定dirfd相対で通常ファイルを安全に読み取りオープンする
     *
     * openat(dirFd, relativePath, O_RDONLY | O_NOFOLLOW) で開き、fstat() で
     * regular fileであることを再確認する。相対パス検証は safeLstatAt と共通
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileReadAt(int dirFd, const QString &relativePath);

    /**
     * @brief 指定dirfd相対で readlinkat() によりsymlink targetを取得する
     *
     * leaf自体はsymlink非追従で読み出す。相対パス検証は safeLstatAt と共通
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param targetOut 読み出したtargetの格納先
     * @return 成功時: true (targetOutにsymlink targetの生バイト列を格納する)
     */
    bool safeReadLinkNoFollowAt(int dirFd, const QString &relativePath,
                                QByteArray *targetOut);

    /**
     * @brief 宛先絶対パスがrootPath配下であることを検証し、rootからの相対表現を取り出す
     *
     * 仕様:
     *   - rootPath / absolutePath はともに絶対パス ('/' 開始) でなければ拒否
     *   - 制御文字 (C0: U+0000..U+001F / DEL: U+007F / C1: U+0080..U+009F) を含む場合は拒否
     *     (埋め込みNULによるsyscall引数切り詰め対策)
     *   - absolutePath の各成分に "." / ".." を含む場合は拒否
     *   - absolutePath が rootPath 自身と一致する場合 (相対成分が空) は拒否
     *   - rootPath の成分列が真のプレフィックスでない場合 (兄弟ディレクトリtrick含む) は拒否
     *   - 連続する '/' や末尾 '/' は空成分として正規化して扱う
     *   - ファイルシステムには一切アクセスしない
     *
     * @note 本関数は入力解析 (境界の一部) でありセキュリティ境界そのものではない。
     *       実際の保証は safeOpenDirectoryBeneathRoot 以降のdirfdベース走査が担う。
     *       文字列比較 (startsWith等) を唯一の根拠とした宛先書き込みを行ってはならない
     *
     * @param rootPath 基点ルートディレクトリ (絶対パス)
     * @param absolutePath 検証対象の宛先絶対パス
     * @param relativeOut rootPathからの相対表現の格納先 (非null、成功時のみ内容を更新する)
     * @return rootPath配下と確定した場合: true、拒否した場合: false (errno設定)
     */
    bool splitDestinationBeneathRoot(const QString &rootPath, const QString &absolutePath,
                                     QString *relativeOut);

    /**
     * @brief rootPathを基点に相対パスをO_NOFOLLOWで辿り、末端ディレクトリのfdを返す
     *
     * 各成分を openat(fd, comp, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) で一段ずつ開く。
     * createMissing = true の場合は存在しない中間成分を mkdirat() で作成してから開き直す。
     * rootPath自身がsymlinkの場合はELOOPで拒否する
     *
     * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
     * @param relativePath rootPathからの相対パス ('/'開始や"." ".."成分、制御文字は拒否)
     * @param createMissing 存在しない成分を作成するか
     * @param mode createMissing = true時の作成モード
     * @return 成功時: 末端ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
     *
     * @note ディレクトリfdは認可(凍結)時点から実行時点へ跨いで保持してはならない。
     *       fdが有効な間でも指先のディレクトリは差し替えられ得るため、
     *       本関数による新鮮な解決を変異のたびにやり直すこと
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode);

    /**
     * @brief rootPath配下に宛先ディレクトリを作成する (中間成分も必要に応じて作成)
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafは mkdirat() で作成する。
     * leafが既存の場合はディレクトリ以外 (symlink含む) ならば拒否する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成するディレクトリのモード
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeCreateDirectoryBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                        mode_t mode = 0755);

    /**
     * @brief rootPath配下の通常ファイルを安全に新規/上書きオープンする
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafを openat(..., O_NOFOLLOW) で開いた後、
     * fstat() でregular fileであることを再確認する。leafがsymlinkの場合はELOOPで拒否する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 対象ファイルの絶対パス (rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeOpenRegularFileWriteBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                            mode_t mode);

    /**
     * @brief rootPath配下に通常ファイルを排他的に新規作成してオープンする
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafを openat(..., O_CREAT | O_EXCL) で作成する。
     * leafが既存の場合 (symlink含む) はEEXISTで失敗するため、呼び出し側は別名で再試行できる
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @param mode 作成時モード (umaskの影響を受けるため、必要なら別途fchmod()すること)
     * @return 成功時: file descriptor (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeCreateRegularFileExclusiveBeneathRoot(const QString &rootPath,
                                                  const QString &destinationPath,
                                                  mode_t mode);

    /**
     * @brief rootPath配下で sourcePath を destinationPath へ移動する (rename-aside用)
     *
     * source / destination ともにrootPath配下であることを検証した上で、
     * それぞれをcomponentwiseなO_NOFOLLOW走査で解決し renameat() を実行する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param sourcePath 移動元の絶対パス (rootPath配下であること)
     * @param destinationPath 移動先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeRenamePathNoFollowBeneathRoot(const QString &rootPath, const QString &sourcePath,
                                           const QString &destinationPath);

    /**
     * @brief rootPath配下のパスをsymlink非追従で再帰削除する
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leaf配下を fstatat(AT_SYMLINK_NOFOLLOW) /
     * unlinkat() で削除する。既に存在しない場合は成功扱いとする (safeRemoveAllと同じ契約)
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除成功時: true、失敗時: false (errno設定)
     */
    bool safeRemoveAllBeneathRoot(const QString &rootPath, const QString &destinationPath);

    /**
     * @brief rootPath配下に symlinkat() でsymlinkを作成する
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leaf位置へsymlinkを作成する。
     * leafが既存の場合 (symlink含む) はEEXISTで失敗する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param target 作成するsymlinkのtarget
     * @param destinationPath 作成先の絶対パス (rootPath配下であること)
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeCreateSymlinkNoFollowBeneathRoot(const QString &rootPath, const QByteArray &target,
                                              const QString &destinationPath);

    /**
     * @brief rootPath配下のsymlink自体のowner / timesをAT_SYMLINK_NOFOLLOWで更新する
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafのsymlinkに対して
     * fchownat() / utimensat() をAT_SYMLINK_NOFOLLOW付きで実行する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param path 対象symlinkの絶対パス (rootPath配下であること)
     * @param owner 設定するowner UID
     * @param group 設定するgroup GID
     * @param times 設定するaccess / modification time
     * @param ownerUpdated owner更新成功結果の返却先
     * @param timesUpdated times更新成功結果の返却先
     * @return 両方成功した場合 true
     */
    bool safeSetSymlinkMetadataNoFollowBeneathRoot(const QString &rootPath, const QString &path,
                                                   uid_t owner, gid_t group,
                                                   const struct timespec times[2],
                                                   bool *ownerUpdated, bool *timesUpdated);
} // namespace qsnapper::security

#endif // QSNAPPER_FILESYSTEMHELPERS_H
