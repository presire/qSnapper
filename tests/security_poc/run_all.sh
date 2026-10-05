#!/bin/bash
#
# @file run_all.sh
# @brief VM上で全PoCを順次実行し、results/へ結果ログを集約する
#
# 各PoCをBASELINEモードで実行し、PASS/FAIL/ERRORを集計する
# C-1 (polkit_race) だけはbob権限が必要なため別扱いでsudo経由で実行する
#
# 使い方 : run_all.sh           (fixedモード)
#          BASELINE=1 run_all.sh (baselineモード)
# 終了コード : FAILとERRORの合計 (0なら全PASS)
#

set -u    # 未定義変数の参照をエラーにする
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"  # 本スクリプトのあるディレクトリ
MODE="${BASELINE:+baseline}${BASELINE:-fixed}"              # BASELINE=1ならbaseline、そうでなければfixed
SUMMARY="$SCRIPT_DIR/results/${MODE}_SUMMARY.txt"           # 集計結果の出力先

mkdir -p "$SCRIPT_DIR/results"
: > "$SUMMARY"    # 前回の集計を空にする

echo "=== qSnapper Security PoC run (mode=$MODE, $(date -Iseconds)) ===" | tee -a "$SUMMARY"
echo "qsnapper version: $(rpm -q qSnapper 2>/dev/null || echo unknown)" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"

# 実行対象のPoC一覧 (キーは表示名、値は実行コマンド)
declare -A POCS=(
    ["C-2 config_traversal"]="bash $SCRIPT_DIR/poc_config_traversal.sh"
    ["C-3 cross_user"]="bash $SCRIPT_DIR/poc_cross_user.sh"
    ["C-4 action_mixup"]="bash $SCRIPT_DIR/poc_action_mixup.sh"
    ["C-5 restore_traversal"]="bash $SCRIPT_DIR/poc_restore_traversal.sh"
    ["C-6 quit_dos"]="bash $SCRIPT_DIR/poc_quit_dos.sh"
    ["C-7 log_leak"]="bash $SCRIPT_DIR/poc_log_leak.sh"
    # C-1 はbobが実行する必要あり、別扱い
)

pass=0; fail=0; err=0
for name in "${!POCS[@]}"; do
    cmd="${POCS[$name]}"
    echo ">>> Running $name" | tee -a "$SUMMARY"
    # PoCのFAILを集計するため、この区間だけerrexitを無効化する
    set +e
    BASELINE="${BASELINE:-}" $cmd
    rc=$?
    set -e
    case $rc in
        0) echo "    PASS" | tee -a "$SUMMARY"; ((pass++)) ;;                       # 期待どおり
        1) echo "    FAIL (unexpected outcome)" | tee -a "$SUMMARY"; ((fail++)) ;;  # 想定外の結果
        *) echo "    ERROR (env issue, rc=$rc)" | tee -a "$SUMMARY"; ((err++)) ;;   # 環境エラー
    esac
done

# C-1はbob権限で実行する
echo ">>> Running C-1 polkit_race (as bob)" | tee -a "$SUMMARY"
set +e
BASELINE="${BASELINE:-}" sudo -u "${BOB_USER:-bob}" \
    python3 "$SCRIPT_DIR/poc_polkit_race.py" --iterations 500
rc=$?
set -e
case $rc in
    0) echo "    PASS" | tee -a "$SUMMARY"; ((pass++)) ;;    # 期待どおり
    1) echo "    FAIL" | tee -a "$SUMMARY"; ((fail++)) ;;    # 想定外の結果
    *) echo "    ERROR" | tee -a "$SUMMARY"; ((err++)) ;;    # 環境エラー
esac

echo "" | tee -a "$SUMMARY"
echo "=== Summary: pass=$pass fail=$fail err=$err ===" | tee -a "$SUMMARY"
exit $(( fail + err ))
