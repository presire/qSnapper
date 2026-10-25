#ifndef SNAPPERDOPERATION_H
#define SNAPPERDOPERATION_H

/**
 * @file snapperdoperation.h
 * @brief config作成・config削除・スナップショットの一括削除を、snapperdへ非同期に転送する
 *
 * これらの操作は、libsnapperがプラグインやsystemctlなどの外部コマンドを起動し得る
 * 本サービスから外部コマンドを起動しない方針のため、変更そのものは常駐するsnapperd (org.opensuse.Snapper) に任せる
 * snapperdはconfigごとのロックと使用中の判定を持ち、snapperdが保持するconfigの一覧も変更に合わせて更新するため、
 * 本サービスがlibsnapperで直接変更するよりも、snapperdや他のクライアントとの不整合が起きにくい
 *
 * 本サービスは単一スレッドで動作し、snapperdの応答を同期で待つと他の全ての要求が止まるため、全ての呼び出しを非同期に行う
 */

#include <QDBusConnection>
#include <QDBusMessage>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

class QDBusServiceWatcher;

namespace qsnapper::dbusservice {

    /**
     * @class SnapperdOperation
     * @brief snapperdへの1回分の変更 (config作成、config削除、一括削除) を非同期に実行する
     *
     * 操作は次の順に進める
     *   - 呼び出し元がバスに接続していることの確認 (切断していれば変更しない)
     *   - snapperdの起動と、snapperdの接続のユニーク名の取得
     *     以降の照会・変更・完了の確認は全てこのユニーク名へ送り、その接続の切断だけを監視する
     *     バス名へ送ると、途中でsnapperdが再起動した場合に、一覧を確かめた相手と変更を受け取る相手が異なり得るため
     *   - (指定した場合) snapperdのスナップショット一覧に、指定した番号が全てあることの確認
     *     snapperdは読み込んだconfigの一覧をしばらく保持するため、本サービスが作成した直後の番号を知らないことがある
     *     その状態で削除を転送すると、意図と異なる結果になり得るため、一致しない場合は変更しない
     *   - 変更の直前のmutationGuardの呼び出し (falseを返した場合は変更しない)
     *   - 変更の転送
     *
     * 変更の成否は、snapperdの応答だけでは判断しない
     * 呼び出し元 (SnapshotOperations) が、完了の通知を受けた後に実際の状態を読み直して判断する
     *
     * snapperdの応答が期限内に届かなかった場合は、変更がまだ進んでいる可能性がある
     * snapperdは呼び出し元の接続ごとに要求を到着順に1件ずつ処理するため、同じ接続から同じsnapperdへ軽い照会 (ListConfigs) を送り、
     * その応答が届いた時点で変更の処理も終わったと判断する (Settled)
     * 照会の応答も期限内に届かなければ、照会を送り直して待ち続ける
     * snapperdの接続が切断された場合は、変更が行われたかどうかが分からないまま完了する (Unknown)
     * 時間切れだけでは完了を通知しないため、呼び出し元は処理中の変更が残っている間に次の変更を受け付けることはない
     *
     * m_bus : snapperdと通信する接続 (本サービスのシステムバス)
     * m_replyDeadline : 呼び出し元へ応答する期限 (期限を過ぎても操作そのものは続ける)
     */
    class SnapperdOperation : public QObject
    {
        Q_OBJECT

    public:
        /** @class Outcome
         *  @brief snapperdとの通信の結果
         */
        enum class Outcome {
            Replied,        // 変更を転送し、snapperdが正常に応答した
            Rejected,       // 変更を転送し、snapperdがエラーで応答した (一部だけ変更された可能性がある)
            NotForwarded,   // snapperdに接続できない、または事前の確認に失敗したため、変更を転送していない
            Outdated,       // snapperdのスナップショット一覧に、指定した番号が無かったため、変更を転送していない
            Interrupted,    // 呼び出し元の切断、またはmutationGuardにより、変更を転送していない
            Settled,        // 変更を転送して応答は届かなかったが、snapperdが処理を終えたことを後続の照会で確認した
            Unknown         // 変更を転送したが応答が届かず、snapperdの接続が切断された
        };

