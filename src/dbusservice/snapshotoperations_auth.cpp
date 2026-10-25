/**
 * @file snapshotoperations_auth.cpp
 * @brief polkit認可ゲートとSnapperインスタンス取得
 *
 * polkit非同期認可の完了対応付け、設定名の正規化と検証
 * 遅延応答と通常応答を切り替えるreplyError、一覧取得用のSnapper再利用を扱う
 */

#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDateTime>
#include <QPointer>
#include "snapshotoperations.h"
#include "snapshotoperations_internal.h"
#include "inputvalidator.h"
#include "filesystemhelpers.h"
#include "csvrecord.h"

// glibのGDBusInterfaceInfoとGDBusObjectSkeletonはsignalsというメンバ名を持ち、Qtのsignalsマクロと衝突する
// そのため、polkitヘッダの取り込み中だけマクロを外す
#undef signals
#include <polkit/polkit.h>
#define signals Q_SIGNALS

#include <snapper/Snapper.h>
#include <snapper/Comparison.h>
#include <snapper/Exception.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <btrfsutil.h>
#include <string>

// ============================================================================
// Polkit非同期認可
//
// polkit-qt6-1の非同期API (Authority::checkAuthorization + checkAuthorizationFinished) は使用しない
// Authorityはシングルトンであり、完了シグナルはResult値しか運ばず、完了callbackのuser_dataもAuthority自身であるため、
// 同時に2件の認可が進行すると、どのD-Bus呼び出しに対する結果なのかを判別できない (rootサービスでは「別要求の許可」を流用してしまう認可の取り違えに直結する)
// またcancellableとエラー状態がシングルトンで共有されており、1件の失敗が後続の全呼び出しを黙って落とす
//
// polkitのGObject APIは呼び出しごとにGSimpleAsyncResultとuser_dataを確保するため、完了callbackを発行元の呼び出しへ厳密に対応付けられる
// 完了callbackは呼び出し時点のthread-default GMainContext (=Qtのイベントループが回すdefault context) で発火する
// ============================================================================

namespace {

    /**
     * @brief polkit authorityをプロセス寿命の間だけ取得して再利用する
     * @return 取得済みauthority、取得できなければnullptr
     */
    PolkitAuthority *polkitAuthorityInstance()
    {
        static PolkitAuthority *authority = []() -> PolkitAuthority * {
            GError *error = nullptr;
            PolkitAuthority *result = polkit_authority_get_sync(nullptr, &error);
            if (!result) {
                qCritical() << "Failed to obtain polkit authority:"
                            << (error ? error->message : "unknown error");
            }
            if (error) {
                g_error_free(error);
            }
            return result;
        }();
        return authority;
    }

    /**
     * @brief 非同期認可1件分の状態 (polkitのuser_dataとして渡す)
     */
    struct AsyncAuthorization {
        // サービスが先に破棄された場合に継続を実行しないためのguard
        QPointer<SnapshotOperations> service;
        std::function<void(bool)> continuation;
    };

    /**
     * @brief polkit非同期認可の完了callback
     *
     * user_dataは本呼び出し専用に確保したAsyncAuthorizationであり、他の認可要求の結果と混ざらない
     *
     * @param source 認可を行ったPolkitAuthority
     * @param result 完了結果
     * @param userData AsyncAuthorization* (本callbackが所有権を引き取る)
     */
    void asyncAuthorizationFinished(GObject *source,
                                    GAsyncResult *result,
                                    gpointer userData)
    {
        const std::unique_ptr<AsyncAuthorization> request(
            static_cast<AsyncAuthorization *>(userData));

        GError *error = nullptr;
        PolkitAuthorizationResult *authorization =
            polkit_authority_check_authorization_finish(
                POLKIT_AUTHORITY(source), result, &error);

        bool granted = false;
        if (authorization) {
            granted = polkit_authorization_result_get_is_authorized(authorization);
            g_object_unref(authorization);
        }
        else {
            qWarning() << "Polkit authorization check failed:"
                       << (error ? error->message : "unknown error");
        }
        if (error) {
            g_error_free(error);
        }

        // サービスが既に破棄されている場合は応答先も存在しない
        if (request->service.isNull() || !request->continuation) {
            return;
        }
        request->continuation(granted);
    }

}

