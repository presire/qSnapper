#!/bin/bash
# poc_restore_traversal.sh - Issue #5a: 復元パストラバーサル (StageRestoreEntries)
#
# ⚠️ 本PoCは /etc/shadow の mtime/inode を変化させる可能性あり。VM限定。
#
# シナリオ:
#   filePaths に `/.snapshots/N/snapshot/../../../etc/shadow` 等を渡し、
#   snapshot内のファイルではなくホストルートの /etc/shadow を書き換え
#   させることを試みる。staged restore では StageRestoreEntries が
#   "."/".." 成分、相対パス、/.snapshots 配下 (正規化後の先頭成分で判定) を
#   InvalidArgs で拒否する。

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

# 悪性filePath候補 (StageRestoreEntriesはconfig相対名を受け取る)
# "/etc/shadow" はroot configでは正当な名前なので含めない (commit時に権威ある比較結果との照合で弾かれる)
PAYLOADS=(
    "/.snapshots/$SNAP_ID/snapshot/../../../etc/shadow"
    "etc/shadow"
    "/.snapshots/$SNAP_ID/snapshot/etc/../../../etc/shadow"
    "//.snapshots/$SNAP_ID/snapshot/etc/shadow"
    "/etc/../etc/shadow"
)

# 認証済み admin で叩く必要がある (alice を使う)
ALICE="${ALICE_USER:-alice}"

# legacy の RestoreFiles / RestoreFilesDirect は廃止済み
# 計画はD-Bus接続 (unique name) に束縛されるため、Begin と Stage を同じ接続で呼ぶ必要があり、dbus-send ではなく python3-dbus を使う
reproduced=0
for mode in yast direct; do
    for payload in "${PAYLOADS[@]}"; do
        log "Testing mode=$mode filePath=$payload"
        OUTPUT=$(sudo -u "$ALICE" python3 - "$SNAP_ID" "$mode" "$payload" 2>&1 <<'PYEOF' || true
import sys
from dbus import SystemBus, Interface
from dbus.exceptions import DBusException

snap_id, mode, payload = int(sys.argv[1]), sys.argv[2], sys.argv[3]
ops = Interface(SystemBus().get_object("com.presire.qsnapper.Operations",
                                       "/com/presire/qsnapper/Operations"),
                "com.presire.qsnapper.Operations")
manifest_id = ops.BeginRestorePlan("root", snap_id, 0, mode)
try:
    ops.StageRestoreEntries(manifest_id, [payload], ["modified"])
    print("STAGED")
except DBusException as e:
    print(f"Error {e.get_dbus_name()}: {e.get_dbus_message()}")
finally:
    try:
        ops.CancelRestorePlan(manifest_id)
    except DBusException:
        pass
PYEOF
)

        if echo "$OUTPUT" | grep -qE "Error.*InvalidArgs"; then
            log "  → blocked"
        else
            log "  → WARNING: no validation error, call returned: $(echo "$OUTPUT" | head -1)"
            reproduced=1
        fi
    done
done

AFTER=$(stat -c '%Y %i %s' /etc/shadow)
log "After : /etc/shadow stat = $AFTER"
if [[ "$BEFORE" != "$AFTER" ]]; then
    log "CRITICAL: /etc/shadow was modified!"
    reproduced=1
fi

(( reproduced )) && report_result reproduced || report_result blocked
