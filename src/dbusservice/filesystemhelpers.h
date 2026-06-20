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
} // namespace qsnapper::security

#endif // QSNAPPER_FILESYSTEMHELPERS_H
