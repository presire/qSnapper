#!/usr/bin/env python3
"""
@file fake_systemd1.py
@brief 隔離したシステムバス上で動作する偽のsystemd (org.freedesktop.systemd1)

qSnapperのヘルパーが使うsystemdのManagerインタフェースの一部だけを実装する
実際のsystemdには一切触れず、受け取った呼び出しを全て記録し、
テストから与えられた振る舞い (欠落、マスク、ジョブ失敗、無応答、遅延、エラー本文の漏洩など) を再現する

公開するもの :
  org.freedesktop.systemd1 の /org/freedesktop/systemd1
    org.freedesktop.systemd1.Manager : GetUnitFileState / GetUnit / EnableUnitFiles /
                                       DisableUnitFiles / StartUnit / StopUnit / Reload / JobRemoved
  org.freedesktop.systemd1 の /org/freedesktop/systemd1/unit/* (FallbackObject)
    org.freedesktop.DBus.Properties : Get (ActiveStateのみ)
  org.freedesktop.systemd1 の /com/presire/qsnapper/Test/FakeSystemd
    com.presire.qsnapper.Test.FakeSystemd : SetBehavior / GetCalls / Reset

制御用のインタフェースを同じバス名に載せるのは、隔離環境のバスポリシーに追加するルールを増やさないため

振る舞いのキー (SetBehavior(key, value)) :
  state.<unit>           : "<unitFileState>,<activeState>" で初期状態を上書きする
  missing.<unit>         : 1 でGetUnitFileStateがFileNotFoundを返し、GetUnitがNoSuchUnitを返す
  masked.<unit>          : 1 でunit fileの状態を常にmaskedにする (Enable/Disableで変わらない)
  job_result             : StartUnit/StopUnitのジョブ結果 (既定はdone)
  jobremoved_early       : 1 でJobRemovedをStartUnit/StopUnitの応答より前に送出する
  job_delay              : JobRemovedを応答の後に送るまでの遅延をミリ秒で指定する (jobの実行中に復元の開始を試みる検査に使う)
  silent.<Method>        : 1 で指定メソッドに応答しない
  delay.<Method>         : 応答までの遅延をミリ秒で指定する
  error.<Method>         : "<エラー名>|<本文>" で指定メソッドにエラーを返す

使い方 :
  python3 fake_systemd1.py
  標準出力へ READY を出力した後、システムバス (DBUS_SYSTEM_BUS_ADDRESS) 上で待ち受ける

終了コード :
  0 : SIGTERMで正常終了した
  2 : python3-dbusまたはPyGObjectが使えない
  3 : バス名 org.freedesktop.systemd1 を取得できない
"""

import json
import signal
import sys

try:
    import dbus
    import dbus.lowlevel
    import dbus.mainloop.glib
    import dbus.service
    from dbus.exceptions import DBusException
except ImportError:
    print("ERROR: python3-dbus required", file=sys.stderr)
    sys.exit(2)

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)

try:
    from gi.repository import GLib  # noqa: E402
except ImportError:
    print("ERROR: PyGObject required", file=sys.stderr)
    sys.exit(2)

BUS_NAME = "org.freedesktop.systemd1"                       # ヘルパーが接続する固定のサービス名
MANAGER_PATH = "/org/freedesktop/systemd1"                  # Managerオブジェクトのパス
MANAGER_IFACE = "org.freedesktop.systemd1.Manager"          # Managerインタフェース
UNIT_IFACE = "org.freedesktop.systemd1.Unit"                # Unitインタフェース
UNIT_PATH_PREFIX = "/org/freedesktop/systemd1/unit"         # unitオブジェクトのパスの接頭辞
JOB_PATH_PREFIX = "/org/freedesktop/systemd1/job"           # jobオブジェクトのパスの接頭辞
PROPS_IFACE = "org.freedesktop.DBus.Properties"             # Propertiesインタフェース
CONTROL_PATH = "/com/presire/qsnapper/Test/FakeSystemd"     # テスト用の制御オブジェクトのパス
CONTROL_IFACE = "com.presire.qsnapper.Test.FakeSystemd"     # テスト用の制御インタフェース
JOB_SIGNAL_DELAY_MS = 100                                   # 通常時にJobRemovedを応答の後に送るまでの遅延
ERR_FILE_NOT_FOUND = "org.freedesktop.DBus.Error.FileNotFound"  # 未知のunit fileに返すエラー名
ERR_NO_SUCH_UNIT = "org.freedesktop.systemd1.NoSuchUnit"        # 未ロードのunitに返すエラー名


