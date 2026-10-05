#!/usr/bin/env python3
"""
@file test_restore_plan_owner_binding.py
@brief 隔離したroot D-Bus上での復元計画の所有者拘束を検証する

tests/integration/isolated_root_dbus_env.shが起動する専用root D-Busバス上で、
2番目のD-Bus送信者 (B) が、1番目の送信者 (A) が所有し認可した段階的復元計画を
継続、キャンセル、追加、または状態列挙できないことを証明する
単一認可の保証 (複数チャンクの復元計画でPolkit相当の認可は正確に1回だけ発生し、
実行中やキャンセル時に再プロンプトされない) も証明する
容量超過の凍結前ステージングチャンク (kMaxEntriesPerStageChunkの5000件超) が
InvalidArgsで拒否され、エントリを0件しか保存せず、認可呼び出しも0回であることも証明する

tests/integration/run_isolated_restore_plan_test.sh経由でのみ実行すること
このランナーが、本スクリプトを起動する前にsystemに似た専用バス (namespace root) を起動し、
QSNAPPER_TEST_AUTH_LOG / QSNAPPER_TEST_APPLY_LOG / QSNAPPER_TEST_READY_FILEを設定する

使い方 : python3 test_restore_plan_owner_binding.py <qsnapper_restoreplan_test_serviceのパス>
終了コード : 0 = 全検査成功、1 = 検査失敗、2 = python3-dbus未導入、64 = 引数不正
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

# 終端状態に達した瞬間にサービスがマニフェストを回収 (削除) してしまう可能性があるため、
# ポーリングではなくrestorePlanFinishedシグナルを確実に観測するにはGLibメインループ連携が必要である
# またdbus.SystemBus()を構築する前に設定しなければならない
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
from gi.repository import GLib  # noqa: E402

IFACE = "com.presire.qsnapper.Operations"                 # 検査対象のD-Busインタフェース名
PATH = "/com/presire/qsnapper/Operations"                 # オブジェクトパス
ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"  # 拒否時に期待するエラー名

STAGE_CHUNK_SIZE = 5000                                   # 1チャンク当たりの正常エントリ数
STAGE_CHUNK_COUNT = 3                                     # 用意する正常チャンク数
FAKE_ROOT = "/tmp/qsnapper-fake-restore"                  # テストで使う擬似復元先ルート

_failures = 0                                             # 失敗件数のカウンタ


def proof(label: str, value: object) -> None:
    """
    @brief 観測値をそのままPROOF_*行として出力する

    Args:
        label: PROOF_の後ろに付けるラベル
        value: 出力する観測値
    """
    print(f"PROOF_{label}: {value}")


def fail(what: str) -> None:
    """
    @brief FAIL行を出力して直ちに非ゼロで終了する

    Args:
        what: 失敗内容

    Raises:
        SystemExit: 常に終了コード1で終了する
    """
    print(f"FAIL: {what}", file=sys.stderr)
    sys.exit(1)


def make_paths(chunk_index: int, count: int) -> list[str]:
    """
    @brief chunk_index用の絶対パスをcount件生成する (チャンク間で重複しない)

    Args:
        chunk_index: チャンク番号
        count: 生成するパス数

    Returns:
        チャンク間で重複しない絶対パスのリスト
    """
    base = chunk_index * count
    return [f"{FAKE_ROOT}/f{base + i}" for i in range(count)]


def expect_access_denied(obj, iface, method_name: str, args: list, tag: str):
    """
    @brief method_nameを引数付きで呼び出し、AccessDeniedエラーを必須とする

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        iface: D-Busインタフェース名
        method_name: 呼び出すメソッド名
        args: メソッドへ渡す引数
        tag: PROOFラベルに使う識別子

    Returns:
        期待どおり拒否された場合は (error_name, error_message) の組
        それ以外はfail()でプロセスを終了する
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
    return None, None  # 到達しない (fail()が終了する)


def read_log_lines(path: str) -> list[str]:
    """
    @brief ログファイルの空でない行を返す

    Args:
        path: 読み取るログファイルのパス

    Returns:
        空でない行のリスト、ファイルが無い場合は空リスト
    """
    if not path or not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        return [line for line in handle.read().splitlines() if line]


