#!/usr/bin/env python3
"""
@file test_restore_gate.py
@brief 復元の実行中に非復元操作を拒否する共通ゲートを、隔離したroot D-Bus上で検証する

tests/integration/isolated_root_dbus_env.shが起動する専用root D-Busバス上で、
本番のSnapshotOperationsをリンクしたqsnapper_restoregate_test_serviceを起動し、次を証明する
    - 復元の実行中は、別configと同じconfigのListSnapshots、WriteSnapperConfig、SetupQuota、
      DeleteSnapshot、RollbackSnapshotが、即時許可の経路でRestoreInProgressとして拒否される
    - 認可待ちの間に復元が始まった場合も、本体を実行せずに拒否される
    - 拒否するときのエラー本文は固定の文言で、config名を含まない
    - ListConfigsと復元計画のメソッドは、ゲートの対象外である
    - 復元の完了後は、各操作が再び本体まで到達する

config名には実在しない名前だけを使う
ゲートが働かずに本体へ到達しても、Snapperの初期化で失敗して実システムに触れないようにするため
本体へ到達したかどうかは、エラーがRestoreInProgress以外で、かつ復元前と同じであることで判定する

tests/integration/run_isolated_restore_plan_test.sh経由でのみ実行すること
このランナーが、本スクリプトを起動する前に専用バスを起動し、QSNAPPER_TEST_READY_FILEを設定する

使い方 : python3 test_restore_gate.py <qsnapper_restoregate_test_serviceのパス>
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

# 遅延認可の経路では呼び出しを非同期に発行し、応答をメインループで受け取る必要がある
# dbus.SystemBus()を構築する前に設定しなければならない
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
from gi.repository import GLib  # noqa: E402

IFACE = "com.presire.qsnapper.Operations"                         # 検査対象のD-Busインタフェース名
PATH = "/com/presire/qsnapper/Operations"                         # オブジェクトパス
RESTORE_IN_PROGRESS = "com.presire.qsnapper.Error.RestoreInProgress"  # ゲートが返すエラー名
RESTORE_IN_PROGRESS_TEXT = "A restore is in progress"             # ゲートが返す固定のエラー本文

RESTORING_CONFIG = "qsnapper-gate-test-restoring"                 # 復元中として記録する実在しないconfig名
OTHER_CONFIG = "qsnapper-gate-test-other"                         # 復元対象とは別の実在しないconfig名
CALL_TIMEOUT_S = 10.0                                             # 1回の非同期呼び出しを待つ上限 (秒)


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


class ServiceControl:
    """
    @brief テスト用サービスの標準入出力を介して、認可の結果と復元中の状態を切り替える

    本番のACLはOperations以外の宛先への送信を許さないため、制御にD-Busは使わない
    """

    def __init__(self, service: subprocess.Popen):
        """
        @brief 起動済みのサービスを制御対象として保持する

        Args:
            service: stdinとstdoutをパイプにして起動したサービス
        """
        self._service = service  # 制御対象のサービス

    def send(self, command: str) -> str:
        """
        @brief コマンドを1行送り、応答を1行受け取る

        Args:
            command: 改行を含まないコマンド

        Returns:
            改行を除いた応答

        Raises:
            SystemExit: サービスが応答せずに終了した場合
        """
        stdin = self._service.stdin
        stdout = self._service.stdout
        if stdin is None or stdout is None:
            fail("service was started without stdin/stdout pipes")
            return ""
        stdin.write(command + "\n")
        stdin.flush()
        line = stdout.readline()
        if not line:
            fail(f"service closed stdout while handling '{command}'")
        return line.rstrip("\n")

    def expect_ok(self, command: str) -> None:
        """
        @brief コマンドを送り、OKが返ることを必須とする

        Args:
            command: 改行を含まないコマンド
        """
        response = self.send(command)
        if response != "OK":
            fail(f"control command '{command}' returned '{response}'")

    def pending(self) -> int:
        """
        @brief 保留中の認可の件数を取得する

        Returns:
            保留中の件数
        """
        response = self.send("pending")
        if not response.startswith("PENDING "):
            fail(f"unexpected pending response '{response}'")
        return int(response.split(" ", 1)[1])


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


def call(obj, method_name: str, args: list) -> tuple[str, str]:
    """
    @brief メソッドを同期的に呼び出し、結果をエラー名と本文の組で返す

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        method_name: 呼び出すメソッド名
        args: メソッドへ渡す引数

    Returns:
        成功した場合は ("OK", 戻り値の文字列)、失敗した場合は (エラー名, エラー本文)
    """
    try:
        result = getattr(obj, method_name)(*args, dbus_interface=IFACE)
        return "OK", repr(result)
    except DBusException as exc:
        return exc.get_dbus_name() or "", exc.get_dbus_message() or ""


def call_deferred(obj, control: ServiceControl, method_name: str, args: list,
                  while_pending: list[str], resolution: str) -> tuple[str, str]:
    """
    @brief 認可待ちの間に状態を変えてから認可を確定し、その呼び出しの結果を返す

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
        method_name: 呼び出すメソッド名
        args: メソッドへ渡す引数
        while_pending: 認可待ちの間に送る制御コマンド
        resolution: 認可の確定方法 (grant / deny)

    Returns:
        成功した場合は ("OK", 戻り値の文字列)、失敗した場合は (エラー名, エラー本文)

    Raises:
        SystemExit: 認可待ちに入らないか、応答がタイムアウトした場合
    """
    loop = GLib.MainLoop()
    outcome: list[tuple[str, str]] = []

    def on_reply(*values):
        """
        @brief 成功の応答を記録してメインループを抜ける

        Args:
            values: メソッドの戻り値
        """
        outcome.append(("OK", repr(values)))
        loop.quit()

    def on_error(exc):
        """
        @brief エラーの応答を記録してメインループを抜ける

        Args:
            exc: D-Busのエラー
        """
        outcome.append((exc.get_dbus_name() or "", exc.get_dbus_message() or ""))
        loop.quit()

    control.expect_ok("auth defer")
    getattr(obj, method_name)(*args, dbus_interface=IFACE,
                              reply_handler=on_reply, error_handler=on_error,
                              timeout=CALL_TIMEOUT_S)

    # 呼び出しがサービスへ届き、認可待ちに入るまで待つ
    deadline = time.time() + CALL_TIMEOUT_S
    while control.pending() != 1:
        if time.time() > deadline:
            fail(f"{method_name} never reached the deferred authorization state")
        GLib.MainContext.default().iteration(False)
        time.sleep(0.02)

    for command in while_pending:
        control.expect_ok(command)
    control.expect_ok(f"resolve {resolution}")

    GLib.timeout_add(int(CALL_TIMEOUT_S * 1000), loop.quit)
    loop.run()
    control.expect_ok("auth grant")
    if not outcome:
        fail(f"{method_name}: no reply after the deferred authorization was resolved")
    return outcome[0]


def expect_restore_in_progress(result: tuple[str, str], tag: str) -> None:
    """
    @brief ゲートによる拒否であり、本文が固定の文言でconfig名を含まないことを必須とする

    Args:
        result: callまたはcall_deferredの結果
        tag: PROOFラベルに使う識別子
    """
    name, text = result
    proof(f"{tag}_ERROR_NAME", name)
    proof(f"{tag}_ERROR_MESSAGE", text)
    if name != RESTORE_IN_PROGRESS:
        fail(f"{tag}: expected {RESTORE_IN_PROGRESS}, got {name}: {text}")
    if text != RESTORE_IN_PROGRESS_TEXT:
        fail(f"{tag}: error text is not the fixed message: {text!r}")
    for config in (RESTORING_CONFIG, OTHER_CONFIG):
        if config in text:
            fail(f"{tag}: error text leaks a config name: {text!r}")


def expect_reached_body(result: tuple[str, str], baseline: tuple[str, str], tag: str) -> None:
    """
    @brief ゲートを通過して本体まで到達したことを必須とする

    実在しないconfig名を使うため本体は失敗するが、その結果は復元の実行前と一致する

    Args:
        result: 検査する呼び出しの結果
        baseline: 復元の実行前に同じ呼び出しで得た結果
        tag: PROOFラベルに使う識別子
    """
    proof(f"{tag}_RESULT", result)
    if result[0] == RESTORE_IN_PROGRESS:
        fail(f"{tag}: still rejected by the restore gate")
    if result != baseline:
        fail(f"{tag}: result {result} differs from the pre-restore baseline {baseline}")


def main() -> int:
    """
    @brief 共通ゲートの検査シナリオを実行する

    Returns:
        0 = 全検査成功、64 = 引数不正
    """
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <service-binary-path>", file=sys.stderr)
        return 64

    service_binary = sys.argv[1]
    if not os.path.isfile(service_binary) or not os.access(service_binary, os.X_OK):
        fail(f"service binary not found or not executable: {service_binary}")

    ready_file = os.environ.get("QSNAPPER_TEST_READY_FILE", "")
    if not ready_file:
        fail("QSNAPPER_TEST_READY_FILE must be set by the runner")
    if os.path.exists(ready_file):
        os.remove(ready_file)

    service = subprocess.Popen([service_binary], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    try:
        wait_for_ready(service, ready_file)
        print("SETUP: restore gate service is READY")
        control = ServiceControl(service)

        bus = dbus.SystemBus(private=True)
        obj = bus.get_object(IFACE, PATH)

        # ゲートの対象となる操作 (実在しないconfig名で、本体まで到達しても実システムに触れない)
        gated_calls = {
            "LIST_OTHER": ("ListSnapshots", [OTHER_CONFIG]),
            "LIST_SAME": ("ListSnapshots", [RESTORING_CONFIG]),
            "WRITE_CONFIG": ("WriteSnapperConfig",
                             [RESTORING_CONFIG,
                              dbus.Dictionary({"TIMELINE_CREATE": "no"}, signature="ss")]),
            "SETUP_QUOTA": ("SetupQuota", [RESTORING_CONFIG]),
            "DELETE": ("DeleteSnapshot", [RESTORING_CONFIG, 1]),
            "ROLLBACK": ("RollbackSnapshot", [RESTORING_CONFIG, 1]),
        }

        # --- 手順1: 復元の実行前の結果を記録する (本体まで到達していることの基準) ---
        baselines = {}
        for tag, (method, args) in gated_calls.items():
            baselines[tag] = call(obj, method, args)
            proof(f"BASELINE_{tag}", baselines[tag])
            if baselines[tag][0] == RESTORE_IN_PROGRESS:
                fail(f"BASELINE_{tag}: rejected by the restore gate before any restore began")

        # --- 手順2: 復元の実行中は、即時許可の経路で全て拒否される ---
        control.expect_ok(f"restore begin {RESTORING_CONFIG}")
        for tag, (method, args) in gated_calls.items():
            expect_restore_in_progress(call(obj, method, args), f"DURING_{tag}")

        # --- 手順3: ゲートの対象外の操作は、復元の実行中も拒否されない ---
        configs = call(obj, "ListConfigs", [])
        proof("DURING_LIST_CONFIGS", configs)
        if configs[0] != "OK":
            fail(f"ListConfigs must stay available during a restore, got {configs}")
        cancel = call(obj, "CancelRestorePlan", ["00000000-0000-0000-0000-000000000000"])
        proof("DURING_CANCEL_RESTORE_PLAN", cancel)
        if cancel[0] == RESTORE_IN_PROGRESS:
            fail("restore plan methods must not be gated by the non-restore gate")
        control.expect_ok("restore end")

        # --- 手順4: 認可待ちの間に復元が始まった場合、本体を実行せずに拒否される ---
        for tag in ("LIST_OTHER", "WRITE_CONFIG", "DELETE", "ROLLBACK"):
            method, args = gated_calls[tag]
            result = call_deferred(obj, control, method, args,
                                   [f"restore begin {RESTORING_CONFIG}"], "grant")
            expect_restore_in_progress(result, f"DEFERRED_{tag}")
            control.expect_ok("restore end")

        # 対照: 復元が始まらなければ、遅延許可の経路でも本体まで到達する
        method, args = gated_calls["SETUP_QUOTA"]
        expect_reached_body(call_deferred(obj, control, method, args, [], "grant"),
                            baselines["SETUP_QUOTA"], "DEFERRED_CONTROL_SETUP_QUOTA")

        # --- 手順5: 復元の完了後は、各操作が再び本体まで到達する ---
        for tag, (method, args) in gated_calls.items():
            expect_reached_body(call(obj, method, args), baselines[tag], f"AFTER_{tag}")

        print("RESTORE_GATE: ALL_OK")
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
