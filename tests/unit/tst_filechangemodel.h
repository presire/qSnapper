/**
 * @file tst_filechangemodel.h
 * @brief FileChangeModel単体テストの共有宣言
 *
 * テスト対象へD-Busなしで応答を注入するTestableFileChangeModelと
 * 復元計画呼び出しを記録するFakeRestorePlanTransportと
 * スロット定義を複数cppへ分散させるTestFileChangeModelの宣言を収める
 * mocはこのヘッダー経由で適用する
 */

#ifndef TST_FILECHANGEMODEL_H
#define TST_FILECHANGEMODEL_H

#include <QtTest>
#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include "filechangemodel.h"

/**
 * @class TestableFileChangeModel
 * @brief D-Bus呼び出しを保留してテスト側から完了駆動するモデル
 *
 * setupModelDataとsetComparisonRangeを公開し
 * requestStringReplyを上書きして応答の完了順序を任意に制御する
 */
class TestableFileChangeModel : public FileChangeModel
{
public:
    struct PendingStringReply
    {
        QString methodName;
        QVariantList arguments;
        AsyncStringReplyHandler done;
    };

    using FileChangeModel::setupModelData;
    using FileChangeModel::setComparisonRange;

    QList<PendingStringReply> pendingStringReplies;

    void requestStringReply(const QString &methodName,
                            const QVariantList &arguments,
                            AsyncStringReplyHandler done) override
    {
        pendingStringReplies.append({methodName, arguments, done});
    }

    void completeStringReply(int index, bool ok, const QString &value = QString(),
                             const QString &error = QString())
    {
        Q_ASSERT(index >= 0 && index < pendingStringReplies.size());
        const PendingStringReply pending = pendingStringReplies.takeAt(index);
        pending.done(ok, value, error);
    }
};

/**
 * @brief RestorePlanTransportのテスト用モック
 *
 * 全ての呼び出しを記録し、コールバックの完了をテスト側から駆動できる
 * シグナル推送はsubscribePlanSignals()で登録されたreceiverのスロットをQMetaObject::invokeMethodで直接呼び出すことで模倣する
 */
class FakeRestorePlanTransport : public RestorePlanTransport
{
public:
    struct Call
    {
        enum Kind { Begin, Stage, Commit, Cancel, Status };
        Kind kind = Begin;
        QString configName;
        int snapshotNumber = 0;
        int counterpartNumber = 0;
        QString restoreMode;
        QString manifestId;
        QStringList paths;
        QStringList changeTypes;
    };

    QList<Call> calls;
    QString nextManifestId = QStringLiteral("manifest-1");
    QObject *receiver = nullptr;
    int unsubscribeCount = 0;

    int countKind(Call::Kind kind) const
    {
        int count = 0;
        for (const Call &call : calls) {
            if (call.kind == kind) {
                ++count;
            }
        }
        return count;
    }

    void completePending(bool ok, const QString &value = QString(), const QString &error = QString())
    {
        if (m_pending.isEmpty()) {
            return;
        }
        const Pending pending = m_pending.takeFirst();
        pending.invoke(ok, value, error);
    }

    void completeAllPendingOk()
    {
        while (!m_pending.isEmpty()) {
            const Pending pending = m_pending.takeFirst();
            pending.invoke(true, pending.kind == Call::Begin ? nextManifestId : QString(), QString());
        }
    }