def to_native(value):
    """
    @brief dbus-pythonの型を、JSONへ変換できる素のPython型へ再帰的に変換する

    Args:
        value: 変換元の値

    Returns:
        str / bool / int / list へ変換した値

    Note:
        記録した呼び出しの引数をテスト側で厳密に比較するため、型情報は落として値だけを残す
    """
    if isinstance(value, dbus.Boolean):
        return bool(value)
    if isinstance(value, (dbus.Array, list, tuple)):
        return [to_native(item) for item in value]
    if isinstance(value, dbus.String):
        return str(value)
    if isinstance(value, (dbus.Int16, dbus.Int32, dbus.Int64, dbus.UInt16, dbus.UInt32, dbus.UInt64)):
        return int(value)
    return value


def unit_path(unit):
    """
    @brief unit名をオブジェクトパスへ変換する

    Args:
        unit: unit名

    Returns:
        systemdと同じ方式 (英数字以外を _XX の16進で表す) のオブジェクトパス
    """
    escaped = "".join(ch if ch.isalnum() and ch.isascii() else "_%02x" % ord(ch) for ch in unit)
    return dbus.ObjectPath(UNIT_PATH_PREFIX + "/" + escaped)


def unit_from_path(path):
    """
    @brief unit_path()の逆変換でオブジェクトパスからunit名を求める

    Args:
        path: unitのオブジェクトパス

    Returns:
        unit名
    """
    escaped = str(path)[len(UNIT_PATH_PREFIX) + 1:]
    out = []
    i = 0
    while i < len(escaped):
        if escaped[i] == "_" and i + 2 < len(escaped):
            out.append(chr(int(escaped[i + 1:i + 3], 16)))
            i += 3
        else:
            out.append(escaped[i])
            i += 1
    return "".join(out)


class FakeState:
    """
    @brief 偽のsystemdが持つ全ての状態 (記録した呼び出し、振る舞い、unitの状態、ジョブ番号)

    Args:
        なし

    Note:
        Reset()で初期状態へ戻せるように、状態は一箇所へまとめる
    """

    def __init__(self):
        self.calls = []        # 受信順の呼び出し記録 ("メソッド名\t引数のJSON")
        self.behaviors = {}    # SetBehaviorで与えられた振る舞い
        self.units = {}        # unit名 -> [unitFileState, activeState]
        self.next_job_id = 1   # 次に払い出すジョブ番号

    def reset(self):
        """
        @brief 記録、振る舞い、unitの状態、ジョブ番号を初期状態へ戻す

        Returns:
            なし
        """
        self.calls = []
        self.behaviors = {}
        self.units = {}
        self.next_job_id = 1

    def record(self, method, args):
        """
        @brief 受信した呼び出しを記録する

        Args:
            method: メソッド名
            args: 引数のリスト (dbus-pythonの型でもよい)

        Returns:
            なし
        """
        self.calls.append(method + "\t" + json.dumps(to_native(list(args))))

    def behavior(self, key, default=""):
        """
        @brief 振る舞いの値を取得する

        Args:
            key: 振る舞いのキー
            default: 未設定のときの値

        Returns:
            設定値、未設定ならdefault
        """
        return self.behaviors.get(key, default)

    def flag(self, key):
        """
        @brief 振る舞いが1として有効化されているかを返す

        Args:
            key: 振る舞いのキー

        Returns:
            有効ならTrue
        """
        return self.behaviors.get(key, "") == "1"

    def unit_state(self, unit):
        """
        @brief unitの現在の状態を返す (未登録なら disabled / inactive を登録する)

        Args:
            unit: unit名

        Returns:
            [unitFileState, activeState] の可変リスト
        """
        if unit not in self.units:
            initial = self.behaviors.get("state." + unit)
            if initial is not None:
                file_state, _, active = initial.partition(",")
                self.units[unit] = [file_state, active]
            else:
                self.units[unit] = ["disabled", "inactive"]
        return self.units[unit]


STATE = FakeState()  # プロセス全体で共有する状態


