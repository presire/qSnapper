#ifndef QSNAPPER_RESTOREVALIDATION_H
#define QSNAPPER_RESTOREVALIDATION_H

#include <QHash>
#include <QString>
#include <QtGlobal>

#include <string>

#include "restoremanifest.h"

namespace qsnapper::restore {

/**
 * @brief snapper::FileのStatusFlagsと同一のビット値
 *
 * libsnapperに依存しない純粋関数群から状態ビットを扱うため、/usr/include/snapper/File.hのenum StatusFlagsの値を複製したものである
 * snapper側の値が変わった場合はここを同期させる
 */
enum RestoreStatusFlag {
    RestoreStatusCreated = 1,
    RestoreStatusDeleted = 2,
    RestoreStatusType = 4,
    RestoreStatusContent = 8,
    RestoreStatusPermissions = 16,
    RestoreStatusOwner = 32,
    RestoreStatusGroup = 64,
    RestoreStatusXattrs = 128,
    RestoreStatusAcl = 256
};

/**
 * @brief 状態ビットからクライアント契約のchangeTypeを導出する
 *
 * サーバ側シリアライザは "+-tc pugxa" の順でstatus文字列を組み立て、
 * クライアントは先頭文字のみを "created" / "deleted" / "typechanged" / "modified" へ写像する
 * その写像と完全に一致させるため、本関数は状態文字列を構築せずビットから直接、次の優先順位で導出する:
 *   1. CREATED -> "created" (全てに優先)
 *   2. DELETED -> "deleted"
 *   3. TYPE -> "typechanged"
 *   4. それ以外 (CONTENTやmetadataのみ、空も含む) -> "modified"
 *
 * @param statusBits snapper::File::getPreToPostStatus()と同一のビット列
 * @return クライアント契約のchangeType文字列
 */
QString restoreExpectedChangeType(unsigned int statusBits);

/**
 * @brief 復元entryのpathとsnapper::File::getName()を同一規則で正規化する
 *
 * クライアントのparseChangeRecord() + normalizeRestorePlanPath()と同じ規則:
 *   1. 長さ1を超える限り末尾の'/'を除去する
 *   2. 連続する'/'を1つへ潰す
 * QDir::cleanPath()は使用しない ('.' / '..'を折り畳むと既存の拒否境界を弱める)
 *
 * @param path 正規化対象の絶対パス
 * @return 正規化済みの突き合わせキー
 */
QString canonicalizeRestorePlanPathKey(const QString &path);

/**
 * @brief libsnapperが返す生のファイル名を権威名として復号・検証する
 *
 * libsnapperのfilelistキャッシュは "status + ' ' + 生ファイル名 + '\n'" という行指向の無エスケープ形式で保存され、load()は名前を一切検証しない
 * さらにread-only snapshot同士の比較ではComparisonの構築だけでsave()が走るため、改行を含むファイル名を持つsnapshotを比較すると、
 * キャッシュ再読込時に改行以降が独立した偽entryとして権威ある比較結果へ混入し得る
 *
 * この汚染された比較結果をそのまま照合の基準にすると、本preflight自体が攻撃者の用意した「安全に見えるパス」を権威として受理してしまう
 * したがってレコード区切りを破壊する制御文字を含む権威名はfail-closedで拒否する
 *
 * またgetName()はraw bytesであり、不正UTF-8はQString変換でU+FFFDへ潰れる
 * 復号結果を再符号化した値が元のバイト列と一致しない場合、「検証した名前」と「executorが実際に使う名前」がズレるため受理しない
 *
 * 1件でも拒否対象があれば計画全体を失敗させる (該当entryだけを落とすと、欠落した比較結果を完全な比較結果として扱ってしまう)
 *
 * @param rawName snapper::File::getName()の生バイト列
 * @param decodedOut 受理した場合の復号済み名の格納先 (省略可)
 * @return 権威名として受理できる場合true
 */
bool decodeAuthoritativeRestoreName(const std::string &rawName,
                                    QString *decodedOut);

/**
 * @brief 権威ある比較結果1件を正規化キーと期待changeTypeのmapへ登録する
 *
 * 異なるraw名が同一の正規化キーへ潰れ、かつ期待changeTypeが食い違う場合は検証不能として衝突を報告する (計画全体をfail-closedさせる)
 *
 * @param expectedTypes 登録先のmap (正規化キー -> 期待changeType)
 * @param rawName snapper::File::getName()の値
 * @param statusBits snapper::File::getPreToPostStatus()の値
 * @return 登録成功時true、同一キーで異なる期待型の衝突時false
 */
bool registerAuthoritativeRestoreEntry(QHash<QString, QString> *expectedTypes,
                                       const QString &rawName,
                                       unsigned int statusBits);

/**
 * @brief 凍結済みの全entryを権威ある比較mapと突き合わせて検証する
 *
 * entryはkMaxEntriesPerStageChunkを超えないbounded slice (64件ずつ) で読む
 *
 * 正規化キーがmapに存在しないentry、および期待changeTypeと一致しないエントリが1件でもあれば失敗する
 * mapに存在しない = その比較に現れないパスであり、計画に載ってよい理由がない
 *
 * @note 本検証は「best-effortの整合性検査」であり、tamper-proofなセキュリティ境界ではない
 *       mapの元になるsnapper::Comparisonは、read-only snapshot同士の比較ではlibsnapperが保存するfilelistキャッシュを優先して読む
 *       そのキャッシュは、"status + ' ' + 生ファイル名 + '\n'" という無エスケープの行指向形式であり、
 *       改行を含むファイル名を比較対象に置かれると、キャッシュ再読込時に攻撃者の選んだパスを持つ偽entryが混入し得る
 *       (decodeAuthoritativeRestoreName()は、このうち復号不能な名前と制御文字を含む名前のみを拒否できる)
 *
 *       キャッシュを公開APIから無効化する手段が無いため (Snapshot::deleteFilelists()は、private、SDir / filelist_nameは未インストール)、
 *       この経路を塞ぐことはできない
 *
 *       実際の防護線は実行段にあり、そちらはキャッシュの影響を受けない:
 *         - isConfirmedAbsentAt() が、復元元に存在するパスの削除を拒否する
 *         - applyRestoreEntry() のsource種別判定が、復元元に存在しないパスの復元を拒否する
 *       いずれも認可時にpinしたsnapshot dirfdから直接算出する
 *       本検証は、changeTypeの虚偽申告に対するコストを上げる多層防御として維持する
 *
 * @param registry 凍結済み計画を保持するregistry
 * @param manifestId owner束縛されたマニフェストID
 * @param owner 呼び出し元owner
 * @param expectedTypes 正規化キー -> 期待changeTypeのmap
 * @param err 結果エラーの格納先 (省略可)
 * @return 全entryが権威ある比較と一致した場合true
 */
bool validateFrozenEntriesAgainstAuthoritative(
    RestoreManifestRegistry &registry,
    const QString &manifestId,
    const QString &owner,
    const QHash<QString, QString> &expectedTypes,
    ManifestError *err);

/**
 * @brief config相対の復元対象名がスナップショット管理領域 (.snapshots) を指すか判定する
 *
 * 文字列の前方一致ではなく、空成分を除いた先頭成分で判定する
 * これにより"//.snapshots/..."のような空成分を挟んだ表記による迂回を防ぐ
 *
 * @param configRelativeName libsnapper File::getName()由来のconfig相対名
 * @return 先頭成分が".snapshots"の場合true
 */
bool isSnapshotMetadataRestoreName(const QString &configRelativeName);

/**
 * @brief configのSUBVOLUMEを空成分を除いた絶対パスへ正規化する
 * @param subvolume configのSUBVOLUME
 * @param rootPathOut 正規化結果 (rootサブボリュームなら"/")
 * @return 絶対パスで、"." / ".."成分・制御文字を含まない場合true
 */
bool normalizeRestoreSubvolume(const QString &subvolume, QString *rootPathOut);

/**
 * @brief config相対の復元対象名とconfigのSUBVOLUMEから、live側の宛先を組み立てる
 *
 * GetFileChanges*が返し、クライアントがStageする名前はconfig相対名 (例: SUBVOLUME=/homeでは"/alice/file") である
 * live側の宛先は"<SUBVOLUME>/<name>"であり、"/"基準で解決してはならない
 *
 * subvolume / 名前のいずれも、空成分を除いて正規化する
 * "." / ".."成分、制御文字、空の名前、".snapshots"配下の名前は拒否する
 *
 * @param subvolume configのSUBVOLUME (絶対パス)
 * @param configRelativeName config相対の復元対象名 (絶対パス表記)
 * @param rootPathOut 正規化したSUBVOLUME (*BeneathRoot系helperのrootPathに渡す)
 * @param destinationOut live側の宛先絶対パス (rootPathOut配下)
 * @param relativeOut rootPathOut (およびスナップショット dir) からの相対パス
 * @return 組み立てに成功した場合true
 */
bool buildRestoreDestination(const QString &subvolume,
                             const QString &configRelativeName,
                             QString *rootPathOut,
                             QString *destinationOut,
                             QString *relativeOut);

/**
 * @brief 復元後にrootサブボリュームをrwへ戻す安全ネットの実行条件
 */
struct RootReadWriteSafetyNetState {
    bool executionStarted = false;          // 認可済みで実行を開始した計画か
    bool targetsRootSubvolume = false;      // 対象configのSUBVOLUMEが"/"か
    bool preRestoreStateKnown = false;      // 復元前のread-only状態をcommit時に記録できたか
    bool preRestoreReadOnly = true;         // 復元前のrootサブボリュームがread-onlyだったか
};

/**
 * @brief 復元後にrootサブボリュームをrwへ戻してよいか判定する
 *
 * 次の全てを満たす場合のみtrueを返す
 *   - 認可済みで実際に実行を開始した計画である
 *   - 対象configのSUBVOLUMEが"/"である
 *   - commit時に記録した復元前の状態がrwであり、復元後にread-onlyになっている
 * 1つでも判定できない場合はfalse (特権操作を行わない) とする
 *
 * @param state commit時に記録した状態
 * @param readOnlyAfterRestore 復元後に観測したrootサブボリュームのread-only状態
 * @return rwへ戻すべき場合true
 */
bool shouldRestoreRootReadWrite(const RootReadWriteSafetyNetState &state,
                                bool readOnlyAfterRestore);

} // namespace qsnapper::restore

#endif // QSNAPPER_RESTOREVALIDATION_H
