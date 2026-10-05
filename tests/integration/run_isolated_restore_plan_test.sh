#!/usr/bin/env bash
#
# @file run_isolated_restore_plan_test.sh
# @brief 隔離root環境で段階的復元計画のD-Busオーナーバインディングsecurityシナリオを実行する
#
# 指定したビルドディレクトリからqsnapper_restoreplan_test_serviceを探し、
# isolated_root_dbus_env.sh(プライベートなroot向けsystemバス、ホストバスとsudoを使わない)に委譲して
# test_restore_plan_owner_binding.pyを名前空間rootとして実行する
#
# 使い方 : run_isolated_restore_plan_test.sh [build-dir]   (既定値: build)
# 終了コード : シナリオスクリプト/ハーネスの終了コードをそのまま返す
#
set -Eeuo pipefail

BUILD_DIR_ARG="${1:-build}"                                 # 第1引数で指定されたビルドディレクトリ(未指定ならbuild)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"    # 本スクリプトのあるディレクトリ
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"                  # リポジトリルート

# 絶対パスならそのまま、相対パスならリポジトリルート基準にする
if [[ "$BUILD_DIR_ARG" = /* ]]; then
	BUILD_DIR="$BUILD_DIR_ARG"
else
	BUILD_DIR="$REPO_DIR/$BUILD_DIR_ARG"
fi

# ビルドディレクトリが無ければ環境エラー(66)で終了する
if [ ! -d "$BUILD_DIR" ]; then
	echo "ERROR: build directory not found: $BUILD_DIR" >&2
	exit 66
fi

# サービスバイナリをビルドディレクトリ配下から探す
SERVICE_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_restoreplan_test_service 2>/dev/null | head -n1)"
if [ -z "$SERVICE_BIN" ] || [ ! -x "$SERVICE_BIN" ]; then
	echo "ERROR: qsnapper_restoreplan_test_service not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.: cmake --build \"$BUILD_DIR\" --target qsnapper_restoreplan_test_service" >&2
	exit 66
fi

echo "RUNNER: build dir     = $BUILD_DIR"
echo "RUNNER: service binary = $SERVICE_BIN"
echo "RUNNER: repo dir      = $REPO_DIR"

# 名前空間内で実行する内部コマンド
# ログ出力先を一時ルート配下に固定し、サービスを起動する
INNER_CMD='
set -Eeuo pipefail
SERVICE_BIN="$1"
REPO_DIR="$2"
mkdir -p "$ISOLATED_DBUS_TMPROOT/fixtures"
export QSNAPPER_TEST_AUTH_LOG="$ISOLATED_DBUS_TMPROOT/fixtures/auth.log"
export QSNAPPER_TEST_APPLY_LOG="$ISOLATED_DBUS_TMPROOT/fixtures/apply.log"
export QSNAPPER_TEST_READY_FILE="$ISOLATED_DBUS_TMPROOT/fixtures/service.ready"
: > "$QSNAPPER_TEST_AUTH_LOG"
: > "$QSNAPPER_TEST_APPLY_LOG"
rm -f "$QSNAPPER_TEST_READY_FILE"
cd "$REPO_DIR"
exec python3 tests/integration/test_restore_plan_owner_binding.py "$SERVICE_BIN"
'

# 隔離ハーネスに内部コマンドとサービス/リポジトリのパスを渡して実行する
exec "$SCRIPT_DIR/isolated_root_dbus_env.sh" bash -c "$INNER_CMD" bash "$SERVICE_BIN" "$REPO_DIR"
