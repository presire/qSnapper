/**
 * @file snapshotoperations_types.h
 * @brief SnapshotOperationsが使う非QObject補助型の宣言
 *
 * unified diffの上限と結果、pin済み復元元の読み取り専用判定
 * polkitプロンプト待ち予算、復元実行コンテキストと認可結果を置く
 * SnapshotOperationsクラス本体はsnapshotoperations.hに残す
 */
#ifndef SNAPSHOTOPERATIONS_TYPES_H
#define SNAPSHOTOPERATIONS_TYPES_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QVariant>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QtGlobal>
#include <string>
#include "restorevalidation.h"

namespace qsnapper::diff {

    /** @class UnifiedDiffLimits
     * @brief unified diff生成の資源上限
     *
     * rootで動作する単一スレッドのサービスが、巨大なファイルや差分過多の入力でメモリやCPUを使い果たさないよう制限する
     */
    struct UnifiedDiffLimits {
        qint64 maxFileBytes = 4LL * 1024 * 1024;        // 1ファイルあたりの最大読み込みサイズ
        qint64 binaryProbeBytes = 8LL * 1024;           // バイナリ判定 (NULの有無) を行う先頭のバイト数
        qint64 maxLinesPerFile = 262144;                // 1ファイルあたりの最大行数
        qint64 maxTraceBytes = 64LL * 1024 * 1024;      // Myers diffの作業領域 (V配列とtrace) の最大サイズ
        qint64 maxSteps = 64LL * 1024 * 1024;           // Myers diffの探索ステップ数の上限 (CPU時間の上限)
    };

    /** @class UnifiedDiffResult
     * @brief unified diffの生成結果
     */
    struct UnifiedDiffResult {
        QString text;           // unified diff (差分なし・省略時は空)
        QString omittedReason;  // 省略理由 (binary / too_large / too_many_changes / special_file)、省略していなければ空
    };

    /**
     * @brief 2つのファイルを読み込み、unified diff形式の文字列を生成する
     *
     * "diff -u"コマンドと互換性のあるフォーマットで、QMLのformatDiffHtml()でパース可能
     * FIFOやデバイスは開かず、上限を超える入力やバイナリは差分を生成せずに省略理由を返す
     *
     * @param oldPath 旧ファイルパス (--- ヘッダに使用)
     * @param newPath 新ファイルパス (+++ ヘッダに使用)
     * @param limits 資源上限
     * @return 生成結果 (読み込めないファイルは従来どおり差分なし・省略理由なし)
     */
    UnifiedDiffResult generateUnifiedDiff(const QString &oldPath,
                                          const QString &newPath,
                                          const UnifiedDiffLimits &limits = UnifiedDiffLimits());

}

namespace qsnapper::restore {

    /**
     * @brief pin済みの復元元ディレクトリが、ファイルシステムの側で読み取り専用であるかを判定する
     *
     * btrfsでは、pin済みfdが属するsubvolumeのread-onlyフラグを見る
     * それ以外 (LVM thinのxfs / ext4など) では、snapperがsnapshotをMS_RDONLYでmountするため、mountが読み取り専用であることを見る
     * 判定できない場合は読み取り専用とみなさない
     *
     * @param snapshotDirFd pin済みの復元元ディレクトリのfd
     * @param fstype configのFSTYPE
     * @return 読み取り専用であることを確認できた場合はtrue
     */
    bool isPinnedRestoreSourceReadOnly(int snapshotDirFd, const std::string &fstype);

}

namespace qsnapper::security {

