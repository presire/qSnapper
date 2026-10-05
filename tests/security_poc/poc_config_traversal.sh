#!/bin/bash
#
# @file poc_config_traversal.sh
# @brief Issue #2: configNameパストラバーサルの再現PoC
#
# configNameに../や絶対パスを混入させ、snapper::Snapper()が意図しないファイルを参照するかを確認する
# 修正版(P0-3)ではvalidateConfigName()によりInvalidArgsで即座に拒否される
#
# 使い方 : poc_config_traversal.sh            (fixedモード)
#          BASELINE=1 poc_config_traversal.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm
assert_qsnapper_service

log "qsnapper version: $(get_qsnapper_version)"

# 悪性configName候補の一覧
PAYLOADS=(
    "../etc/snapper"
    "/etc/snapper/configs/root"
    ""
    "root/../etc"
    $'root\x00evil'
    "root; rm -rf /"
    "日本語"
)

reproduced=0    # 脆弱性を再現したら1にする
for payload in "${PAYLOADS[@]}"; do
    log "Testing configName=$(printf '%q' "$payload")"
    # 副作用が最も軽いGetFstypeを代表として使用する
    output=$(dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
        "$DBUS_PATH" "${DBUS_IFACE}.GetFstype" string:"$payload" 2>&1 || true)

    if echo "$output" | grep -qE "Error.*InvalidArgs"; then
        log "  → blocked (InvalidArgs)"
    elif echo "$output" | grep -q "Error"; then
        # snapper内部でエラーになったなら検証を通過したとみなし、脆弱と判定する
        log "  → passed validation, snapper raised: $(echo "$output" | head -1)"
        reproduced=1
    else
        # エラーなく応答が返った場合も検証を通過したとみなす
        log "  → passed validation, returned: $(echo "$output" | head -1)"
        reproduced=1
    fi
done

# TODO: 全14メソッド (GetFstype, ListSnapshots, CreateSnapshot, ModifySnapshot,
#       DeleteSnapshot, RollbackSnapshot, GetFileChanges, GetFileChangesBetween,
#       GetFileDiffAndDetails, GetFileDiffBetween, BeginRestorePlan,
#       WriteSnapperConfig, SetupQuota) に対して同じ
#       14種類のpayload走査を tests/integration/test_configname_dbus.py に移譲する
#       本PoCは代表確認のみ

# 1つでも再現すればreproduced、しなければblocked
(( reproduced )) && report_result reproduced || report_result blocked
