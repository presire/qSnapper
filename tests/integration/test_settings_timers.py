#!/usr/bin/env python3
"""
@file test_settings_timers.py
@brief 設定の読み書きとsnapperタイマー操作の特権メソッドを、隔離したroot D-Bus上で検証する

tests/integration/isolated_root_dbus_env.shが起動する専用root D-Busバス上で、
本番のSnapshotOperationsをリンクしたqsnapper_restoregate_test_serviceと、
偽のsystemd (fake_systemd1.py) を起動し、次の4つのメソッドを検査する
    - GetConfig        : 入力検証は認可の前、認可拒否、復元中の拒否、本文に情報を含まない失敗
    - WriteSnapperConfig : 許可リスト外のキーと不正な値の拒否は認可の前、復元中の拒否、本文に情報を含まない失敗
    - GetTimerStates   : 3つの固定unitの状態の取得、systemdのエラー本文を転送しないこと、不正な状態の拒否
    - SetTimerEnabled  : 許可リスト外のunit名の拒否は認可の前、有効化と無効化の呼び出し順序、
                         ジョブ失敗、JobRemovedの先着、無応答のときの応答性と排他、復元の開始による中断、
                         照会の実行中でも変更を拒否しないこと (照会と変更は別の排他枠で数える)

config名には実在しない名前だけを使う
WriteSnapperConfigが本体に到達しても、存在しない設定は新規作成されず、/etc/snapper/configsへ書き込まない
実在するconfig (root など) は、どの呼び出しにも使わない
systemdは偽物だけを相手にし、実システムのsystemdには触れない

tests/integration/run_isolated_restore_plan_test.sh経由でのみ実行すること
このランナーが、本スクリプトを起動する前に専用バスを起動し、QSNAPPER_TEST_READY_FILEを設定する

使い方 : python3 test_settings_timers.py <qsnapper_restoregate_test_serviceのパス>
終了コード : 0 = 全検査成功、1 = 検査失敗、2 = python3-dbus未導入、64 = 引数不正
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time

# 同じディレクトリの共通部品 (サービス制御、PROOF出力、遅延認可の呼び出し) を再利用する
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_restore_gate import (  # noqa: E402
    CALL_TIMEOUT_S, IFACE, PATH, RESTORE_IN_PROGRESS, RESTORE_IN_PROGRESS_TEXT,
    ServiceControl, call_deferred, fail, proof, wait_for_ready,
)

import dbus  # noqa: E402
from dbus.exceptions import DBusException  # noqa: E402
from gi.repository import GLib  # noqa: E402

FAKE_NAME = "org.freedesktop.systemd1"                            # 偽のsystemdが所有するバス名
FAKE_CONTROL_PATH = "/com/presire/qsnapper/Test/FakeSystemd"      # 偽のsystemdの制御オブジェクト
FAKE_CONTROL_IFACE = "com.presire.qsnapper.Test.FakeSystemd"      # 偽のsystemdの制御インタフェース
FAKE_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_systemd1.py")  # 偽のsystemd

ERR_INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"       # 入力検証の失敗
ERR_FAILED = "org.freedesktop.DBus.Error.Failed"                  # 本体の失敗
ERR_ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"     # 認可の拒否
ERR_LIMITS = "org.freedesktop.DBus.Error.LimitsExceeded"          # タイマー操作の排他

NONEXISTENT_CONFIG = "qsnapper-nonexistent-test"                  # 実在しないconfig名
RESTORE_CONFIG = "x"                                              # 復元中として記録するconfig名
TIMELINE = "snapper-timeline.timer"                               # 許可リストのunit (timeline)
CLEANUP = "snapper-cleanup.timer"                                 # 許可リストのunit (cleanup)
BOOT = "snapper-boot.timer"                                       # 許可リストのunit (boot)
ALLOWED_UNITS = [TIMELINE, CLEANUP, BOOT]                         # ヘルパーが操作してよいunit
LEAK_MARKER = "LEAK-/etc/secret-snapper-timeline.timer"           # 偽のsystemdが埋め込む秘密の目印
MUTATING = {"EnableUnitFiles", "DisableUnitFiles", "StartUnit", "StopUnit", "Reload"}  # systemdを変更する呼び出し
SHORT_TIMEOUT_S = 3.0                                             # 認可の前に拒否されるはずの呼び出しの待ち上限
LONG_TIMEOUT_S = 40.0                                             # タイマー変更の全体期限 (30秒) を超える待ち上限

# どのエラー本文にも含めてはならない文字列 (unit名、config名、値、パス、秘密の目印)
FORBIDDEN_IN_TEXT = ["snapper-", ".timer", "LEAK", "/", "nonexistent", "etc", "secret",
                     "TIMELINE", "NUMBER", "ALLOW_USERS", "SUBVOLUME", "FSTYPE", "SPACE_LIMIT"]


def check_error(tag: str, result: tuple, name: str, text: str) -> None:
    """
    @brief エラー名と本文が期待と完全に一致し、禁止語を含まないことを必須とする

    Args:
        tag: PROOFラベルに使う識別子
        result: rpcまたはcall_deferredの結果 (エラー名, 本文)
        name: 期待するエラー名
        text: 期待する本文

    Raises:
        SystemExit: 一致しない場合、または本文に禁止語が含まれる場合
    """
    got_name, got_text = result[0], result[1]
    proof(f"{tag}_ERROR_NAME", got_name)
    proof(f"{tag}_ERROR_MESSAGE", got_text)
    if got_name != name:
        fail(f"{tag}: expected {name}, got {got_name}: {got_text!r}")
    if got_text != text:
        fail(f"{tag}: expected text {text!r}, got {got_text!r}")
    for word in FORBIDDEN_IN_TEXT:
        if word in got_text:
            fail(f"{tag}: error text contains forbidden word {word!r}: {got_text!r}")


def check_restore_in_progress(tag: str, result: tuple) -> None:
    """
    @brief 復元の実行中を示す固定のエラーであることを必須とする

    Args:
        tag: PROOFラベルに使う識別子
        result: rpcまたはcall_deferredの結果
    """
    check_error(tag, result, RESTORE_IN_PROGRESS, RESTORE_IN_PROGRESS_TEXT)


def rpc(obj, method: str, args: list, timeout: float = CALL_TIMEOUT_S) -> tuple:
    """
    @brief メソッドを同期的に呼び出し、結果を (名前, 値または本文) の組で返す

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        method: メソッド名
        args: 引数
        timeout: 応答を待つ上限 (秒)

    Returns:
        成功した場合は ("OK", 戻り値そのもの)、失敗した場合は (エラー名, エラー本文)
    """
    try:
        value = getattr(obj, method)(*args, dbus_interface=IFACE, timeout=timeout)
        return "OK", value
    except DBusException as exc:
        return exc.get_dbus_name() or "", exc.get_dbus_message() or ""


def pump(seconds: float) -> None:
    """
    @brief メインループを一定時間回して、非同期の応答を受け取る

    Args:
        seconds: 回す時間 (秒)
    """
    deadline = time.time() + seconds
    context = GLib.MainContext.default()
    while time.time() < deadline:
        while context.iteration(False):
            pass
        time.sleep(0.02)


class AsyncCall:
    """
    @brief 非同期に発行した呼び出しの応答を集める

    応答の回数を数え、1回の呼び出しに応答が複数返らないことを確かめられるようにする
    """

    def __init__(self, obj, method: str, args: list, timeout: float):
        """
        @brief 呼び出しを非同期に発行する

        Args:
            obj: 呼び出し対象のD-Busオブジェクト
            method: メソッド名
            args: 引数
            timeout: クライアント側の待ち上限 (秒)
        """
        self.replies: list[tuple] = []  # 受け取った応答 (回数の検査に使う)
        getattr(obj, method)(*args, dbus_interface=IFACE, reply_handler=self._on_reply,
                             error_handler=self._on_error, timeout=timeout)

    def _on_reply(self, *values) -> None:
        """
        @brief 成功の応答を記録する

        Args:
            values: メソッドの戻り値
        """
        self.replies.append(("OK", values))

    def _on_error(self, exc) -> None:
        """
        @brief エラーの応答を記録する

        Args:
            exc: D-Busのエラー
        """
        self.replies.append((exc.get_dbus_name() or "", exc.get_dbus_message() or ""))

    def wait(self, timeout_s: float) -> bool:
        """
        @brief 応答が届くまでメインループを回す

        Args:
            timeout_s: 待ち上限 (秒)

        Returns:
            応答が届いたらTrue
        """
        deadline = time.time() + timeout_s
        while not self.replies and time.time() < deadline:
            pump(0.05)
        return bool(self.replies)


class FakeSystemd:
    """
    @brief 偽のsystemdの子プロセスと、その制御インタフェースを扱う

    実際のsystemdには接続せず、隔離バス上の偽物だけを相手にする
    """

    def __init__(self, bus):
        """
        @brief 偽のsystemdを起動し、バス名の取得を待って制御用のプロキシを作る

        Args:
            bus: 隔離バスへの接続

        Raises:
            SystemExit: 起動に失敗した場合
        """
        self._process = subprocess.Popen([sys.executable, FAKE_SCRIPT], stdout=subprocess.PIPE,
                                         text=True)  # 偽のsystemdの子プロセス
        stdout = self._process.stdout
        line = stdout.readline() if stdout is not None else ""
        if line.strip() != "READY":
            fail(f"fake systemd did not become ready: {line!r}, rc={self._process.poll()}")
        proxy = bus.get_object(FAKE_NAME, FAKE_CONTROL_PATH, introspect=False)
        self._control = dbus.Interface(proxy, FAKE_CONTROL_IFACE)  # 制御インタフェース

    def set(self, key: str, value: str = "1") -> None:
        """
        @brief 振る舞いを1件設定する

        Args:
            key: 振る舞いのキー
            value: 値
        """
        self._control.SetBehavior(key, value)

    def reset(self) -> None:
        """
        @brief 記録と振る舞いを初期状態へ戻す
        """
        self._control.Reset()

    def calls(self) -> list[tuple[str, list]]:
        """
        @brief 受け取った呼び出しを受信順に返す

        Returns:
            (メソッド名, 引数のリスト) の配列
        """
        parsed = []
        for entry in self._control.GetCalls():
            method, _, args = str(entry).partition("\t")
            parsed.append((method, json.loads(args)))
        return parsed

    def names(self) -> list[str]:
        """
        @brief 受け取った呼び出しのメソッド名だけを受信順に返す

        Returns:
            メソッド名の配列
        """
        return [method for method, _ in self.calls()]

    def mutating(self) -> list[str]:
        """
        @brief systemdの状態を変える呼び出しのメソッド名だけを受信順に返す

        Returns:
            メソッド名の配列
        """
        return [name for name in self.names() if name in MUTATING]

    def wait_for_call(self, method: str, timeout_s: float = 5.0) -> None:
        """
        @brief 指定のメソッドが偽のsystemdへ届くまで待つ

        Args:
            method: 待つメソッド名
            timeout_s: 待ち上限 (秒)

        Raises:
            SystemExit: 届かなかった場合
        """
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if method in self.names():
                return
            pump(0.05)
        fail(f"fake systemd never received {method}")

    def stop(self) -> None:
        """
        @brief 子プロセスを終了させる
        """
        self._process.terminate()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()


def settings(values: dict) -> dbus.Dictionary:
    """
    @brief a{ss}として送る辞書を作る

    Args:
        values: 設定のキーと値

    Returns:
        シグネチャを ss に固定した辞書

    Note:
        空の辞書はdbus-pythonが型を推測できないため、シグネチャを明示する
    """
    return dbus.Dictionary(values, signature="ss")


def unit_object_path(unit: str) -> str:
    """
    @brief systemdと同じ方式でunit名からオブジェクトパスを求める

    Args:
        unit: unit名

    Returns:
        オブジェクトパス
    """
    escaped = "".join(ch if ch.isalnum() else "_%02x" % ord(ch) for ch in unit)
    return "/org/freedesktop/systemd1/unit/" + escaped


def states_to_native(value) -> dict:
    """
    @brief GetTimerStatesの戻り値を素のPythonの辞書へ変換する

    Args:
        value: a{s(ss)}の戻り値

    Returns:
        unit名から (unitFileState, activeState) への辞書
    """
    return {str(key): (str(item[0]), str(item[1])) for key, item in value.items()}


def check_ok_true(tag: str, result: tuple) -> None:
    """
    @brief 成功し、戻り値がTrueであることを必須とする

    Args:
        tag: PROOFラベルに使う識別子
        result: rpcの結果
    """
    proof(f"{tag}_RESULT", (result[0], bool(result[1]) if result[0] == "OK" else result[1]))
    if result[0] != "OK" or not result[1]:
        fail(f"{tag}: expected a successful True reply, got {result!r}")


def check_names_only_allowlisted(tag: str, calls: list, unit: str) -> None:
    """
    @brief systemdへ渡されたunit名が、許可リストの定数と完全に一致することを必須とする

    Args:
        tag: PROOFラベルに使う識別子
        calls: FakeSystemd.calls()の結果
        unit: 操作対象として許可されたunit名
    """
    for method, args in calls:
        if method in ("GetUnitFileState", "GetUnit", "StartUnit", "StopUnit"):
            seen = args[0]
        elif method in ("EnableUnitFiles", "DisableUnitFiles"):
            if len(args[0]) != 1:
                fail(f"{tag}: {method} received more than one unit: {args[0]!r}")
            seen = args[0][0]
        elif method == "Properties.Get":
            if args[0] != unit_object_path(unit):
                fail(f"{tag}: Properties.Get was sent to {args[0]!r}")
            continue
        else:
            continue
        if seen != unit or seen not in ALLOWED_UNITS:
            fail(f"{tag}: {method} received unit {seen!r}, expected {unit!r}")
    proof(f"{tag}_UNIT_NAMES", f"all systemd calls carried only {unit}")


def reject_before_auth(obj, control: ServiceControl, method: str, cases: list,
                       expected_text: str, tag: str, fake: FakeSystemd | None) -> None:
    """
    @brief 不正な入力が認可の前に拒否され、認可の要求が1件も発生しないことを検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御
        method: 検査するメソッド名
        cases: (ラベル, 引数のリスト) の配列
        expected_text: 期待するエラー本文
        tag: PROOFラベルに使う識別子
        fake: 偽のsystemd、指定した場合は呼び出しが1件もないことも確かめる

    Note:
        認可を保留にしておくことで、認可へ到達した呼び出しは応答せずに止まり、保留数が増える
        これにより、入力検証が認可より前に行われることを保留数0で証明する
    """
    control.expect_ok("auth defer")
    try:
        for label, args in cases:
            result = rpc(obj, method, args, SHORT_TIMEOUT_S)
            pending = control.pending()
            proof(f"{tag}_{label}", f"{result[0]} | {result[1]} | pending={pending}")
            if pending != 0:
                fail(f"{tag}_{label}: the call reached authorization (pending={pending})")
            check_error(f"{tag}_{label}", result, ERR_INVALID_ARGS, expected_text)
    finally:
        control.expect_ok("auth grant")
    if fake is not None and fake.calls():
        fail(f"{tag}: fake systemd received calls: {fake.calls()!r}")


def test_get_config(obj, control: ServiceControl) -> None:
    """
    @brief GetConfigの入力検証、認可、復元ゲート、失敗時の本文を検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御
    """
    names = [("NEWLINE", "bad\nname"), ("DOTDOT", "../etc"), ("SLASH", "a/b"), ("LEADING_DASH", "-bad")]
    reject_before_auth(obj, control, "GetConfig", [(label, [value]) for label, value in names],
                       "Invalid configName", "GETCONFIG_INVALID_NAME", None)

    result = call_deferred(obj, control, "GetConfig", [NONEXISTENT_CONFIG], [], "deny")
    check_error("GETCONFIG_AUTH_DENIED", result, ERR_ACCESS_DENIED, "Authorization failed")

    control.expect_ok(f"restore begin {RESTORE_CONFIG}")
    check_restore_in_progress("GETCONFIG_RESTORE_BEFORE", rpc(obj, "GetConfig", [NONEXISTENT_CONFIG]))
    control.expect_ok("restore end")

    result = call_deferred(obj, control, "GetConfig", [NONEXISTENT_CONFIG],
                           [f"restore begin {RESTORE_CONFIG}"], "grant")
    control.expect_ok("restore end")
    check_restore_in_progress("GETCONFIG_RESTORE_DURING_AUTH", result)

    result = rpc(obj, "GetConfig", [NONEXISTENT_CONFIG])
    check_error("GETCONFIG_NONEXISTENT", result, ERR_FAILED, "Failed to read config")


def test_write_config(obj, control: ServiceControl) -> None:
    """
    @brief WriteSnapperConfigの入力検証、復元ゲート、失敗時の本文を検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御

    Note:
        実在するconfigへ書き込むと /etc/snapper/configs を変更するため、実在しない名前だけを使う
    """
    invalid = [
        ("EMPTY_DICT", {}),
        ("KEY_ALLOW_USERS", {"ALLOW_USERS": "root"}),
        ("KEY_SUBVOLUME", {"SUBVOLUME": "/"}),
        ("KEY_FSTYPE", {"FSTYPE": "btrfs"}),
        ("KEY_UNKNOWN", {"NO_SUCH_KEY": "yes"}),
        ("VALUE_TRAILING_NEWLINE", {"TIMELINE_CREATE": "yes\n"}),
        ("VALUE_INJECTED_LINE", {"NUMBER_CLEANUP": "no\nALLOW_USERS=\"x\""}),
        ("VALUE_DOUBLE_QUOTE", {"NUMBER_CLEANUP": "\"yes"}),
        ("VALUE_SINGLE_QUOTE", {"NUMBER_CLEANUP": "'yes"}),
        ("VALUE_CONTROL_CHAR", {"NUMBER_CLEANUP": "yes\x01"}),
        ("VALUE_OUT_OF_RANGE", {"NUMBER_LIMIT": "1000000000"}),
        ("VALUE_REVERSED_RANGE", {"NUMBER_LIMIT": "10-2"}),
        ("VALUE_FRACTION_OVER_ONE", {"SPACE_LIMIT": "1.5"}),
    ]
    cases = [(label, [NONEXISTENT_CONFIG, settings(values)]) for label, values in invalid]
    reject_before_auth(obj, control, "WriteSnapperConfig", cases, "Invalid config settings",
                       "WRITECONFIG_INVALID_SETTINGS", None)

    valid = settings({"TIMELINE_CREATE": "no"})
    control.expect_ok(f"restore begin {RESTORE_CONFIG}")
    check_restore_in_progress("WRITECONFIG_RESTORE_BEFORE",
                              rpc(obj, "WriteSnapperConfig", [NONEXISTENT_CONFIG, valid]))
    control.expect_ok("restore end")

    result = call_deferred(obj, control, "WriteSnapperConfig", [NONEXISTENT_CONFIG, valid],
                           [f"restore begin {RESTORE_CONFIG}"], "grant")
    control.expect_ok("restore end")
    check_restore_in_progress("WRITECONFIG_RESTORE_DURING_AUTH", result)

    result = rpc(obj, "WriteSnapperConfig", [NONEXISTENT_CONFIG, valid])
    check_error("WRITECONFIG_NONEXISTENT", result, ERR_FAILED, "Failed to write config")


def test_get_timer_states(obj, control: ServiceControl, fake: FakeSystemd) -> None:
    """
    @brief GetTimerStatesの正常系、欠落、エラー本文の非転送、不正な状態、復元ゲートを検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御
        fake: 偽のsystemd
    """
    fake.reset()
    fake.set(f"state.{TIMELINE}", "enabled,active")
    fake.set(f"state.{CLEANUP}", "disabled,inactive")
    fake.set(f"state.{BOOT}", "masked,inactive")
    result = rpc(obj, "GetTimerStates", [])
    if result[0] != "OK":
        fail(f"GETTIMERSTATES_NORMAL: unexpected error {result!r}")
    states = states_to_native(result[1])
    proof("GETTIMERSTATES_NORMAL", json.dumps(states, sort_keys=True))
    expected = {TIMELINE: ("enabled", "active"), CLEANUP: ("disabled", "inactive"),
                BOOT: ("masked", "inactive")}
    if states != expected:
        fail(f"GETTIMERSTATES_NORMAL: expected {expected!r}, got {states!r}")
    if fake.mutating():
        fail(f"GETTIMERSTATES_NORMAL: a query must not mutate systemd: {fake.mutating()!r}")
    queried = {args[0] for method, args in fake.calls() if method == "GetUnitFileState"}
    if queried != set(ALLOWED_UNITS):
        fail(f"GETTIMERSTATES_NORMAL: queried units {queried!r}")
    proof("GETTIMERSTATES_QUERIED_UNITS", sorted(queried))

    fake.reset()
    fake.set(f"missing.{CLEANUP}")
    result = rpc(obj, "GetTimerStates", [])
    states = states_to_native(result[1]) if result[0] == "OK" else {}
    proof("GETTIMERSTATES_MISSING", json.dumps(states, sort_keys=True))
    if states.get(CLEANUP) != ("not-found", "unknown"):
        fail(f"GETTIMERSTATES_MISSING: got {result!r}")

    fake.reset()
    fake.set("error.GetUnitFileState", f"{ERR_FAILED}|{LEAK_MARKER}")
    result = rpc(obj, "GetTimerStates", [])
    check_error("GETTIMERSTATES_ERROR_BODY_NOT_FORWARDED", result, ERR_FAILED,
                "Failed to query timer state")

    for label, raw in (("NEWLINE_TOKEN", "Enabled\n,inactive"), ("SPACE_TOKEN", "x y,active")):
        fake.reset()
        fake.set(f"state.{TIMELINE}", raw)
        result = rpc(obj, "GetTimerStates", [])
        check_error(f"GETTIMERSTATES_INVALID_STATE_{label}", result, ERR_FAILED,
                    "Failed to query timer state")

    fake.reset()
    control.expect_ok(f"restore begin {RESTORE_CONFIG}")
    check_restore_in_progress("GETTIMERSTATES_RESTORE_BEFORE", rpc(obj, "GetTimerStates", []))
    control.expect_ok("restore end")
    result = call_deferred(obj, control, "GetTimerStates", [], [f"restore begin {RESTORE_CONFIG}"], "grant")
    control.expect_ok("restore end")
    check_restore_in_progress("GETTIMERSTATES_RESTORE_DURING_AUTH", result)
    if fake.calls():
        fail(f"GETTIMERSTATES_RESTORE: fake systemd received calls: {fake.calls()!r}")
    proof("GETTIMERSTATES_RESTORE_FAKE_CALLS", 0)


def test_set_timer_basic(obj, control: ServiceControl, fake: FakeSystemd) -> None:
    """
    @brief SetTimerEnabledの入力検証、復元ゲート、有効化、無効化、失敗、JobRemoved先着を検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御
        fake: 偽のsystemd
    """
    fake.reset()
    invalid = [
        ("SERVICE_UNIT", "sshd.service"),
        ("TIMELINE_SERVICE", "snapper-timeline.service"),
        ("TEMPLATE_UNIT", "snapper-timeline@root.timer"),
        ("ABSOLUTE_PATH", "/usr/lib/systemd/system/snapper-timeline.timer"),
        ("PARENT_PATH", "../snapper-timeline.timer"),
        ("TRAILING_NEWLINE", "snapper-timeline.timer\n"),
        ("UPPER_CASE", "SNAPPER-TIMELINE.TIMER"),
        ("EMPTY", ""),
        ("WILDCARD", "snapper-*.timer"),
    ]
    reject_before_auth(obj, control, "SetTimerEnabled", [(label, [unit, True]) for label, unit in invalid],
                       "Invalid timer unit", "SETTIMER_INVALID_UNIT", fake)

    result = call_deferred(obj, control, "SetTimerEnabled", [TIMELINE, True],
                           [f"restore begin {RESTORE_CONFIG}"], "grant")
    control.expect_ok("restore end")
    check_restore_in_progress("SETTIMER_RESTORE_DURING_AUTH", result)
    if fake.mutating():
        fail(f"SETTIMER_RESTORE_DURING_AUTH: mutating calls reached systemd: {fake.mutating()!r}")
    proof("SETTIMER_RESTORE_DURING_AUTH_MUTATING_CALLS", 0)

    control.expect_ok(f"restore begin {RESTORE_CONFIG}")
    check_restore_in_progress("SETTIMER_RESTORE_BEFORE", rpc(obj, "SetTimerEnabled", [TIMELINE, True]))
    control.expect_ok("restore end")
    if fake.calls():
        fail(f"SETTIMER_RESTORE_BEFORE: fake systemd received calls: {fake.calls()!r}")

    fake.reset()
    began = time.time()
    result = rpc(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    proof("SETTIMER_ENABLE_SECONDS", f"{time.time() - began:.2f}")
    calls = fake.calls()
    proof("SETTIMER_ENABLE_FAKE_CALLS", json.dumps(calls))
    check_ok_true("SETTIMER_ENABLE", result)
    proof("SETTIMER_ENABLE_MUTATING_ORDER", fake.mutating())
    if fake.mutating() != ["EnableUnitFiles", "Reload", "StartUnit"]:
        fail(f"SETTIMER_ENABLE: unexpected mutating order {fake.mutating()!r}")
    by_name = {method: args for method, args in calls if method in MUTATING}
    if by_name["EnableUnitFiles"] != [[TIMELINE], False, False]:
        fail(f"SETTIMER_ENABLE: EnableUnitFiles args {by_name['EnableUnitFiles']!r}")
    if by_name["StartUnit"] != [TIMELINE, "replace"]:
        fail(f"SETTIMER_ENABLE: StartUnit args {by_name['StartUnit']!r}")
    proof("SETTIMER_ENABLE_ARGS", f"EnableUnitFiles={by_name['EnableUnitFiles']} StartUnit={by_name['StartUnit']}")
    check_names_only_allowlisted("SETTIMER_ENABLE", calls, TIMELINE)

    fake.reset()
    fake.set(f"state.{CLEANUP}", "enabled,active")
    result = rpc(obj, "SetTimerEnabled", [CLEANUP, False], LONG_TIMEOUT_S)
    check_ok_true("SETTIMER_DISABLE", result)
    proof("SETTIMER_DISABLE_MUTATING_ORDER", fake.mutating())
    if fake.mutating() != ["StopUnit", "DisableUnitFiles", "Reload"]:
        fail(f"SETTIMER_DISABLE: unexpected mutating order {fake.mutating()!r}")
    check_names_only_allowlisted("SETTIMER_DISABLE", fake.calls(), CLEANUP)

    fake.reset()
    fake.set("job_result", "failed")
    result = rpc(obj, "SetTimerEnabled", [BOOT, True], LONG_TIMEOUT_S)
    check_error("SETTIMER_JOB_FAILED", result, ERR_FAILED, "Failed to change the timer state")

    fake.reset()
    fake.set("jobremoved_early")
    result = rpc(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    check_ok_true("SETTIMER_JOBREMOVED_BEFORE_REPLY", result)

    fake.reset()
    fake.set("masked." + TIMELINE)
    result = rpc(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    check_error("SETTIMER_MASKED_ENABLE", result, ERR_FAILED, "Failed to change the timer state")

    fake.reset()
    fake.set("error.EnableUnitFiles", f"{ERR_FAILED}|{LEAK_MARKER}")
    result = rpc(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    check_error("SETTIMER_ERROR_BODY_NOT_FORWARDED", result, ERR_FAILED, "Failed to change the timer state")
    if "StartUnit" in fake.mutating() or "Reload" in fake.mutating():
        fail(f"SETTIMER_ERROR_BODY_NOT_FORWARDED: continued after a failed Enable: {fake.mutating()!r}")


def test_set_timer_in_flight(obj, control: ServiceControl, fake: FakeSystemd) -> None:
    """
    @brief systemdが応答しない間の応答性と排他、および実行中に復元が始まった場合の中断を検査する

    Args:
        obj: ヘルパーのD-Busオブジェクト
        control: テスト用サービスの制御
        fake: 偽のsystemd
    """
    fake.reset()
    fake.set("silent.EnableUnitFiles")
    started = time.time()
    first = AsyncCall(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    fake.wait_for_call("EnableUnitFiles")

    begin = time.time()
    result = rpc(obj, "GetConfig", [NONEXISTENT_CONFIG], SHORT_TIMEOUT_S)
    elapsed = time.time() - begin
    proof("SETTIMER_INFLIGHT_OTHER_REQUEST_SECONDS", f"{elapsed:.2f}")
    check_error("SETTIMER_INFLIGHT_OTHER_REQUEST", result, ERR_FAILED, "Failed to read config")
    if elapsed > 2.0 or first.replies:
        fail(f"SETTIMER_INFLIGHT_OTHER_REQUEST: helper was blocked (elapsed={elapsed:.2f}s, "
             f"first replied={bool(first.replies)})")

    check_error("SETTIMER_INFLIGHT_SECOND_SET", rpc(obj, "SetTimerEnabled", [CLEANUP, False], SHORT_TIMEOUT_S),
                ERR_LIMITS, "Another timer operation is in progress")
    # 照会は変更とは別の排他枠のため、変更の応答待ちの間も状態を読み取れる
    result = rpc(obj, "GetTimerStates", [], LONG_TIMEOUT_S)
    proof("SETTIMER_INFLIGHT_GET_STATES", result[0])
    if result[0] != "OK":
        fail(f"SETTIMER_INFLIGHT_GET_STATES: a query was rejected during a change: {result!r}")

    if not first.wait(25.0):
        fail("SETTIMER_INFLIGHT: the in-flight call never received a reply")
    proof("SETTIMER_INFLIGHT_REPLY_SECONDS", f"{time.time() - started:.2f}")
    pump(0.5)
    if len(first.replies) != 1:
        fail(f"SETTIMER_INFLIGHT: expected exactly one reply, got {first.replies!r}")
    check_error("SETTIMER_INFLIGHT_RESULT", first.replies[0], ERR_FAILED, "Failed to change the timer state")
    proof("SETTIMER_INFLIGHT_REPLY_COUNT", len(first.replies))
    if "Reload" in fake.names() or "StartUnit" in fake.names():
        fail(f"SETTIMER_INFLIGHT: continued after the timed-out call: {fake.names()!r}")

    fake.reset()
    result = rpc(obj, "GetTimerStates", [])
    proof("SETTIMER_INFLIGHT_AFTER_RELEASE", result[0])
    if result[0] != "OK":
        fail(f"SETTIMER_INFLIGHT: the timer lock was not released: {result!r}")

    fake.reset()
    fake.set("delay.EnableUnitFiles", "1000")
    call = AsyncCall(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    fake.wait_for_call("EnableUnitFiles")
    control.expect_ok(f"restore begin {RESTORE_CONFIG}")
    try:
        if not call.wait(25.0):
            fail("SETTIMER_RESTORE_DURING_ENABLE: no reply")
        pump(0.5)
    finally:
        control.expect_ok("restore end")
    if len(call.replies) != 1:
        fail(f"SETTIMER_RESTORE_DURING_ENABLE: expected exactly one reply, got {call.replies!r}")
    check_restore_in_progress("SETTIMER_RESTORE_DURING_ENABLE", call.replies[0])
    proof("SETTIMER_RESTORE_DURING_ENABLE_MUTATING_ORDER", fake.mutating())
    if fake.mutating() != ["EnableUnitFiles"]:
        fail(f"SETTIMER_RESTORE_DURING_ENABLE: calls continued after the restore began: {fake.mutating()!r}")


def test_query_in_flight_does_not_block_change(obj, fake: FakeSystemd) -> None:
    """
    @brief systemdが照会に応答しない間も、timerの変更が拒否されないことを検査する

    GetTimerStatesはパスワードなしで呼べるため、照会が変更と同じ排他枠を占めると、
    照会を繰り返すだけで管理者のSetTimerEnabledを拒否させられる
    照会どうしは同時に1件に限ることも合わせて確かめる

    Args:
        obj: ヘルパーのD-Busオブジェクト
        fake: 偽のsystemd
    """
    fake.reset()
    fake.set("silent.GetUnitFileState")
    query = AsyncCall(obj, "GetTimerStates", [], LONG_TIMEOUT_S)
    fake.wait_for_call("GetUnitFileState")

    check_error("QUERY_INFLIGHT_SECOND_QUERY", rpc(obj, "GetTimerStates", [], SHORT_TIMEOUT_S),
                ERR_LIMITS, "Another timer operation is in progress")

    # 応答しない照会を残したまま、変更の側は通常どおり応答させる
    fake.reset()
    result = rpc(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    check_ok_true("QUERY_INFLIGHT_CHANGE", result)
    proof("QUERY_INFLIGHT_CHANGE_MUTATING_ORDER", fake.mutating())
    if fake.mutating() != ["EnableUnitFiles", "Reload", "StartUnit"]:
        fail(f"QUERY_INFLIGHT_CHANGE: unexpected mutating order {fake.mutating()!r}")

    if not query.wait(25.0):
        fail("QUERY_INFLIGHT: the silent query never received a reply")
    pump(0.5)
    if len(query.replies) != 1:
        fail(f"QUERY_INFLIGHT: expected exactly one reply, got {query.replies!r}")
    proof("QUERY_INFLIGHT_REPLY", query.replies[0][0])

    fake.reset()
    result = rpc(obj, "GetTimerStates", [])
    proof("QUERY_INFLIGHT_AFTER_RELEASE", result[0])
    if result[0] != "OK":
        fail(f"QUERY_INFLIGHT: the query slot was not released: {result!r}")


def test_restore_commit_during_timer_job(obj, fake: FakeSystemd) -> None:
    """
    @brief systemdへ送信済みのtimerの変更の完了を待つ間は、復元の開始 (CommitRestorePlan) が拒否されることを検査する

    timer側の確認は変更の直前ごとにしか行えず、送信済みのStartUnitは取り消せないため、
    復元の開始側が実行中のtimerの変更を見て拒否することを確かめる
    計画が無いIDでも、timerの判定は計画の検索より前に行われるため、拒否の理由で判別できる

    Args:
        obj: ヘルパーのD-Busオブジェクト
        fake: 偽のsystemd
    """
    manifest = "00000000-0000-4000-8000-000000000000"

    fake.reset()
    baseline = rpc(obj, "CommitRestorePlan", [manifest], SHORT_TIMEOUT_S)
    proof("COMMIT_WITHOUT_TIMER_JOB", baseline)
    if baseline[0] == "OK" or baseline[1] == "A timer operation is in progress":
        fail(f"COMMIT_WITHOUT_TIMER_JOB: unexpected result without a timer job: {baseline!r}")

    fake.set("job_delay", "2000")
    call = AsyncCall(obj, "SetTimerEnabled", [TIMELINE, True], LONG_TIMEOUT_S)
    fake.wait_for_call("StartUnit")
    pump(0.3)
    if call.replies:
        fail(f"COMMIT_DURING_TIMER_JOB: the timer call finished before its job: {call.replies!r}")
    check_error("COMMIT_DURING_TIMER_JOB", rpc(obj, "CommitRestorePlan", [manifest], SHORT_TIMEOUT_S),
                ERR_FAILED, "A timer operation is in progress")

    if not call.wait(25.0):
        fail("COMMIT_DURING_TIMER_JOB: the timer call never received a reply")
    pump(0.3)
    check_ok_true("COMMIT_DURING_TIMER_JOB_TIMER_RESULT", call.replies[0])

    after = rpc(obj, "CommitRestorePlan", [manifest], SHORT_TIMEOUT_S)
    proof("COMMIT_AFTER_TIMER_JOB", after)
    if after[1] == "A timer operation is in progress":
        fail(f"COMMIT_AFTER_TIMER_JOB: the restore start was still blocked: {after!r}")


def main() -> int:
    """
    @brief 設定とタイマーの検査シナリオを実行する

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

    fake = None
    service = subprocess.Popen([service_binary], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    try:
        wait_for_ready(service, ready_file)
        print("SETUP: settings and timers service is READY")
        control = ServiceControl(service)

        bus = dbus.SystemBus(private=True)
        fake = FakeSystemd(bus)
        print("SETUP: fake systemd is READY")
        obj = bus.get_object(IFACE, PATH)

        test_get_config(obj, control)
        test_write_config(obj, control)
        test_get_timer_states(obj, control, fake)
        test_set_timer_basic(obj, control, fake)
        test_set_timer_in_flight(obj, control, fake)
        test_query_in_flight_does_not_block_change(obj, fake)
        test_restore_commit_during_timer_job(obj, fake)

        print("SETTINGS_TIMERS: ALL_OK")
        return 0
    finally:
        if fake is not None:
            fake.stop()
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
            service.wait()


if __name__ == "__main__":
    sys.exit(main())