    void emitPlanProgress(const QString &manifestId, int current, int total)
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanProgress",
                                  Q_ARG(QString, manifestId), Q_ARG(int, current),
                                  Q_ARG(int, total));
    }

    void emitPlanFinished(const QString &manifestId, const QString &terminalState, const QString &message)
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanFinished",
                                  Q_ARG(QString, manifestId), Q_ARG(QString, terminalState),
                                  Q_ARG(QString, message));
    }

    /**
     * @brief 復元サービスの消失通知を送出する
     */
    void emitServiceVanished()
    {
        if (!receiver) {
            return;
        }
        QMetaObject::invokeMethod(receiver, "onRestorePlanServiceVanished");
    }

    // --- RestorePlanTransport interface ---

    void beginPlan(const QString &configName, int snapshotNumber,
                   int counterpartSnapshotNumber, const QString &restoreMode,
                   std::function<void(bool ok, const QString &manifestId, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Begin;
        call.configName = configName;
        call.snapshotNumber = snapshotNumber;
        call.counterpartNumber = counterpartSnapshotNumber;
        call.restoreMode = restoreMode;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Begin;
        pending.invoke = [done](bool ok, const QString &value, const QString &error) {
            done(ok, value, error);
        };
        m_pending.append(pending);
    }

    void stageEntries(const QString &manifestId, const QStringList &paths, const QStringList &changeTypes,
                      std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Stage;
        call.manifestId = manifestId;
        call.paths = paths;
        call.changeTypes = changeTypes;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Stage;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void commitPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Commit;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Commit;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void cancelPlan(const QString &manifestId,
                    std::function<void(bool ok, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Cancel;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Cancel;
        pending.invoke = [done](bool ok, const QString &, const QString &error) {
            done(ok, error);
        };
        m_pending.append(pending);
    }

    void requestStatus(const QString &manifestId,
                       std::function<void(bool ok, const QString &statusCsv, const QString &error)> done) override
    {
        Call call;
        call.kind = Call::Status;
        call.manifestId = manifestId;
        calls.append(call);

        Pending pending;
        pending.kind = Call::Status;
        pending.invoke = [done](bool ok, const QString &value, const QString &error) {
            done(ok, value, error);
        };
        m_pending.append(pending);
    }

    /**
     * @brief 計画のシグナル購読要求を記録し、常に成功を返す
     * @param signalReceiver シグナルの受け取り先として登録するオブジェクト
     * @return 常にtrue
     */
    bool subscribePlanSignals(QObject *signalReceiver) override
    {
        receiver = signalReceiver;
        return true;
    }

    /**
     * @brief 計画のシグナル購読の解除要求を記録する
     * @param signalReceiver 解除対象のオブジェクト、登録済みの受け取り先と一致する場合だけ登録を外す
     */
    void unsubscribePlanSignals(QObject *signalReceiver) override
    {
        if (receiver == signalReceiver) {
            receiver = nullptr;
        }
        ++unsubscribeCount;
    }

private:
    struct Pending
    {
        Call::Kind kind = Call::Begin;
        std::function<void(bool ok, const QString &value, const QString &error)> invoke;
    };

    QList<Pending> m_pending;
};

/**
 * @class TestFileChangeModel
 * @brief FileChangeModelの単体テスト本体
 *
 * スロット定義はtst_filechangemodel系列の各cppへ分散し
 * 宣言だけをこのヘッダーに集約する
 */
class TestFileChangeModel : public QObject
{
    Q_OBJECT

private slots:
    /**
     * @brief テスト用のQSettings退避など共通準備を行う
     */
    void initTestCase();
    /**
     * @brief 空白付きパスの保持と重複排除と親合成を検証する
     */
    void treeBuildRetainsSpacesDeduplicatesAndMarksParents();
    /**
     * @brief フラット構築の入力順保持と明示ディレクトリを検証する
     */
    void flatBuildPreservesInputOrderAndExplicitDirectories();
    /**
     * @brief 接頭辞の重いパスのツリー構築を検証する
     */
    void treeBuildHandlesPrefixHeavyPaths();
    /**
     * @brief hasCheckedItems()が、チェックの追加・解除・一覧の読み直しに追従することを検証する
     * @note RestorePreviewDialogは、hasCheckedItems()の結果で復元ボタンの有効状態を決める
     *       1つもチェックしていない状態で復元ボタンを押せないことを保証するために検証する
     */
    void hasCheckedItemsTracksCheckState();
    /**
     * @brief チャンク分割ステージングと確定の1回性を検証する
     */
    void stagedRestoreStagesBoundedChunksAndCommitsOnce();
    /**
     * @brief 確定後に可変値を送らないことを検証する
     */
    void stagedRestoreSendsNoMutableValuesAfterCommit();
    /**
     * @brief 進捗の単調性と総件数固定を検証する
     */
    void stagedRestoreProgressIsMonotonicOverOriginalTotal();
    /**
     * @brief 完了端末状態での完了通知を検証する
     */
    void stagedRestoreCompletedTerminalEmitsCompletionOnce();
    /**
     * @brief 失敗端末状態での所有者確認を検証する
     */
    void stagedRestoreFailedTerminalRequestsOwnerStatus();
    /**
     * @brief 確定後のサービス消失を失敗で確定させることを検証する
     */
    void stagedRestoreServiceVanishAfterCommitFinishesWithFailure();
    /**
     * @brief ステージング中のサービス消失を失敗で確定させることを検証する
     */
    void stagedRestoreServiceVanishDuringStagingFinishesWithFailure();
    /**
     * @brief ステージング失敗時の異常終了を検証する
     */
    void stagedRestoreStageFailureAbortsFlowWithError();
    /**
     * @brief 確定失敗時の異常終了を検証する
     */
    void stagedRestoreCommitFailureAbortsFlowWithError();
    /**
     * @brief キャンセル時に端末状態の到着を待つことを検証する
     */
    void stagedRestoreCancelWaitsForTerminalCancelled();
    /**
     * @brief 別計画の通知を無視することを検証する
     */
    void stagedRestoreIgnoresForeignManifestId();
    /**
     * @brief 末尾スラッシュ付きディレクトリの正規化を検証する
     */
    void stagedRestoreNormalizesTrailingSlashDirectory();
    /**
     * @brief 無効エントリの事前除外を検証する
     */
    void stagedRestoreFiltersInvalidEntriesBeforeStaging();
    /**
     * @brief ステージング中のキャンセルを局所完了させることを検証する
     */
    void stagedRestoreCancelDuringStagingFinishesLocally();
    /**
     * @brief 比較元を使った復元とモード保持を検証する
     */
    void prePostRestoreUsesComparisonSourceAndPreservesBetweenMode();
    /**
     * @brief 比較先からの復元を拒否することを検証する
     */
    void prePostRestoreFromComparisonTargetIsRejected();
    /**
     * @brief 両方向での対向番号引き継ぎを検証する
     */
    void beginPlanCarriesCounterpartFromLoadedComparisonInBothDirections();
    /**
     * @brief 対カレント比較での対向番号ゼロを検証する
     */
    void beginPlanCarriesZeroCounterpartInVsCurrentMode();
    /**
     * @brief 空フラグの合成親が計画に届かないことを検証する
     */
    void syntheticParentWithEmptyStatusFlagsNeverReachesThePlan();
    /**
     * @brief 対カレント比較での不一致復元拒否を検証する
     */
    void restoreFromMismatchedSnapshotIsRejectedInVsCurrentMode();
    /**
     * @brief 単一ファイルの不一致復元拒否を検証する
     */
    void singleFileRestoreFromMismatchedSnapshotIsRejected();
    /**
     * @brief 単一ファイル復元が計画を使うことを検証する
     */
    void singleFileRestoreUsesStagedPlan();
    /**
     * @brief 失敗置換で復元可能エントリを消去することを検証する
     */
    void failedComparisonReplacementClearsRestorableEntries();
    /**
     * @brief 古い読み込み応答を無視することを検証する
     */
    void staleComparisonLoadResponseIsIgnored();
    /**
     * @brief 古い差分応答を無視することを検証する
     */
    void staleFileDiffResponseIsIgnored();
    /**
     * @brief 差異なしエントリの非表示を検証する
     */
    void filteredComparisonHidesEntriesUnchangedAgainstCurrentSystem();
    /**
     * @brief 末尾スラッシュに依らない照合を検証する
     */
    void filteredComparisonMatchesDirectoriesRegardlessOfTrailingSlash();
    /**
     * @brief 残なし時の変更なし報告を検証する
     */
    void filteredComparisonWithoutRemainingEntriesReportsNoChangesWithoutError();
    /**
     * @brief 対カレント失敗時の後続要求省略を検証する
     */
    void filteredComparisonCurrentComparisonFailureSkipsSecondRequest();
    /**
     * @brief 古い絞り込み応答を無視することを検証する
     */
    void staleFilteredComparisonResponseIsIgnored();
    /**
     * @brief 絞り込み後の復元元と変更タイプ保持を検証する
     */
    void filteredComparisonPreservesRestoreSourceAndChangeTypes();

    // --- 行指向出力への改行注入に対する多層防御 ---
    /**
     * @brief 混在時に親だけを抑制することを検証する
     */
    void createdParentWithNonCreatedDescendantSuppressesOnlyParent();
    /**
     * @brief 正当なCreatedツリー到達を検証する
     */
    void legitimateCreatedDirectoryTreeStillReachesThePlan();
    /**
     * @brief 全Created子孫時の区別不可を検証する
     */
    void createdParentWithOnlyCreatedDescendantsIsIndistinguishable();

private:
    /**
     * @brief 変更出力と選択状態からstaged restoreの前提を整える
     * @param model 対象モデル
     * @param fake 記録用transport
     * @param changeOutput 変更出力
     * @param pathsToCheck 選択するパス
     * @param batchSize チャンク最大件数
     * @param flatMode フラット構築する場合はtrue
     */
    void prepareStagedRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                              const QString &changeOutput, const QStringList &pathsToCheck, int batchSize,
                              bool flatMode = true);
    /**
     * @brief 記録されたステージング呼び出しからパスだけを取り出す
     * @param fake 記録用transport
     * @return ステージング済みパス
     */
    static QStringList stagedPathsOf(const FakeRestorePlanTransport &fake);
    /**
     * @brief 記録されたステージング呼び出しから変更タイプだけを取り出す
     * @param fake 記録用transport
     * @return ステージング済み変更タイプ
     */
    static QStringList stagedChangeTypesOf(const FakeRestorePlanTransport &fake);
    /**
     * @brief 2スナップショット比較の復元前提を整える
     * @param model 対象モデル
     * @param fake 記録用transport
     * @param number1 比較元番号
     * @param number2 比較先番号
     * @param changeOutput 変更出力
     * @param pathsToCheck 選択するパス
     */
    void prepareComparisonRestore(TestableFileChangeModel *model, FakeRestorePlanTransport *fake,
                                  int number1, int number2,
                                  const QString &changeOutput, const QStringList &pathsToCheck);
};

#endif // TST_FILECHANGEMODEL_H
