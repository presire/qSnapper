/**
 * @file filesystemhelpers.h
 * @brief symlinkを辿らない経路でファイルシステムを読み書きするヘルパー
 *
 * 復元先(live filesystem)の絶対パスと、開いたまま保持したスナップショットのdirfd(ディレクトリfd)に対して、
 * メタデータの取得、ディレクトリの作成、ファイルの差し替え、削除を行う
 * 呼び出し元はsnapshotoperations系の復元・差分処理と単体テストである
 *
 * 入力 : rootPath(復元先の基点)または基点dirfd、およびdirfdからの相対パス
 * 出力 : stat、file descriptor(fd)、操作の成否(失敗時はerrno)
 * 依存 : openat2とstatx(openat2はカーネル5.6以降が必要で、未対応のカーネルではENOSYSで失敗する)、Qt Core
 *
 * パス文字列の検証結果を保持したまま後でそのパスを使い直すと、
 * 検証から使用までの間に親ディレクトリを差し替えられるTOCTOU(検査と使用の間の競合)が生じる
 * そのため、検証したパスは文字列のまま使い続けず、dirfdを基点に操作する
 */
#ifndef QSNAPPER_FILESYSTEMHELPERS_H
#define QSNAPPER_FILESYSTEMHELPERS_H

#include <QString>
#include <QByteArray>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>

namespace qsnapper::security {

    /**
     * @brief 祖先の所有権を集積した状態を表す型の前方宣言
     *
     * 実装は内部ヘッダfilesystemhelpers_internal.hに置き、
     * 公開APIでは所有権の状態を受け渡すためだけに型名を使う
     * 呼び出し側が内部ヘッダの内容へ依存しないよう、定義は公開しない
     */
    struct DirectoryChainOwnership;

    /**
     * @brief シンボリックリンクを辿らずにディレクトリ階層を作成する
     *
     * 各パス成分をopenat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)で1段ずつ開き、
     * 存在しない成分はmkdirat()で作成する
     * 中間成分をsymlinkへ差し替えられても基点の外へは辿らない
     *
     * @param path 作成対象の絶対パス
     * @param mode 新規作成するディレクトリのmode(umaskの影響を受ける)
     * @return 全ての成分を用意できた場合true(失敗時はerrno設定)
     */
    bool safeMkpath(const QString &path, mode_t mode = 0755);

    /**
     * @brief シンボリックリンクを辿らずに既存ディレクトリを開く
     *
     * 各パス成分をopenat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)で1段ずつ開き、最終ディレクトリのfdを返す
     *
     * requireTrustedChainがtrueの場合は、「/」から対象までの全ディレクトリがroot所有で、
     * groupとotherに書き込みを許さないことも要求する
     * libsnapperのmountのようにパス文字列だけを受け取る外部ライブラリへ渡す前に、
     * 一般ユーザが名前を差し替えられないことを確かめるために使う
     *
     * @param path 対象ディレクトリの絶対パス
     * @param requireTrustedChain 信頼できるディレクトリだけを辿ることを要求する場合true
     * @return 成功時はfile descriptor(fd)、失敗時は-1(信頼できないディレクトリを含む場合はEPERM)
     */
    int safeOpenDirectory(const QString &path, bool requireTrustedChain = false);

    /**
     * @brief 通常ファイルを安全に読み取りオープンする
     *
     * 親ディレクトリをO_NOFOLLOWで開いた後、fstatat()でleafがregular fileであることを確認してから
     * O_NONBLOCK | O_NOCTTYでopenat()し、fstat()でregular fileであることを再確認する
     * FIFOやデバイスを開くと読み取りが永久にブロックし得るため、種別の確認を開く前後に行う
     *
     * @param path 対象ファイルの絶対パス
     * @return 成功時はfile descriptor(fd)、失敗時は-1(errno設定)
     */
    int safeOpenRegularFileRead(const QString &path);

    /**
     * @brief read-onlyなメタデータ取得用のlstat()ラッパー
     *
     * fdを基点とする他のヘルパーとは異なり、パス文字列をそのままlstat()に渡す
     * そのため、セキュリティ判断の後に同じ経路へ書き込む用途には使わず、
     * 信頼済みの親ディレクトリ配下や表示用メタデータの取得に限定して使う
     *
     * @param path 対象の絶対パス
     * @param out statの出力先
     * @return 取得に成功した場合true(失敗時はerrno設定)
     */
    bool safeLstat(const QString &path, struct stat *out);

