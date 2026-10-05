#!/bin/bash
#
# @file poc_cross_user.sh
# @brief Issue #4a: m_authenticatedのクロスユーザー汚染PoC
#
# シナリオ:
#   1. alice (admin) がCreateSnapshotを呼び、Polkit認証を成立させる
#      → v1.3.2ではm_authenticatedがtrueになる
#   2. その直後、bob (non-admin) が復元計画をcommitする (CommitRestorePlan)
#      → v1.3.2ではm_authenticated=trueの影響で通過する
#   3. 修正版(P0-2)ではm_authenticatedが撤去され、bob側は常に拒否される
#
# 事前準備: alice, bobが存在し、aliceがwheel/polkit_adminに属すること
#           対話的認証を避けるため、aliceはpkexecでパスワード入力済みの
#           セッションを別端末で確立した状態で実行するか、
#           expectスクリプトで自動化する
#
# 使い方 : poc_cross_user.sh            (fixedモード)
#          BASELINE=1 poc_cross_user.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

ALICE="${ALICE_USER:-alice}"    # 認証を成立させるadminユーザー(環境変数で上書き可)
BOB="${BOB_USER:-bob}"          # 復元計画をcommitするnon-adminユーザー(環境変数で上書き可)

if ! id "$ALICE" >/dev/null 2>&1 || ! id "$BOB" >/dev/null 2>&1; then
    log "ERROR: users $ALICE / $BOB not present"
    exit 2
fi

# Step 1: aliceで認証を成立させる (要: 事前にsudo認証済みまたは自動応答の設定)
log "Step 1: alice triggers authenticated call"
# TODO: 対話的認証の自動化は環境依存
#       ここではpkttyagentを想定するか、
#       テストVMでaliceのポリシーを一時的にallow_active=yesに緩和する
# aliceの認証呼び出しをバックグラウンドで開始する (bobの要求と重ねるため完了を待たない)
sudo -u "$ALICE" dbus-send --system --print-reply \
    --dest="$DBUS_SERVICE" "$DBUS_PATH" \
    "${DBUS_IFACE}.CreateSnapshot" \
    string:"root" string:"single" string:"cross-user-test" \
    int32:0 string:"" dict:string:string:"" boolean:false \
    > "$RESULT_DIR/${MODE}_cross_user_alice.log" 2>&1 &
ALICE_PID=$!
sleep 0.5

# Step 2: bobが復元計画をcommitする (通常ならnot authorized)
# legacyのRestoreFilesは廃止済み
# 計画はD-Bus接続に束縛されるため、Begin / Stage / Commitを同じ接続で呼ぶ (python3-dbus)
log "Step 2: bob attempts CommitRestorePlan while alice session is hot"
SNAP_ID=$(ensure_snapshot root)
OUTPUT=$(sudo -u "$BOB" python3 - "$SNAP_ID" 2>&1 <<'PYEOF' || true
import sys
from dbus import SystemBus, Interface
from dbus.exceptions import DBusException

ops = Interface(SystemBus().get_object("com.presire.qsnapper.Operations",
                                       "/com/presire/qsnapper/Operations"),
                "com.presire.qsnapper.Operations")
try:
    manifest_id = ops.BeginRestorePlan("root", int(sys.argv[1]), 0, "yast")
    ops.StageRestoreEntries(manifest_id, ["/etc/hostname"], ["modified"])
    ops.CommitRestorePlan(manifest_id)
    print("COMMITTED")
except DBusException as e:
    print(f"Error {e.get_dbus_name()}: {e.get_dbus_message()}")
PYEOF
)

wait "$ALICE_PID" 2>/dev/null || true

log "bob outcome: $OUTPUT"
# 拒否されれば阻止成功、通ってしまえばクロスユーザー汚染の再現
if echo "$OUTPUT" | grep -qE "Error.*(AccessDenied|NotAuthorized)"; then
    report_result blocked
else
    report_result reproduced
fi
