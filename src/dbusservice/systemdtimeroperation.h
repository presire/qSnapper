#ifndef SYSTEMDTIMEROPERATION_H
#define SYSTEMDTIMEROPERATION_H

/**
 * @file systemdtimeroperation.h
 * @brief snapperのsystemd timerの状態の取得と有効化・無効化を、systemdのD-Bus APIで非同期に行う
 *
 * 外部コマンド (systemctl) は起動せず、org.freedesktop.systemd1.Managerのメソッドを直接呼ぶ
 * 本サービスは単一スレッドで動作するため、同期呼び出しでsystemdの応答を待つと、待つ間は他の全ての要求が止まる
 * そのため、全ての呼び出しを非同期に行い、呼び出しごとの期限と操作全体の期限を設ける
 */

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include "snappertimerstate.h"

namespace qsnapper::dbusservice {

    /**
     * @class SystemdTimerOperation
     * @brief snapperのtimerに対する1回分の操作 (状態の取得、有効化、無効化) を非同期に実行する
     *
     * 操作は次の順に進める
     *   - 取得 : 各unitのUnitFileStateとActiveStateを読む
     *   - 有効化 : EnableUnitFiles、Reload、StartUnit (jobの完了を待つ)、状態の読み直し
     *   - 無効化 : StopUnit (jobの完了を待つ)、DisableUnitFiles、Reload、状態の読み直し
     * 有効化と無効化の成否は、systemdの応答ではなく、最後に読み直した状態で判定する
     * 途中で失敗・中断・期限切れになった場合も、変更を巻き戻さずに失敗として終える (呼び出し元は状態を読み直して表示する)
     *
     * 変更を伴う呼び出しの直前には、毎回mutationGuardを呼ぶ
     * 認可やsystemdの応答を待つ間に、復元が始まったり呼び出し元が切断したりし得るため、変更の直前に状態を判定し直す
     * mutationGuardがfalseを返した後は、新たな変更を一切行わない
     *
     * 本クラスは、unit名に呼び出し元の文字列を使わない
     * コンストラクタへ渡すunit名は、qsnapper::security::canonicalSnapperTimerUnitが返した固定名に限る
     *
     * m_bus : systemdへ問い合わせる接続 (本サービスのシステムバス)
     * m_completion : 完了の通知先 (1回だけ呼ぶ)
     * m_deadline : 操作全体の期限
     */
    class SystemdTimerOperation : public QObject
    {
        Q_OBJECT

    public:
        /** @class Kind
         *  @brief 操作の種類
         */
        enum class Kind {
            Query,      // 状態の取得だけを行う
            Enable,     // 起動時の有効化と、現在の起動
            Disable     // 現在の停止と、起動時の無効化
        };

        /** @class Outcome
         *  @brief 操作の結果
         */
        enum class Outcome {
            Succeeded,  // 取得できた、または読み直した状態が要求どおりになった
            Failed,     // systemdの呼び出しの失敗、期限切れ、または読み直した状態が要求と異なる
            Interrupted // mutationGuardが変更を止めた
        };

        using MutationGuard = std::function<bool()>;                                            // 変更の直前に呼ぶ判定 (falseで変更を止める)
        using Completion = std::function<void(Outcome, const SnapperTimerStateMap &)>;          // 完了の通知 (結果と、読み取れたunitの状態)

        static constexpr int CallTimeoutMs = 3000;          // systemdの1回の呼び出しの期限
        static constexpr int ReloadTimeoutMs = 10000;       // Reloadの期限 (unitの数に応じて時間がかかるため長めにする)
        static constexpr int JobTimeoutMs = 10000;          // StartUnitとStopUnitのjobの完了を待つ期限
        static constexpr int QueryDeadlineMs = 8000;        // 取得の操作全体の期限
        static constexpr int ChangeDeadlineMs = 30000;      // 有効化と無効化の操作全体の期限
        static constexpr int MaxBufferedJobResults = 32;    // StartUnitの応答より先に届いたjobの完了を保持する上限

        /**
         * @brief 操作を構築する (開始はstart()で行う)
         * @param bus systemdへ問い合わせる接続
         * @param kind 操作の種類
         * @param units 対象のunit名 (固定名だけを渡す、有効化と無効化では1件)
         * @param mutationGuard 変更の直前に呼ぶ判定 (取得では呼ばない)
         * @param parent Qt親オブジェクト
         */
        SystemdTimerOperation(const QDBusConnection &bus, Kind kind, const QStringList &units,
                              MutationGuard mutationGuard, QObject *parent = nullptr);

        /**
         * @brief 操作を開始する
         * @param completion 完了の通知先 (成功・失敗・中断のいずれでも1回だけ呼ぶ)
         * @note completionはイベントループから呼ぶため、start()の中では呼ばない
         */
        void start(Completion completion);

