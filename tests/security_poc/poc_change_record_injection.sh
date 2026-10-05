#!/usr/bin/env bash
#
# @file poc_change_record_injection.sh
# @brief 変更一覧の行指向出力への改行注入PoC
#
# 本PoCは本番のルートファイルシステムに対しては絶対に実行しない
# 全操作はuser名前空間内のscratch root配下に閉じる
#
# シナリオ:
#   非特権ユーザが改行入りのディレクトリと配下の通常階層でcarrierを作る
#   (パス構成要素に/は含められないため、単一ファイル名での偽装は不可能)
#       <scratch>/home/alice/<carrier\n+.... という名前のディレクトリ>/etc/important
#   このフルパスをサーバが無検証で連結すると1エントリが2行に割れ、
#   2行目 +.... /etc/important が偽のcreatedエントリとしてクライアントに現れる
#   管理者がそれを選んで復元すると任意パスが削除される (ディレクトリなら再帰削除)
#
#   修正版では以下の2層で止まる:
#     1. サーバ側シリアライズのfail-closed (isRecordSafeText)
#     2. created削除前の復元元存在確認 (isConfirmedAbsentAt)
#
# 使い方 : poc_change_record_injection.sh [build-dir]              (修正版の阻止確認)
#          BASELINE=1 poc_change_record_injection.sh [build-dir]   (修正前の再現)
#          build-dirの既定値はbuild
# 終了コード (tests/security_poc/README.mdと同じ):
#   0 = 期待どおり (baselineなら再現成功、修正版なら阻止成功)
#   1 = 想定外 (baselineで再現失敗、修正版で再現成功、いずれもregression)
#   2 = 環境エラー
#
set -Eeuo pipefail

BUILD_DIR_ARG="${1:-build}"    # 第1引数で指定されたビルドディレクトリ(未指定ならbuild)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
REPO_DIR="$(cd "$SCRIPT_DIR/.." && cd .. && pwd)"    # リポジトリルート
ISOLATED_ENV="$REPO_DIR/tests/integration/isolated_root_dbus_env.sh"    # 隔離ハーネス

# 絶対パスならそのまま、相対パスならリポジトリルート基準にする
if [[ "$BUILD_DIR_ARG" = /* ]]; then
    BUILD_DIR="$BUILD_DIR_ARG"
else
    BUILD_DIR="$REPO_DIR/$BUILD_DIR_ARG"
fi

# ビルドディレクトリが無ければ環境エラー(2)で終了する
if [ ! -d "$BUILD_DIR" ]; then
	echo "ERROR: build directory not found: $BUILD_DIR" >&2
	exit 2
fi
if [ ! -x "$ISOLATED_ENV" ]; then
	echo "ERROR: isolation harness not found or not executable: $ISOLATED_ENV" >&2
	exit 2
fi

# PoCバイナリをビルドディレクトリ配下から探す
POC_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_change_record_injection_poc 2>/dev/null | head -n1)"
if [ -z "$POC_BIN" ] || [ ! -x "$POC_BIN" ]; then
	echo "ERROR: qsnapper_change_record_injection_poc not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.:" >&2
	echo "         cmake -S \"$REPO_DIR\" -B \"$BUILD_DIR\" -DQSNAPPER_BUILD_TESTS=ON" >&2
	echo "         cmake --build \"$BUILD_DIR\" --target qsnapper_change_record_injection_poc" >&2
	exit 2
fi

# BASELINEが設定されていればbaseline(修正前の再現)、未設定ならfixed
POC_ARGS=()    # PoCバイナリへ渡す追加引数
MODE="fixed"    # 実行モード
if [ -n "${BASELINE:-}" ]; then
	POC_ARGS+=(--baseline)
	MODE="baseline"
fi

RESULT_DIR="$SCRIPT_DIR/results"    # 結果ログの出力先
mkdir -p "$RESULT_DIR"
LOG_FILE="$RESULT_DIR/${MODE}_$(basename "$0" .sh).log"    # この実行のログファイル

echo "RUNNER: repo dir  = $REPO_DIR"
echo "RUNNER: build dir = $BUILD_DIR"
echo "RUNNER: poc binary= $POC_BIN"
echo "RUNNER: mode      = $MODE"
echo "RUNNER: log file  = $LOG_FILE"

# user名前空間内 (uid 0) で、ISOLATED_DBUS_TMPROOT配下のscratch rootに閉じて実行する
# 本番の / には一切触れない
INNER_CMD='
set -Eeuo pipefail
POC_BIN="$1"
shift
SCRATCH_ROOT="$ISOLATED_DBUS_TMPROOT/fixtures/scratch-root"
mkdir -p "$SCRATCH_ROOT"
exec "$POC_BIN" "$SCRATCH_ROOT" "$@"
'

# teeの終了コードではなくハーネス本体の終了コードを返すため、PIPESTATUS経由で取り出す (その間だけerrexitを無効化する)
set +e
"$ISOLATED_ENV" bash -c "$INNER_CMD" bash "$POC_BIN" "${POC_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
RC="${PIPESTATUS[0]}"
set -e

echo "RUNNER: exit code = $RC"
exit "$RC"