class FinishedSignalWaiter:
    """
    @class FinishedSignalWaiter
    @brief 引き金となるD-Bus呼び出しの前にrestorePlanFinishedの待ち受けを開始する

    バスデーモンは、ある接続のマッチルールが届く前にブロードキャストされたシグナルを再送しない
    そのためCancelRestorePlan()が戻った後にadd_signal_receiver()を呼ぶと、
    サービスがその呼び出しの内部で同期的に発行したシグナル (メソッド応答を送る前) は
    既にブロードキャスト済みで、この接続では取りこぼされている可能性がある
    マッチを先に登録することでこのraceを塞ぐ

    bus : シグナルを受信するD-Bus接続
    manifest_id : 待ち受ける対象のマニフェストID
    """

    def __init__(self, bus, manifest_id: str):
        """
        @brief 指定したマニフェストIDのrestorePlanFinished待ち受けを準備する

        Args:
            bus: シグナルを受信するD-Bus接続
            manifest_id: 待ち受ける対象のマニフェストID
        """
        self._captured: dict[str, str] = {}          # 受信したterminal_stateとmessageを保持する
        self._loop = GLib.MainLoop()                 # シグナル待ちに使うGLibメインループ
        self._manifest_id = manifest_id              # 待ち受ける対象のマニフェストID
        self._match = bus.add_signal_receiver(       # マッチハンドル (wait()の最後でremoveする)
            self._handler, signal_name="restorePlanFinished",
            dbus_interface=IFACE, path=PATH,
        )

    def _handler(self, mid, terminal_state, message):
        """
        @brief restorePlanFinishedシグナルを受信し、対象IDなら結果を保持して待ちを終える

        Args:
            mid: シグナルが対象とするマニフェストID
            terminal_state: 終端状態
            message: 終端状態に対応する固定文言
        """
        if mid == self._manifest_id and "terminal_state" not in self._captured:
            self._captured["terminal_state"] = terminal_state
            self._captured["message"] = message
            self._loop.quit()

    def _on_timeout(self) -> bool:
        """
        @brief タイムアウト時にメインループを終了する

        Returns:
            False (GLibのタイマーを再登録しない)
        """
        self._loop.quit()
        return False

    def wait(self, timeout_s: float):
        """
        @brief シグナルが発火するまで (上限付きで) 待機する

        Args:
            timeout_s: 待機時間の上限 (秒)

        Returns:
            発火時は (terminal_state, message)、タイムアウト時は (None, None)
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
    """
    @brief サービスがready_fileへREADYを書き込むまで待機する

    Args:
        service: 起動したサービスのプロセス
        ready_file: サービスの準備完了を示すファイルのパス
        timeout_s: 待機時間の上限 (秒)

    Raises:
        SystemExit: サービスが途中終了したか、タイムアウトした場合
    """
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
    """
    @brief 所有者拘束と単一認可の検査シナリオを実行する

    Returns:
        0 = 全検査成功、64 = 引数不正
    """
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

        # --- 手順1: Aが復元計画を開始する ---
        # counterpartSnapshotNumber=0は「現在のシステム」を表す番兵値である
        manifest_id = obj_a.BeginRestorePlan(
            "root", 1, 0, "direct", dbus_interface=IFACE
        )
        proof("BEGIN_MANIFEST_ID", manifest_id)
        if not manifest_id:
            fail("BeginRestorePlan returned an empty manifest id")

        # --- 手順2: 容量超過の凍結前ステージングチャンク (5001件 > kMaxEntriesPerStageChunkの
        # 5000件上限) はInvalidArgsで拒否され、エントリを0件しか保存せず、
        # 認可呼び出しも0回でなければならない
        # 容量ガードは完全に凍結前かつ認可前に動作する ---
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

        # --- 手順3: Aが正常チャンクを複数 (3 x 5000件以上) ステージングする ---
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

        # --- 手順4: BはAの計画へステージングできない ---
        expect_access_denied(
            obj_b, IFACE, "StageRestoreEntries",
            [manifest_id, [f"{FAKE_ROOT}/intruder"], ["modified"]],
            "STAGE_CROSS_OWNER",
        )

        # --- 手順5: Aがコミットする (ここまでで認可は正確に1回) ---
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

        # --- 手順6: 計画は実行中で、Bは全ての制御メソッドで拒否される ---
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

        # --- 手順7: 列挙オラクルの検査 ---
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

        # --- 手順8: Aは引き続き状態を読める
        # totalEntriesは固定で、processedは連続する2回の読み取りで減少しない ---
        first_read = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        time.sleep(0.05)
        second_read = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        proof("OWNER_STATUS_READ_1", first_read)
        proof("OWNER_STATUS_READ_2", second_read)

        def parse_status(csv: str):
            """
            @brief GetRestorePlanStatusのCSV応答をフィールドへ分解する

            Args:
                csv: 状態を表すCSV文字列

            Returns:
                id、state、total、cursor、processedを持つ辞書
            """
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

        # --- 手順9: Aがキャンセルし、finishedシグナルで終端を確認する ---
        # この単一スレッドのサービスでは、executorはCancelRestorePlan内で
        # 同期的に計画を回収する
        # シグナル待ちは、それを発生させる呼び出しより前に準備する必要がある
        # (FinishedSignalWaiterのdocstringを参照)
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
        # finishedシグナルはsystem busの全接続へ届くため、終端状態ごとの
        # 固定文言だけを載せる (pathや詳細は載せない)
        if terminal_message != "Restore cancelled":
            fail(
                "restorePlanFinished must carry only the fixed text for its "
                f"terminal state, got {terminal_message!r}"
            )

        # 詳細は短時間だけ、ownerに限り取得できる
        status_terminal = obj_a.GetRestorePlanStatus(manifest_id, dbus_interface=IFACE)
        proof("OWNER_STATUS_AFTER_TERMINAL", status_terminal)
        terminal_state_field = status_terminal.split(",", 2)[1] if status_terminal else ""
        if terminal_state_field != "cancelled":
            fail(
                "owner could not read the terminal status after the finished "
                f"signal: got {status_terminal!r}"
            )
        terminal_cross_name, terminal_cross_msg = expect_access_denied(
            obj_b, IFACE, "GetRestorePlanStatus", [manifest_id],
            "STATUS_AFTER_TERMINAL_CROSS_OWNER"
        )
        if (terminal_cross_name, terminal_cross_msg) != (oracle_name, oracle_msg):
            fail(
                "GetRestorePlanStatus for a finished plan (wrong owner) differs from "
                "a fabricated id -- an existence oracle is observable: "
                f"{(terminal_cross_name, terminal_cross_msg)!r} vs "
                f"{(oracle_name, oracle_msg)!r}"
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

        # --- 手順10: 終端後の再実行は拒否されなければならない ---
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

        # --- 手順11: 再プロンプト検査 (全体で認可は正確に1回) ---
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