        using MutationGuard = std::function<bool()>;    // 変更の直前に呼ぶ判定 (falseで変更を止める)
        using Completion = std::function<void(Outcome)>; // 完了の通知 (1回だけ呼ぶ)
        using DeadlineHandler = std::function<void()>;  // 応答の期限切れの通知 (完了より先に期限が来た場合だけ1回呼ぶ)

        static constexpr int QueryTimeoutMs = 30000;                // 接続の確認とスナップショット一覧の取得の期限
        static constexpr int MutationTimeoutMs = 2 * 60 * 60 * 1000; // 変更の転送の期限 (大量の削除でも途中で打ち切らないよう長くする)
        static constexpr int BarrierTimeoutMs = 10 * 60 * 1000;     // 完了を確かめる照会1回の期限 (期限を過ぎたら送り直す)
        static constexpr int BarrierRetryMs = 5000;                 // バスが照会を拒否した場合に送り直すまでの間隔
        static constexpr int MaxListedSnapshots = 100000;           // スナップショット一覧として受け付ける件数の上限

        static const QString Service;   // snapperdのバス名
        static const QString Path;      // snapperdのオブジェクトパス
        static const QString Interface; // snapperdのインターフェース名

        /**
         * @brief 操作を準備する
         * @param bus snapperdと通信する接続
         * @param caller 呼び出し元のユニーク名 (切断を監視する)
         * @param mutation snapperdへ転送する変更のメッセージ
         * @param listConfig 事前にスナップショット一覧を確かめるconfig名 (空の場合は確かめない)
         * @param requiredNumbers listConfigのスナップショット一覧に存在を求める番号
         * @param mutationGuard 変更の直前に呼ぶ判定
         * @param parent 親オブジェクト
         */
        SnapperdOperation(const QDBusConnection &bus, const QString &caller, const QDBusMessage &mutation,
                          const QString &listConfig, const QList<uint> &requiredNumbers,
                          MutationGuard mutationGuard, QObject *parent = nullptr);

        /**
         * @brief 操作を開始する
         * @param replyDeadlineMs 呼び出し元へ応答する期限
         * @param onDeadline 期限が完了より先に来た場合に呼ぶ処理
         * @param completion 完了の通知先 (1回だけ呼ぶ)
         */
        void start(int replyDeadlineMs, DeadlineHandler onDeadline, Completion completion);

        /**
         * @brief 変更の転送の期限を変える
         *
         * 応答が届かない場合の扱いを、テストで短い時間に再現するためだけに使う
         * @param timeoutMs 変更の転送の期限
         * @note startより前に呼ぶ
         */
        void setMutationTimeoutMs(int timeoutMs) { m_mutationTimeoutMs = timeoutMs; }

        /**
         * @brief 完了を確かめる照会のメソッド名を変える
         *
         * バスが照会を拒否した場合の扱いを、隔離したバスのポリシーで再現するためだけに使う
         * @param member 照会に使うメソッド名
         * @note startより前に呼ぶ
         */
        void setBarrierMemberForTesting(const QString &member) { m_barrierMember = member; }

        /**
         * @brief snapperdのD-Busエラーを、変更を転送していない失敗として扱えるかを判定する
         *
         * バス自身が返すエラー (宛先が無い、権限が無い、メソッドが無い) の場合、snapperdは変更を受け取っていない
         * @param errorName D-Busエラー名
         * @return 変更を転送していないと判断できる場合: true
         */
        static bool isNotForwardedError(const QString &errorName);

    private:
        using ReplyHandler = std::function<void(const QDBusMessage &)>; // 1回の呼び出しの応答を受け取る処理

