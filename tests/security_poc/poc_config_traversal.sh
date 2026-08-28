#!/bin/bash
# poc_config_traversal.sh - Issue #2: configName パストラバーサル再現
#
# 目的:
#   configName に `../` や絶対パスを混入させて snapper::Snapper() が
#   意図しないファイルを参照することを確認。修正版 (P0-3) では
#   validateConfigName() により InvalidArgs で即座に拒否される。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

log "qsnapper version: $(get_qsnapper_version)"

# 悪性configName候補
PAYLOADS=(
    "../etc/snapper"
    "/etc/snapper/configs/root"
    ""
    "root/../etc"
    $'root\x00evil'
    "root; rm -rf /"
    "日本語"
)

reproduced=0
for payload in "${PAYLOADS[@]}"; do
    log "Testing configName=$(printf '%q' "$payload")"
    # GetFstype は副作用が最も軽いので代表として使用
    output=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
        "$DBUS_PATH" "${DBUS_IFACE}.GetFstype" string:"$payload" 2>&1 || true)

    if echo "$output" | grep -qE "Error.*InvalidArgs"; then
        log "  → blocked (InvalidArgs)"
    elif echo "$output" | grep -q "Error"; then
        # snapper内部でエラーになった可能性 = 検証通過 = 脆弱
        log "  → passed validation, snapper raised: $(echo "$output" | head -1)"
        reproduced=1
    else
        log "  → passed validation, returned: $(echo "$output" | head -1)"
        reproduced=1
    fi
done

# TODO: 全14メソッド (GetFstype, ListSnapshots, CreateSnapshot, ModifySnapshot,
#       DeleteSnapshot, RollbackSnapshot, GetFileChanges, GetFileChangesBetween,
#       GetFileDiffAndDetails, GetFileDiffBetween, RestoreFiles,
#       RestoreFilesDirect, WriteSnapperConfig, SetupQuota) に対して同じ
#       14 payload 走査を tests/integration/test_configname_dbus.py に移譲。
#       本PoCは代表確認のみ。

(( reproduced )) && report_result reproduced || report_result blocked