    /**
     * @brief 指定dirfd相対でleafをsymlink非追従のfstatat()によりstatする
     *
     * 開いたまま保持したsnapshotのdirfdなど、寿命と同一性が呼び出し側で保証されたdirfdを基点にメタデータを取得する
     * relativePathは絶対パス、[.]/[..]成分、制御文字を含んではならない
     * 絶対パスを渡すとopenat系がdirfdを無視するため、入力段階で拒否する
     *
     * 中間成分はopenat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)で解決する
     * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSで失敗する
     * snapshot dirfdは「/」のbtrfs snapshotであるため、「var/run -> /run」のような絶対symlinkも
     * snapshot内では正当な存在であり、chrootと同じくroot内解決を採用する
     * これにより中間symlinkがdirfdの外(live filesystem)へ解決されることは構造的にない
     *
     * isConfirmedAbsentAt()と中間symlinkの方針が異なるのは意図的である
     * isConfirmedAbsentAt()は破壊的削除の直前の不在確認であり、中間symlinkの追跡結果がlive filesystemへ依存するためfail-closedで拒否する
     * 一方、本関数群は読み取り専用であり、root内解決で閉じ込めた上でsnapshot自身の内容を取得することが目的である
     *
     * leaf自体はAT_SYMLINK_NOFOLLOWのままのため、leafがsymlinkならsymlinkとして報告する
     *
     * @param dirFd 基点ディレクトリfd(O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param out stat構造体の出力先
     * @return 取得に成功した場合true(失敗時はerrno設定)
     */
    bool safeLstatAt(int dirFd, const QString &relativePath, struct stat *out);

    /**
     * @brief 指定dirfd相対で、対象が存在しないことを確定的に確認する
     *
     * 復元計画の[created]エントリは「復元元のsnapshotに存在しない」ことを前提に、live側の削除として実行される
     * 本関数は、その前提を破壊の直前に確かめるための述語である
     *
     * safeLstatAt()の真偽値だけでは、「存在しない」と「statできなかった」を区別できない
     * 本関数はerrnoを見て、不在を確定できる場合だけtrueを返す
     * 対象のstatに成功した(=存在する)場合はfalse
     * 成分のstatがENOENT(不在)で失敗した場合はtrue
     * symlink以外の中間成分が非ディレクトリの場合も、その配下は存在し得ないためtrue
     * それ以外(EACCES / ELOOP / EINVALなど)は「不在を確認できなかった」であって「不在である」ではないため、falseを返す(fail-closed)
     *
     * 中間成分はfstatat(..., AT_SYMLINK_NOFOLLOW)でsymlinkを明示的に検出しながら、
     * openat(..., O_DIRECTORY | O_NOFOLLOW)で辿る
     * O_DIRECTORY | O_NOFOLLOWは中間symlinkに対してENOTDIRを返し、「親が実ファイル」の場合と区別できない
     * そのためsymlinkの検出をfstatat側で行い、symlink成分では「不在を確認できなかった」(ELOOP相当)として扱う
     *
     * @note 呼び出し側は、検証したrelativePathと実際に操作する対象が同一の入力から導出されていることを保証すること
     *       検証後に値を取り直してはならない
     *
     * @param dirFd 基点ディレクトリfd(O_DIRECTORYで開いたfd、read-onlyなsnapshotを想定)
     * @param relativePath dirFdからの相対パス
     * @return 不在を確定できた場合true、存在するか確認できなかった場合false
     */
    bool isConfirmedAbsentAt(int dirFd, const QString &relativePath);

    /**
     * @brief observePathNoFollowAt()の観測結果
     *
     * パスが無い場合と、観測できなかった場合を呼び出し側が区別できるよう3値にしている
     * 復元計画の検証ではAbsentだけを「復元元に無い」と断定してよい
     */
    enum class PathPresenceAt {
        Present,  // パスが実在し、statを出力済み
        Absent,   // パスがdirfdを根とする木に含まれない
        Unknown   // 入出力エラーやmount境界により存在を確定できなかった
    };

