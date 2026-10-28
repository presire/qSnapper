#ifndef DISPLAYTEXT_H
#define DISPLAYTEXT_H

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

/**
 * @file displaytext.h
 * @brief 他のユーザが付けた名前 (パスなど) を、表示する前に無害化する
 */

/**
 * @class DisplayText
 * @brief QMLが表示する文字列を無害化するシングルトン
 *
 * RestorePreviewDialogとCompareSnapshotsDialogが、パスやエラー文言の表示に使う
 * QMLにJavaScriptファイルを持たせないため、置き換えの規則をC++に置く
 * インラインコンポーネントからは外部のidを参照できないため、型名で呼び出せるシングルトンにしている
 * 状態を持たないため、QMLエンジンが生成して所有する
 */
class DisplayText : public QObject
{
    Q_OBJECT
    QML_ELEMENT     // qmllintが型を解決できるように、ビルド時の型情報 (qmltypes) へ出力する
    QML_SINGLETON   // 型情報上もシングルトンとして扱わせ、qmllintにインスタンス化を要求させない

public:
    /**
     * @brief DisplayTextを構築する
     * @param parent Qt親オブジェクト
     */
    explicit DisplayText(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /**
     * @brief 双方向制御文字 (U+202A..U+202E、U+2066..U+2069) を置換文字 (U+FFFD) に置き換える
     * @param text 表示する文字列 (QMLのundefinedとnullは空文字列として扱う)
     * @return 置き換えた文字列
     * @note U+202E (RLO) などはPlainTextでも表示の並びを入れ替え、復元や削除の対象を別のパスに見せかけられる
     *       戻り値は表示だけに使い、復元要求や選択状態には元のパスを使う
     *       LRM / RLM / ALMは並びを入れ替えないため残す (D-Busサービス側のsanitizeRecordTextと同じ)
     *       改行などの制御文字は残す
     *       呼び出し側は複数のパスを改行で連結してから渡すため、置き換えると一覧の区切りが消える
     *       引数をQStringにすると、Qt 6.12以降はundefinedが文字列の"undefined"へ変換されて届くため、
     *       QJSValueで受け取り、undefinedとnullを自前で空文字列にする
     */
    Q_INVOKABLE QString sanitize(const QJSValue &text) const
    {
        QString result = (text.isUndefined() || text.isNull()) ? QString() : text.toString();
        for (QChar &c : result) {
            const char16_t u = c.unicode();
            if ((u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)) {
                c = QChar::ReplacementCharacter;
            }
        }
        return result;
    }
};

#endif // DISPLAYTEXT_H