/**
 * @brief configNameを正規化 + 検証し、不正ならD-Busエラー応答を送信する
 *
 * 空文字列入力を "root" に正規化した上で qsnapper::security::validateConfigName で検証する
 * 無効な場合はQDBusError::InvalidArgsを送信し、std::nulloptを返す
 * Polkitプロンプトを出す前に呼び出して、攻撃者が任意configNameでpolkitを浪費するのを防ぐ
 *
 * @param configName 検査する設定名 (空文字列は "root" として扱う)
 * @return 正規化後の設定名 (有効時)、無効でエラー送信済み (std::nullopt)
 */
std::optional<QString> SnapshotOperations::resolveConfigOrFail(const QString &configName)
{
    const QString effective = configName.isEmpty() ? QStringLiteral("root") : configName;
    if (!qsnapper::security::validateConfigName(effective)) {
        replyError(QDBusError::InvalidArgs,
                       QStringLiteral("Invalid configName"));
        return std::nullopt;
    }
    // configの作成・削除や一括削除の途中は、configの一覧とスナップショットが変わる途中であるため、
    // configを指定する全ての操作 (復元計画の作成を含む) を、認可の前に拒否する
    if (rejectWhileSnapperdOperationRunning()) {
        return std::nullopt;
    }
    return effective;
}

/**
 * @brief 現在のD-Bus呼び出しの応答contextをcaptureする
 *
 * message()はスロットから戻ると無効になるため、認可待ちを跨ぐ経路では
 * 本関数の戻り値を保持して応答する
 *
 * @return capture済み応答context
 */
SnapshotOperations::CallReply SnapshotOperations::captureCallReply() const
{
    CallReply reply;
    if (!calledFromDBus()) {
        return reply;
    }

    reply.message = message();
    reply.connection = connection();
    reply.fromDBus = true;
    return reply;
}

/**
 * @brief 同期応答と遅延応答のどちらでも正しく名前付きのD-Busエラーを返す
 *
 * エラー種別を受け取る版 (snapshotoperations.h) も、種別をエラー名へ変換して本関数へ委ねる
 * エラー応答を送出する箇所を本関数の1か所に保つため (遅延応答の取りこぼしを静的検査で防ぐ)
 *
 * @param errorName 返すD-Busエラー名
 * @param text エラーメッセージ
 */
void SnapshotOperations::replyError(const QString &errorName, const QString &text)
{
    if (m_deferredReply) {
        // 継続の中ではQDBusContextの呼び出しcontextが失われているため、
        // capture済みmessageから直接エラー応答を組み立てて送出する
        if (!m_deferredReply->replied && m_deferredReply->fromDBus) {
            m_deferredReply->replied = true;
            m_deferredReply->connection.send(
                m_deferredReply->message.createErrorReply(errorName, text));
        }
        return;
    }

    // QDBusContext::sendErrorReplyはD-Busの呼び出しcontextが無いと無効なmessageを参照して異常終了する
    // スロットを直接呼ぶ単体テストでは返す相手がいないため、エラー応答を送らずに終える
    if (!calledFromDBus()) {
        return;
    }
    sendErrorReply(errorName, text);
}

/**
 * @brief 認可済みの本体で想定外の例外が起きたことを記録し、固定の文言でエラー応答する
 *
 * 遅延許可の経路で本体が応答を確保済みの場合、replyErrorは送出済みとして扱い、二重には応答しない
 */
void SnapshotOperations::replyUnexpectedFailure()
{
    qWarning() << "Unexpected exception in an authorized operation";
    replyError(QDBusError::Failed, QStringLiteral("Internal error"));
}

/**
 * @brief 復元の実行中であれば、操作を固定の文言で拒否する
 *
 * m_restoreExecutionsは、復元計画の認可後にmountしてから終端するまでの間だけ要素を持つ
 * この間に共有のSnapperインスタンスを作り直したり、比較キャッシュを破棄したりすると、
 * 実行中の復元と終了時のunmountが別のインスタンスを参照してしまうため、非復元操作を一律に拒否する
 * エラー本文には、config名やpathなど呼び出し元が制御し得る値を載せない
 *
 * @return 拒否してエラー応答を送出した場合: true
 */