def make_error(spec):
    """
    @brief 振る舞い指定の文字列からD-Busエラーを作る

    Args:
        spec: "<エラー名>|<本文>" の形式の文字列

    Returns:
        DBusExceptionのインスタンス
    """
    name, _, message = spec.partition("|")
    return DBusException(message, name=name)


def schedule(delay_ms, func):
    """
    @brief 指定時間の後に関数を1度だけ実行する

    Args:
        delay_ms: 遅延 (ミリ秒)、0以下なら即時に実行する
        func: 引数を取らない関数

    Returns:
        なし
    """
    if delay_ms <= 0:
        func()
        return

    def once():
        func()
        return False

    GLib.timeout_add(delay_ms, once)


class Manager(dbus.service.Object):
    """
    @brief org.freedesktop.systemd1.Manager の偽実装

    Args:
        bus_name: 取得済みのバス名
        bus: 接続先のバス

    Note:
        全メソッドを非同期応答にし、無応答と遅延の振る舞いを再現できるようにする
    """

    def __init__(self, bus_name, bus):
        super().__init__(bus_name, MANAGER_PATH)

    def dispatch(self, method, args, produce, reply_cb, error_cb):
        """
        @brief 呼び出しを記録し、振る舞いに従って応答する共通処理

        Args:
            method: メソッド名
            args: 引数のリスト
            produce: 応答値のタプルを返す関数 (DBusExceptionを送出するとエラー応答になる)
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし

        Note:
            silentの場合は応答を返さず、呼び出し側の時間切れを確かめられるようにする
            状態の変更は応答の直前に行い、遅延中の呼び出しの前後関係を観測できるようにする
        """
        STATE.record(method, args)
        if STATE.flag("silent." + method):
            return

        def respond():
            error_spec = STATE.behavior("error." + method)
            if error_spec:
                error_cb(make_error(error_spec))
                return
            try:
                values = produce()
            except DBusException as exc:
                error_cb(exc)
                return
            reply_cb(*values)

        schedule(int(STATE.behavior("delay." + method, "0")), respond)

    @dbus.service.method(MANAGER_IFACE, in_signature="s", out_signature="s",
                         async_callbacks=("reply_cb", "error_cb"))
    def GetUnitFileState(self, unit, reply_cb, error_cb):
        """
        @brief unit fileの状態を返す

        Args:
            unit: unit名
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし
        """
        unit = str(unit)

        def produce():
            if STATE.flag("missing." + unit):
                raise DBusException("unit file not found", name=ERR_FILE_NOT_FOUND)
            if STATE.flag("masked." + unit):
                return (dbus.String("masked"),)
            return (dbus.String(STATE.unit_state(unit)[0]),)

        self.dispatch("GetUnitFileState", [unit], produce, reply_cb, error_cb)

    @dbus.service.method(MANAGER_IFACE, in_signature="s", out_signature="o",
                         async_callbacks=("reply_cb", "error_cb"))
    def GetUnit(self, unit, reply_cb, error_cb):
        """
        @brief ロード済みunitのオブジェクトパスを返す

        Args:
            unit: unit名
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし
        """
        unit = str(unit)

        def produce():
            if STATE.flag("missing." + unit):
                raise DBusException("unit not loaded", name=ERR_NO_SUCH_UNIT)
            return (unit_path(unit),)

        self.dispatch("GetUnit", [unit], produce, reply_cb, error_cb)

    @dbus.service.method(MANAGER_IFACE, in_signature="asbb", out_signature="ba(sss)",
                         async_callbacks=("reply_cb", "error_cb"))
    def EnableUnitFiles(self, files, runtime, force, reply_cb, error_cb):
        """
        @brief unit fileを有効にする (マスク中のunitは変えない)

        Args:
            files: unit fileの名前の配列
            runtime: ランタイムのみかどうか
            force: 既存のリンクを上書きするかどうか
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし
        """
        names = [str(item) for item in files]

        def produce():
            changes = []
            for name in names:
                if not STATE.flag("masked." + name):
                    STATE.unit_state(name)[0] = "enabled"
                changes.append(("symlink", "/fake/" + name, "/fake/target"))
            return (dbus.Boolean(True), dbus.Array(
                [dbus.Struct(item, signature="sss") for item in changes], signature="(sss)"))

        self.dispatch("EnableUnitFiles", [names, bool(runtime), bool(force)],
                      produce, reply_cb, error_cb)

    @dbus.service.method(MANAGER_IFACE, in_signature="asb", out_signature="a(sss)",
                         async_callbacks=("reply_cb", "error_cb"))
    def DisableUnitFiles(self, files, runtime, reply_cb, error_cb):
        """
        @brief unit fileを無効にする (マスク中のunitは変えない)

        Args:
            files: unit fileの名前の配列
            runtime: ランタイムのみかどうか
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし
        """
        names = [str(item) for item in files]

        def produce():
            changes = []
            for name in names:
                if not STATE.flag("masked." + name):
                    STATE.unit_state(name)[0] = "disabled"
                changes.append(("unlink", "/fake/" + name, ""))
            return (dbus.Array(
                [dbus.Struct(item, signature="sss") for item in changes], signature="(sss)"),)

        self.dispatch("DisableUnitFiles", [names, bool(runtime)], produce, reply_cb, error_cb)

    def send_job_removed(self, requester, job_id, job_path, unit, result):
        """
        @brief JobRemovedを送出する

        Args:
            requester: ジョブを要求した接続の一意名 (不明ならNone)
            job_id: ジョブ番号
            job_path: ジョブのオブジェクトパス
            unit: 対象のunit名
            result: ジョブの結果

        Returns:
            なし

        Note:
            本物のsystemdは、Subscribeしていない要求元へ、宛先を指定した単一宛先のシグナルとして送る
            放送で送ると、ヘルパーが受け取る経路 (宛先指定) と異なる経路を検査してしまうため、要求元が不明な場合だけ放送する
        """
        if requester is None:
            self.JobRemoved(dbus.UInt32(job_id), job_path, dbus.String(unit), dbus.String(result))
            return
        message = dbus.lowlevel.SignalMessage(MANAGER_PATH, MANAGER_IFACE, "JobRemoved")
        message.set_destination(str(requester))
        message.append(dbus.UInt32(job_id), job_path, dbus.String(unit), dbus.String(result),
                       signature="uoss")
        self.connection.send_message(message)

    def run_job(self, unit, active_after, requester):
        """
        @brief StartUnit/StopUnitの共通処理でジョブを作り、結果に応じてactiveStateを更新する

        Args:
            unit: unit名
            active_after: ジョブが成功したときのactiveState
            requester: ジョブを要求した接続の一意名

        Returns:
            ジョブのオブジェクトパス

        Note:
            jobremoved_earlyが有効ならJobRemovedを応答より前に送出し、
            そうでなければ応答の後に少し遅らせて送出する
            ヘルパーが応答前のシグナルも取りこぼさないことを確かめるため
        """
        job_id = STATE.next_job_id
        STATE.next_job_id += 1
        job_path = dbus.ObjectPath("%s/%d" % (JOB_PATH_PREFIX, job_id))
        result = STATE.behavior("job_result", "done")
        if result == "done" and not STATE.flag("masked." + unit):
            STATE.unit_state(unit)[1] = active_after

        def emit():
            self.send_job_removed(requester, job_id, job_path, unit, result)

        if STATE.flag("jobremoved_early"):
            emit()
        else:
            schedule(int(STATE.behavior("job_delay", str(JOB_SIGNAL_DELAY_MS))), emit)
        return job_path

    @dbus.service.method(MANAGER_IFACE, in_signature="ss", out_signature="o",
                         async_callbacks=("reply_cb", "error_cb"), sender_keyword="sender")
    def StartUnit(self, unit, mode, reply_cb, error_cb, sender=None):
        """
        @brief unitを起動するジョブを作る

        Args:
            unit: unit名
            mode: ジョブのモード
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック
            sender: 呼び出した接続の一意名

        Returns:
            なし
        """
        unit = str(unit)
        self.dispatch("StartUnit", [unit, str(mode)],
                      lambda: (self.run_job(unit, "active", sender),), reply_cb, error_cb)

    @dbus.service.method(MANAGER_IFACE, in_signature="ss", out_signature="o",
                         async_callbacks=("reply_cb", "error_cb"), sender_keyword="sender")
    def StopUnit(self, unit, mode, reply_cb, error_cb, sender=None):
        """
        @brief unitを停止するジョブを作る

        Args:
            unit: unit名
            mode: ジョブのモード
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック
            sender: 呼び出した接続の一意名

        Returns:
            なし
        """
        unit = str(unit)
        self.dispatch("StopUnit", [unit, str(mode)],
                      lambda: (self.run_job(unit, "inactive", sender),), reply_cb, error_cb)

    @dbus.service.method(MANAGER_IFACE, in_signature="", out_signature="",
                         async_callbacks=("reply_cb", "error_cb"))
    def Reload(self, reply_cb, error_cb):
        """
        @brief systemdの設定再読み込みを受け付ける (状態は変えない)

        Args:
            reply_cb: 正常応答のコールバック
            error_cb: エラー応答のコールバック

        Returns:
            なし
        """
        self.dispatch("Reload", [], lambda: (), reply_cb, error_cb)

    @dbus.service.signal(MANAGER_IFACE, signature="uoss")
    def JobRemoved(self, job_id, job, unit, result):
        """
        @brief ジョブの完了を通知するシグナル

        Args:
            job_id: ジョブ番号
            job: ジョブのオブジェクトパス
            unit: 対象のunit名
            result: ジョブの結果 (done / failed など)

        Returns:
            なし
        """


