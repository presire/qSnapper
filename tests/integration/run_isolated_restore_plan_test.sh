#!/usr/bin/env bash
# run_isolated_restore_plan_test.sh - runs the isolated-root D-Bus owner-binding
# security scenario for the staged restore plan API.
#
# Locates qsnapper_restoreplan_test_service under the given build directory,
# then delegates to isolated_root_dbus_env.sh (private root system-like bus,
# no host bus, no sudo) to run test_restore_plan_owner_binding.py as
# namespace root.
#
# Usage:
#   run_isolated_restore_plan_test.sh [build-dir]   (default: build)
#
# Exit status is propagated from the scenario script / harness.
set -Eeuo pipefail

BUILD_DIR_ARG="${1:-build}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [[ "$BUILD_DIR_ARG" = /* ]]; then
	BUILD_DIR="$BUILD_DIR_ARG"
else
	BUILD_DIR="$REPO_DIR/$BUILD_DIR_ARG"
fi

if [ ! -d "$BUILD_DIR" ]; then
	echo "ERROR: build directory not found: $BUILD_DIR" >&2
	exit 66
fi

SERVICE_BIN="$(find "$BUILD_DIR" -maxdepth 6 -type f -name qsnapper_restoreplan_test_service 2>/dev/null | head -n1)"
if [ -z "$SERVICE_BIN" ] || [ ! -x "$SERVICE_BIN" ]; then
	echo "ERROR: qsnapper_restoreplan_test_service not found (or not executable) under $BUILD_DIR" >&2
	echo "       Build it first, e.g.: cmake --build \"$BUILD_DIR\" --target qsnapper_restoreplan_test_service" >&2
	exit 66
fi

echo "RUNNER: build dir     = $BUILD_DIR"
echo "RUNNER: service binary = $SERVICE_BIN"
echo "RUNNER: repo dir      = $REPO_DIR"

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

exec "$SCRIPT_DIR/isolated_root_dbus_env.sh" bash -c "$INNER_CMD" bash "$SERVICE_BIN" "$REPO_DIR"