bool SnapshotOperations::rejectWhileRestoreRunning()
{
    if (m_restoreExecutions.isEmpty()) {
        return rejectWhileSnapperdOperationRunning();
    }

    replyError(QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_NAME),
               QString::fromLatin1(qsnapper::dbusservice::RESTORE_IN_PROGRESS_ERROR_TEXT));
    return true;
}

/**
 * @brief snapperdへ転送した変更の実行中であれば、操作を固定の文言で拒否する
 *
 * rejectWhileRestoreRunning()から呼び、共通ゲートの3か所 (認可の前、即時認可の直後、遅延認可の後) で同じ判定にする
 * @return 拒否してエラー応答を送出した場合: true
 */
bool SnapshotOperations::rejectWhileSnapperdOperationRunning()
{
    if (!snapperdOperationRunning()) {
        return false;
    }

    replyError(QString::fromLatin1(qsnapper::dbusservice::OPERATION_IN_PROGRESS_ERROR_NAME),
               QString::fromLatin1(qsnapper::dbusservice::OPERATION_IN_PROGRESS_ERROR_TEXT));
    return true;
}

#ifdef QSNAPPER_ENABLE_TEST_HOOKS
/**
 * @brief 認可の結果をテスト用に固定する
 * @param mode 以降の認可要求に返す結果
 */
void SnapshotOperations::setAuthorizationForTesting(TestAuthorization mode)
{
    m_testAuthorization = mode;
}

/**
 * @brief 保留中のテスト用認可の件数を返す
 * @return 保留中の件数
 */
int SnapshotOperations::pendingTestAuthorizationCount() const
{
    return static_cast<int>(m_testPendingAuthorizations.size());
}

/**
 * @brief テスト用の認可に届いた要求の累計件数を返す
 * @return beginAuthorizationが呼ばれた回数
 */
int SnapshotOperations::testAuthorizationRequestCount() const
{
    return m_testAuthorizationRequests;
}

/**
 * @brief 保留中のテスト用認可をすべて確定し、各呼び出しの継続を実行する
 *
 * polkitの非同期応答と同じく、event loopから継続を呼び出す
 * 継続の実行中に新たな保留が積まれても取りこぼさないよう、先に一覧を取り出してから実行する
 *
 * @param granted 許可する場合はtrue
 */
void SnapshotOperations::resolveTestAuthorizations(bool granted)
{
    const auto pending = std::exchange(m_testPendingAuthorizations, {});
    for (const auto &[reply, continuation] : pending) {
        continuation(reply, granted);
    }
}

/**
 * @brief 復元の実行中の状態を、mountせずに作る
 *
 * mountedとsnapshotDirFdを未使用の値にしておくため、
 * 解除やサービス終了時の後片付け (cleanupRestoreExecution) はunmountもcloseも行わない
 *
 * @param configName 復元中として記録するconfig名
 */
void SnapshotOperations::beginRestoreExecutionForTesting(const QString &configName)
{
    RestoreExecution execution;
    execution.configName = configName;
    m_restoreExecutions.insert(QStringLiteral("test-restore-execution"), execution);
}

/**
 * @brief beginRestoreExecutionForTestingで作った状態を解除する
 */
void SnapshotOperations::endRestoreExecutionForTesting()
{
    cleanupRestoreExecution(QStringLiteral("test-restore-execution"));
}

/**
 * @brief snapperdへ転送した変更の実行中の状態を、snapperdへ転送せずに作る
 *
 * 開始しない操作を保持させ、本番と同じ判定 (snapperdOperationRunning) で拒否させる
 */
void SnapshotOperations::beginSnapperdOperationForTesting()
{
    if (m_snapperdOperation) {
        return;
    }
    m_snapperdOperation = new qsnapper::dbusservice::SnapperdOperation(
        QDBusConnection::systemBus(), QString(), QDBusMessage(), QString(), {},
        []() { return false; }, this);
    resetIdleTimer();
}

/**
 * @brief beginSnapperdOperationForTestingで作った状態を解除する
 */
void SnapshotOperations::endSnapperdOperationForTesting()
{
    delete m_snapperdOperation.data();
    m_snapperdOperation.clear();
    resetIdleTimer();
}

