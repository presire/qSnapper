/**
 * @file inputvalidator.cpp
 * @brief inputvalidator.h宣言の実装
 *
 * 本ファイルは、FSへのアクセスを行わない純粋関数のみを含む
 * テスト: tests/unit/tst_configname.cpp, tests/unit/tst_filepaths.cpp
 */

#include <QRegularExpression>
#include <QString>
#include <filesystem>
#include "inputvalidator.h"

namespace qsnapper {
    namespace security {
        namespace {
            /**
             * @brief configName許容文字の正規表現 (ASCII-only allowlist)
             *
             * プログラム開始時に1度だけコンパイルされ、以降の呼び出しで使い回される
             * QRegularExpressionは、thread-safeなconst操作のみを行えば再入可能
             *
             * PCREの '$' は末尾の改行の直前にも一致するため ("root\n" を受理してしまう)、
             * anchoredPattern() で "\A(?:...)\z" に包み、文字列全体の一致を要求する
             */
            const QRegularExpression &configNameRegex()
            {
                static const QRegularExpression re(
                    QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_.-]+")));
                return re;
            }

            /**
             * @brief 制御文字 (非印字文字) を含むかを判定する補助関数
             *
             * 拒否対象は以下の Unicode 範囲:
             *   - C0 制御文字  : U+0000 .. U+001F (NUL/HT/LF/CR/ESC 等を含む)
             *   - DEL         : U+007F
             *   - C1制御文字   : U+0080 .. U+009F
             *
             * QStringが内部でUTF-16を保持しているため、制御文字の埋め込みは正規表現よりQChar比較で確実に検出できる
             * ファイルパスやconfig名にこれらが正当に含まれることはない
             */
            bool containsDangerousChar(const QString &s)
            {
                for (const QChar &c : s) {
                    const ushort u = c.unicode();
                    if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
                        return true;
                    }
                }
                return false;
            }

        } // namespace

