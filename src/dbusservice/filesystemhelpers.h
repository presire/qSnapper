#ifndef QSNAPPER_FILESYSTEMHELPERS_H
#define QSNAPPER_FILESYSTEMHELPERS_H

#include <QString>
#include <QByteArray>
#include <functional>
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
     * 各パス成分を openat(..., O_DIRECTORY | O_NOFOLLOW) で辿り、最終ディレクトリのfdを返す
     *
     * @return 成功時は file descriptor、失敗時は-1
     */
    int safeOpenDirectory(const QString &path);

    /**
     * @brief 通常ファイルを安全に読み取りオープンする
     *
     * 親ディレクトリをO_NOFOLLOWで開いた後、fstatat()でleafがregular fileであることを確認してから
     * O_NONBLOCK | O_NOCTTYでopenat()し、fstat() でregular fileであることを再確認する
     * FIFOやデバイスは開かないため、呼び出し側が永久にブロックすることはない
     *
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileRead(const QString &path);

    /**
     * @brief read-onlyなメタデータ取得用のlstat()ラッパー
     *
     * fdベースhelper群とは異なり、path stringをそのままlstat()に渡す
     * セキュリティ判断後に同一路径へ書き込む用途では使わず、信頼済み親配下や表示用メタデータ取得に限定して使用する
     */
    bool safeLstat(const QString &path, struct stat *out);

    /**
     * @brief 指定dirfd相対で leafをsymlink非追従のfstatat()によりstatする
     *
     * pin済みのsnapshot dirfd等、寿命と同一性が呼び出し側で保証されたdirfdを基点にメタデータを取得する
     * relativePathは絶対パス・"." / ".." 成分・制御文字を含んではならない
     * (絶対パスはopenat系と同じくdirfdを無視してしまうため入力段階で拒否する)
     *
     * 中間成分は openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS) で解決する
     * (カーネル5.6未満は同等のin-root解決をfdスタックで再現するフォールバックを使用する)
     * snapshot dirfdは"/"のbtrfs snapshotであるため、"var/run -> /run" のような絶対symlinkは
     * snapshot内で正当な存在であり、chrootと同じくroot内解決を採用する
     * これにより中間symlinkがdirfdの外 (live filesystem) へ解決されることは構造的にない
     *
     * isConfirmedAbsentAt()と中間symlinkの方針が異なるのは意図的である:
     * isConfirmedAbsentAt()は破壊的削除の直前の不在確認であり、中間symlinkの追跡結果がlive filesystemへ依存するためfail-closedで拒否する
     * 一方、本関数群は読み取り専用であり、root内解決で閉じ込めた上でsnapshot自身の内容を取得することが目的である
     *
     * leaf自体はAT_SYMLINK_NOFOLLOWのままのため、leafがsymlinkならsymlinkとして報告する
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param out stat構造体の出力先
     * @return 成功時: true
     */
    bool safeLstatAt(int dirFd, const QString &relativePath, struct stat *out);

    /**
     * @brief 指定dirfd相対で、対象が存在しないことを確定的に確認する
     *
     * 復元の"created"エントリは「復元元snapshotに存在しない」ことを前提に、live側の削除として実行される
     * その前提を破壊の直前に確認するための述語である
     *
     * safeLstatAt()は失敗理由を返さないため、「存在しない」と「statできなかった」を区別できない
     * 本関数は、errnoを見て前者だけをtrueとする:
     *   - lstatが成功した (= 存在する) 場合はfalse
     *   - ENOENT (不在) と、親成分が非ディレクトリであることを示すENOTDIR (この場合も対象は確定的に存在し得ない) の場合のみtrue
     *   - それ以外 (EACCES / ELOOP / EINVAL等) は「不在を確認できなかった」であって「不在である」ではないため、falseを返す (fail-closed)
     *
     * 中間成分は fstatat(..., AT_SYMLINK_NOFOLLOW) でsymlinkを明示的に検出しながら、openat(..., O_DIRECTORY | O_NOFOLLOW) で辿る
     * O_DIRECTORY | O_NOFOLLOW は中間symlinkに対してENOTDIRを返し、「親が実ファイル」の場合と区別できないため、
     * symlinkの検出をfstatat側で行い、symlink成分では「不在を確認できなかった」(ELOOP相当) として扱う
     *
     * @note 呼び出し側は、検証したrelativePathと実際に操作する対象が同一の入力から導出されていることを保証すること
     *       検証後に値を取り直してはならない
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd、read-onlyなsnapshotを想定)
     * @param relativePath dirFdからの相対パス
     * @return 不在を確定できた場合: true、存在するか確認できなかった場合: false
     */
    bool isConfirmedAbsentAt(int dirFd, const QString &relativePath);

    /**
     * @brief 指定dirfd相対で通常ファイルを安全に読み取りオープンする
     *
     * 親成分は openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS) でin-root解決し
     * (カーネル5.6未満は同等のフォールバックを使用)、leafがregular fileであることをfstatat()で確認してから
     * openat(..., O_NOFOLLOW | O_NONBLOCK | O_NOCTTY) で開き、fstat()でregular fileであることを再確認する
     * 中間成分が絶対symlinkでもdirfdの外へ解決されることはなく、snapshot内の同一位置へ収まる
     * leaf自体はsymlink非追従のため、leafがsymlinkならELOOPで拒否される
     * 相対パス検証は、safeLstatAtと共通
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @return 成功時: file descriptor、失敗時: -1
     */
    int safeOpenRegularFileReadAt(int dirFd, const QString &relativePath);

    /**
     * @brief 指定dirfd相対でreadlinkat()によりsymlink targetを取得する
     *
     * 親成分は openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS) でin-root解決し
     * (カーネル5.6未満は同等のフォールバックを使用)、leaf自体はsymlink非追従で読み出す
     * 中間成分が絶対symlinkでもdirfdの外へ解決されることはなく、snapshot内の同一位置へ収まる
     * 相対パス検証は、safeLstatAtと共通
     *
     * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param targetOut 読み出したtargetの格納先
     * @return 成功時: true (targetOutにsymlink targetの生バイト列を格納する)
     */
    bool safeReadLinkNoFollowAt(int dirFd, const QString &relativePath,
                                QByteArray *targetOut);

    /**
     * @brief 単体テスト専用の内部入口
     *
     * production codeからは使用しない
     */
    namespace detail {
        /**
         * @brief safeLstatAt系が使う親ディレクトリのin-root解決を直接呼び出す
         *
         * openat2フォールバック (カーネル5.6未満向け) は対応カーネル上では到達しないため、
         * forceNoOpenat2で明示的に選択できるようにしてテストで検証する
         *
         * @param dirFd 基点ディレクトリfd (O_DIRECTORYで開いたfd)
         * @param relativePath dirFdからの相対パス (leaf成分を含む)
         * @param leafNameOut 最終成分名の返却先 (省略可)
         * @param forceNoOpenat2 trueならopenat2を使わずフォールバック経路を強制する
         * @return 成功時: 親ディレクトリのfd (呼び出し側でclose)、失敗時: -1
         */
        int openParentDirectoryInRootForTesting(int dirFd, const QString &relativePath,
                                                QByteArray *leafNameOut,
                                                bool forceNoOpenat2);

        /**
         * @brief replaceRegularFileAt / replaceSymlinkAtが一時objectをrenameする直前に呼ぶフックを設定する
         *
         * 一時名や親ディレクトリの差し替え (TOCTOU) を決定的に再現するためのテスト専用入口
         * 空のstd::functionを渡すと解除する
         *
         * @param hook 親dirfdと一時leaf名を受け取るフック
         */
        void setBeforeReplaceRenameHookForTesting(
                std::function<void(int parentFd, const QByteArray &temporaryName)> hook);
    }

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
     * @note 本関数は入力解析 (境界の一部) でありセキュリティ境界そのものではない
     *       実際の保証は、safeOpenDirectoryBeneathRoot以降のdirfdベース走査が担う
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
     * 各成分をopenat(fd, comp, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)で1段ずつ開く
     * createMissing = trueの場合は、存在しない中間成分を mkdirat() で作成してから開き直す
     * rootPath自身がsymlinkの場合はELOOPで拒否する
     *
     * @param rootPath 基点ルートディレクトリ (絶対パス、実ディレクトリであること)
     * @param relativePath rootPathからの相対パス ('/'開始や"." ".."成分、制御文字は拒否)
     * @param createMissing 存在しない成分を作成するか
     * @param mode createMissing = true時の作成モード
     * @return 成功時: 末端ディレクトリのfd (呼び出し側でclose)、失敗時: -1 (errno設定)
     *
     * @note ディレクトリfdは認可(凍結)時点から実行時点へ跨いで保持してはならない
     *       fdが有効な間でも指先のディレクトリは差し替えられ得るため、本関数による新鮮な解決を変異のたびにやり直すこと
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode);

    /**
     * @brief rootPath配下に宛先ディレクトリを作成する (中間成分も必要に応じて作成)
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafはmkdirat()で作成する
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
     * @brief rootPath配下に、復元先の欠けている親ディレクトリを作成する
     *
     * 既存の成分はO_NOFOLLOWで辿るだけで、所有者やmodeを変更しない
     * 欠けている成分は0700・作成者所有で作成し、snapshot側の同じ相対パスにディレクトリがあれば、その所有者・mode・ACL・xattrを適用する
     * 親ディレクトリが他ユーザーに差し替えられ得る場合、metadataは適用せず、他ユーザーに書き込みを与えない0711で作成する
     * snapshot側に無い場合 (存在しない・ディレクトリ以外) は、0700・作成者所有のままにする
     * snapshot側の状態を判定できない場合は、その成分を作成せずに失敗する
     *
     * @param rootPath 基点ルートディレクトリ (snapshot dirfdと同じ相対パスで対応付く)
     * @param parentPath 作成する親ディレクトリの絶対パス (rootPath配下であること)
     * @param sourceDirFd pin済みのsnapshot dirfd
     * @param mustPreserveMetadata 必須metadata (所有者、mode、ACL) を適用できない場合に失敗させる場合true
     * @return 成功時: true、失敗時: false (errno設定)
     */
    bool safeCreateParentDirectoriesFromSourceBeneathRoot(const QString &rootPath,
                                                          const QString &parentPath,
                                                          int sourceDirFd,
                                                          bool mustPreserveMetadata);

    /**
     * @brief 作成したディレクトリへsnapshotのmetadataを安全に適用できる親ディレクトリかを判定する
     *
     * sticky bitが無い状態でgroup/otherへ書き込みが許可されている場合と、root以外が所有する書き込み可能な
     * ディレクトリでは、他ユーザーが作成直後のエントリを差し替え得るため、安全と判定しない
     * sticky bit付きでrootが所有するディレクトリでは、root所有のエントリを他ユーザーは差し替えられない
     *
     * @param parentStat 親ディレクトリのstat
     * @return 安全に適用できる場合: true
     */
    bool parentDirectoryIsSafeForMetadata(const struct stat &parentStat);

    /**
     * @brief rootPath配下の通常ファイルを安全に新規/上書きオープンする
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafをopenat(..., O_NOFOLLOW)で開いた後、fstat()でregular fileであることを再確認する
     * leafがsymlinkの場合はELOOPで拒否する
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
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafをopenat(..., O_CREAT | O_EXCL)で作成する
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
     * それぞれをcomponentwiseなO_NOFOLLOW走査で解決し、renameat2(RENAME_NOREPLACE)を実行する
     * destinationに既存のエントリ (ファイル・symlink・空ディレクトリを含む) がある場合は置換せず、EEXISTで失敗する
     * RENAME_NOREPLACEに対応しないカーネル / FS (EINVAL / ENOSYS) でも、置換し得るrenameat()へは切り替えずに失敗する
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
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leaf配下をfstatat(AT_SYMLINK_NOFOLLOW) / unlinkat()で削除する
     * 既に存在しない場合は成功扱いとする (safeRemoveAllと同じ契約)
     * mount境界 (別mountのroot、または親と異なるst_dev。btrfsのネストしたsubvolumeを含む) を検出した時点で降下を止め、errno=EXDEVで失敗する
     * 境界の内側は削除しない
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除成功時: true、失敗時: false (errno設定)
     */
    bool safeRemoveAllBeneathRoot(const QString &rootPath, const QString &destinationPath);

    /**
     * @brief rootPath配下にsymlinkat()でsymlinkを作成する
     *
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leaf位置へsymlinkを作成する
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
     * componentwiseなO_NOFOLLOW走査で親まで辿り、leafのsymlinkに対して、
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

    /**
     * @brief dirFdを基点に、相対パス上の既存ディレクトリを読み取り用に開く
     *
     * 中間成分の解決はsafeOpenRegularFileReadAtと同じ (dirFdの外へ解決しない)
     * leafはO_NOFOLLOW | O_DIRECTORYで開くため、symlinkや非ディレクトリは拒否する
     *
     * @param dirFd 基点ディレクトリfd (pin済みのsnapshot dirfdなど)
     * @param relativePath dirFdからの相対パス
     * @return 成功時: ディレクトリfd (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int safeOpenDirectoryReadAt(int dirFd, const QString &relativePath);

    /**
     * @brief rootPath配下の宛先について、親ディレクトリを1回だけ解決して開く
     *
     * 一時objectの作成からrenameat()まで同じ親dirfdを使い続けるための入口
     * 解決方法は他の*BeneathRoot helperと同じ (componentwiseなO_NOFOLLOW走査、中間ディレクトリは作成しない)
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 宛先の絶対パス (rootPath配下であること)
     * @param leafNameOut 最終成分名の返却先
     * @return 成功時: 親ディレクトリfd (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int openDestinationParentBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                         QByteArray *leafNameOut);

    /**
     * @brief applyRestoredMetadataの結果
     *
     * 必須metadata (所有者、mode、POSIX ACL、file capability) と、best-effortのxattrを区別して返す
     */
    struct RestoredMetadataResult {
        bool mandatoryFailed = false;   // 必須metadataのいずれかを適用できなかった
        int mandatoryErrno = 0;         // 最初に失敗した必須metadataのerrno
        int optionalFailures = 0;       // コピーできなかったbest-effortのxattrの件数
    };

    /**
     * @brief 復元元のmetadataを、fd経由で復元先へ適用する
     *
     * 適用順序は次のとおり (名前の再解決は行わない):
     * 1. fchown() : 所有者変更はS_ISUID / S_ISGIDとsecurity.capabilityを落とすため、最初に行う
     * 2. fchmod() : S_ISUID / S_ISGIDを含むmodeを設定する
     * 3. system.posix_acl_access (ディレクトリはsystem.posix_acl_defaultも) :
     *    復元元にあれば設定し、無ければ復元先から削除する (親のdefault ACLから継承したentryを残さない)
     * 4. その他のxattr (security.selinux、user.*など) : best-effortでコピーし、失敗件数のみを返す
     * 5. security.capability (通常ファイルのみ) : 所有者とmodeの変更後に設定する
     *
     * @param sourceFd 復元元のfd
     * @param destinationFd 復元先のfd
     * @param isDirectory ディレクトリとして扱う場合true (default ACLを同期し、capabilityは扱わない)
     * @return 適用結果
     */
    RestoredMetadataResult applyRestoredMetadata(int sourceFd, int destinationFd, bool isDirectory);

    /**
     * @brief 親dirfdを保持したまま、通常ファイルを一時名へ書き出して差し替える
     *
     * 親dirfd上にO_CREAT | O_EXCL | O_NOFOLLOWで一時ファイル (mode 0600) を作成し、data → metadata → 時刻をfd経由で適用してから、同じ親dirfd同士でrenameat()する
     * rename直前に一時名が自分の作成したinodeを指すこと、rename直後に宛先が同じinodeであることを確認し、一致しなければ失敗する (errno=ESTALE)
     * 失敗時は、一時名が自分のinodeを指している場合に限り削除する
     *
     * @param destinationParentFd openDestinationParentBeneathRootで開いた親dirfd
     * @param leafName 宛先の最終成分名
     * @param sourceFd 復元元の通常ファイルfd
     * @param tryReflink FICLONEを先に試す場合true
     * @param mustPreserveMetadata 必須metadataや時刻を適用できない場合に失敗させる場合true
     * @param metadataOut metadataの適用結果の返却先 (省略可)
     * @return 差し替えに成功した場合true (失敗時はerrno設定)
     */
    bool replaceRegularFileAt(int destinationParentFd, const QByteArray &leafName, int sourceFd,
                              bool tryReflink, bool mustPreserveMetadata,
                              RestoredMetadataResult *metadataOut = nullptr);

    /**
     * @brief 親dirfdを保持したまま、symlinkを一時名で作成して差し替える
     *
     * 作成した一時symlinkをO_PATH | O_NOFOLLOWで開き、自分が作成したもの (symlink、所有者がeuid、targetが一致) であることを確認してから、
     * 所有者と時刻をAT_EMPTY_PATHでfd経由に設定する
     * renameat()の前後の検証と失敗時の後始末はreplaceRegularFileAtと同じ
     *
     * @param destinationParentFd openDestinationParentBeneathRootで開いた親dirfd
     * @param leafName 宛先の最終成分名
     * @param linkTarget 作成するsymlinkのtarget
     * @param sourceStat 復元元symlinkのstat (所有者と時刻に使う)
     * @param metadataApplied 所有者と時刻を両方適用できたかの返却先 (省略可)
     * @return 差し替えに成功した場合true (失敗時はerrno設定)
     */
    bool replaceSymlinkAt(int destinationParentFd, const QByteArray &leafName,
                          const QByteArray &linkTarget, const struct stat &sourceStat,
                          bool *metadataApplied = nullptr);

    /**
     * @brief スナップショット一覧の変化を検出するための指紋を計算する
     *
     * snapper設定の .snapshots ディレクトリについて、ディレクトリ自体と、数字のみの名前の各エントリ (先頭に0が付いた名前を含む)、その info.xml のstat情報 (inode・サイズ・mtime・ctime) から指紋を作る
     * スナップショットの作成・削除・変更 (info.xmlの書き換え) は、いずれかの値を変える
     * 読み取りだけを行い、何も変更しない
     *
     * @param snapshotsDirPath .snapshots ディレクトリの絶対パス
     * @return 指紋 (SHA-256)、判定できない場合は空のQByteArray (呼び出し側は「変化あり」として扱うこと)
     */
    QByteArray snapshotListFingerprint(const QString &snapshotsDirPath);
} // namespace qsnapper::security

#endif // QSNAPPER_FILESYSTEMHELPERS_H