/**
 * @brief config操作を止める復元計画があるかの判定を、認可とD-Busを介さずに呼び出す
 * @param configName 判定対象の設定名
 * @return hasUnfinishedRestorePlanの結果
 */
bool SnapshotOperations::hasUnfinishedRestorePlanForTesting(const QString &configName)
{
    return hasUnfinishedRestorePlan(configName);
}

/**
 * @brief discardStagingRestorePlansを、config操作を介さずに呼び出す
 * @param configName 対象の設定名
 * @param snapshotNumber 参照する計画だけに絞るスナップショット番号 (負の値なら絞らない)
 */
void SnapshotOperations::discardStagingRestorePlansForTesting(const QString &configName, int snapshotNumber)
{
    discardStagingRestorePlans(configName, snapshotNumber);
}

/**
 * @brief ownerの接続が切れたときの処理を、D-Busの通知を介さずに呼び出す
 * @param owner 切断されたことにするowner
 */
void SnapshotOperations::handleRestoreOwnerUnregisteredForTesting(const QString &owner)
{
    handleRestoreOwnerUnregistered(owner);
}

/**
 * @brief 復元計画registryの時計をテスト用に差し替える
 * @param clock 現在時刻 (ミリ秒) を返す関数
 */
void SnapshotOperations::setRestoreRegistryClockForTesting(std::function<qint64()> clock)
{
    m_restoreRegistry.setClock(clock);
}

/**
 * @brief 復元計画のowner表にマニフェストIDが残っているかを返す
 * @param manifestId 対象のマニフェストID
 * @return 残っている場合true
 */
bool SnapshotOperations::hasRestorePlanOwnerForTesting(const QString &manifestId) const
{
    return m_restorePlanOwners.contains(manifestId);
}

/**
 * @brief 認可済みのcommitが作る実行状態を、mountせずに作る
 *
 * mountedは未設定のままにするため、終了処理はunmountを行わず、渡したfdだけを閉じる
 *
 * @param manifestId 実行状態に対応付けるマニフェストID
 * @param sourcePath 復元元として扱うディレクトリの絶対パス
 * @param sourceDirFd sourcePathを開いたfd (所有権を移す)
 * @param subvolume 復元先の基準とする絶対パス
 * @param useReflink ダイレクトコピー方式として扱う場合true
 * @param rootReadWriteState 終了時の安全ネットの判定材料
 */
void SnapshotOperations::beginRestoreApplyForTesting(
    const QString &manifestId,
    const QString &sourcePath,
    int sourceDirFd,
    const QString &subvolume,
    bool useReflink,
    const qsnapper::restore::RootReadWriteSafetyNetState &rootReadWriteState)
{
    RestoreExecution execution;
    execution.configName = QStringLiteral("test");
    execution.snapshotDir = sourcePath;
    execution.snapshotDirFd = sourceDirFd;
    execution.subvolume = subvolume;
    execution.useReflink = useReflink;
    execution.rootReadWriteState = rootReadWriteState;
    m_restoreExecutions.insert(manifestId, execution);
}

/**
 * @brief applyRestoreEntryを、executorとD-Busを介さずに呼び出す
 * @param manifestId beginRestoreApplyForTestingで作った実行状態のID
 * @param entry 適用するentry
 * @return applyRestoreEntryの結果
 */
qsnapper::restore::RestoreEntryOutcome SnapshotOperations::applyRestoreEntryForTesting(
    const QString &manifestId,
    const qsnapper::restore::RestoreEntry &entry)
{
    return applyRestoreEntry(manifestId, entry);
}

/**
 * @brief finishRestorePlanを、executorを介さずに呼び出す
 * @param manifestId 終了させるマニフェストID
 * @param terminal executorが報告した終端状態
 */
void SnapshotOperations::finishRestorePlanForTesting(const QString &manifestId,
                                                     qsnapper::restore::ManifestState terminal)
{
    finishRestorePlan(manifestId, terminal, QString());
}

/**
 * @brief 例外を投げる本体をauthorizeThenで実行し、例外がスロットの外へ出ないかを確かめる
 * @param body 認可済みの本体として実行する処理 (例外を投げる)
 * @return authorizeThenの戻り値 (例外を止めた場合はfalse)
 */