    /**
     * @brief 指定dirfdを根とする木に、symlinkを辿らずにパスが含まれるかを観測する
     *
     * 復元計画の検証で、復元元と比較相手の木にパスが含まれるかを、libsnapperの比較と同じ意味で判定するために使う
     * libsnapperの比較はsymlinkを辿らないため、次の場合はAbsentとする
     *   - 成分のどれかがENOENT
     *   - 中間成分が非ディレクトリ(symlinkを含む)
     * 中間成分がマウントポイントや別のsubvolume(st_devが根と異なる、またはmountのroot)の場合は、EXDEVでUnknownとする
     * 境界の下を「存在しない」と扱うと、境界の先に実在するファイルが偽のdeletedとして検証を通り、
     * 復元が別のファイルシステムへ書き込んでしまうため
     * leafはAT_SYMLINK_NOFOLLOWでstatし、leafがsymlinkならsymlinkとして報告する
     *
     * 中間成分は、fstatat()で確認したinodeとopenat(O_NOFOLLOW)で開いたinodeを照合しながら辿る
     * 確認の後に書き込み可能な親の下で差し替えられた場合は、ESTALEでUnknownとする
     *
     * @param dirFd 根とするディレクトリfd(O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス(絶対パス、[.]/[..]成分、制御文字は不可)
     * @param out Presentの場合にleafのstatを書き込む先(Present以外では変更しない)
     * @return 観測結果の種類(Unknownの場合はerrnoに理由が入る)
     */
    PathPresenceAt observePathNoFollowAt(int dirFd, const QString &relativePath, struct stat *out);

    /**
     * @brief 指定dirfd相対で通常ファイルを安全に読み取りオープンする
     *
     * 親成分はopenat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)でroot内解決する
     * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSで失敗する
     * leafがregular fileであることをfstatat()で確認してから
     * openat(..., O_NOFOLLOW | O_NONBLOCK | O_NOCTTY)で開き、fstat()でregular fileであることを再確認する
     * 中間成分が絶対symlinkでもdirfdの外へ解決されることはなく、snapshot内の同一位置へ収まる
     * leaf自体はsymlink非追従のため、leafがsymlinkならELOOPで拒否される
     * 相対パスの検証はsafeLstatAtと共通である
     *
     * @param dirFd 基点ディレクトリfd(O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @return 成功時はfile descriptor(fd)、失敗時は-1(errno設定)
     */
    int safeOpenRegularFileReadAt(int dirFd, const QString &relativePath);

