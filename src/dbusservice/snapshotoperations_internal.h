/**
 * @file snapshotoperations_internal.h
 * @brief dbusservice内部で共有する小さな補助関数の宣言
 *
 * 匿名名前空間では複数のcppから使えないため、qsnapper::internalにinline定義する
 * 所有者名や権限表記、一時退避パスの組み立てだけを置く
 * 本ヘッダはrootサービスの実装専用で、D-Bus公開APIには含めない
 */
#ifndef SNAPSHOTOPERATIONS_INTERNAL_H
#define SNAPSHOTOPERATIONS_INTERNAL_H

#include <QString>
#include <QLatin1Char>
#include <QRandomGenerator>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <limits.h>

namespace qsnapper::internal {


    /**
     * @brief 一時退避先の兄弟パスを組み立てる
     *
     * 退避先は書き込み可能な親ディレクトリの中に作られ得るため、他のユーザが名前を予測して先回りできないよう
     * 接尾辞に暗号論的な乱数を使う (pidや時刻は外から推測できる)
     * leaf名がNAME_MAXを超えないよう、UTF-8の文字境界を壊さずに文字単位で切り詰める
     *
     * @param path 元パス
     * @param tag 用途を示す識別子
     * @return 退避先の兄弟パス
     */
    inline QString siblingTemporaryPath(const QString &path, const QString &tag)
    {
        const int slashIndex = path.lastIndexOf(QLatin1Char('/'));
        const QString dirPath = slashIndex <= 0 ? QStringLiteral("/") : path.left(slashIndex);
        const QString baseName = slashIndex < 0 ? path : path.mid(slashIndex + 1);

        // suffixはASCIIのみで構成されるため、文字数がそのままbyte数になる
        const QString suffix = QStringLiteral(".") + tag + QStringLiteral(".")
                + QStringLiteral("%1").arg(QRandomGenerator::system()->generate64(), 16, 16,
                                           QLatin1Char('0'));

        // 長いbase名はここで切り詰めるが、一意性は乱数の接尾辞で保たれる
        const qsizetype maxBaseBytes = static_cast<qsizetype>(NAME_MAX) - 1 - suffix.size();
        QString trimmedBase = baseName;
        while (!trimmedBase.isEmpty() && trimmedBase.toUtf8().size() > maxBaseBytes) {
            trimmedBase.chop(1);
        }

        return dirPath
                + QLatin1Char('/')
                + QLatin1Char('.')
                + trimmedBase
                + suffix;
    }

    /**
     * @brief UIDに対応するユーザ名を返す
     *
     * 解決できないUIDは数値表記に落とす
     *
     * @param uid ユーザID
     * @return ユーザ名または数値表記
     */
    inline QString ownerName(uid_t uid)
    {
        if (passwd *pwd = ::getpwuid(uid)) {
            return QString::fromLocal8Bit(pwd->pw_name);
        }
        return QString::number(uid);
    }

    /**
     * @brief GIDに対応するグループ名を返す
     *
     * 解決できないGIDは数値表記に落とす
     *
     * @param gid グループID
     * @return グループ名または数値表記
     */
    inline QString groupName(gid_t gid)
    {
        if (group *grp = ::getgrgid(gid)) {
            return QString::fromLocal8Bit(grp->gr_name);
        }
        return QString::number(gid);
    }

    /**
     * @brief modeの権限ビットを4桁8進表記へ変換する
     *
     * @param mode ファイルmode
     * @return 4桁8進文字列
     */
    inline QString permsToOctal(mode_t mode)
    {
        return QString("%1").arg(static_cast<unsigned int>(mode & 07777), 4, 8, QChar('0'));
    }

} // namespace qsnapper::internal

#endif // SNAPSHOTOPERATIONS_INTERNAL_H
