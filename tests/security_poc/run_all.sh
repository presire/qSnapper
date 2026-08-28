#!/bin/bash
# run_all.sh - 全PoCを順次実行し、results/ に結果ログを集約する。
# BASELINE=1 を付けて実行すると baseline モード、付けないと fixed モード。

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${BASELINE:+baseline}${BASELINE:-fixed}"
SUMMARY="$SCRIPT_DIR/results/${MODE}_SUMMARY.txt"

mkdir -p "$SCRIPT_DIR/results"
: > "$SUMMARY"

echo "=== qSnapper Security PoC run (mode=$MODE, $(date -Iseconds)) ===" | tee -a "$SUMMARY"
echo "qsnapper version: $(rpm -q qSnapper 2>/dev/null || echo unknown)" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"

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
    set +e
    BASELINE="${BASELINE:-}" $cmd
    rc=$?
    set -e
    case $rc in
        0) echo "    PASS" | tee -a "$SUMMARY"; ((pass++)) ;;
        1) echo "    FAIL (unexpected outcome)" | tee -a "$SUMMARY"; ((fail++)) ;;
        *) echo "    ERROR (env issue, rc=$rc)" | tee -a "$SUMMARY"; ((err++)) ;;
    esac
done

# C-1 は bob 権限で走らせる
echo ">>> Running C-1 polkit_race (as bob)" | tee -a "$SUMMARY"
set +e
BASELINE="${BASELINE:-}" sudo -u "${BOB_USER:-bob}" \
    python3 "$SCRIPT_DIR/poc_polkit_race.py" --iterations 500
rc=$?
set -e
case $rc in
    0) echo "    PASS" | tee -a "$SUMMARY"; ((pass++)) ;;
    1) echo "    FAIL" | tee -a "$SUMMARY"; ((fail++)) ;;
    *) echo "    ERROR" | tee -a "$SUMMARY"; ((err++)) ;;
esac

echo "" | tee -a "$SUMMARY"
echo "=== Summary: pass=$pass fail=$fail err=$err ===" | tee -a "$SUMMARY"
exit $(( fail + err ))
