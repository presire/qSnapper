#!/usr/bin/env bash
#
# @file run_isolated_restore_plan_test.sh
# @brief 隔離root環境で段階的復元計画のD-Busオーナーバインディングsecurityシナリオ、復元中の共通ゲートのシナリオ、設定とタイマーのシナリオを実行する
#
# 指定したビルドディレクトリからqsnapper_restoreplan_test_serviceとqsnapper_restoregate_test_serviceを探し、
# isolated_root_dbus_env.sh(プライベートなroot向けsystemバス、ホストバスとsudoを使わない)に委譲して
# test_restore_plan_owner_binding.py、test_restore_gate.py、test_settings_timers.py、test_configs_bulk.py、
# test_snapperd_operation.pyを名前空間rootとして順に実行する
# 各サービスは同じD-Bus名を取得するため、同時には起動しない
# test_settings_timers.pyは、同じ隔離バス上に偽のsystemd(fake_systemd1.py)も起動する
# test_snapperd_operation.pyは、同じ隔離バス上に偽のsnapperd(fake_snapperd.py)とsnapperdop_test_driverを起動する
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

# 共通ゲートのサービスバイナリも同様に探す
GATE_SERVICE_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_restoregate_test_service 2>/dev/null | head -n1)"
if [ -z "$GATE_SERVICE_BIN" ] || [ ! -x "$GATE_SERVICE_BIN" ]; then
	echo "ERROR: qsnapper_restoregate_test_service not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.: cmake --build \"$BUILD_DIR\" --target qsnapper_restoregate_test_service" >&2
	exit 66
fi

# snapperd操作のドライバも同様に探す
DRIVER_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name snapperdop_test_driver 2>/dev/null | head -n1)"
if [ -z "$DRIVER_BIN" ] || [ ! -x "$DRIVER_BIN" ]; then
	echo "ERROR: snapperdop_test_driver not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.: cmake --build \"$BUILD_DIR\" --target snapperdop_test_driver" >&2
	exit 66
fi

echo "RUNNER: build dir     = $BUILD_DIR"
echo "RUNNER: service binary = $SERVICE_BIN"
echo "RUNNER: gate service binary = $GATE_SERVICE_BIN"
echo "RUNNER: snapperd driver = $DRIVER_BIN"
echo "RUNNER: repo dir      = $REPO_DIR"

# 名前空間内で実行する内部コマンド
# ログ出力先を一時ルート配下に固定し、サービスを起動する
INNER_CMD='
set -Eeuo pipefail
SERVICE_BIN="$1"
REPO_DIR="$2"
GATE_SERVICE_BIN="$3"
DRIVER_BIN="$4"
mkdir -p "$ISOLATED_DBUS_TMPROOT/fixtures"
export QSNAPPER_TEST_AUTH_LOG="$ISOLATED_DBUS_TMPROOT/fixtures/auth.log"
export QSNAPPER_TEST_APPLY_LOG="$ISOLATED_DBUS_TMPROOT/fixtures/apply.log"
export QSNAPPER_TEST_READY_FILE="$ISOLATED_DBUS_TMPROOT/fixtures/service.ready"
: > "$QSNAPPER_TEST_AUTH_LOG"
: > "$QSNAPPER_TEST_APPLY_LOG"
rm -f "$QSNAPPER_TEST_READY_FILE"
cd "$REPO_DIR"
python3 tests/integration/test_restore_plan_owner_binding.py "$SERVICE_BIN"
rm -f "$QSNAPPER_TEST_READY_FILE"
# 後続のシナリオも実行するため、execを使わず、失敗時はset -eで直ちに打ち切る
python3 tests/integration/test_restore_gate.py "$GATE_SERVICE_BIN"
rm -f "$QSNAPPER_TEST_READY_FILE"
python3 tests/integration/test_settings_timers.py "$GATE_SERVICE_BIN"
rm -f "$QSNAPPER_TEST_READY_FILE"
python3 tests/integration/test_configs_bulk.py "$GATE_SERVICE_BIN"
python3 tests/integration/test_snapperd_operation.py "$DRIVER_BIN"
'

# 隔離ハーネスに内部コマンドとサービス/リポジトリのパスを渡して実行する
exec "$SCRIPT_DIR/isolated_root_dbus_env.sh" bash -c "$INNER_CMD" bash "$SERVICE_BIN" "$REPO_DIR" "$GATE_SERVICE_BIN" "$DRIVER_BIN"