        /**
         * @brief 操作がunitの構成や稼働状態を変えるかを判定する
         * @return 有効化または無効化の場合はtrue、状態の取得だけの場合はfalse
         * @note 復元の開始を拒否するかの判定に使う (状態の取得は復元と重なっても害がないため)
         */
        bool mutatesUnits() const { return m_kind != Kind::Query; }

        /**
         * @brief UnitFileStateとActiveStateの値が、systemdの状態名として妥当かを判定する
         * @param value systemdから受け取った値
         * @return 小文字の英字とハイフンだけからなる64文字以内の値の場合はtrue
         * @note 応答はそのまま利用者へ返すため、想定外の値は受け取らずに失敗とする
         */
        static bool isStateToken(const QString &value);

    private slots:
        /**
         * @brief systemdのJobRemovedシグナルを受け取る
         * @param id jobの番号
         * @param job jobのオブジェクトパス
         * @param unit unit名
         * @param result jobの結果 (done / canceled / timeout / failed / dependency / skipped)
         */
        void handleJobRemoved(uint id, const QDBusObjectPath &job, const QString &unit, const QString &result);

    private:
        using ReplyHandler = std::function<void(const QDBusMessage &)>;   // systemdの応答 (エラー応答を含む) を受け取る処理

        /**
         * @brief systemd1のManagerのメソッドを非同期に呼ぶ
         * @param method メソッド名
         * @param arguments 引数
         * @param timeoutMs 呼び出しの期限
         * @param handler 応答を受け取る処理 (操作が終わった後に届いた応答では呼ばない)
         */
        void callManager(const QString &method, const QVariantList &arguments, int timeoutMs, ReplyHandler handler);

        /**
         * @brief 任意のオブジェクトとインタフェースのメソッドを、systemd1へ非同期に呼ぶ
         * @param message 送信するメソッド呼び出し
         * @param timeoutMs 呼び出しの期限
         * @param handler 応答を受け取る処理
         */
        void callSystemd(const QDBusMessage &message, int timeoutMs, ReplyHandler handler);

        /**
         * @brief index番目のunitから順に状態を読み、全て読み終えたらnextを呼ぶ
         * @param index 読み始めるunitの位置
         * @param next 全てのunitを読み終えた後の処理
         */
        void queryUnit(int index, std::function<void()> next);

        /**
         * @brief 変更の直前に、変更を続けてよいかを判定する
         * @return 続けてよい場合はtrue、止めた場合は状態を読み直してInterruptedで終える処理を始めてfalse
         */
        bool mayMutate();

        /**
         * @brief StartUnitまたはStopUnitを呼び、jobの完了を待ってからnextを呼ぶ
         * @param method StartUnitまたはStopUnit
         * @param next jobの結果 (doneの場合はtrue) を受け取る処理
         */
        void runUnitJob(const QString &method, std::function<void(bool)> next);

        /**
         * @brief 対象のunitの状態を読み直し、要求どおりかどうかで操作を終える
         * @param outcomeIfMatched 要求どおりだった場合に返す結果
         */
        void verifyAndFinish(Outcome outcomeIfMatched);

        /**
         * @brief 読み取った状態が、有効化または無効化の要求どおりかを判定する
         * @return 要求どおりの場合はtrue
         */
        bool stateMatchesRequest() const;

        /**
         * @brief 操作を終え、完了の通知先を1回だけ呼ぶ
         * @param outcome 結果
         */
        void finish(Outcome outcome);

        QDBusConnection m_bus;                      // systemdへ問い合わせる接続
        Kind m_kind;                                // 操作の種類
        QStringList m_units;                        // 対象のunit名 (固定名)
        MutationGuard m_mutationGuard;              // 変更の直前に呼ぶ判定
        Completion m_completion;                    // 完了の通知先
        SnapperTimerStateMap m_states;              // 読み取ったunitの状態
        QTimer m_deadline;                          // 操作全体の期限
        bool m_finished = false;                    // 完了を通知したか (以降に届いた応答とシグナルは無視する)
        bool m_interrupted = false;                 // 変更を止めたか (止めた後の読み直しが失敗しても中断として通知する)
        bool m_jobSignalConnected = false;          // JobRemovedシグナルを受け取る設定をしたか
        QString m_waitingJob;                       // 完了を待っているjobのオブジェクトパス (待っていなければ空)
        std::function<void(bool)> m_jobFinished;    // 待っているjobの完了時の処理
        QMap<QString, QString> m_earlyJobResults;   // StartUnitの応答より先に届いたjobの結果 (jobのパスから結果)
        QTimer m_jobTimeout;                        // jobの完了を待つ期限
    };

} // namespace qsnapper::dbusservice

#endif // SYSTEMDTIMEROPERATION_H
