#ifndef QSNAPPER_RESTOREPLANSTATUS_H
#define QSNAPPER_RESTOREPLANSTATUS_H

/**
 * @file restoreplanstatus.h
 * @brief GetRestorePlanStatusが返すCSVの1レコードを解析する
 *
 * qsnapper-dbus-serviceが返すフィールドの並びは次の通り
 *   0 id、1 state、2 totalEntries、3 cursor、4 processed、5 mode、6 configName、7 snapshotNumber、8 lastError
 *   9 deletedCount、10 skippedCount、11 failedIndex、12 failedPath、13 skippedPaths (改行区切り、先頭100件まで)
 * 9〜13は後から追加されたフィールドであり、旧バージョンのqsnapper-dbus-serviceが返す9フィールドの応答も正常な応答として受け付ける
 *
 * 利用する側 : FileChangeModelの復元完了処理と、復元結果を表示するQML
 * 依存 : csvrecord.h (D-Bus応答のCSVをRFC 4180に従って分割する)
 *
 * lastErrorとfailedPathとskippedPathsには復元対象のパスが含まれ得るため、
 * 呼び出し側はこれらの内容をログへ出力してはならない (画面表示にだけ使う)
 *
 * 対応する単体テスト: tests/unit/tst_restoreplanstatus.cpp
 */

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include "csvrecord.h"

namespace qsnapper {
    // 復元計画の状態通知
    namespace restore {
        /**
         * @struct RestorePlanStatus
         * @brief GetRestorePlanStatusの応答を解析した結果
         */
        struct RestorePlanStatus
        {
            bool valid = false;         // 必須の9フィールドを解析できたかどうか
            QString id;                 // 復元計画のマニフェストID
            QString state;              // 状態 (staging / frozen / running / completed / failed / cancelled)
            int totalEntries = 0;       // 計画の総エントリ数
            int cursor = 0;             // 次に処理するエントリの位置
            int processed = 0;          // 処理済みのエントリ数 (スキップしたエントリを含む)
            QString mode;               // 復元方式 (yast / direct)
            QString configName;         // Snapper設定名
            int snapshotNumber = 0;     // 復元元のスナップショット番号
            QString lastError;          // 最後のエラー文言 (パスを含み得る)

            bool hasExtended = false;   // 追加の5フィールドを受け取ったかどうか (旧サーバではfalse)
            int deletedCount = 0;       // 現在のシステム上でパスを削除したエントリ数
            int skippedCount = 0;       // 復元元がFIFO、ソケット、デバイスのためスキップしたエントリ数
            int failedIndex = -1;       // 失敗したエントリの位置 (0始まり、失敗が無ければ-1)
            QString failedPath;         // 失敗したエントリのパス (失敗が無ければ空)
            QStringList skippedPaths;   // スキップしたパス (qsnapper-dbus-serviceが返す先頭100件まで)

            /**
             * @brief 実際に復元を適用したエントリ数を返す
             * @return processedからskippedCountを引いた値、負にはならない
             */
            int appliedCount() const
            {
                return qMax(0, processed - skippedCount);
            }

            /**
             * @brief QMLへ渡すためのマップへ変換する
             * @return 解析結果を表すQVariantMap
             */
            QVariantMap toVariantMap() const
            {
                QVariantMap map;
                map.insert(QStringLiteral("valid"), valid);
                map.insert(QStringLiteral("state"), state);
                map.insert(QStringLiteral("totalEntries"), totalEntries);
                map.insert(QStringLiteral("processed"), processed);
                map.insert(QStringLiteral("hasExtended"), hasExtended);
                map.insert(QStringLiteral("appliedCount"), appliedCount());
                map.insert(QStringLiteral("deletedCount"), deletedCount);
                map.insert(QStringLiteral("skippedCount"), skippedCount);
                map.insert(QStringLiteral("failedIndex"), failedIndex);
                map.insert(QStringLiteral("failedPath"), failedPath);
                map.insert(QStringLiteral("lastError"), lastError);
                map.insert(QStringLiteral("skippedPaths"), QVariant::fromValue(skippedPaths));
                return map;
            }
        };

        /**
         * @brief GetRestorePlanStatusの応答CSVを解析する
         *
         * 必須の9フィールドのどれかが欠けている、数値でない、または引用符が閉じていない場合は、valid=falseの結果を返す
         * 追加の5フィールドは、14個そろっていて数値が正しい場合だけ採用する
         * 9個だけの旧形式や、途中までしか無い応答はhasExtended=falseとして扱い、追加分の値は既定値のままにする
         *
         * @param statusCsv 応答のCSV (末尾の改行は取り除く)
         * @return 解析結果
         */
        inline RestorePlanStatus parseRestorePlanStatus(const QString &statusCsv)
        {
            RestorePlanStatus status;

            // 末尾の改行はレコードの一部ではない (フィールド内の改行は引用符の内側にあり、ここでは取り除かれない)
            QString record = statusCsv;
            while (record.endsWith(QLatin1Char('\n')) || record.endsWith(QLatin1Char('\r'))) {
                record.chop(1);
            }

            const QStringList fields = csv::splitRecord(record);
            constexpr int legacyFieldCount = 9;         // 追加フィールドが無い旧形式のフィールド数
            constexpr int extendedFieldCount = 14;      // 追加フィールドを含む現行形式のフィールド数
            if (fields.size() < legacyFieldCount) {
                return status;
            }

            bool totalOk = false;
            bool cursorOk = false;
            bool processedOk = false;
            bool snapshotOk = false;
            const int totalEntries = fields.at(2).toInt(&totalOk);
            const int cursor = fields.at(3).toInt(&cursorOk);
            const int processed = fields.at(4).toInt(&processedOk);
            const int snapshotNumber = fields.at(7).toInt(&snapshotOk);
            if (!totalOk || !cursorOk || !processedOk || !snapshotOk) {
                return status;
            }

            status.id = fields.at(0);
            status.state = fields.at(1);
            status.totalEntries = totalEntries;
            status.cursor = cursor;
            status.processed = processed;
            status.mode = fields.at(5);
            status.configName = fields.at(6);
            status.snapshotNumber = snapshotNumber;
            status.lastError = fields.at(8);
            status.valid = true;

            if (fields.size() >= extendedFieldCount) {
                bool deletedOk = false;
                bool skippedOk = false;
                bool failedIndexOk = false;
                const int deletedCount = fields.at(9).toInt(&deletedOk);
                const int skippedCount = fields.at(10).toInt(&skippedOk);
                const int failedIndex = fields.at(11).toInt(&failedIndexOk);
                if (deletedOk && skippedOk && failedIndexOk) {
                    status.hasExtended = true;
                    status.deletedCount = qMax(0, deletedCount);
                    status.skippedCount = qMax(0, skippedCount);
                    status.failedIndex = failedIndex >= 0 ? failedIndex : -1;
                    status.failedPath = fields.at(12);
                    if (!fields.at(13).isEmpty()) {
                        status.skippedPaths = fields.at(13).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                    }
                }
            }

            return status;
        }
    } // namespace restore
} // namespace qsnapper

#endif // QSNAPPER_RESTOREPLANSTATUS_H