    /** @class PendingAuthorizationBudget
     * @brief polkitプロンプト待ちの件数を、呼び出し元UID単位と全体の両方で数える
     *
     * プロンプトはタイムアウトを持たないため、未応答のまま滞留し得る
     * 全体の上限だけでは、1人のローカルユーザが上限まで積むことで他のユーザの操作をすべて拒否させられる
     * UID単位の上限で1ユーザが占有できる量を抑え、全体の上限で総量を抑える
     */
    class PendingAuthorizationBudget
    {
    public:
        /**
         * @brief 上限を指定して構築する
         * @param perUidLimit 1つのUIDが同時に保持できる件数の上限
         * @param globalLimit 全体で同時に保持できる件数の上限
         */
        PendingAuthorizationBudget(int perUidLimit, int globalLimit);

        /**
         * @brief 1件分の枠を確保する
         * @param uid 呼び出し元のUID
         * @return UID単位と全体の両方の上限内であれば確保してtrue、それ以外は何もせずfalse
         */
        bool tryAcquire(uint uid);

        /**
         * @brief tryAcquire()で確保した1件分の枠を返す
         * @param uid tryAcquire()に渡したUID
         */
        void release(uint uid);

        /**
         * @brief 全体の保持件数を返す
         * @return 保持件数
         */
        int total() const;

        /**
         * @brief 指定したUIDの保持件数を返す
         * @param uid 呼び出し元のUID
         * @return 保持件数
         */
        int countFor(uint uid) const;

    private:
        int m_perUidLimit;          // UID単位の上限
        int m_globalLimit;          // 全体の上限
        int m_total = 0;            // 全体の保持件数
        QMap<uint, int> m_perUid;   // UIDごとの保持件数 (0件になったUIDは削除する)
    };

}

namespace qsnapper::dbusservice {

    // 復元の実行中に非復元操作を拒否するときのD-Busエラー
    // クライアントはエラー名で判別するため、名前と本文は固定とし、config名やpathを含めない
    // 隔離環境のテスト用サービスも同じ定数を使い、本番と同じ応答を返す
    inline constexpr char RESTORE_IN_PROGRESS_ERROR_NAME[] = "com.presire.qsnapper.Error.RestoreInProgress";    // エラー名
    inline constexpr char RESTORE_IN_PROGRESS_ERROR_TEXT[] = "A restore is in progress";                        // エラー本文

    // snapperdへ転送したconfigやスナップショットの変更の実行中に、他の操作を拒否するときのD-Busエラー
    // 変更の途中で共有のSnapperインスタンスを作り直したり復元を始めたりすると、変更前の状態を前提に動いてしまうため、完了まで拒否する
    inline constexpr char OPERATION_IN_PROGRESS_ERROR_NAME[] = "com.presire.qsnapper.Error.OperationInProgress";  // エラー名
    inline constexpr char OPERATION_IN_PROGRESS_ERROR_TEXT[] = "Another configuration change is in progress";    // エラー本文

    // snapperdへ転送した変更の結果が分からないときのD-Busエラー
    // 応答の期限切れやsnapperdの終了の後は、変更が行われたかどうかを判断できないため、成功とも失敗とも扱わず読み直しを促す
    inline constexpr char OUTCOME_UNKNOWN_ERROR_NAME[] = "com.presire.qsnapper.Error.OutcomeUnknown";             // エラー名
    inline constexpr char OUTCOME_UNKNOWN_ERROR_TEXT[] = "The result of the operation could not be confirmed";   // エラー本文

    // 終わっていない復元計画があるため、configやスナップショットの削除を拒否するときのD-Busエラー
    inline constexpr char RESTORE_PLAN_PENDING_ERROR_NAME[] = "com.presire.qsnapper.Error.RestorePlanPending";   // エラー名
    inline constexpr char RESTORE_PLAN_PENDING_ERROR_TEXT[] = "A restore plan is pending";                       // エラー本文

    // snapperdが保持するスナップショット一覧が本サービスの一覧と一致しないため、削除を転送しないときのD-Busエラー
    inline constexpr char SNAPSHOT_LIST_OUTDATED_ERROR_NAME[] = "com.presire.qsnapper.Error.SnapshotListOutdated";   // エラー名
    inline constexpr char SNAPSHOT_LIST_OUTDATED_ERROR_TEXT[] = "The snapshot list is out of date";                 // エラー本文

