#ifndef QSNAPPER_CONFIGPATHVALIDATION_H
#define QSNAPPER_CONFIGPATHVALIDATION_H

/**
 * @file configpathvalidation.h
 * @brief config作成と削除で、root以外が差し替えられないパスかどうかを確かめる関数群
 *
 * config作成と削除はsnapperdへ転送し、snapperdはパスの文字列から対象を開き直す
 * そのため、本サービスが検査した対象とsnapperdが使う対象を一致させるには、
 * 途中の全てのディレクトリがrootだけに書き込めること (root以外がパスの指す先を差し替えられないこと) を確かめる必要がある
 * 各成分はO_PATHとO_NOFOLLOWで1つずつ開き、symlinkを辿らずに所有者と権限を確かめる
 * mount pointを跨ぐことは許す (RESOLVE_NO_XDEVに相当する制限は付けない)
 *
 * 対応する単体テスト:
 *   - tests/unit/tst_configpathvalidation.cpp
 */

#include <QString>
#include <sys/stat.h>

namespace qsnapper::security {

    /**
     * @class TrustedPathResult
     * @brief パスの検査結果
     *
     * 呼び出し側はOk以外を全て拒否として扱い、種類は利用者向けの固定の文言を選ぶためだけに使う
     */
    enum class TrustedPathResult {
        Ok,             // 全ての成分がsymlinkでなく、rootだけが書き込める
        Missing,        // いずれかの成分が存在しない
        SymbolicLink,   // いずれかの成分がsymlinkである
        Untrusted,      // いずれかの成分がroot以外の所有、またはgroupかotherが書き込める
        WrongType,      // 途中の成分がディレクトリでない、または最後の成分が期待する種類でない
        Failed          // 文字列の形式が不正、またはシステムコールが想定外の理由で失敗した
    };

    /**
     * @brief 新しく作るSnapper設定名が安全かどうかを判定する
     *
     * 既存の設定を指すvalidateConfigNameよりも狭く、英数字で始まる英数字・下線・ハイフンの1から64文字だけを許す
     * libsnapperは名前を/etc/snapper/configs/の下のファイル名とsysconfigの値にそのまま埋め込み、
     * カンマ・空白・タブ以外を検査しないため、ここで「/」「..」・引用符・改行を確実に除く
     * 末尾の改行も拒否するため、正規表現は全体の一致で判定する
     *
     * @param name D-Busから受け取った設定名
     * @return 安全な場合: true
     */
    bool validateNewConfigName(const QString &name);

    /**
     * @brief config作成と削除で扱うsubvolumeのパスの文字列が安全かどうかを判定する
     *
     * 正規化済みの絶対パス (「//」「.」「..」と末尾の「/」を含まない) で、英数字と「._@+-/」だけを許す
     * libsnapperはSUBVOLUMEを設定ファイルへ引用符で囲んだだけで書き込むため、
     * 引用符・バックスラッシュ・ドル記号・空白・制御文字を値に含めると、設定ファイルの解釈が変わり得る
     * 「/」そのものはrootの設定に当たるため、ここでは許可し、呼び出し側が別に拒否する
     *
     * @param path D-Busから受け取った、または設定ファイルから読んだパス
     * @return 安全な場合: true
     */
    bool validateConfigSubvolumePath(const QString &path);

    /**
     * @brief 途中の全ての成分を検査してディレクトリを開く
     *
     * 最後の成分を含む全てのディレクトリについて、symlinkでなく、rootが所有し、groupとotherが書き込めないことを確かめる
     * @param absolutePath validateConfigSubvolumePathなどで検証済みの絶対パス
     * @param fdOut 成功時に最後のディレクトリのO_PATHのfdを受け取る (呼び出し側が閉じる)
     * @param statOut 成功時に最後のディレクトリのfstatの結果を受け取る (省略可)
     * @return 検査結果 (Ok以外では*fdOutに-1を設定する)
     */
    TrustedPathResult openTrustedDirectory(const QString &absolutePath, int *fdOut, struct stat *statOut = nullptr);

    /**
     * @brief 親ディレクトリを検査し、通常ファイルがrootだけに書き込めることを確かめる
     *
     * ハードリンクを通じて別の場所から内容を書き換えられないよう、リンク数が1であることも求める
     * @param absolutePath 検証済みの絶対パス
     * @return 検査結果
     */
    TrustedPathResult checkTrustedRegularFile(const QString &absolutePath);

    /**
     * @brief 親ディレクトリを検査し、最後の成分が存在しないことを確かめる
     *
     * 存在しない場合だけOkを返し、存在する場合は種類を問わずWrongTypeを返す
     * @param absolutePath 検証済みの絶対パス
     * @return 検査結果
     */
    TrustedPathResult checkTrustedAbsent(const QString &absolutePath);

    /**
     * @brief fdがBtrfsのsubvolumeの最上位ディレクトリを指すかどうかを判定する
     *
     * Btrfsではsubvolumeの最上位ディレクトリのinode番号が常に256であることを使う
     * @param fd 検査するディレクトリのfd (O_PATHでもよい)
     * @param statOut fstatの結果を受け取る (省略可)
     * @return Btrfsのsubvolumeの最上位ディレクトリの場合: true
     */
    bool isBtrfsSubvolumeRoot(int fd, struct stat *statOut = nullptr);

} // namespace qsnapper::security

#endif // QSNAPPER_CONFIGPATHVALIDATION_H