        /**
         * @brief 応答を非同期に待つ呼び出しを送る
         * @param message 送るメッセージ
         * @param timeoutMs 呼び出しの期限
         * @param handler 応答 (エラーを含む) を受け取る処理 (完了の後に届いた応答では呼ばない)
         */
        void call(const QDBusMessage &message, int timeoutMs, ReplyHandler handler);

        /**
         * @brief snapperdの接続のユニーク名を取得して監視を始める
         * @param activated 起動の要求を送った後の再取得であればtrue
         *
         * 所有者がいない場合だけ起動を要求して1度だけ取得し直す
         * 起動の要求を先に送らないのは、D-Busデーモンが起動用の定義のない名前への要求を、所有者がいても拒否するため
         * 取得できない場合は、変更を転送せずに完了する (NotForwarded)
         */
        void resolveSnapperdOwner(bool activated);

        /**
         * @brief メッセージの宛先を、取得したsnapperdのユニーク名に置き換える
         * @param message 置き換える元のメッセージ (パス・インターフェース・メソッド・引数を引き継ぐ)
         * @return 宛先を置き換えたメッセージ
         */
        QDBusMessage addressedToSnapperd(const QDBusMessage &message) const;

        /**
         * @brief snapperdのスナップショット一覧を確かめてから変更を転送する
         */
        void checkSnapshotList();

        /**
         * @brief ListSnapshotsの応答から番号を取り出す
         * @param reply snapperdの応答
         * @param numbers 取り出した番号を受け取る
         * @return 形式と件数が正しい場合: true
         */
        static bool parseSnapshotNumbers(const QDBusMessage &reply, QList<uint> *numbers);

        /**
         * @brief 変更の直前に状態を確かめ、変更を転送する
         */
        void forwardMutation();

        /**
         * @brief 応答が届かなかった変更について、snapperdが処理を終えたかを照会で確かめる
         *
         * 照会は変更と同じ接続から同じsnapperdへ送るため、変更の処理が終わるまで応答が返らない
         * snapperdの接続が切断されていれば、結果が分からないまま完了する
         */
        void sendBarrier();

        /**
         * @brief 完了を1回だけ通知する
         * @param outcome 通知する結果
         */
        void finish(Outcome outcome);

        QDBusConnection m_bus;                      // snapperdと通信する接続
        QString m_caller;                           // 呼び出し元のユニーク名
        QDBusMessage m_mutation;                    // 転送する変更のメッセージ
        QString m_listConfig;                       // 事前にスナップショット一覧を確かめるconfig名
        QList<uint> m_requiredNumbers;              // スナップショット一覧に存在を求める番号
        MutationGuard m_mutationGuard;              // 変更の直前に呼ぶ判定
        DeadlineHandler m_onDeadline;               // 応答の期限切れの通知先
        Completion m_completion;                    // 完了の通知先
        QTimer m_replyDeadline;                     // 呼び出し元へ応答する期限
        QDBusServiceWatcher *m_callerWatcher = nullptr;   // 呼び出し元の切断の監視
        QDBusServiceWatcher *m_snapperdWatcher = nullptr; // 取得したsnapperdの接続の切断の監視
        QString m_snapperdOwner;                    // 照会と変更を送るsnapperdの接続のユニーク名
        int m_mutationTimeoutMs = MutationTimeoutMs; // 変更の転送の期限
        QString m_barrierMember = QStringLiteral("ListConfigs"); // 完了を確かめる照会のメソッド名
        bool m_callerGone = false;                  // 呼び出し元が切断したか
        bool m_snapperdLost = false;                // 取得したsnapperdの接続が切断されたか
        bool m_awaitingSettlement = false;          // 変更の応答が届かず、snapperdの処理の終わりを待っているか
        bool m_finished = false;                    // 完了を通知したか (以降に届いた応答は無視する)
    };

} // namespace qsnapper::dbusservice

#endif // SNAPPERDOPERATION_H