        /**
         * @brief configNameを軽量に検証する
         *
         * 詳細な契約は inputvalidator.h 側の説明を参照すること
         */
        bool validateConfigName(const QString &name)
        {
            if (name.isEmpty()) {
                return false;
            }
            if (name.size() > 255) {
                return false;
            }
            if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
                return false;
            }
            if (name.startsWith(QLatin1Char('-'))) {
                return false;
            }
            const QRegularExpressionMatch m = configNameRegex().match(name);
            return m.hasMatch();
        }

        /**
         * @brief 絶対ファイルパスとして扱える最小条件を検証する
         *
         * 空文字、制御文字、relative path、PATH_MAX 超過を拒否する
         * 実在確認やsymlink解決は行わず、純粋な文字列検証に留める
         */
        bool validateAbsoluteFilePath(const QString &path)
        {
            if (path.isEmpty()) {
                return false;
            }
            if (containsDangerousChar(path)) {
                return false;
            }
            if (!path.startsWith(QLatin1Char('/'))) {
                return false;
            }
            return path.size() <= PATH_MAX - 1;
        }

        /**
         * @brief snapshot root containmentを純粋文字列処理で判定する
         *
         * 詳細な設計意図と制約は、inputvalidator.h側の説明を参照すること
         */
        bool isPathWithinSnapshotRoot(const QString &filePath, const QString &snapshotRoot)
        {
            if (!validateAbsoluteFilePath(filePath) || snapshotRoot.isEmpty()) {
                return false;
            }
            if (containsDangerousChar(snapshotRoot)) {
                return false;
            }
            if (!snapshotRoot.startsWith(QLatin1Char('/'))) {
                return false;
            }
            // PATH_MAXを超えるパスはFS側でもエラーになるので即時棄却
            if (filePath.size() > PATH_MAX - 1) {
                return false;
            }

            try {
                namespace fs = std::filesystem;

                // 末尾の余分な '/' を落としてから正規化
                // (lexically_normalは末尾スラッシュを残すことがあり、文字列比較の結果がぶれるため)
                QString rootTrim = snapshotRoot;
                while (rootTrim.size() > 1 && rootTrim.endsWith(QLatin1Char('/'))) {
                    rootTrim.chop(1);
                }

                const fs::path p    = fs::path(filePath.toStdString()).lexically_normal();
                const fs::path root = fs::path(rootTrim.toStdString()).lexically_normal();

                const std::string ps = p.string();
                const std::string rs = root.string();

                // (a) 長さがroot未満ならプレフィクス成立不可
                if (ps.size() < rs.size()) {
                    return false;
                }
                // (b) 先頭がrootと一致しなければ拒否
                if (ps.compare(0, rs.size(), rs) != 0) {
                    return false;
                }
                // (c) 完全一致 (=root自体)、または root 直後がパス区切り '/' (= 子要素) のみ許可
                //     これがないと "/a/root" と "/a/root-evil" を取り違える
                return ps.size() == rs.size() || ps[rs.size()] == '/';
            }
            catch (const std::exception &) {
                return false;
            }
        }

        /**
         * @brief レコード区切りを破壊する文字が含まれていないかを検証する
         *
         * 詳細な契約は、inputvalidator.h側の説明を参照すること
         */
        bool isRecordSafeText(const QString &value)
        {
            return !containsDangerousChar(value);
        }

        /**
         * @brief レコード区切りを破壊する文字をU+FFFDに置き換える
         *
         * 詳細な契約は、inputvalidator.h側の説明を参照すること
         */
        QString sanitizeRecordText(const QString &value)
        {
            QString result = value;
            for (QChar &c : result) {
                const ushort u = c.unicode();
                if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
                    c = QChar::ReplacementCharacter;
                }
            }
            return result;
        }

        namespace {
            /**
             * @brief 先頭0なしの10進整数 (0..999999999) を表す部分パターン
             *
             * 9桁に制限することで、snapper側の整数変換で桁あふれしない値に限定する
             */
            const QString &counterPattern()
            {
                static const QString pattern = QStringLiteral("(?:0|[1-9][0-9]{0,8})");
                return pattern;
            }

            /**
             * @brief 値の形式を、文字列全体の一致で判定する
             * @param value 検査対象の値
             * @param pattern アンカーなしの正規表現
             * @return 文字列全体が一致する場合: true
             */
            bool matchesWhole(const QString &value, const QString &pattern)
            {
                const QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
                return re.match(value).hasMatch();
            }

            /**
             * @brief 個数のキーの値 ("N" または "最小-最大") を検証する
             * @param value 検査対象の値
             * @return 形式が正しく、範囲の場合は最小 <= 最大である場合: true
             */
            bool isValidCountOrRange(const QString &value)
            {
                if (matchesWhole(value, counterPattern())) {
                    return true;
                }

                const QRegularExpression rangeRe(QRegularExpression::anchoredPattern(
                    QStringLiteral("(%1)-(%1)").arg(counterPattern())));
                const QRegularExpressionMatch m = rangeRe.match(value);
                if (!m.hasMatch()) {
                    return false;
                }
                return m.captured(1).toLongLong() <= m.captured(2).toLongLong();
            }
        } // namespace

        /**
         * @brief WriteSnapperConfigの設定を許可リストで検証する
         *
         * 詳細な契約は、inputvalidator.h側の説明を参照すること
         */
        bool validateSnapperConfigSettings(const QMap<QString, QString> &settings)
        {
            enum class ValueKind { Boolean, Seconds, CountOrRange, Fraction };

            static const QMap<QString, ValueKind> allowedKeys = {
                { QStringLiteral("NUMBER_CLEANUP"),           ValueKind::Boolean },
                { QStringLiteral("TIMELINE_CREATE"),          ValueKind::Boolean },
                { QStringLiteral("TIMELINE_CLEANUP"),         ValueKind::Boolean },
                { QStringLiteral("EMPTY_PRE_POST_CLEANUP"),   ValueKind::Boolean },
                { QStringLiteral("BACKGROUND_COMPARISON"),    ValueKind::Boolean },
                { QStringLiteral("NUMBER_MIN_AGE"),           ValueKind::Seconds },
                { QStringLiteral("TIMELINE_MIN_AGE"),         ValueKind::Seconds },
                { QStringLiteral("EMPTY_PRE_POST_MIN_AGE"),   ValueKind::Seconds },
                { QStringLiteral("NUMBER_LIMIT"),             ValueKind::CountOrRange },
                { QStringLiteral("NUMBER_LIMIT_IMPORTANT"),   ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_HOURLY"),    ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_DAILY"),     ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_WEEKLY"),    ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_MONTHLY"),   ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_QUARTERLY"), ValueKind::CountOrRange },
                { QStringLiteral("TIMELINE_LIMIT_YEARLY"),    ValueKind::CountOrRange },
                { QStringLiteral("SPACE_LIMIT"),              ValueKind::Fraction },
                { QStringLiteral("FREE_LIMIT"),               ValueKind::Fraction },
            };

            if (settings.isEmpty()) {
                return false;
            }

            for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
                const auto kind = allowedKeys.constFind(it.key());
                if (kind == allowedKeys.constEnd()) {
                    return false;
                }

                const QString &value = it.value();
                bool valid = false;
                switch (kind.value()) {
                    case ValueKind::Boolean:
                        valid = value == QLatin1String("yes") || value == QLatin1String("no");
                        break;
                    case ValueKind::Seconds:
                        valid = matchesWhole(value, counterPattern());
                        break;
                    case ValueKind::CountOrRange:
                        valid = isValidCountOrRange(value);
                        break;
                    case ValueKind::Fraction:
                        // 0以上1以下に限定する (1.5 などの1を超える値は "1" の分岐に一致しない)
                        valid = matchesWhole(value, QStringLiteral("0(?:\\.[0-9]{1,6})?|1(?:\\.0{1,6})?"));
                        break;
                }
                if (!valid) {
                    return false;
                }
            }
            return true;
        }
    } // namespace security
} // namespace qsnapper
