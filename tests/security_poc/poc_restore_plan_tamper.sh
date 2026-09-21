#!/usr/bin/env bash
#
# poc_restore_plan_tamper.sh - staged restoreのコミット時changeType改ざんPoC
#
# 本PoCは本番のルートファイルシステムに対しては絶対に実行しない
# 全操作はuser namespace内のscratch root配下に閉じる
#
# シナリオ:
#   サーバがコミット時にクライアント申告のchangeTypeを信頼しない場合の挙動を検証する
#   - 差分のmodified entryをcreatedへ改ざんした計画はコミット時に拒否され、liveは不変
#   - 差分に現れないパスをmodifiedと偽った計画 (スナップショット内容のlive上書き攻撃) も拒否される
#   - 正当な計画はRevert to Pre / Re-apply to Postの両方向で検証を通過する (過剰拒否なし)
#
# 検証対象は本番の実ソースそのものである:
#
# | 層 | 実装 |
# |---|---|
# | 状態ビット -> changeType写像 / path正規化 / 権威あるmap構築 | qsnapper::restore::restorevalidation |
# | 凍結済み計画の保持とbounded slice読み出し | RestoreManifestRegistry |
# | created削除前の復元元存在確認 | qsnapper::security::isConfirmedAbsentAt() |
# | 実際の削除 | qsnapper::security::safeRemoveAllBeneathRoot() |
#
# 実行:
#   poc_restore_plan_tamper.sh [build-dir]
#
#   build-dir 既定値: build
#
# Exitコード規約 (tests/security_poc/README.md と同じ):
#   0 = 期待どおり (阻止成功 + 両方向の正当計画が通過)
#   1 = 想定外 (regression)
#   2 = 環境エラー
set -Eeuo pipefail

BUILD_DIR_ARG="${1:-build}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && cd .. && pwd)"
ISOLATED_ENV="$REPO_DIR/tests/integration/isolated_root_dbus_env.sh"

if [[ "$BUILD_DIR_ARG" = /* ]]; then
    BUILD_DIR="$BUILD_DIR_ARG"
else
    BUILD_DIR="$REPO_DIR/$BUILD_DIR_ARG"
fi

if [ ! -d "$BUILD_DIR" ]; then
	echo "ERROR: build directory not found: $BUILD_DIR" >&2
	exit 2
fi
if [ ! -x "$ISOLATED_ENV" ]; then
	echo "ERROR: isolation harness not found or not executable: $ISOLATED_ENV" >&2
	exit 2
fi

POC_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_restore_plan_tamper_poc 2>/dev/null | head -n1)"
if [ -z "$POC_BIN" ] || [ ! -x "$POC_BIN" ]; then
	echo "ERROR: qsnapper_restore_plan_tamper_poc not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.:" >&2
	echo "         cmake -S \"$REPO_DIR\" -B \"$BUILD_DIR\" -DQSNAPPER_BUILD_TESTS=ON" >&2
	echo "         cmake --build \"$BUILD_DIR\" --target qsnapper_restore_plan_tamper_poc" >&2
	exit 2
fi

RESULT_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULT_DIR"
LOG_FILE="$RESULT_DIR/fixed_$(basename "$0" .sh).log"

echo "RUNNER: repo dir  = $REPO_DIR"
echo "RUNNER: build dir = $BUILD_DIR"
echo "RUNNER: poc binary= $POC_BIN"
echo "RUNNER: log file  = $LOG_FILE"

# user namespace内 (uid 0) で、ISOLATED_DBUS_TMPROOT配下のscratch rootに閉じて実行する
# 本番の / には一切触れない
INNER_CMD='
set -Eeuo pipefail
POC_BIN="$1"
shift
SCRATCH_ROOT="$ISOLATED_DBUS_TMPROOT/fixtures/tamper-scratch-root"
rm -rf "$SCRATCH_ROOT"
mkdir -p "$SCRATCH_ROOT"
exec "$POC_BIN" "$SCRATCH_ROOT" "$@"
'

set +e
"$ISOLATED_ENV" bash -c "$INNER_CMD" bash "$POC_BIN" 2>&1 | tee "$LOG_FILE"
RC="${PIPESTATUS[0]}"
set -e

echo "RUNNER: exit code = $RC"
exit "$RC"
