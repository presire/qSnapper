#!/usr/bin/env bash
# isolated_root_dbus_env.sh - isolated root D-Bus test harness for qSnapper.
#
# Runs an arbitrary command as uid 0 inside a user+mount+PID namespace with a
# PRIVATE dbus-daemon configured as <type>system</type>. The daemon:
#   - listens on a unix socket in a tmpfs dir under a mktemp -d root,
#   - uses EXTERNAL auth only,
#   - includes a COPY of the real repo ACL
#     dbus/com.presire.qsnapper.Operations.conf (deny-by-default, root-only
#     ownership of com.presire.qsnapper.Operations).
#
# The host system bus, /etc, /usr, /var and /run are never touched; no sudo.
#
# Usage:
#   isolated_root_dbus_env.sh <command> [args...]
#   isolated_root_dbus_env.sh --selftest
#
# Environment exported for <command>:
#   DBUS_SYSTEM_BUS_ADDRESS  address of the private bus
#   ISOLATED_DBUS_TMPROOT    the temp root (socket dir, config, fixtures)
set -Eeuo pipefail

SELFTEST=0
if [ "${1:-}" = "--selftest" ]; then
	SELFTEST=1
	shift
fi

if [ "$SELFTEST" -eq 0 ] && [ "$#" -lt 1 ]; then
	echo "usage: $0 [--selftest] <command> [args...]" >&2
	exit 64
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ACL_SRC="$REPO_DIR/dbus/com.presire.qsnapper.Operations.conf"
DBUS_DAEMON="${DBUS_DAEMON:-/usr/bin/dbus-daemon}"

if [ ! -r "$ACL_SRC" ]; then
	echo "ERROR: ACL conf not readable: $ACL_SRC" >&2
	exit 66
fi
if [ ! -x "$DBUS_DAEMON" ]; then
	echo "ERROR: dbus-daemon not found: $DBUS_DAEMON" >&2
	exit 66
fi
if ! unshare --map-root-user true 2>/dev/null; then
	echo "ERROR: 'unshare --map-root-user' failed; user namespaces likely disabled." >&2
	unshare --map-root-user true || true
	exit 77
fi

TMPROOT="$(mktemp -d /tmp/qsnapper-isolated-dbus.XXXXXX)"
mkdir -p "$TMPROOT/run" "$TMPROOT/conf" "$TMPROOT/acl" "$TMPROOT/fixtures"

cleanup() {
	local rc=$?
	# Belt and braces: the namespace teardown already kills the daemon when the
	# inner shell exits, but make sure nothing survives even on early failure.
	pkill -f "dbus-daemon.*$TMPROOT" 2>/dev/null || true
	rm -rf -- "$TMPROOT"
	echo "CLEANUP: killed any dbus-daemon using $TMPROOT; removed temp root $TMPROOT (bus socket tmpfs, generated system.conf, ACL conf copy, fixture scripts)"
	exit "$rc"
}
trap cleanup EXIT INT TERM

# Copy of the REAL per-member ACL so the deny-by-default policy is exercised.
cp -- "$ACL_SRC" "$TMPROOT/acl/com.presire.qsnapper.Operations.conf"
chmod 0444 "$TMPROOT/acl/com.presire.qsnapper.Operations.conf"

SOCKDIR="$TMPROOT/run/socket"
MAIN_CONF="$TMPROOT/conf/system.conf"
cat >"$MAIN_CONF" <<'EOF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<!--
  Generated private "system-like" bus config for the qSnapper isolated test
  harness. Placeholders are substituted by isolated_root_dbus_env.sh.
-->
<busconfig>
  <!-- Behave like a system bus (EXTERNAL auth, no session semantics). -->
  <type>system</type>
  <auth>EXTERNAL</auth>
  <!-- Private socket inside the namespaced tmpfs; never the host bus path. -->
  <listen>unix:tmpdir=@SOCKDIR@</listen>

  <!--
    Standard system-bus baseline policy (mirrors /usr/share/dbus-1/system.conf
    minus host-specific bits: user drop, fork, pidfile, servicedirs, syslog).
    dbus-daemon policy is last-match-wins, so the qSnapper ACL included below
    can tighten exactly what it needs while driver traffic stays functional.
  -->
  <policy context="default">
    <!-- All users can connect to the bus -->
    <allow user="*"/>
    <!-- Holes must be punched in service configuration files for
         name ownership and sending method calls -->
    <deny own="*"/>
    <deny send_type="method_call"/>
    <!-- Signals and reply messages (method returns, errors) are allowed
         by default -->
    <allow send_type="signal"/>
    <allow send_requested_reply="true" send_type="method_return"/>
    <allow send_requested_reply="true" send_type="error"/>
    <!-- All messages may be received by default -->
    <allow receive_type="method_call"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
    <allow receive_type="signal"/>
    <!-- Allow anyone to talk to the message bus itself -->
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Introspectable"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Properties"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Containers1"/>
    <deny send_destination="org.freedesktop.DBus"
          send_interface="org.freedesktop.DBus"
          send_member="UpdateActivationEnvironment"/>
    <deny send_destination="org.freedesktop.DBus"
          send_interface="org.freedesktop.DBus.Debug.Stats"/>
    <deny send_destination="org.freedesktop.DBus"
          send_interface="org.freedesktop.systemd1.Activator"/>
  </policy>

  <!-- root may monitor / stats inside this private bus. -->
  <policy user="root">
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.systemd1.Activator"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Monitoring"/>
    <allow send_destination="org.freedesktop.DBus"
           send_interface="org.freedesktop.DBus.Debug.Stats"/>
  </policy>

  <!-- Real qSnapper ACL: root-only ownership + per-member allowlist.
       Included AFTER the baseline so its rules take precedence. -->
  <include>@ACLFILE@</include>