    /** @class PendingDirectoryTimes
     *  @brief 全エントリの適用後にまとめて設定するディレクトリの時刻
     *
     * 子エントリを後から作成・削除すると親ディレクトリのmtimeが更新されるため、
     * ディレクトリを適用した時点では時刻を確定できない
     * 設定時に別のディレクトリへ差し替えられていないか確認できるよう、適用時のst_devとst_inoも保持する
     */
    struct PendingDirectoryTimes {
        QString rootPath;                                           // 名前解決の基準 (configのSUBVOLUME)
        QString relativePath;                                       // rootPathからの相対パス
        quint64 device = 0;                                         // 適用時のst_dev
        quint64 inode = 0;                                          // 適用時のst_ino
        qint64 accessSec = 0;                                       // 復元元のatime (秒)
        qint64 accessNsec = 0;                                      // 復元元のatime (ナノ秒)
        qint64 modifySec = 0;                                       // 復元元のmtime (秒)
        qint64 modifyNsec = 0;                                      // 復元元のmtime (ナノ秒)
    };

    /** @class RestoreExecution
     *  @brief 非同期復元計画がchunk間で必要とする固定実行コンテキスト
     */
    struct RestoreExecution {
        QString owner;                                              // 計画ownerのD-Busユニーク名
        QString configName;                                         // 対象Snapper設定名
        int snapshotNumber = -1;                                    // 復元元スナップショット番号
        QString snapshotDir;                                        // 復元元ディレクトリのパス
        // 認可時に開いて計画寿命の間保持するスナップショット dirfd
        // ソース読み取りを全て本fd相対で行うことで、実行中のスナップショット削除や同番号再作成やmount状態の変化に依存せず、
        // 認可された計画が参照したスナップショットそのものから復元し続ける
        int snapshotDirFd = -1;                                     // pin済みスナップショットdirfd
        // commit時に固定したconfigのSUBVOLUME (正規化前)
        // 復元先はSUBVOLUMEからの相対であり、ルート基準で解決してはならない
        QString subvolume;                                          // 固定SUBVOLUME
        // 復元後にrootサブボリュームをrwへ戻す安全ネットの実行条件 (commit時に記録する)
        qsnapper::restore::RootReadWriteSafetyNetState rootReadWriteState; // 安全ネット条件
        bool useReflink = false;                                    // reflink複写を使うか
        bool mounted = false;                                       // スナップショットをマウント済みか
        QList<PendingDirectoryTimes> pendingDirectoryTimes;         // 終了時に設定するディレクトリの時刻
    };

    /** @class CallReply
     *  @brief 認可待ちを跨いで応答するためにキャプチャしたD-Bus呼び出しコンテキスト
     *
     * QDBusContextのmessageとsendErrorReplyはスロットから戻った後は使用できない
     * polkitプロンプトを待って遅延応答する経路では、スロット冒頭でキャプチャした本構造体の値を用いて返信する
     */
    struct CallReply {
        QDBusMessage message;                                       // 返信先のキャプチャ済みメッセージ
        QDBusConnection connection = QDBusConnection::systemBus();  // 応答送出に用いる接続
        bool fromDBus = false;                                      // D-Bus経由の呼び出しか
        bool replied = false;                                       // errorとvalueを送出済みか
    };

    /** @class AuthorizationOutcome
     *  @brief 認可要求の即時結果
     */
    enum class AuthorizationOutcome {
        Granted,    // 対話なしで許可済みであり、呼び出し元はそのまま同期実行してよい
        Denied,     // 拒否であり、D-Busエラー応答は送出済み
        Deferred    // polkitプロンプト待ちであり、応答は完了継続から送出される
    };

} // namespace qsnapper::dbusservice

#endif // SNAPSHOTOPERATIONS_TYPES_H
