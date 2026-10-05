/**
 * @file snapshotoperations_diff.cpp
 * @brief プロセス内unified diff生成
 *
 * Myers法による差分生成でdiffコマンドの起動を置き換える
 * 資源上限を超える入力やバイナリは省略理由だけを返す
 */

#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QStringList>
#include <QByteArray>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include <unistd.h>
#include <algorithm>
#include <vector>

// ============================================================================
// In-process unified diff (Myers diff algorithm)
// "diff -u"コマンドの置き換え
// ============================================================================

namespace qsnapper::diff {

    namespace {

        /**
         * @brief Myers diffの編集操作
         */
        struct DiffOp {
            enum Type { Equal, Delete, Insert };
            Type type;
            int aIdx, bIdx;  // 0-based index into old/new lines (-1 if N/A)
        };

        /**
         * @brief diff入力の読み込み結果
         */
        enum class DiffInputStatus {
            Ok,             // 読み込み成功
            NotRegular,     // ディレクトリやsymlink (差分の対象外)
            SpecialFile,    // FIFO・デバイス・ソケット (開かない)
            TooLarge,       // サイズまたは行数が上限を超える
            Binary,         // 先頭にNULを含む
            Unreadable      // 読み込めない
        };

        /**
         * @brief diff入力のファイルを上限付きで読み込む
         *
         * 開く前にlstatで種別とサイズを確認し、FIFOやデバイスは開かない
         * 読み込み中にファイルが伸びても、上限 + 1バイトまでしか読まない
         *
         * @param path 対象パス
         * @param limits 資源上限
         * @param contentOut 読み込んだ内容 ('\r'は除去済み)
         * @return 読み込み結果
         */
        DiffInputStatus readDiffInput(const QString &path, const UnifiedDiffLimits &limits,
                                      QByteArray *contentOut)
        {
            struct stat st;
            if (!qsnapper::security::safeLstat(path, &st)) {
                return DiffInputStatus::Unreadable;
            }
            if (S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)
                    || S_ISSOCK(st.st_mode)) {
                return DiffInputStatus::SpecialFile;
            }
            if (!S_ISREG(st.st_mode)) {
                return DiffInputStatus::NotRegular;
            }
            if (st.st_size > limits.maxFileBytes) {
                return DiffInputStatus::TooLarge;
            }

            const int fd = qsnapper::security::safeOpenRegularFileRead(path);
            if (fd < 0) {
                return DiffInputStatus::Unreadable;
            }

            QByteArray data;
            const qint64 readLimit = limits.maxFileBytes + 1;
            data.resize(static_cast<qsizetype>(qMin<qint64>(qMax<qint64>(st.st_size, 0) + 1, readLimit)));
            qint64 total = 0;
            bool readFailed = false;
            while (total < readLimit) {
                if (total == data.size()) {
                    data.resize(static_cast<qsizetype>(qMin<qint64>(qMax<qint64>(total * 2, 4096), readLimit)));
                }
                const ssize_t n = ::read(fd, data.data() + total, static_cast<size_t>(data.size() - total));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    readFailed = true;
                    break;
                }
                if (n == 0) {
                    break;
                }
                total += n;
            }
            ::close(fd);

            if (readFailed) {
                return DiffInputStatus::Unreadable;
            }
            if (total > limits.maxFileBytes) {
                return DiffInputStatus::TooLarge;
            }
            data.truncate(static_cast<qsizetype>(total));

            const qsizetype probeLength = static_cast<qsizetype>(qMin<qint64>(total, limits.binaryProbeBytes));
            if (QByteArrayView(data.constData(), probeLength).contains('\0')) {
                return DiffInputStatus::Binary;
            }
            if (data.count('\n') + 1 > limits.maxLinesPerFile) {
                return DiffInputStatus::TooLarge;
            }

