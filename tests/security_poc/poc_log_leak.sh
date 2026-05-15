#!/bin/bash
# poc_log_leak.sh - Issue #5c: /var/log/qsnapper 情報漏洩
#
# v1.3.2 では /var/log/qsnapper/*.log が 0644 で作成されており、
# 一般ユーザー bob が内容を読める。修正版 (P1-7) では
# ディレクトリ0700 + ファイル0600 に変更される。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm

LOG_DIR="/var/log/qsnapper"
BOB="${BOB_USER:-bob}"

if [[ ! -d "$LOG_DIR" ]]; then
    log "Log dir $LOG_DIR does not exist — triggering a call to create logs"
    dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
        "$DBUS_PATH" "${DBUS_IFACE}.IsConfigured" >/dev/null 2>&1 || true
fi

DIR_MODE=$(stat -c '%a' "$LOG_DIR" 2>/dev/null || echo "missing")
log "Dir mode of $LOG_DIR: $DIR_MODE"

# 最初に見つかったログファイル
LOGFILE=$(find "$LOG_DIR" -maxdepth 1 -type f -name '*.log' 2>/dev/null | head -1)
if [[ -z "$LOGFILE" ]]; then
    log "No log file found — cannot test read access"
    report_result blocked
fi

FILE_MODE=$(stat -c '%a' "$LOGFILE")
log "File mode of $LOGFILE: $FILE_MODE"

# bob で読めるか試す
if sudo -u "$BOB" cat "$LOGFILE" >/dev/null 2>&1; then
    log "bob could read $LOGFILE — leak reproduced"
    report_result reproduced
else
    log "bob denied — permissions properly restricted"
    # 期待モード確認
    if [[ "$DIR_MODE" == "700" && "$FILE_MODE" == "600" ]]; then
        log "Modes match the spec (dir=700, file=600)"
    else
        log "WARN: denied but modes differ from spec (dir=$DIR_MODE file=$FILE_MODE)"
    fi
    report_result blocked
fi
