#!/bin/bash
# common.sh - 共通ヘルパ関数。各PoCスクリプトから `source` される。

set -u

readonly DBUS_SERVICE="com.presire.qsnapper.Operations"
readonly DBUS_PATH="/com/presire/qsnapper/Operations"
readonly DBUS_IFACE="com.presire.qsnapper.Operations"
readonly RESULT_DIR="$(dirname "${BASH_SOURCE[0]}")/results"
readonly MODE="${BASELINE:+baseline}${BASELINE:-fixed}"  # BASELINE=1ならbaseline、そうでなければfixed

mkdir -p "$RESULT_DIR"

log() {
    local msg="$*"
    echo "[$(date -Iseconds)] $msg" | tee -a "$RESULT_DIR/${MODE}_$(basename "$0" .sh).log"
}

assert_running_in_vm() {
    # VM検出: systemd-detect-virtのチェック。ベアメタルでは実行拒否。
    if ! command -v systemd-detect-virt >/dev/null; then
        echo "ERROR: systemd-detect-virt not available" >&2
        exit 2
    fi
    local virt
    virt=$(systemd-detect-virt)
    if [[ "$virt" == "none" ]]; then
        echo "ERROR: This PoC must run inside a VM. Aborting." >&2
        echo "       Detected: bare metal (systemd-detect-virt=none)" >&2
        exit 2
    fi
    log "VM check passed (virt=$virt)"
}

assert_qsnapper_service() {
    if ! busctl --system list 2>/dev/null | grep -q "$DBUS_SERVICE"; then
        # D-Bus activation を期待して一度叩く
        dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
            "$DBUS_PATH" "${DBUS_IFACE}.IsConfigured" >/dev/null 2>&1 || true
    fi
    if ! busctl --system list 2>/dev/null | grep -q "$DBUS_SERVICE"; then
        echo "ERROR: $DBUS_SERVICE is not available on the system bus" >&2
        exit 2
    fi
    log "qSnapper D-Bus service is up"
}

get_qsnapper_version() {
    rpm -q qSnapper --qf "%{VERSION}-%{RELEASE}\n" 2>/dev/null || echo "unknown"
}

# 結果判定: BASELINEモードでは「再現成功=PASS」、修正版モードでは「阻止成功=PASS」
# $1: "reproduced" | "blocked"
report_result() {
    local outcome="$1"
    local pass=0
    if [[ "$MODE" == "baseline" && "$outcome" == "reproduced" ]]; then pass=1; fi
    if [[ "$MODE" == "fixed"    && "$outcome" == "blocked"    ]]; then pass=1; fi

    if (( pass )); then
        log "RESULT: PASS (mode=$MODE, outcome=$outcome)"
        exit 0
    else
        log "RESULT: FAIL (mode=$MODE, outcome=$outcome) — UNEXPECTED"
        exit 1
    fi
}

# snapshot IDを1つ確保 (なければ作る)
ensure_snapshot() {
    local config="${1:-root}"
    local snap_id
    snap_id=$(snapper -c "$config" list --columns number | awk 'NR>3 && $1 ~ /^[0-9]+$/ {print $1; exit}')
    if [[ -z "$snap_id" ]]; then
        snap_id=$(snapper -c "$config" create -d "poc-test" -p)
    fi
    echo "$snap_id"
}