bool SnapshotOperations::authorizeThrowingBodyForTesting(const std::function<bool()> &body)
{
    return authorizeThen<bool>(QStringLiteral("com.presire.qsnapper.view-diff"), body);
}
#endif

/**
 * @brief 認可待ち1件の終了を記録し、必要ならアイドルタイマを再開する
 * @param callerUid 認可待ちを開始したときの呼び出し元UID
 */
void SnapshotOperations::endPendingAuthorization(uint callerUid)
{
    m_pendingAuthorizations.release(callerUid);
    resetIdleTimer();
}

/**
 * @brief PolicyKitによる認可を要求する
 *
 * SubjectはSystemBusNameを用いる
 * UnixProcess (PIDベース) はPIDがレース中に再割り当てされるTOCTOU脆弱性 (CVE-2013-4288) があり、polkit自身も非推奨としている
 * SystemBusNameはカーネルのD-Bus name-owner情報をpolkitdが参照するため、呼び出し元の取り違えが起きない
 *
 * 対話を許可しない問い合わせを先に行う (高速経路)
 * allow_active=yesやauth_admin_keepのキャッシュ済み認可はここで許可となり、プロンプトが出ないためevent loopはミリ秒しか止まらない
 * 対話が必要な場合のみ非同期APIへ回す
 * 同期版の対話呼び出しはタイムアウトを持たず、任意のローカルユーザが未応答のプロンプトを1つ開くだけでrootサービスのevent loop全体を無期限に凍結できるため使用しない
 *
 * @param actionId チェックするアクションID
 * @param continuation 認可完了時に呼ぶ継続
 * @return 即時許可 / 即時拒否 / 遅延のいずれか
 */