</busconfig>
EOF
sed -i \
	-e "s|@SOCKDIR@|$SOCKDIR|g" \
	-e "s|@ACLFILE@|$TMPROOT/acl/com.presire.qsnapper.Operations.conf|g" \
	"$MAIN_CONF"
chmod 0444 "$MAIN_CONF"

# Inner shell: executes INSIDE the user/mount/PID namespace as mapped uid 0.
INNER="$TMPROOT/inner.sh"
cat >"$INNER" <<'EOF'
#!/usr/bin/env bash
# inner.sh <TMPROOT> <command> [args...] - run inside the namespace.
set -Eeuo pipefail
TMPROOT="$1"
shift
RUNDIR="$TMPROOT/run"
SOCKDIR="$RUNDIR/socket"

# Isolate mounts: nothing we do below may leak to (or come from) the host view.
mount --make-rprivate /
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$RUNDIR"
mkdir -p "$SOCKDIR"

ADDR_FILE="$RUNDIR/bus.address"
PID_FILE="$RUNDIR/bus.pid"
exec 10>"$ADDR_FILE" 11>"$PID_FILE"
/usr/bin/dbus-daemon \
	--config-file="$TMPROOT/conf/system.conf" \
	--nofork --nopidfile --nosyslog \
	--print-address=10 --print-pid=11 &
BUSPID=$!
exec 10>&- 11>&-

for _ in $(seq 1 200); do
	if ! kill -0 "$BUSPID" 2>/dev/null; then
		echo "ERROR: private dbus-daemon exited during startup" >&2
		exit 1
	fi
	[ -s "$ADDR_FILE" ] && break
	sleep 0.05
done
if [ ! -s "$ADDR_FILE" ]; then
	echo "ERROR: timeout waiting for private bus address" >&2
	exit 1
fi

DBUS_SYSTEM_BUS_ADDRESS="$(cat "$ADDR_FILE")"
export DBUS_SYSTEM_BUS_ADDRESS
export ISOLATED_DBUS_TMPROOT="$TMPROOT"
# Safety interlock: never let a test reach the HOST system bus by accident
# (qSnapper may be installed there). Abort unless the address points into
# this run's temp root.
case "$DBUS_SYSTEM_BUS_ADDRESS" in
	*"$TMPROOT"*) ;;
	*) echo "ERROR: refusing to run: DBUS_SYSTEM_BUS_ADDRESS does not point into $TMPROOT" >&2
		exit 1 ;;
esac
unset DBUS_SESSION_BUS_ADDRESS 2>/dev/null || true
echo "ISOLATED_DAEMON_PID: $BUSPID"
echo "ISOLATED_BUS_ADDRESS: $DBUS_SYSTEM_BUS_ADDRESS"

rc=0
"$@" || rc=$?
kill "$BUSPID" 2>/dev/null || true
wait "$BUSPID" 2>/dev/null || true
exit "$rc"
EOF
chmod 0700 "$INNER"

if [ "$SELFTEST" -eq 1 ]; then
	# Throwaway service owning com.presire.qsnapper.Operations as uid 0.
	cat >"$TMPROOT/fixtures/fixture_service.py" <<'EOF'
#!/usr/bin/env python3
"""Throwaway D-Bus service for the harness self-test.

Owns com.presire.qsnapper.Operations (allowed only for uid 0 by the ACL)
and serves two members: ListConfigs (allowlisted) and NotAllowedMethod
(NOT in the bus-level allowlist).
"""
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop

DBusGMainLoop(set_as_default=True)
from gi.repository import GLib  # noqa: E402

IFACE = "com.presire.qsnapper.Operations"
PATH = "/com/presire/qsnapper/Operations"


class Operations(dbus.service.Object):
	@dbus.service.method(IFACE, out_signature="as")
	def ListConfigs(self):
		return []

	@dbus.service.method(IFACE, out_signature="s")
	def NotAllowedMethod(self):
		# Must never be reachable: the bus ACL denies it before delivery.
		return "ACL-BYPASS"


loop = GLib.MainLoop()
conn = dbus.SystemBus()
print("SVC_ADDR:", conn.get_unique_name(), flush=True)
bn = dbus.service.BusName(IFACE, conn)
print("SVC_OWN_CHECK:", conn.name_has_owner(IFACE), flush=True)
Operations(conn, PATH)
print("SERVICE_READY", flush=True)
loop.run()
EOF

	# Self-test client: proves uid 0, name ownership, allowed call,
	# AccessDenied on non-allowlisted member, and two distinct unique names.
	cat >"$TMPROOT/fixtures/selftest.py" <<'EOF'
