#!/usr/bin/env python3
"""test_restore_plan_owner_binding.py - isolated root D-Bus security scenario.

Proves that a second D-Bus sender (B) cannot continue, cancel, stage into, or
enumerate the state of a staged restore plan owned and authorized by a first
sender (A), on the private root D-Bus bus started by
tests/integration/isolated_root_dbus_env.sh. Also proves the single-
authorization guarantee: exactly one Polkit-equivalent authorization occurs
for a multi-chunk restore plan, with no re-prompt during execution/cancel.
Also proves that an over-capacity pre-freeze staging chunk (> the 5000-entry
kMaxEntriesPerStageChunk limit) is rejected with InvalidArgs, stores ZERO
entries, and triggers ZERO authorization calls.

Run only via tests/integration/run_isolated_restore_plan_test.sh, which starts
the private system-like bus (namespace root) before invoking this script and
sets QSNAPPER_TEST_AUTH_LOG / QSNAPPER_TEST_APPLY_LOG / QSNAPPER_TEST_READY_FILE.

Usage:
    python3 test_restore_plan_owner_binding.py <path-to-qsnapper_restoreplan_test_service>
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

try:
    import dbus
    import dbus.mainloop.glib
    from dbus.exceptions import DBusException
except ImportError:
    print("ERROR: python3-dbus required", file=sys.stderr)
    sys.exit(2)

# GLib mainloop integration is required so restorePlanFinished signals can be
# observed deterministically instead of polling a manifest that may already
# have been reaped (removed) by the service the instant it reaches a terminal
# state. Must be set BEFORE any dbus.SystemBus() is constructed.
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
from gi.repository import GLib  # noqa: E402

IFACE = "com.presire.qsnapper.Operations"
PATH = "/com/presire/qsnapper/Operations"
ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"

STAGE_CHUNK_SIZE = 5000
STAGE_CHUNK_COUNT = 3
FAKE_ROOT = "/tmp/qsnapper-fake-restore"

_failures = 0


def proof(label: str, value: object) -> None:
    """Print one PROOF_* line with a verbatim observable."""
    print(f"PROOF_{label}: {value}")


def fail(what: str) -> None:
    """Print a FAIL line and exit non-zero immediately."""
    print(f"FAIL: {what}", file=sys.stderr)
    sys.exit(1)


def make_paths(chunk_index: int, count: int) -> list[str]:
    """Build count absolute paths for chunk_index, unique across chunks."""
    base = chunk_index * count
    return [f"{FAKE_ROOT}/f{base + i}" for i in range(count)]


def expect_access_denied(obj, iface, method_name: str, args: list, tag: str):
    """Call method_name(*args) and require an AccessDenied D-Bus error.

    Returns (error_name, error_message) on success (i.e. the call WAS denied
    as expected); exits the process via fail() otherwise.
    """
    method = getattr(obj, method_name)
    try:
        result = method(*args, dbus_interface=iface)
        fail(f"{tag}: {method_name} unexpectedly succeeded -> {result!r}")
    except DBusException as exc:
        name = exc.get_dbus_name()
        text = exc.get_dbus_message()
        proof(f"{tag}_ERROR_NAME", name)
        proof(f"{tag}_ERROR_MESSAGE", text)
        if name != ACCESS_DENIED:
            fail(f"{tag}: expected {ACCESS_DENIED}, got {name}: {text}")
        return name, text
    return None, None  # unreachable; fail() exits


def read_log_lines(path: str) -> list[str]:
    """Return non-empty lines of a log file, or an empty list if absent."""
    if not path or not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        return [line for line in handle.read().splitlines() if line]


class FinishedSignalWaiter:
    """Arms a restorePlanFinished match BEFORE the triggering D-Bus call is
    made.

    The bus daemon never replays a signal broadcast before a connection's
    match rule reached it: if add_signal_receiver() is only called AFTER
    e.g. CancelRestorePlan() has already returned, the signal (which the
    service emits synchronously while still inside that same call, before
    sending the method reply) can already have been broadcast and dropped
    for this connection. Registering the match first closes that race.
    """

    def __init__(self, bus, manifest_id: str):
        self._captured: dict[str, str] = {}
        self._loop = GLib.MainLoop()
        self._manifest_id = manifest_id
        self._match = bus.add_signal_receiver(
            self._handler, signal_name="restorePlanFinished",
            dbus_interface=IFACE, path=PATH,
        )

    def _handler(self, mid, terminal_state, message):
        if mid == self._manifest_id and "terminal_state" not in self._captured:
            self._captured["terminal_state"] = terminal_state
            self._captured["message"] = message
            self._loop.quit()

    def _on_timeout(self) -> bool:
        self._loop.quit()
        return False

    def wait(self, timeout_s: float):
        """Block (bounded) until the signal fires, then return
        (terminal_state, message), or (None, None) on timeout.
        """
        if "terminal_state" not in self._captured:
            timeout_id = GLib.timeout_add(int(timeout_s * 1000), self._on_timeout)
            try:
                self._loop.run()
            finally:
                GLib.source_remove(timeout_id)
        self._match.remove()
        return self._captured.get("terminal_state"), self._captured.get("message")


def wait_for_ready(service: subprocess.Popen, ready_file: str, timeout_s: float = 20.0) -> None:
    """Block until the service writes READY to ready_file, or fail loudly."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if service.poll() is not None:
            fail(f"service exited early during startup, rc={service.returncode}")
        if os.path.exists(ready_file):
            try:
                with open(ready_file, "r", encoding="utf-8") as handle:
                    if handle.read().strip() == "READY":
                        return
            except OSError:
                pass
        time.sleep(0.05)
    fail(f"timed out waiting for {ready_file} (service never became ready)")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <service-binary-path>", file=sys.stderr)
        return 64

    service_binary = sys.argv[1]
    if not os.path.isfile(service_binary) or not os.access(service_binary, os.X_OK):
        fail(f"service binary not found or not executable: {service_binary}")

    auth_log = os.environ.get("QSNAPPER_TEST_AUTH_LOG", "")
    apply_log = os.environ.get("QSNAPPER_TEST_APPLY_LOG", "")
    ready_file = os.environ.get("QSNAPPER_TEST_READY_FILE", "")
    if not auth_log or not apply_log or not ready_file:
        fail(
            "QSNAPPER_TEST_AUTH_LOG / QSNAPPER_TEST_APPLY_LOG / "
            "QSNAPPER_TEST_READY_FILE must all be set by the runner"
        )

    for path in (auth_log, apply_log, ready_file):
        try:
            if os.path.exists(path):
                os.remove(path)
        except OSError:
            pass

    service = subprocess.Popen([service_binary])
    try:
        wait_for_ready(service, ready_file)
        print("SETUP: service is READY")

        bus_a = dbus.SystemBus(private=True)
        bus_b = dbus.SystemBus(private=True)
        name_a = bus_a.get_unique_name()
        name_b = bus_b.get_unique_name()
        proof("SENDER_NAMES", f"A={name_a} B={name_b}")
        if name_a == name_b:
            fail(f"sender unique names are identical: {name_a}")

        obj_a = bus_a.get_object(IFACE, PATH)
        obj_b = bus_b.get_object(IFACE, PATH)

        # --- Step 1: A begins a restore plan ---
        manifest_id = obj_a.BeginRestorePlan(
            "root", 1, "direct", dbus_interface=IFACE
        )
        proof("BEGIN_MANIFEST_ID", manifest_id)
        if not manifest_id:
            fail("BeginRestorePlan returned an empty manifest id")

        # --- Step 2: an over-capacity pre-freeze staging chunk (5001 > the
        # 5000-entry kMaxEntriesPerStageChunk limit) must be rejected with
        # InvalidArgs, store ZERO entries, and trigger ZERO authorization
        # calls (the capacity guard runs entirely pre-freeze/pre-auth) ---
        auth_lines_before_any_staging = read_log_lines(auth_log)
        if len(auth_lines_before_any_staging) != 0:
            fail(
                "auth log must have ZERO lines before any staging attempt, "
                f"got {auth_lines_before_any_staging}"
            )
        oversized_size = STAGE_CHUNK_SIZE + 1
        oversized_paths = [f"{FAKE_ROOT}/oversized/f{i}" for i in range(oversized_size)]
        oversized_change_types = ["modified"] * oversized_size
        try:
            obj_a.StageRestoreEntries(
                manifest_id, oversized_paths, oversized_change_types,
                dbus_interface=IFACE,
            )
            fail("oversized StageRestoreEntries chunk unexpectedly succeeded")
        except DBusException as exc:
            oversized_name = exc.get_dbus_name()
            oversized_msg = exc.get_dbus_message()
            proof("OVERSIZED_STAGE_ERROR_NAME", oversized_name)
            proof("OVERSIZED_STAGE_ERROR_MESSAGE", oversized_msg)
            if oversized_name != "org.freedesktop.DBus.Error.InvalidArgs":
                fail(
                    "oversized StageRestoreEntries chunk: expected "
                    f"org.freedesktop.DBus.Error.InvalidArgs, got {oversized_name}: "
                    f"{oversized_msg}"
                )

        status_after_oversized = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        stored_entries = int(status_after_oversized.split(",")[2])
        proof("OVERSIZED_STAGE_STORED_ENTRIES", stored_entries)
        if stored_entries != 0:
            fail(
                "over-capacity chunk must store ZERO entries, "
                f"totalEntries={stored_entries} (CSV={status_after_oversized!r})"
            )

        auth_lines_after_oversized = read_log_lines(auth_log)
        proof("OVERSIZED_STAGE_AUTH_CALLS", len(auth_lines_after_oversized))
        if len(auth_lines_after_oversized) != 0:
            fail(
                "over-capacity chunk must trigger ZERO authorization calls, "
                f"auth log={auth_lines_after_oversized}"
            )

        # --- Step 3: A stages more than one GOOD chunk (>= 3 x 5000 entries) ---
        total_staged = 0
        for chunk_index in range(STAGE_CHUNK_COUNT):
            paths = make_paths(chunk_index, STAGE_CHUNK_SIZE)
            change_types = ["modified"] * STAGE_CHUNK_SIZE
            ok = obj_a.StageRestoreEntries(
                manifest_id, paths, change_types, dbus_interface=IFACE
            )
            if not ok:
                fail(f"StageRestoreEntries chunk {chunk_index} returned False")
            total_staged += STAGE_CHUNK_SIZE
        proof("TOTAL_STAGED", total_staged)

        # --- Step 4: B cannot stage into A's plan ---
        expect_access_denied(
            obj_b, IFACE, "StageRestoreEntries",
            [manifest_id, [f"{FAKE_ROOT}/intruder"], ["modified"]],
            "STAGE_CROSS_OWNER",
        )

        # --- Step 5: A commits; exactly one authorization so far ---
        committed = obj_a.CommitRestorePlan(manifest_id, dbus_interface=IFACE)
        proof("COMMIT_RESULT", committed)
        if not committed:
            fail("CommitRestorePlan returned False")
        auth_lines_after_commit = read_log_lines(auth_log)
        proof("AUTH_LOG_AFTER_COMMIT", auth_lines_after_commit)
        if len(auth_lines_after_commit) != 1:
            fail(
                "auth log must contain EXACTLY ONE line after CommitRestorePlan, "
                f"got {len(auth_lines_after_commit)}: {auth_lines_after_commit}"
            )

        # --- Step 6: plan is Running; B is denied on every control method ---
        status_running = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        proof("STATUS_AFTER_COMMIT", status_running)
        state_field = status_running.split(",", 2)[1] if status_running else ""
        if state_field not in ("running", "completed"):
            fail(f"expected running/completed state right after commit, got {state_field!r}")

        continue_name, continue_msg = expect_access_denied(
            obj_b, IFACE, "ContinueRestorePlan", [manifest_id], "CONTINUE_CROSS_OWNER"
        )
        cancel_name, cancel_msg = expect_access_denied(
            obj_b, IFACE, "CancelRestorePlan", [manifest_id], "CANCEL_CROSS_OWNER"
        )
        status_name, status_msg = expect_access_denied(
            obj_b, IFACE, "GetRestorePlanStatus", [manifest_id], "STATUS_CROSS_OWNER"
        )

        # --- Step 7: enumeration-oracle check ---
        fabricated_id = "rm-00000000000000000000000000000000"
        oracle_name, oracle_msg = expect_access_denied(
            obj_b, IFACE, "GetRestorePlanStatus", [fabricated_id], "STATUS_FABRICATED_ID"
        )
        proof(
            "ENUMERATION_ORACLE_COMPARISON",
            f"real_id=({status_name!r},{status_msg!r}) "
            f"fabricated_id=({oracle_name!r},{oracle_msg!r})",
        )
        if (oracle_name, oracle_msg) != (status_name, status_msg):
            fail(
                "GetRestorePlanStatus error for a real id (wrong owner) differs from "
                "a fabricated id (not found) -- an existence oracle is observable: "
                f"{(status_name, status_msg)!r} vs {(oracle_name, oracle_msg)!r}"
            )

        # --- Step 8: A can still read status; totalEntries fixed, processed
        # non-decreasing across two consecutive reads ---
        first_read = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        time.sleep(0.05)
        second_read = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        proof("OWNER_STATUS_READ_1", first_read)
        proof("OWNER_STATUS_READ_2", second_read)

        def parse_status(csv: str):
            fields = csv.split(",")
            return {
                "id": fields[0],
                "state": fields[1],
                "total": int(fields[2]),
                "cursor": int(fields[3]),
                "processed": int(fields[4]),
            }

        parsed_1 = parse_status(first_read)
        parsed_2 = parse_status(second_read)
        if parsed_1["id"] != manifest_id or parsed_2["id"] != manifest_id:
            fail(f"CSV id field mismatch: {parsed_1['id']!r} / {parsed_2['id']!r}")
        if parsed_1["total"] != total_staged or parsed_2["total"] != total_staged:
            fail(
                f"totalEntries must stay {total_staged}, got "
                f"{parsed_1['total']} then {parsed_2['total']}"
            )
        if parsed_2["processed"] < parsed_1["processed"]:
            fail(
                "processed count went backwards: "
                f"{parsed_1['processed']} -> {parsed_2['processed']}"
            )
        proof(
            "PROGRESS_MONOTONIC",
            f"processed {parsed_1['processed']} -> {parsed_2['processed']} "
            f"(total={total_staged})",
        )

        # --- Step 9: A cancels; observe termination via the finished signal
        # (GetRestorePlanStatus becomes AccessDenied the instant the plan is
        # reaped, which the executor does synchronously within
        # CancelRestorePlan in this single-threaded service). The signal
        # match MUST be armed before the call that triggers it (see
        # FinishedSignalWaiter docstring). ---
        finished_waiter = FinishedSignalWaiter(bus_a, manifest_id)
        applied_before_cancel = len(read_log_lines(apply_log))
        cancelled = obj_a.CancelRestorePlan(manifest_id, dbus_interface=IFACE)
        proof("CANCEL_RESULT", cancelled)
        if not cancelled:
            fail("CancelRestorePlan returned False")

        terminal_state, terminal_message = finished_waiter.wait(timeout_s=30.0)
        proof("TERMINAL_SIGNAL", f"state={terminal_state!r} message={terminal_message!r}")
        if terminal_state != "cancelled":
            fail(
                "timed out or wrong terminal state waiting for restorePlanFinished "
                f"after CancelRestorePlan: got {terminal_state!r}"
            )

        time.sleep(0.2)
        applied_after_cancel = len(read_log_lines(apply_log))
        proof(
            "APPLY_LOG_STOPS_GROWING",
            f"before_cancel={applied_before_cancel} after_cancel_settle={applied_after_cancel}",
        )
        time.sleep(0.2)
        applied_settled = len(read_log_lines(apply_log))
        if applied_settled != applied_after_cancel:
            fail(
                "apply log kept growing after cancellation: "
                f"{applied_after_cancel} -> {applied_settled}"
            )

        # --- Step 10: replay after terminal must be rejected ---
        try:
            obj_a.ContinueRestorePlan(manifest_id, dbus_interface=IFACE)
            fail("ContinueRestorePlan after terminal unexpectedly succeeded")
        except DBusException as exc:
            replay_name = exc.get_dbus_name()
            replay_msg = exc.get_dbus_message()
            proof("REPLAY_AFTER_TERMINAL_ERROR_NAME", replay_name)
            proof("REPLAY_AFTER_TERMINAL_ERROR_MESSAGE", replay_msg)
            failed_ok = (
                replay_name == "org.freedesktop.DBus.Error.Failed"
                and replay_msg == "Restore plan is already terminal"
            )
            denied_ok = replay_name == ACCESS_DENIED
            if not (failed_ok or denied_ok):
                fail(
                    "replay after terminal returned an unexpected error: "
                    f"{replay_name}: {replay_msg}"
                )

        # --- Step 11: re-prompt check -- exactly one authorization overall ---
        final_auth_lines = read_log_lines(auth_log)
        proof("AUTH_LOG_FINAL", final_auth_lines)
        if len(final_auth_lines) != 1:
            fail(
                "auth log must still contain EXACTLY ONE line at the very end "
                f"(no re-prompt), got {len(final_auth_lines)}: {final_auth_lines}"
            )

        print("RESTORE_PLAN_OWNER_BINDING: ALL_OK")
        return 0
    finally:
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
            service.wait()


if __name__ == "__main__":
    sys.exit(main())
