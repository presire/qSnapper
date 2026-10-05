#ifndef QSNAPPER_CSVRECORD_H
#define QSNAPPER_CSVRECORD_H

/**
 * @file csvrecord.h
 * @brief スナップショット一覧CSVのRFC 4180 quote / パースを行う純粋関数群
 *
 * D-Busサービス (producer) とクライアント (consumer) の両方から同じ実装を使うことで、
 * quote側とパース側の規約の対称性を構成によって保証する
 *
 * 対応する単体テスト: tests/unit/tst_csvrecord.cpp
 */

#include <QString>
#include <QStringList>

namespace qsnapper {
    namespace csv {
        /**
         * @brief 1つのCSVフィールドをRFC 4180形式でquoteする
         *
         * 仕様:
         *   - カンマ / 二重引用符 / CR / LF のいずれかを含むフィールドは二重引用符で囲み、
         *     内部の二重引用符は "" へエスケープする
         *   - 上記の文字を含まないフィールドは無quoteのまま返す (旧クライアントとの互換性維持)
         *
         * @note 本関数は表示上の列ずれを防ぐためのものであり、入力検証の代替ではない
         *       制御文字を含むメタデータは呼び出し側の qsnapper::security::isRecordSafeText()
         *       によりfail-closedで拒否されるため、CR / LFを含む値はここへ到達しない
         *
         * @param field quote対象のフィールド値
         * @return CSV行へ安全に埋め込めるフィールド
         */
        inline QString quoteField(const QString &field)
        {
            if (!field.contains(QLatin1Char(','))
                    && !field.contains(QLatin1Char('"'))
                    && !field.contains(QLatin1Char('\r'))
                    && !field.contains(QLatin1Char('\n'))) {
                return field;
            }

            QString escaped = field;
            escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QLatin1Char('"') + escaped + QLatin1Char('"');
        }

        /**
         * @brief RFC 4180形式の1レコード (1行) をフィールドへ分割する
         *
         * 仕様:
         *   - 二重引用符で始まるフィールドは閉じ引用符までを読み、"" は 1個の " へ展開する
         *   - 二重引用符で始まらないフィールドは次のカンマまでを文字通り扱う
         *     (内部に散在する " も含む。旧サーバーの無quote出力との互換性維持)
         *   - 空フィールド、前後の空白、引用符のみの空フィールド ("" )、末尾の空フィールドを保持する
         *   - 空レコードは1つの空フィールドを返す (QString::split(',') と同一)
         *
         * @note レコード分割 (改行での split) は本関数の責務ではない
         *       サーバー側の qsnapper::security::isRecordSafeText() が制御文字 (改行含む) を
         *       fail-closedで拒否するため、呼び出し側は1行 = 1レコードの前提を維持できる
         *
         * @param record CSVの1行 (改行を含まないこと)
         * @return 分割されたフィールドのリスト
         */
        inline QStringList splitRecord(const QString &record)
        {
            QStringList fields;
            QString current;
            bool inQuotes = false;

            const int length = record.length();
            int i = 0;
            while (i < length) {
                const QChar ch = record.at(i);
                if (inQuotes) {
                    if (ch == QLatin1Char('"')) {
                        if (i + 1 < length && record.at(i + 1) == QLatin1Char('"')) {
                            current += ch;
                            ++i;
                        }
                        else {
                            inQuotes = false;
                        }
                    }
                    else {
                        current += ch;
                    }
                }
                else if (ch == QLatin1Char('"') && current.isEmpty()) {
                    inQuotes = true;
                }
                else if (ch == QLatin1Char(',')) {
                    fields.append(current);
                    current.clear();
                }
                else {
                    current += ch;
                }
                ++i;
            }
            fields.append(current);
            return fields;
        }

        /**
         * @brief CreateSnapshotの応答CSVから、作成されたスナップショットの番号を取り出す
         *
         * 応答はヘッダ行 ("number,...") と作成されたスナップショット1件のレコード行から成る
         * 一覧の末尾を新規作成分とみなすと、並行して作成された別のスナップショットや
         * 番号の並び順によって取り違えるため、サーバーが返した番号で特定する
         *
         * @param reply CreateSnapshotの応答文字列
         * @return スナップショット番号 (1以上)、応答の形式が不正な場合: -1
         */
        inline int parseCreatedSnapshotNumber(const QString &reply)
        {
            const QStringList lines = reply.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (lines.size() != 2 || splitRecord(lines.at(0)).value(0) != QLatin1String("number")) {
                return -1;
            }

            bool ok = false;
            const int number = splitRecord(lines.at(1)).value(0).toInt(&ok);
            return ok && number > 0 ? number : -1;
        }
    } // namespace csv
} // namespace qsnapper

#endif // QSNAPPER_CSVRECORD_H
