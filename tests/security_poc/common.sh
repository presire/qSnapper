#!/bin/bash
#
# @file common.sh
# @brief 各PoCスクリプトがsourceで読み込む共通ヘルパ関数群
#
# VM検出、qSnapper D-Busサービスの存在確認、結果判定、スナップショット確保を提供する
# BASELINEを設定するとbaselineモード(修正前の再現確認)、未設定ならfixedモード(修正後の阻止確認)になる
#
# 使い方 : 各PoCスクリプトから source "$SCRIPT_DIR/common.sh" で読み込む
# 終了コード : 各関数のreport_resultがPASS/FAILに応じて0/1でexitする
#

set -u    # 未定義変数の参照をエラーにする

readonly DBUS_SERVICE="com.presire.qsnapper.Operations"         # qSnapperのD-Busサービス名
readonly DBUS_PATH="/com/presire/qsnapper/Operations"           # D-Busオブジェクトパス
readonly DBUS_IFACE="com.presire.qsnapper.Operations"           # D-Busインターフェース名
readonly RESULT_DIR="$(dirname "${BASH_SOURCE[0]}")/results"    # 結果ログの出力先
readonly MODE="${BASELINE:+baseline}${BASELINE:-fixed}"         # BASELINE=1ならbaseline、そうでなければfixed

# 結果ログの出力先を用意する
mkdir -p "$RESULT_DIR"

#
# @brief タイムスタンプ付きメッセージを標準出力と結果ログの両方へ出力する
# @param $* 出力するメッセージ
# @stdout タイムスタンプ付きメッセージ
# @return 0
# @global RESULT_DIR MODE 結果ログの出力先とモード名に使う
# @note 結果ログはresults/<MODE>_<呼び出し元スクリプト名>.logへ追記する
#
log() {
    local msg="$*"
    echo "[$(date -Iseconds)] $msg" | tee -a "$RESULT_DIR/${MODE}_$(basename "$0" .sh).log"
}

#
# @brief 実行環境がVMかどうかをsystemd-detect-virtで確認する
# @return なし (VMでなければexit 2)
# @stdout VM検出結果のログ
# @note ベアメタルでの実行を拒否する安全装置
#
assert_running_in_vm() {
    # systemd-detect-virtが無い環境では実行できない
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

#
# @brief qSnapper D-Busサービスがシステムバスに存在するか確認する
# @return なし (存在しなければexit 2)
# @stdout 存在確認のログ
# @note D-Bus activationを期待して一度メソッドを叩き、その後で再確認する
#
assert_qsnapper_service() {
    if ! busctl --system list 2>/dev/null | grep -q "$DBUS_SERVICE"; then
        # D-Bus activationを期待して一度叩く
        dbus-send --system --print-reply --dest="$DBUS_SERVICE" \
            "$DBUS_PATH" "${DBUS_IFACE}.IsConfigured" >/dev/null 2>&1 || true
    fi
    if ! busctl --system list 2>/dev/null | grep -q "$DBUS_SERVICE"; then
        echo "ERROR: $DBUS_SERVICE is not available on the system bus" >&2
        exit 2
    fi
    log "qSnapper D-Bus service is up"
}

#
# @brief インストール済みqSnapperのバージョンを取得する
# @stdout バージョン文字列(取得できない場合はunknown)
# @return 0
#
get_qsnapper_version() {
    rpm -q qSnapper --qf "%{VERSION}-%{RELEASE}\n" 2>/dev/null || echo "unknown"
}

#
# @brief 判定結果をBASELINEモードに応じてPASS/FAILへ変換して終了する
# @param $1 結果 ("reproduced" = 再現成功、"blocked" = 阻止成功)
# @stdout 判定ログ
# @return 0 = 期待どおり、1 = 想定外
# @global MODE baselineかfixedかでPASSの意味が変わる
# @note baselineモードでは再現成功がPASS、fixedモードでは阻止成功がPASS
#
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

#
# @brief 指定configのsnapshot IDを1つ確保する (無ければ作成する)
# @param $1 snapper config名 (既定: root)
# @stdout 確保したsnapshot ID
# @return 0
# @note snapshotが1つも無い場合はpoc-testの説明で新規作成する
#
ensure_snapshot() {
    local config="${1:-root}"
    local snap_id
    snap_id=$(snapper -c "$config" list --columns number | awk 'NR>3 && $1 ~ /^[0-9]+$/ {print $1; exit}')
    if [[ -z "$snap_id" ]]; then
        snap_id=$(snapper -c "$config" create -d "poc-test" -p)
    fi
    echo "$snap_id"
}
