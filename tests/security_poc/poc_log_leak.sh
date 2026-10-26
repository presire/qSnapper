#!/bin/bash
#
# @file poc_log_leak.sh
# @brief Issue #5c: /var/log/qsnapperの情報漏洩PoC
#
# v1.3.2では/var/log/qsnapper/*.logが0644で作成されており、一般ユーザーbobが内容を読める
# 修正版(P1-7)ではディレクトリ0700 + ファイル0600に変更される
#
# 使い方 : poc_log_leak.sh            (fixedモード)
#          BASELINE=1 poc_log_leak.sh (baselineモード)
# 終了コード : 0 = 期待どおり、1 = 想定外、2 = 環境エラー
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
source "$SCRIPT_DIR/common.sh"

assert_running_in_vm

LOG_DIR="/var/log/qsnapper"    # 漏洩対象のログディレクトリ
BOB="${BOB_USER:-bob}"          # 読み取りを試す非特権ユーザー(環境変数で上書き可)

# ログディレクトリが無ければ一度D-Busメソッドを叩いて作成させる
if [[ ! -d "$LOG_DIR" ]]; then
    log "Log dir $LOG_DIR does not exist — triggering a call to create logs"
    dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
        "$DBUS_PATH" "${DBUS_IFACE}.IsConfigured" >/dev/null 2>&1 || true
fi

DIR_MODE=$(stat -c '%a' "$LOG_DIR" 2>/dev/null || echo "missing")
log "Dir mode of $LOG_DIR: $DIR_MODE"

# 最初に見つかったログファイルを対象にする
LOGFILE=$(find "$LOG_DIR" -maxdepth 1 -type f -name '*.log' 2>/dev/null | head -1)
if [[ -z "$LOGFILE" ]]; then
    log "No log file found — cannot test read access"
    report_result blocked
fi

FILE_MODE=$(stat -c '%a' "$LOGFILE")
log "File mode of $LOGFILE: $FILE_MODE"

# bobで読めるか試す
if sudo -u "$BOB" cat "$LOGFILE" >/dev/null 2>&1; then
    log "bob could read $LOGFILE — leak reproduced"
    report_result reproduced
else
    log "bob denied — permissions properly restricted"
    # 拒否された場合は期待モード(0700/0600)と一致するか確認する
    if [[ "$DIR_MODE" == "700" && "$FILE_MODE" == "600" ]]; then
        log "Modes match the spec (dir=700, file=600)"
    else
        log "WARN: denied but modes differ from spec (dir=$DIR_MODE file=$FILE_MODE)"
    fi
    report_result blocked
fi