SnapshotOperations::AuthorizationOutcome SnapshotOperations::beginAuthorization(
    const QString &actionId,
    std::function<void(const CallReply &, bool)> continuation)
{
    resetIdleTimer();

#ifdef QSNAPPER_ENABLE_TEST_HOOKS
    // 隔離環境にはpolkitdが無いため、テスト用サービスでは認可の結果だけを差し替える
    // 即時許可と遅延許可のどちらの経路も、本番と同じ呼び出し側 (authorizeThen) を通す
    // 即時許可は応答の遅延を伴わないため、単体テストがスロットを直接呼ぶ場合 (D-Bus経由でない呼び出し) にも許可する
    // 本番のビルドにはこの分岐が存在せず、D-Bus経由でない呼び出しは下の検査で必ず拒否される
    ++m_testAuthorizationRequests;
    if (m_testAuthorization == TestAuthorization::Grant) {
        return AuthorizationOutcome::Granted;
    }
#endif

    const CallReply reply = captureCallReply();
    if (!reply.fromDBus || reply.message.service().isEmpty()) {
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

#ifdef QSNAPPER_ENABLE_TEST_HOOKS
    if (m_testAuthorization == TestAuthorization::Defer) {
        m_testPendingAuthorizations.append({reply, continuation});
        setDelayedReply(true);
        return AuthorizationOutcome::Deferred;
    }
#endif

    PolkitAuthority *authority = polkitAuthorityInstance();
    if (!authority) {
        replyError(QDBusError::Failed,
                   QStringLiteral("Authorization service is unavailable"));
        return AuthorizationOutcome::Denied;
    }

    PolkitSubject *subject =
        polkit_system_bus_name_new(reply.message.service().toUtf8().constData());
    if (!subject) {
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

    GError *error = nullptr;
    PolkitAuthorizationResult *immediate = polkit_authority_check_authorization_sync(
        authority, subject, actionId.toUtf8().constData(), nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE, nullptr, &error);

    bool authorized = false;
    bool challenge = false;
    if (immediate) {
        authorized = polkit_authorization_result_get_is_authorized(immediate);
        challenge = polkit_authorization_result_get_is_challenge(immediate);
        g_object_unref(immediate);
    }
    else {
        qWarning() << "Polkit authorization pre-check failed:"
                   << (error ? error->message : "unknown error");
    }
    if (error) {
        g_error_free(error);
    }

    if (authorized) {
        g_object_unref(subject);
        return AuthorizationOutcome::Granted;
    }

    if (!challenge) {
        // 拒否が確定している (対話しても結果が変わらない) か、polkitdへ到達できなかった
        g_object_unref(subject);
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }

    // 認可待ちの枠は呼び出し元のUID単位で数える
    // UIDはBeginRestorePlanと同じく、バスデーモンが保持する接続の資格情報から取得する
    // 取得できない場合は誰の枠か決められないため拒否する
    const QDBusConnectionInterface *bus = reply.connection.interface();
    const QDBusReply<uint> uidReply = bus ? bus->serviceUid(reply.message.service()) : QDBusReply<uint>();
    if (!uidReply.isValid()) {
        g_object_unref(subject);
        replyError(QDBusError::AccessDenied, QStringLiteral("Authorization failed"));
        return AuthorizationOutcome::Denied;
    }
    const uint callerUid = uidReply.value();

    if (!m_pendingAuthorizations.tryAcquire(callerUid)) {
        g_object_unref(subject);
        replyError(QDBusError::LimitsExceeded,
                   QStringLiteral("Too many pending authorization requests"));
        return AuthorizationOutcome::Denied;
    }

    // 認可要求ごとに固有のuser_dataを渡し、完了結果が発行元の呼び出しへ
    // 一意に紐づくようにする
    auto *request = new AsyncAuthorization;
    request->service = this;
    request->continuation = [this, reply, continuation, callerUid](bool granted) {
        endPendingAuthorization(callerUid);
        continuation(reply, granted);
    };

    polkit_authority_check_authorization(
        authority, subject, actionId.toUtf8().constData(), nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, nullptr,
        asyncAuthorizationFinished, request);

    // polkit_authority_check_authorization()はsubjectを同期的にGVariant化するため、呼び出し直後に解放してよい
    g_object_unref(subject);

    setDelayedReply(true);
    // プロンプト応答待ちの間にアイドル終了しないようタイマを止める
    m_idleTimer.stop();
    return AuthorizationOutcome::Deferred;
}

/**
 * @brief Snapperインスタンスを取得
 *
 * 指定された設定名でSnapperインスタンスを取得または作成する
 * 設定が変更された場合は新しいインスタンスを作成する
 *
 * @param configName Snapper設定名
 * @param forceReload 同じ設定名でもインスタンスを作り直すか
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 * @note 復元の実行中は、インスタンスの差し替え (別config・forceReload) を一切行わない
 *       実行中の復元と終了時のunmount (unmountRestoreExecution) は同じインスタンスを使い続ける必要があり、
 *       破棄するとmount状態と比較キャッシュの寿命が壊れるため
 *       非復元操作はauthorizeThenのゲートで拒否されるが、ゲートを迂回する経路が将来できても
 *       共有インスタンスを破棄しないよう、ここでも多重に防御する (判定できない場合はnullptrで拒否する)
 */
snapper::Snapper* SnapshotOperations::getSnapper(const QString &configName, bool forceReload)
{
    // snapperdへ転送した変更の途中は、configやスナップショットが変わる途中の状態を読み込まないよう、インスタンスを渡さない
    if (snapperdOperationRunning()) {
        return nullptr;
    }

    if (!m_restoreExecutions.isEmpty()) {
        bool recordedConfig = false;
        for (const RestoreExecution &execution : std::as_const(m_restoreExecutions)) {
            if (execution.configName == configName) {
                recordedConfig = true;
                break;
            }
        }

        if (recordedConfig && !forceReload && m_snapper && m_currentConfig == configName) {
            return m_snapper.get();
        }

        // config名は呼び出し元が制御し得るため、ログには固定の文言だけを出す
        qWarning() << "Refusing to replace the Snapper instance while a restore is in progress";
        return nullptr;
    }

    try {
        // 設定変更時・初回・強制リロード指定時に新しいSnapperインスタンスを作成する
        // libsnapperのSnapperオブジェクトは構築時にスナップショット一覧を読み込み、
        // 外部で作成された新規スナップショットを自動で取り込まないため、一覧更新時にはforceReloadでインスタンスを作り直す必要がある
        if (!m_snapper || m_currentConfig != configName || forceReload) {
            // Snapper差し替え前にキャッシュを無効化し、
            // 古いSnapperを指すComparisonが残らない (mount/Filesの寿命が古Snapperに依存する) ようにする
            m_comparisonCache.clear();
            // 指紋は再生成前に取ったものだけが有効であるため、ここで破棄する (getSnapperForListing()が設定し直す)
            m_snapshotListFingerprint.clear();
            m_snapper.reset(new snapper::Snapper(configName.toStdString(), "/"));
            m_currentConfig = configName;
        }
        return m_snapper.get();
    }
    catch (const snapper::Exception &) {
        // e.what()には設定名やpathが含まれ得るため、ログには載せない
        qWarning() << "Failed to create Snapper instance";
        return nullptr;
    }
    catch (const std::exception &) {
        // bad_allocなどsnapper以外の例外も呼び出し元のスロットの外へ出すとサービスが異常終了するため、作成の失敗として扱う
        qWarning() << "Failed to create Snapper instance: unexpected exception";
        return nullptr;
    }
}

/**
 * @brief 一覧の取得用にSnapperインスタンスを取得する
 *
 * libsnapperのSnapperは構築時に一覧を読み込み、外部で作成・削除・変更されたスナップショットを取り込まない
 * そのため、.snapshots の指紋が読み込み時から変わっていれば再生成する
 *
 * 一覧を無効化する条件:
 *   - 設定の切り替え・強制再生成 (getSnapper()が指紋を破棄する)
 *   - 本サービスによる作成・削除・変更・rollback・設定変更 (各処理が変更前に指紋を破棄する)
 *   - 外部による作成・削除・変更 (指紋の不一致として検出する)
 *   - 指紋を計算できない場合
 *
 * @param configName 検証済みSnapper設定名
 * @return Snapperインスタンスへのポインタ、失敗時はnullptr
 */
snapper::Snapper* SnapshotOperations::getSnapperForListing(const QString &configName)
{
    // getSnapper()と同じ理由で、変更の途中はキャッシュしたインスタンスも渡さない
    if (snapperdOperationRunning()) {
        return nullptr;
    }

    // 指紋は再生成より先に取る
    // 再生成の途中で一覧が変わった場合も、保存した指紋とは一致しなくなるため、次回に再生成される
    QByteArray fingerprint;
    try {
        const snapper::ConfigInfo configInfo = snapper::Snapper::getConfig(configName.toStdString(), "/");
        QString snapshotsDir = QString::fromStdString(configInfo.get_subvolume());
        if (!snapshotsDir.endsWith(QLatin1Char('/'))) {
            snapshotsDir += QLatin1Char('/');
        }
        snapshotsDir += QStringLiteral(".snapshots");
        fingerprint = qsnapper::security::snapshotListFingerprint(snapshotsDir);
    }
    catch (const snapper::Exception &) {
        // e.what()には設定ファイルのpath (指定された設定名) が含まれ得るため、ログには載せない
        qWarning() << "Failed to read Snapper config for listing";
    }
    catch (const std::exception &) {
        // 指紋が空のままなら下の判定で作り直しへ進むため、読み取りの失敗と同じに扱う
        qWarning() << "Failed to read Snapper config for listing: unexpected exception";
    }

    if (m_snapper && m_currentConfig == configName && !fingerprint.isEmpty()
            && fingerprint == m_snapshotListFingerprint) {
        return m_snapper.get();
    }

    snapper::Snapper *snapper = getSnapper(configName, /*forceReload=*/true);
    if (snapper) {
        m_snapshotListFingerprint = fingerprint;
    }
    return snapper;
}

/**
 * @brief スナップショットタイプを文字列に変換
 *
 * snapperライブラリのスナップショットタイプ列挙値を文字列表現に変換する
 *
 * @param type スナップショットタイプ (snapper::SINGLE, PRE, POST)
 * @return タイプの文字列表現 ("single", "pre", "post")
 */
QString SnapshotOperations::snapshotTypeToString(int type)
{
    switch (type) {
        case snapper::SINGLE: return "single";
        case snapper::PRE: return "pre";
        case snapper::POST: return "post";
        default: return "single"    ;
    }
}

/**
 * @brief 文字列をスナップショットタイプに変換
 *
 * 文字列表現をsnapperライブラリのスナップショットタイプ列挙値に変換する
 *
 * @param typeStr タイプの文字列表現 ("single", "pre", "post")
 * @return スナップショットタイプ列挙値
 */
int SnapshotOperations::stringToSnapshotType(const QString &typeStr)
{
    if (typeStr == "pre") return snapper::PRE;
    if (typeStr == "post") return snapper::POST;
    return snapper::SINGLE;
}

/**
 * @brief スナップショット一覧をCSV形式に変換
 *
 * Snapperインスタンスから取得したスナップショット一覧をCSV形式の文字列に変換する
 *
 * @param snapper Snapperインスタンスへのポインタ
 * @return CSV形式のスナップショット情報文字列
 */
QString SnapshotOperations::formatSnapshotToCSV(const snapper::Snapper *snapper)
{
    if (!snapper) {
        return QString();
    }

    QString csv;
    csv += "number,type,pre-number,date,user,cleanup,description,userdata\n";

    const snapper::Snapshots &snapshots = snapper->getSnapshots();
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
        const snapper::Snapshot &snapshot = *it;

        // 本CSVは、"1行 = 1スナップショット"である
        // description / cleanup / userdataは、snapper CLIやpluginからも書き込まれる信頼できない値であり、
        // 改行を混入されると行が割れて攻撃者の選んだnumberを持つ偽のスナップショット行が生まれる
        // 偽のnumberはクライアントからDeleteSnapshot / RollbackSnapshotへそのまま渡る
        //
        // これらは表示用の値であるため、制御文字をU+FFFDに置き換えて出力する
        // 1件の不正な値で一覧全体を失敗させず、かつ行を割らない (numberはlibsnapperの値であり、置き換えの影響を受けない)
        // カンマは置き換えない (正当なdescriptionにも現れ得る)
        // 代わりにqsnapper::csv::quoteField()によるRFC 4180準拠のクォートで列ずれを防ぐ
        //
        // クォートは表示正しさのための修正であり、sanitizeRecordText()による制御文字の置き換えの上に重ねる層であって代替ではない
        const QString rawCleanup     = QString::fromStdString(snapshot.getCleanup());
        const QString rawDescription = QString::fromStdString(snapshot.getDescription());
        bool recordSafe = qsnapper::security::isRecordSafeText(rawCleanup)
                && qsnapper::security::isRecordSafeText(rawDescription);
        const QString cleanup     = qsnapper::security::sanitizeRecordText(rawCleanup);
        const QString description = qsnapper::security::sanitizeRecordText(rawDescription);

        // ユーザデータを key1=value1,key2=value2形式に変換
        // 各ペアは個別のCSVフィールドであるため、ペア文字列を組み立てた上で1フィールド毎にクォートする
        // (keyやvalueにカンマがあってもペアが2フィールドに割れない)
        const std::map<std::string, std::string> &userdata = snapshot.getUserdata();
        QStringList userdataPairs;
        for (const auto &pair : userdata) {
            const QString key   = QString::fromStdString(pair.first);
            const QString value = QString::fromStdString(pair.second);
            if (!qsnapper::security::isRecordSafeText(key) || !qsnapper::security::isRecordSafeText(value)) {
                recordSafe = false;
            }
            userdataPairs.append(qsnapper::csv::quoteField(qsnapper::security::sanitizeRecordText(key) + "="
                                                           + qsnapper::security::sanitizeRecordText(value)));
        }
        if (!recordSafe) {
            // 攻撃者が制御する値はログに載せない (ログ注入防止)
            qWarning() << "Snapshot listing: replaced control characters in metadata of snapshot"
                       << snapshot.getNum();
        }

        csv += QString::number(snapshot.getNum()) + ",";
        csv += snapshotTypeToString(snapshot.getType()) + ",";
        csv += QString::number(snapshot.getPreNum()) + ",";

        // 日時をISO形式に変換
        QDateTime dateTime = QDateTime::fromSecsSinceEpoch(snapshot.getDate());
        csv += dateTime.toString(Qt::ISODate) + ",";

        csv += QString::number(snapshot.getUid()) + ",";
        // cleanup / description / userdataペアはquoteField()でクォートする (前述の注記を参照)
        csv += qsnapper::csv::quoteField(cleanup) + ",";
        csv += qsnapper::csv::quoteField(description) + ",";

        csv += userdataPairs.join(",");
        csv += "\n";
    }

    return csv;
}
