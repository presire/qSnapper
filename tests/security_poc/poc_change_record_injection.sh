#!/usr/bin/env bash
#
# poc_change_record_injection.sh - 変更一覧の行指向出力への改行注入PoC
#
# 本PoCは本番のルートファイルシステムに対しては絶対に実行しない
# 全操作はuser namespace内のscratch root配下に閉じる
#
# シナリオ:
#   非特権ユーザが「改行入りのディレクトリ + 配下の通常階層」でcarrierを作る
#   (パス構成要素に '/' は含められないため、単一ファイル名での偽装は不可能)
#       <scratch>/home/alice/<"carrier\n+.... " という名前のディレクトリ>/etc/important
#   このフルパスをサーバが無検証で連結すると1エントリが2行に割れ、
#   2行目 "+.... /etc/important" が偽のcreatedエントリとしてクライアントに現れる
#   管理者がそれを選んで復元すると、任意パスが削除される (ディレクトリなら再帰削除)
#
#   修正版では以下の2層で止まる:
#     1. サーバ側シリアライズのfail-closed (isRecordSafeText)
#     2. created削除前の復元元存在確認 (isConfirmedAbsentAt)
#
# 実行:
#   poc_change_record_injection.sh [build-dir]              (修正版の阻止確認)
#   BASELINE=1 poc_change_record_injection.sh [build-dir]   (修正前の再現)
#
#   build-dir 既定値: build
#
# Exitコード規約 (tests/security_poc/README.md と同じ):
#   0 = 期待どおり (baselineなら再現成功、修正版なら阻止成功)
#   1 = 想定外 (baselineで再現失敗、修正版で再現成功 — いずれもregression)
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

POC_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_change_record_injection_poc 2>/dev/null | head -n1)"
if [ -z "$POC_BIN" ] || [ ! -x "$POC_BIN" ]; then
	echo "ERROR: qsnapper_change_record_injection_poc not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.:" >&2
	echo "         cmake -S \"$REPO_DIR\" -B \"$BUILD_DIR\" -DQSNAPPER_BUILD_TESTS=ON" >&2
	echo "         cmake --build \"$BUILD_DIR\" --target qsnapper_change_record_injection_poc" >&2
	exit 2
fi

POC_ARGS=()
MODE="fixed"
if [ -n "${BASELINE:-}" ]; then
	POC_ARGS+=(--baseline)
	MODE="baseline"
fi

RESULT_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULT_DIR"
LOG_FILE="$RESULT_DIR/${MODE}_$(basename "$0" .sh).log"

echo "RUNNER: repo dir  = $REPO_DIR"
echo "RUNNER: build dir = $BUILD_DIR"
echo "RUNNER: poc binary= $POC_BIN"
echo "RUNNER: mode      = $MODE"
echo "RUNNER: log file  = $LOG_FILE"

# user namespace内 (uid 0) で、ISOLATED_DBUS_TMPROOT配下のscratch rootに閉じて実行する
# 本番の / には一切触れない
INNER_CMD='
set -Eeuo pipefail
POC_BIN="$1"
shift
SCRATCH_ROOT="$ISOLATED_DBUS_TMPROOT/fixtures/scratch-root"
mkdir -p "$SCRATCH_ROOT"
exec "$POC_BIN" "$SCRATCH_ROOT" "$@"
'

set +e
"$ISOLATED_ENV" bash -c "$INNER_CMD" bash "$POC_BIN" "${POC_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
RC="${PIPESTATUS[0]}"
set -e

echo "RUNNER: exit code = $RC"
exit "$RC"