            // QIODevice::Textでの読み込みと同じく'\r'を除去する
            data.replace("\r", "");
            *contentOut = data;
            return DiffInputStatus::Ok;
        }

        /**
         * Myers diffアルゴリズムで2つの行ID列間の最短編集スクリプトを計算する
         *
         * 各dステップ開始時のVのうち、バックトラックで参照する範囲 [-d, d] だけをtraceに保存する (合計 (D + 1)^2 要素)
         * V配列とtraceの合計サイズ、および探索ステップ数が上限を超えた場合は計算を打ち切る
         *
         * @param a 旧ファイルの行ID列
         * @param b 新ファイルの行ID列
         * @param limits 資源上限
         * @param opsOut 編集操作のリスト (正順)
         * @return 上限内で計算できた場合true
         */
        bool computeMyersDiff(const QVector<int> &a, const QVector<int> &b,
                              const UnifiedDiffLimits &limits, QVector<DiffOp> *opsOut)
        {
            const qsizetype N = a.size(), M = b.size();
            QVector<DiffOp> &result = *opsOut;
            result.clear();

            if (N == 0 && M == 0) return true;
            if (N == 0) {
                result.reserve(M);
                for (qsizetype i = 0; i < M; i++)
                    result.append({DiffOp::Insert, -1, static_cast<int>(i)});
                return true;
            }
            if (M == 0) {
                result.reserve(N);
                for (qsizetype i = 0; i < N; i++)
                    result.append({DiffOp::Delete, static_cast<int>(i), -1});
                return true;
            }

            const qsizetype MAX = N + M, OFF = MAX;
            const qint64 vBytes = static_cast<qint64>(2 * MAX + 1) * static_cast<qint64>(sizeof(int));
            if (vBytes > limits.maxTraceBytes) {
                return false;
            }

            // V[k + OFF] = 対角線k上の最遠到達x座標
            QVector<int> V(2 * MAX + 1, 0);

            // dステップ開始前 (= d-1ステップ終了後) のV[-d..d] (offset d*d、長さ2d+1)
            QVector<int> trace;
            qint64 steps = 0;
            qsizetype finalD = -1;

            for (qsizetype d = 0; d <= MAX && finalD < 0; d++) {
                const qint64 traceBytes = static_cast<qint64>(trace.size() + 2 * d + 1)
                                          * static_cast<qint64>(sizeof(int));
                if (vBytes + traceBytes > limits.maxTraceBytes) {
                    return false;
                }
                for (qsizetype k = -d; k <= d; k++) {
                    trace.append(V[OFF + k]);
                }

                for (qsizetype k = -d; k <= d; k += 2) {
                    if (++steps > limits.maxSteps) {
                        return false;
                    }
                    qsizetype x = (k == -d || (k != d && V[OFF + k - 1] < V[OFF + k + 1]))
                                  ? V[OFF + k + 1] : V[OFF + k - 1] + 1;
                    qsizetype y = x - k;
                    while (x < N && y < M && a[x] == b[y]) {
                        x++; y++;
                        if (++steps > limits.maxSteps) {
                            return false;
                        }
                    }
                    V[OFF + k] = static_cast<int>(x);
                    if (x >= N && y >= M) {
                        finalD = d;
                        break;
                    }
                }
            }

            // バックトラックで編集スクリプトを逆順に構築
            QVector<DiffOp::Type> revTypes;
            revTypes.reserve(N + M);
            qsizetype x = N, y = M;

            for (qsizetype d = finalD; d > 0; d--) {
                const int *vp = trace.constData() + d * d + d;  // vp[k] = d-1ステップ終了後のV[k]
                const qsizetype k = x - y;
                const bool down = (k == -d) || (k != d && vp[k - 1] < vp[k + 1]);
                const qsizetype pk = down ? k + 1 : k - 1;
                const qsizetype px = vp[pk], py = px - pk;
                const qsizetype mx = down ? px : px + 1, my = mx - k;

                // 対角線上の等号行 (snake) を逆順に記録
                while (x > mx && y > my) {
                    x--; y--;
                    revTypes.append(DiffOp::Equal);
                }

                // 非対角移動 (挿入/削除)
                revTypes.append(down ? DiffOp::Insert : DiffOp::Delete);
                x = px; y = py;
            }

            // d=0の初期snake (等号行のみ、編集なし)
            while (x > 0 && y > 0) {
                x--; y--;
                revTypes.append(DiffOp::Equal);
            }

            // 正順に反転
            std::reverse(revTypes.begin(), revTypes.end());

            // 操作タイプからインデックス付きDiffOpに変換
            result.reserve(revTypes.size());
            int ai = 0, bi = 0;
            for (auto t : revTypes) {
                switch (t) {
                    case DiffOp::Equal:
                        result.append({DiffOp::Equal, ai, bi}); ai++; bi++; break;
                    case DiffOp::Delete:
                        result.append({DiffOp::Delete, ai, -1}); ai++; break;
                    case DiffOp::Insert:
                        result.append({DiffOp::Insert, -1, bi}); bi++; break;
                }
            }
            return true;
        }

        /**
         * @brief 内容を行に分割する (末尾の改行で生じる空要素は除去する)
         * @param content ファイル内容
         * @return 行のリスト
         */
        QStringList splitDiffLines(const QByteArray &content)
        {
            QStringList lines = QString::fromUtf8(content).split('\n');
            if (!lines.isEmpty() && lines.last().isEmpty()) {
                lines.removeLast();
            }
            return lines;
        }

    }

    UnifiedDiffResult generateUnifiedDiff(const QString &oldPath, const QString &newPath,
                                          const UnifiedDiffLimits &limits)
    {
        try {
            QByteArray oldContent;
            QByteArray newContent;
            const DiffInputStatus oldStatus = readDiffInput(oldPath, limits, &oldContent);
            const DiffInputStatus newStatus = readDiffInput(newPath, limits, &newContent);

            const auto either = [&](DiffInputStatus status) {
                return oldStatus == status || newStatus == status;
            };
            if (either(DiffInputStatus::SpecialFile)) {
                return {QString(), QStringLiteral("special_file")};
            }
            if (either(DiffInputStatus::TooLarge)) {
                return {QString(), QStringLiteral("too_large")};
            }
            if (either(DiffInputStatus::Binary)) {
                return {QString(), QStringLiteral("binary")};
            }
            if (oldStatus != DiffInputStatus::Ok || newStatus != DiffInputStatus::Ok) {
                return {};
            }

            const QStringList a = splitDiffLines(oldContent);
            const QStringList b = splitDiffLines(newContent);

            // 行を整数IDへ置き換え、Myers diffの比較を整数比較にする
            QHash<QString, int> lineIds;
            const auto toIds = [&lineIds](const QStringList &lines) {
                QVector<int> ids;
                ids.reserve(lines.size());
                for (const QString &line : lines) {
                    auto it = lineIds.constFind(line);
                    if (it == lineIds.cend()) {
                        it = lineIds.insert(line, static_cast<int>(lineIds.size()));
                    }
                    ids.append(it.value());
                }
                return ids;
            };
            const QVector<int> aIds = toIds(a);
            const QVector<int> bIds = toIds(b);

            QVector<DiffOp> ops;
            if (!computeMyersDiff(aIds, bIds, limits, &ops)) {
                return {QString(), QStringLiteral("too_many_changes")};
            }

            // 変更がない場合は空文字列を返す (diff -u の差分なしと同じ挙動)
            const int context = 3;
            QVector<int> changes;
            for (int i = 0; i < ops.size(); i++) {
                if (ops[i].type != DiffOp::Equal) changes.append(i);
            }
            if (changes.isEmpty()) return {};

            // hunkにグループ化 (距離が2*context以内の変更をマージ)
            struct Hunk { int start, end; };
            QVector<Hunk> hunks;
            int hs = changes[0], he = changes[0];
            for (int i = 1; i < changes.size(); i++) {
                if (changes[i] - he <= 2 * context)
                    he = changes[i];
                else {
                    hunks.append({hs, he});
                    hs = he = changes[i];
                }
            }
            hunks.append({hs, he});

            // unified diff形式で出力
            QString out;
            out += "--- " + oldPath + "\n";
            out += "+++ " + newPath + "\n";

            for (const auto &h : hunks) {
                int s = qMax(0, h.start - context);
                int e = qMin(static_cast<int>(ops.size()) - 1, h.end + context);

                // hunk前の行数をカウント (行番号計算用)
                int aBefore = 0, bBefore = 0;
                for (int i = 0; i < s; i++) {
                    if (ops[i].type != DiffOp::Insert) aBefore++;
                    if (ops[i].type != DiffOp::Delete) bBefore++;
                }

                // hunk内の行数をカウント
                int aCount = 0, bCount = 0;
                for (int i = s; i <= e; i++) {
                    if (ops[i].type != DiffOp::Insert) aCount++;
                    if (ops[i].type != DiffOp::Delete) bCount++;
                }

                // 行番号は1ベース、空hunkの場合は0
                out += QString("@@ -%1,%2 +%3,%4 @@\n")
                    .arg(aCount == 0 ? 0 : aBefore + 1).arg(aCount)
                    .arg(bCount == 0 ? 0 : bBefore + 1).arg(bCount);

                for (int i = s; i <= e; i++) {
                    switch (ops[i].type) {
                        case DiffOp::Equal:
                            out += " " + a[ops[i].aIdx] + "\n";
                            break;
                        case DiffOp::Delete:
                            out += "-" + a[ops[i].aIdx] + "\n";
                            break;
                        case DiffOp::Insert:
                            out += "+" + b[ops[i].bIdx] + "\n";
                            break;
                    }
                }
            }

            return {out, QString()};
        }
        catch (const std::bad_alloc &) {
            return {QString(), QStringLiteral("too_large")};
        }
    }

}