class UnitProperties(dbus.service.FallbackObject):
    """
    @brief unitオブジェクト配下のProperties.Getだけを実装する偽オブジェクト

    Args:
        bus: 接続先のバス

    Note:
        FallbackObjectにすることで、unitごとにオブジェクトを登録せずに任意のunitパスへ応答する
    """

    def __init__(self, bus):
        super().__init__(bus, UNIT_PATH_PREFIX)

    @dbus.service.method(PROPS_IFACE, in_signature="ss", out_signature="v", path_keyword="path")
    def Get(self, interface, name, path=None):
        """
        @brief ActiveStateだけを返す

        Args:
            interface: インタフェース名
            name: プロパティ名
            path: 呼び出し先のオブジェクトパス

        Returns:
            ActiveStateの文字列を包んだ値

        Raises:
            DBusException: ActiveState以外が要求された場合
        """
        unit = unit_from_path(path)
        STATE.record("Properties.Get", [str(path), str(interface), str(name)])
        if str(interface) != UNIT_IFACE or str(name) != "ActiveState":
            raise DBusException("unknown property", name="org.freedesktop.DBus.Error.UnknownProperty")
        return dbus.String(STATE.unit_state(unit)[1])


class Control(dbus.service.Object):
    """
    @brief テストが振る舞いの設定と呼び出し記録の取得に使う制御オブジェクト

    Args:
        bus_name: 取得済みのバス名
    """

    def __init__(self, bus_name):
        super().__init__(bus_name, CONTROL_PATH)

    @dbus.service.method(CONTROL_IFACE, in_signature="ss", out_signature="")
    def SetBehavior(self, key, value):
        """
        @brief 振る舞いを1件設定する

        Args:
            key: 振る舞いのキー
            value: 値

        Returns:
            なし
        """
        STATE.behaviors[str(key)] = str(value)
        # 初期状態の指定は、既に登録済みのunitへも反映する
        # それ以外のキーでunitの状態を消すと、テスト途中の設定で進行中の状態が失われるため消さない
        if str(key).startswith("state."):
            STATE.units.pop(str(key)[len("state."):], None)

    @dbus.service.method(CONTROL_IFACE, in_signature="", out_signature="as")
    def GetCalls(self):
        """
        @brief 受信順の呼び出し記録を返す

        Returns:
            "メソッド名\\t引数のJSON" の配列
        """
        return dbus.Array([dbus.String(item) for item in STATE.calls], signature="s")

    @dbus.service.method(CONTROL_IFACE, in_signature="", out_signature="")
    def Reset(self):
        """
        @brief 記録、振る舞い、unitの状態を初期状態へ戻す

        Returns:
            なし
        """
        STATE.reset()


def main():
    """
    @brief バス名を取得して待ち受ける

    Returns:
        終了コード
    """
    loop = GLib.MainLoop()
    bus = dbus.SystemBus(private=True)
    try:
        name = dbus.service.BusName(BUS_NAME, bus, do_not_queue=True)
    except DBusException:
        print("ERROR: cannot own " + BUS_NAME, file=sys.stderr)
        return 3
    objects = [Manager(name, bus), UnitProperties(bus), Control(name)]
    # SIGTERMでメインループを止め、テスト終了時に子プロセスを残さない
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
    print("READY", flush=True)
    loop.run()
    del objects
    return 0


if __name__ == "__main__":
    sys.exit(main())