#!/usr/bin/env python3
"""Harness self-test: captures the four required observables."""
import os
import re
import subprocess
import sys
import time

import dbus
from dbus.exceptions import DBusException

TMPROOT = sys.argv[1]
IFACE = "com.presire.qsnapper.Operations"
PATH = "/com/presire/qsnapper/Operations"
WELL_KNOWN = IFACE


def fail(code, msg):
	print(f"SELFTEST_FAIL: {msg}", file=sys.stderr)
	sys.exit(code)


uid = os.getuid()
print(f"PROOF_UID0: {uid}")
if uid != 0:
	fail(3, f"expected uid 0 inside namespace, got {uid}")

bus_a = dbus.SystemBus(private=True)
name_a = bus_a.get_unique_name()

svc = subprocess.Popen([sys.executable, os.path.join(TMPROOT, "fixtures/fixture_service.py")])
try:
	deadline = time.time() + 20
	owned = False
	first_poll_note = None
	while time.time() < deadline:
		if svc.poll() is not None:
			fail(7, f"service exited early rc={svc.returncode}")
		try:
			owned = bool(bus_a.name_has_owner(WELL_KNOWN))
			if first_poll_note is None:
				first_poll_note = f"first poll owned={owned}"
		except DBusException as e:
			owned = False
			if first_poll_note is None:
				first_poll_note = f"first poll raised {e.get_dbus_name()}: {e}"
		if owned:
			break
		time.sleep(0.1)
	print(f"SELFTEST_POLL_NOTE: {first_poll_note}")
	if not owned:
		try:
			out = subprocess.run(
				["busctl", "--address", os.environ["DBUS_SYSTEM_BUS_ADDRESS"],
				 "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
				 "org.freedesktop.DBus", "NameHasOwner", "s", WELL_KNOWN],
				capture_output=True, text=True, timeout=10)
			print(f"SELFTEST_BUSCTL_CROSSCHECK: rc={out.returncode} out={out.stdout.strip()} err={out.stderr.strip()}")
			svc_out = subprocess.run(
				[sys.executable, "-c",
				 "import dbus; b=dbus.SystemBus(); print(b.get_unique_name(), "
				 "b.name_has_owner('com.presire.qsnapper.Operations'))"],
				capture_output=True, text=True, timeout=10)
			print(f"SELFTEST_FRESH_CLIENT: {svc_out.stdout.strip()} {svc_out.stderr.strip()}")
		except Exception as e:
			print(f"SELFTEST_CROSSCHECK_EXC: {e!r}")
		fail(7, f"service never acquired {WELL_KNOWN}")

	msg = dbus.lowlevel.MethodCallMessage(
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixUser")
	msg.append(WELL_KNOWN)
	reply = bus_a.send_message_with_reply_and_block(msg)
	owner_uid = int(reply.get_args_list()[0])
	print(f"PROOF_NAME_OWNER_OK: {WELL_KNOWN} owned by uid {owner_uid} "
		  f"(ACL grants own only to root)")
	if owner_uid != 0:
		fail(4, f"name owner uid is {owner_uid}, expected 0")

	obj_a = bus_a.get_object(WELL_KNOWN, PATH)
	res = obj_a.ListConfigs(dbus_interface=IFACE)
	print(f"PROOF_ALLOWED_CALL_OK: ListConfigs -> {list(res)!r}")

	try:
		obj_a.NotAllowedMethod(dbus_interface=IFACE)
		fail(5, "NotAllowedMethod unexpectedly succeeded; ACL not enforced")
	except DBusException as e:
		err_name = e.get_dbus_name()
		err_text = str(e)
		print(f"PROOF_DENIED_CALL_ERROR_NAME: {err_name}")
		print(f"PROOF_DENIED_CALL_MESSAGE: {err_text}")
		if err_name != "org.freedesktop.DBus.Error.AccessDenied":
			fail(5, f"expected org.freedesktop.DBus.Error.AccessDenied, got {err_name}")

	bus_b = dbus.SystemBus(private=True)
	name_b = bus_b.get_unique_name()
	distinct = name_a != name_b
	valid = all(re.fullmatch(r":1\.\d+", n) for n in (name_a, name_b))
	print(f"PROOF_UNIQUE_NAMES: senderA={name_a} senderB={name_b} "
		  f"distinct={'yes' if distinct else 'NO'}")
	if not distinct or not valid:
		fail(6, f"unique names invalid or identical: {name_a} vs {name_b}")

	print("HARNESS_PROOF: ALL_OK")
finally:
	svc.terminate()
	try:
		svc.wait(timeout=5)
	except subprocess.TimeoutExpired:
		svc.kill()
		svc.wait()
EOF
	RUN_CMD=(python3 "$TMPROOT/fixtures/selftest.py" "$TMPROOT")
else
	RUN_CMD=("$@")
fi

echo "HARNESS_TMPROOT: $TMPROOT"
unshare --map-root-user --mount --pid --fork --mount-proc bash "$INNER" "$TMPROOT" "${RUN_CMD[@]}"