    /**
     * @brief 指定dirfd相対でreadlinkat()によりsymlinkのtargetを取得する
     *
     * 親成分はopenat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)でroot内解決する
     * openat2が無いカーネル5.6未満では代替実装を持たず、ENOSYSで失敗する
     * leaf自体はsymlink非追従で読み出す
     * 中間成分が絶対symlinkでもdirfdの外へ解決されることはなく、snapshot内の同一位置へ収まる
     * 相対パスの検証はsafeLstatAtと共通である
     *
     * @param dirFd 基点ディレクトリfd(O_DIRECTORYで開いたfd)
     * @param relativePath dirFdからの相対パス
     * @param targetOut 読み出したtargetの格納先
     * @return 成功した場合true(targetOutにsymlink targetの生バイト列を格納する)
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
         * @brief replaceRegularFileAt / replaceSymlinkAtが一時objectをrenameする直前に呼ぶフックを設定する
         *
         * 一時名や親ディレクトリの差し替え(TOCTOU)を決定的に再現するためのテスト専用入口
         * 空のstd::functionを渡すと解除する
         *
         * @param hook 親dirfdと一時leaf名を受け取るフック
         */
        void setBeforeReplaceRenameHookForTesting(
                std::function<void(int parentFd, const QByteArray &temporaryName)> hook);
    }

    /**
     * @brief 宛先絶対パスがrootPath配下であることを検証し、rootPathからの相対表現を取り出す
     *
     * 受け付ける条件は次のとおりである
     *   - rootPathとabsolutePathはともに絶対パス(「/」で始まる)であること
     *   - 制御文字(C0: U+0000..U+001F、DEL: U+007F、C1: U+0080..U+009F)を含まないこと
     *     埋め込みNULはsyscallの引数を途中で切り詰めるため、パスには一切許容しない
     *   - absolutePathの各成分に[.]/[..]を含まないこと
     *   - absolutePathがrootPath自身と一致しないこと(相対成分が空になるため)
     *   - rootPathの成分列がabsolutePathの真のプレフィックスであること(兄弟ディレクトリへのすり替えを防ぐ)
     *   - 連続する「/」や末尾の「/」は空成分として正規化して扱う
     * ファイルシステムには一切アクセスしない
     *
     * @note 本関数は入力解析(境界の一部)であり、セキュリティ境界そのものではない
     *       実際の保証は、safeOpenDirectoryBeneathRoot以降のdirfdベースの走査が担う
     *       startsWith()などの文字列比較だけを根拠に宛先へ書き込んではならない
     *
     * @param rootPath 基点ルートディレクトリ(絶対パス)
     * @param absolutePath 検証対象の宛先絶対パス
     * @param relativeOut rootPathからの相対表現の格納先(非null、成功時のみ内容を更新する)
     * @return rootPath配下と確定した場合true、拒否した場合false(errno=EINVAL)
     */
    bool splitDestinationBeneathRoot(const QString &rootPath, const QString &absolutePath,
                                     QString *relativeOut);

    /**
     * @brief rootPathを基点に相対パスをsymlink非追従で辿り、末端ディレクトリのfdを返す
     *
     * 各成分をopenat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)で1段ずつ開く
     * createMissingがtrueの場合は、存在しない中間成分をmkdirat()で作成してから開き直す
     * rootPath自身がsymlinkの場合はELOOPで拒否する
     *
     * @param rootPath 基点ルートディレクトリ(絶対パス、実ディレクトリであること)
     * @param relativePath rootPathからの相対パス(「/」で始まるパス、[.]/[..]成分、制御文字は拒否)
     * @param createMissing 存在しない成分を作成するか
     * @param mode createMissingがtrueのときの作成モード
     * @return 成功時は末端ディレクトリのfd(呼び出し側でclose)、失敗時は-1(errno設定)
     *
     * @note 認可で対象を凍結した時点から実行時点まで、同じディレクトリfdを持ち越してはならない
     *       fdが有効な間でも、そのfdが指すディレクトリは親から差し替えられ得るため、変異のたびに本関数で解決し直すこと
     */
    int safeOpenDirectoryBeneathRoot(const QString &rootPath, const QString &relativePath,
                                     bool createMissing, mode_t mode);

    /**
     * @brief rootPath配下に宛先ディレクトリを作成する(中間成分も必要に応じて作成する)
     *
     * 各成分をsymlink非追従で辿って親まで開き、leafはmkdirat()で作成する
     * leafが既に存在する場合は、ディレクトリ以外(symlinkを含む)ならEEXISTで拒否する
     * 既存のsymlinkをディレクトリと誤認して、その先を変更しないようにするためである
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 作成先の絶対パス(rootPath配下であること)
     * @param mode 作成するディレクトリのモード
     * @return 作成できた場合true、失敗した場合false(errno設定)
     */
    bool safeCreateDirectoryBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                        mode_t mode = 0755);

    /**
     * @brief rootPath配下に、復元先で欠けている親ディレクトリを作成する
     *
     * 既存の成分はsymlink非追従で辿るだけで、所有者やmodeを変更しない
     * 欠けている成分は0700・作成者所有で作成し、snapshot側の同じ相対パスにディレクトリがあれば、
     * その所有者・mode・ACL・xattrを適用する
     * root所有かつgroupとotherが書き込み不可の祖先だけを信頼する
     * 信頼できない祖先が現れた時点の所有者をprincipal(変更を委ねる唯一の所有者)として、そこから下はprincipal所有の成分だけを変更する
     * 非信頼チェーンで作成する場合は、復元元の所有者がprincipalと一致するときだけ、
     * principalのfilesystem identity(fsuid/fsgid)で0700から作成し、開き直したinodeの所有者を確認してmetadataを適用する
     * 復元元の所有者がprincipalと一致しない場合や、非信頼点より下に別の所有者の成分が混在する場合は、
     * 権限の境界を越えるためEPERMで失敗する
     * snapshot側に無い場合(存在しない、またはディレクトリ以外)は、信頼チェーンでは0700・作成者所有のままにし、非信頼チェーンでは作成しない
     * 既存の成分が別のファイルシステム(mount境界)にある場合は、rootPathの外へ書き込まないようEXDEVで失敗する
     * snapshot側の状態を判定できない場合は、その成分を作成せずに失敗する
     *
     * @param rootPath 基点ルートディレクトリ(snapshot dirfdと同じ相対パスで対応付く)
     * @param parentPath 作成する親ディレクトリの絶対パス(rootPath配下であること)
     * @param sourceDirFd 開いたまま保持したsnapshotのdirfd
     * @param mustPreserveMetadata 必須metadata(所有者、mode、ACL)を適用できない場合に失敗させるならtrue
     * @return 作成できた場合true、失敗した場合false(errno設定)
     */
    bool safeCreateParentDirectoriesFromSourceBeneathRoot(const QString &rootPath,
                                                          const QString &parentPath,
                                                          int sourceDirFd,
                                                          bool mustPreserveMetadata);

    /**
     * @brief 作成したディレクトリへsnapshotのmetadataを安全に適用できる親ディレクトリかを判定する
     *
     * 信頼できるのは、root所有かつgroupとotherが書き込み不可のディレクトリだけである
     * sticky bitがあっても、他の利用者がrootより先に名前を作れてしまうため信頼しない
     * root以外が所有するディレクトリは、modeが0555でも所有者がmodeを変えて差し替えられるため信頼しない
     *
     * @param parentStat 親ディレクトリのstat
     * @return 安全に適用できる場合true
     */
    bool parentDirectoryIsSafeForMetadata(const struct stat &parentStat);

    /**
     * @brief テスト専用としてrootPath配下の通常ファイルを上書きオープンする
     *
     * 本番コードからは呼び出さない
     * O_EXCLを付けずにO_TRUNCで開くため、実際の復元先で使うと、
     * ハードリンクで共有された既存ファイルの中身まで消してしまう
     * この危険を呼び出し側が見落とさないよう、名前にForTestingを付けている
     * 各成分をsymlink非追従で辿って親まで開き、leafをopenat(..., O_NOFOLLOW)で開いた後、
     * fstat()でregular fileであることを再確認する
     * leafがsymlinkの場合はELOOPで拒否する
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 対象ファイルの絶対パス(rootPath配下であること)
     * @param mode 作成時モード
     * @return 成功時はfile descriptor(fd、呼び出し側でclose)、失敗時は-1(errno設定)
     */
    int safeOpenRegularFileWriteBeneathRootForTesting(const QString &rootPath,
                                                      const QString &destinationPath,
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
     * mount境界 (別mountのroot、または親と異なるst_dev、btrfsのネストしたsubvolumeを含む) を検出した時点で降下を止め
     * errno=EXDEVで失敗する
     * 境界の内側は削除しない
     * 降下の深さには、ディレクトリのfdとメモリを使い切らないよう上限 (最大256段、RLIMIT_NOFILEの半分以下) を設けている
     * 超えた場合はerrno=ELOOPで失敗するが、それまでに削除したエントリは戻らないため、呼び出し側は部分削除として扱うこと
     * root実行時の非信頼チェーンでは単一principal以外のディレクトリへ降りず、EPERMで失敗する
     * 内容は所有者を検査したfdを通じて削除するが、最後のAT_REMOVEDIRは名前による空ディレクトリの削除である
     * 非空の差し替えはENOTEMPTYになり、非ディレクトリは所有者にかかわらずunlinkする
     *
     * @param rootPath 基点ルートディレクトリ
     * @param destinationPath 削除対象の絶対パス (rootPath配下であること)
     * @return 削除成功時: true、失敗時: false (errno設定)
     */
    bool safeRemoveAllBeneathRoot(const QString &rootPath, const QString &destinationPath);

    /**
     * @brief subvolume配下のスナップショットディレクトリ (.snapshots/<番号>/snapshot) をsymlink非追従で開く
     *
     * libsnapperはこのパスへmountし、復元はそのパスを読むため、一般ユーザが差し替えられる成分を辿ると、
     * 認可した番号とは別の木を復元元として使わせられる
     * そのため1成分ずつopenat2 (RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS) で開き、
     * .snapshots と番号ディレクトリにはroot所有でgroupとotherが書き込めないことを要求する
     * subvolume自体の所有者は問わない (利用者ごとのbtrfs設定ではsubvolumeのrootが一般ユーザの所有になる)
     *
     * @param subvolumeFd snapper設定のsubvolumeのディレクトリfd
     * @param snapshotNumber スナップショット番号
     * @param statOut 開いたsnapshotディレクトリのstatの返却先 (nullptr可)
     * @return 成功時snapshotディレクトリのfd、失敗時-1 (symlinkはELOOPまたはENOTDIR、所有者とmodeの違反はEPERM)
     */
    int openSnapshotDirectoryBeneathSubvolume(int subvolumeFd, unsigned int snapshotNumber,
                                              struct stat *statOut);

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
     * @param ownershipOut 同じwalkの所有権状態 (rootが変更主体を確定できなければEPERM)
     * @return 成功時: 親ディレクトリfd (呼び出し側でclose)、失敗時: -1 (errno設定)
     */
    int openDestinationParentBeneathRoot(const QString &rootPath, const QString &destinationPath,
                                         QByteArray *leafNameOut,
                                         DirectoryChainOwnership *ownershipOut = nullptr);

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
     * 2. system.posix_acl_access (ディレクトリはsystem.posix_acl_defaultも) :
     *    復元元にあれば設定し、無ければ復元先から削除する (親のdefault ACLから継承したentryを残さない)
     *    modeを広げるより先に行い、継承した名前付きエントリがmaskの拡大で一時的に有効になるのを防ぐ
     * 3. fchmod() : S_ISUID / S_ISGIDを含むmodeを設定する
     * 4. その他のxattr (security.selinux、user.*など) : best-effortでコピーし、失敗件数のみを返す
     *    security.selinuxがSELinuxユーザの違いで拒否された場合は、ユーザ部分だけを復元先の値にして再試行する
     * 5. security.capability (通常ファイルのみ) : 所有者とmodeの変更後に設定する
     *
     * @param sourceFd 復元元のfd
     * @param destinationFd 復元先のfd
     * @param isDirectory ディレクトリとして扱う場合true (default ACLを同期し、capabilityは扱わない)
     * @return 適用結果
     * @note ACLの同期に失敗した場合は、最初のerrnoを記録して、3以降を行わずに戻る
     *       復元先のmodeは作成時のまま (既存のディレクトリは現在のまま) となり、復元元のmodeまで広がらない
     */
    RestoredMetadataResult applyRestoredMetadata(int sourceFd, int destinationFd, bool isDirectory);

#ifdef QSNAPPER_ENABLE_TEST_METADATA_HOOKS
    // テスト用の観測点と故障注入(処理をわざと失敗させる仕組み)は、
    // QSNAPPER_ENABLE_TEST_METADATA_HOOKSを定義したテスト用ターゲットだけでコンパイルする
    // 製品のhelperがテスト用の入口を持たないよう、宣言もマクロで囲む
    namespace detail {
        /**
         * @brief applyRestoredMetadataの処理のうち、テストが復元先の状態を観測できる段階
         */
        enum class MetadataStage {
            BeforeModeRestore,  // 復元元のmodeを適用する最初のfchmodを呼ぶ直前
        };

        /**
         * @brief 段階の通知を受け取る関数の型
         *
         * テスト専用の入口のために依存を増やさないよう、std::functionではなく関数ポインタと任意のデータへのポインタを使う
         * 通知はapplyRestoredMetadataの実行中に同じスレッドから呼び出される
         * 通知を受け取る関数が復元先fdを閉じたり変更したりすると、その後の適用結果が変わるため、fdの状態の読み取りだけを行うこと
         */
        using MetadataStageObserver = void (*)(MetadataStage stage, int destinationFd, void *context);

        /**
         * @brief applyRestoredMetadataの段階を通知する関数を設定する
         *
         * ACLの同期がmodeの適用より先に終わっていることを、競合のタイミングに頼らずテストで確認するために使う
         * nullptrを渡すと通知を止める
         *
         * @param observer 通知を受け取る関数
         * @param context observerの第3引数へそのまま渡すポインタ (観測結果の記録先など)
         */
        void setMetadataStageObserverForTesting(MetadataStageObserver observer, void *context);

        /**
         * @brief 指定したACLのxattrの同期を、復元先のACLを変更する前に失敗させる
         *
         * ACLの同期に失敗した場合にmodeを広げないことを、
         * 実際のファイルシステムでACLの書き込みを失敗させずにテストで確認するために使う
         *
         * @param xattrName 失敗させるxattr名 (system.posix_acl_accessまたはsystem.posix_acl_default)
         * @param errorNumber 失敗時に設定するerrno (0を渡すと失敗させる設定を解除する)
         */
        void setAclSyncFailureForTesting(const char *xattrName, int errorNumber);

        /**
         * @brief ディレクトリのグループが所有者専用のグループかどうかの判定を置き換える関数の型
         *
         * 所有者専用のグループとは、所有者の主グループで、名前が所有者と同じで、所有者だけが属するグループである
         * 判定はディレクトリの所有権の集計の途中で同じスレッドから呼び出される
         * 引数はディレクトリの所有者のuidとgidで、所有者専用のグループとして扱う場合はtrueを返す
         */
        using UserPrivateGroupCheck = bool (*)(uid_t owner, gid_t group);

        /**
         * @brief 所有者専用のグループかどうかを、NSSの問い合わせと/etc/passwd、/etc/groupの走査の代わりに指定した関数で判定させる
         *
         * テストの結果が、テストを実行するホストの利用者とグループの登録に左右されないようにするために使う
         * 置き換わるのはこの判定だけで、otherの書き込み、groupの書き込み、拡張ACLの判定は置き換わらない
         * nullptrを渡すと本来の判定へ戻す
         *
         * @param check 所有者専用のグループかどうかを判定する関数
         */
        void setUserPrivateGroupCheckForTesting(UserPrivateGroupCheck check);

        /**
         * @brief metadata適用のテスト用フックを全て解除する
         *
         * フックの設定が次のテストへ残ると、無関係なテストの結果まで変わるため、各テストの終了時に呼ぶ
         */
        void clearMetadataTestHooksForTesting();
    }
#endif

    /**
     * @brief スナップショットのSELinuxコンテキストのユーザ部分だけを、復元先の現在のユーザに置き換える
     *
     * SELinuxユーザの異なるコンテキストを書き込むにはcan_change_object_identity属性が必要になる
     * 本パッケージのポリシーはqsnapper_dbus_tにこの属性を与えるが、属性を持たない古いモジュールが読み込まれていると書き込みが拒否される
     * その場合に備えた代わりの手段として使う
     * 例えばunconfined_uのファイルを戻すとき、ユーザ部分だけを復元先の現在の値にすれば、ロール・タイプ・レベルはスナップショットのまま書き込める
     * タイプを保てば、/etc/shadowのshadow_tのようにアクセス制御に効く部分は失われない
     *
     * @param snapshotContext スナップショット側のsecurity.selinuxの値 (末尾のNULを含んでもよい)
     * @param destinationContext 復元先の現在のsecurity.selinuxの値 (末尾のNULを含んでもよい)
     * @return 置き換えたコンテキスト (snapshotContextが末尾にNULを持つ場合は同じく付ける)
     *         形式が不正な場合と、ユーザが既に同じで置き換える意味がない場合は空
     */
    QByteArray selinuxContextWithDestinationUser(const QByteArray &snapshotContext,
                                                 const QByteArray &destinationContext);

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
     * mustPreserveMetadataがtrueで所有者か時刻を適用できない場合は、root所有のsymlinkを残すとfs.protected_symlinksの判定が変わるため、
     * renameせずに失敗する
     * renameat()の前後の検証と失敗時の後始末はreplaceRegularFileAtと同じ
     *
     * @param destinationParentFd openDestinationParentBeneathRootで開いた親dirfd
     * @param leafName 宛先の最終成分名
     * @param linkTarget 作成するsymlinkのtarget
     * @param sourceStat 復元元symlinkのstat (所有者と時刻に使う)
     * @param metadataApplied 所有者と時刻を両方適用できたかの返却先 (省略可)
     * @param mustPreserveMetadata 所有者と時刻を適用できない場合に、renameせず一時symlinkを片付けて失敗させる場合true
     * @return 差し替えに成功した場合true (失敗時はerrno設定)
     * @note 一時symlinkの作成後に失敗した場合は、作成直後に記録したinodeと一致するときだけ一時名を削除する
     */
    bool replaceSymlinkAt(int destinationParentFd, const QByteArray &leafName,
                          const QByteArray &linkTarget, const struct stat &sourceStat,
                          bool *metadataApplied = nullptr, bool mustPreserveMetadata = false);

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
