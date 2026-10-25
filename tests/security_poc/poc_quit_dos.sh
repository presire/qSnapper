#!/bin/bash
#
# @file poc_quit_dos.sh
# @brief Issue #5b: 無認証のQuit()によるサービス停止DoSのPoC
#
# bobがdbus-sendでQuit()を呼び、サービスを停止させられるかを確認する
# 修正版(P1-6)ではQuit() D-Busメソッドが削除されておりUnknownMethodになる
#
# 使い方 : poc_quit_dos.sh            (fixedモード)
#          BASELINE=1 poc_quit_dos.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

BOB="${BOB_USER:-bob}"    # Quit()を呼ぶ非特権ユーザー(環境変数で上書き可)

HELPER_PATTERN="^/usr/libexec/qsnapper-dbus-service"    # 現行のヘルパーの実行ファイル (旧版の--dbus-serviceの起動形式は存在しない)

# サービス稼働中であることを再確認するためPIDを取得する
PID_BEFORE=$(pgrep -f "$HELPER_PATTERN" | head -1 || true)
log "qSnapper service PID before: $PID_BEFORE"

# 変更前のPIDが取れないと、後の比較で停止したと誤判定するため、環境エラーとして終える
if [[ -z "$PID_BEFORE" ]]; then
    log "ERROR: helper process not found"
    exit 2
fi

# bobがQuit()を呼ぶ
OUTPUT=$(sudo -u "$BOB" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.Quit" 2>&1 || true)
log "Quit() response: $OUTPUT"

# プロセス停止を待つ
sleep 1
PID_AFTER=$(pgrep -f "$HELPER_PATTERN" | head -1 || true)
log "qSnapper service PID after: $PID_AFTER"

# 応答の種類より先にプロセスの生存を判定する
# 拒否の応答が返っても、ヘルパーが停止または入れ替わっていれば阻止できていないため
if [[ "$PID_BEFORE" != "$PID_AFTER" || -z "$PID_AFTER" ]]; then
    log "Service was killed by unauthenticated Quit() — vulnerability reproduced"
    report_result reproduced
# Quit()がヘルパーに公開されていなければUnknownMethodが返る (修正版)
elif echo "$OUTPUT" | grep -q "UnknownMethod"; then
    log "Quit() is not exported by the helper — good"
    report_result blocked
# D-Busの送信許可 (com.presire.qsnapper.Operations.conf) が公開メソッドだけを許可するため、バスがヘルパーへ届ける前に拒否する
# これは非特権の呼び出しを遮断した証拠であり、メソッドの削除そのものはcheck_dbus_contract_sync.pyで確認する
elif echo "$OUTPUT" | grep -q "AccessDenied"; then
    log "Quit() call was denied by the bus policy — good"
    report_result blocked
# Quit()が残っていても停止しなければ阻止成功とみなす
else
    log "Service survived despite Quit() being available — unusual"
    report_result blocked
fi
