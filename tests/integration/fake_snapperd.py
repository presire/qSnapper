#!/usr/bin/env python3
"""
@file fake_snapperd.py
@brief 隔離したシステムバス上で動作する偽のsnapperd (org.opensuse.Snapper)

SnapperdOperationが変更要求を転送する相手のうち、一覧の照会とスナップショットの削除だけを実装する
実snapperdは起動せず、受け取った呼び出しを記録し、テストから与えられた振る舞い
(成功、エラー、無応答、遅延応答、応答前の終了) を再現する

公開するもの :
  org.opensuse.Snapper の /org/opensuse/Snapper
    org.opensuse.Snapper : ListSnapshots / DeleteSnapshots / ListConfigs (応答を失った後の完了の確認)
  org.opensuse.Snapper の /org/opensuse/Snapper
    com.presire.qsnapper.Test.FakeSnapperd : SetSnapshots / SetMutationBehavior /
                                            GetCalls / GetMutations / Reset

制御用のインタフェースを同じバス名と同じオブジェクトに載せるのは、隔離環境のバスポリシーに追加するルールを増やさないため

変更要求を受け取ったことは、制御インタフェースのGetMutationsと、
環境変数QSNAPPER_FAKE_SNAPPERD_MUTATION_FILEが指すファイルの両方で分かる
無応答のときは制御インタフェースが応答できないため、ファイルだけが手掛かりになる

使い方 :
  QSNAPPER_FAKE_SNAPPERD_MUTATION_FILE=<path> python3 fake_snapperd.py
  標準出力へ READY を出力した後、システムバス (DBUS_SYSTEM_BUS_ADDRESS) 上で待ち受ける

終了コード :
  0 : SIGTERMで正常終了した
  2 : python3-dbusまたはPyGObjectが使えない
  3 : バス名 org.opensuse.Snapper を取得できない
"""

import os
import signal
import sys
import time

try:
    import dbus
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

BUS_NAME = "org.opensuse.Snapper"                                    # SnapperdOperationが呼ぶ固定のサービス名
SNAPPER_PATH = "/org/opensuse/Snapper"                               # snapperdのオブジェクトパス
SNAPPER_IFACE = "org.opensuse.Snapper"                               # snapperdのインタフェース
CONTROL_IFACE = "com.presire.qsnapper.Test.FakeSnapperd"             # テスト用の制御インタフェース
MUTATION_FILE_ENV = "QSNAPPER_FAKE_SNAPPERD_MUTATION_FILE"           # 変更要求の受信を伝えるファイル
HANG_SECONDS = 120                                                   # 無応答を再現する間、応答を止めておく時間
SLOW_SECONDS = 2.5                                                   # 期限の後に応答させるための遅延


class FakeSnapperd(dbus.service.Object):
    """
    @brief snapperdの一覧照会と削除要求を、テストから指定された振る舞いで処理する
    """

    def __init__(self, name, bus):
        """
        @brief 制御状態を初期化してオブジェクトを公開する

        Args:
            name: 所有するバス名
            bus: 隔離されたシステムバス
        """
        super().__init__(name, SNAPPER_PATH)
        self._snapshots = []                       # ListSnapshotsが返すスナップショット番号
        self._mutation_behavior = "ok"             # DeleteSnapshotsの振る舞い
        self._calls = []                           # 受け取ったメソッド呼び出しの記録
        self._mutations = 0                        # 変更要求を受け取った回数
        self._mutation_file = os.environ.get(MUTATION_FILE_ENV, "")  # 受信を伝えるファイル

    def _record(self, label):
        """
        @brief 呼び出しを記録する

        Args:
            label: 記録する表示名
        """
        self._calls.append(label)

    def _mark_mutation(self, numbers):
        """
        @brief 変更要求の受信を数とファイルへ記録する

        Args:
            numbers: 要求されたスナップショット番号
        """
        self._mutations += 1
        if not self._mutation_file:
            return
        # 無応答や終了のときは制御インタフェースを読めないため、ファイルへ先に書き出す
        with open(self._mutation_file, "a", encoding="utf-8") as handle:
            handle.write(",".join(str(number) for number in numbers) + "\n")
            handle.flush()

    @dbus.service.method(SNAPPER_IFACE, in_signature="s", out_signature="a(uquxussa{ss})")
    def ListSnapshots(self, config_name):
        """
        @brief 設定された番号の一覧をsnapperdの形式で返す

        Args:
            config_name: 対象の設定名

        Returns:
            (番号, 種別, id, 時刻, 利用者id, 種別名, 説明, 利用者データ) の配列
        """
        self._record("ListSnapshots\t" + str(config_name))
        result = []
        for number in self._snapshots:
            result.append((dbus.UInt32(number), dbus.UInt16(1), dbus.UInt32(number),
                           dbus.Int64(0), dbus.UInt32(0), dbus.String("single"),
                           dbus.String("fake"), dbus.Dictionary({}, signature="ss")))
        return dbus.Array(result, signature="(uquxussa{ss})")

    @dbus.service.method(SNAPPER_IFACE, in_signature="sau", out_signature="au")
    def DeleteSnapshots(self, config_name, numbers):
        """
        @brief 削除要求を、指定された振る舞いで処理する

        Args:
            config_name: 対象の設定名
            numbers: 削除するスナップショット番号

        Returns:
            受け付けた番号の配列

        Raises:
            DBusException: エラーを返す振る舞いのとき
        """
        native = [int(number) for number in numbers]
        self._record("DeleteSnapshots\t" + str(config_name))
        behavior = self._mutation_behavior
        if behavior == "error":
            self._mark_mutation(native)
            raise DBusException("error.something", "snapperd failed")
        self._mark_mutation(native)
        if behavior == "hang":
            # 応答を返さず、テストがプロセスを終了するまで待つ
            time.sleep(HANG_SECONDS)
        elif behavior == "slow":
            time.sleep(SLOW_SECONDS)
        elif behavior == "exit":
            # 応答を返せる状態を残さずに終了し、呼び出し元に切断だけを見せる
            sys.stdout.flush()
            os._exit(0)
        return dbus.Array(native, signature="u")

    @dbus.service.method(SNAPPER_IFACE, in_signature="", out_signature="a(ssa{ss})")
    def ListConfigs(self):
        """
        @brief 完了の確認に使われる照会を記録し、空のconfig一覧を返す

        本物と同じく、同じ接続からの要求は到着順に処理されるため、無応答や遅延の間はこの応答も遅れる

        Returns:
            空のconfig一覧
        """
        self._record("ListConfigs")
        return dbus.Array([], signature="(ssa{ss})")

    @dbus.service.method(CONTROL_IFACE, in_signature="au", out_signature="")
    def SetSnapshots(self, numbers):
        """
        @brief ListSnapshotsが返す番号を設定する

        Args:
            numbers: 返す番号の配列
        """
        self._snapshots = [int(number) for number in numbers]

    @dbus.service.method(CONTROL_IFACE, in_signature="s", out_signature="")
    def SetMutationBehavior(self, behavior):
        """
        @brief DeleteSnapshotsの振る舞いを設定する

        Args:
            behavior: ok / error / hang / slow / exit
        """
        self._mutation_behavior = str(behavior)

    @dbus.service.method(CONTROL_IFACE, in_signature="", out_signature="as")
    def GetCalls(self):
        """
        @brief 受け取った呼び出しの記録を返す

        Returns:
            メソッド名と引数をタブで区切った表示名の配列
        """
        return dbus.Array([dbus.String(item) for item in self._calls], signature="s")

    @dbus.service.method(CONTROL_IFACE, in_signature="", out_signature="u")
    def GetMutations(self):
        """
        @brief 変更要求を受け取った回数を返す

        Returns:
            受信回数
        """
        return dbus.UInt32(self._mutations)

    @dbus.service.method(CONTROL_IFACE, in_signature="", out_signature="")
    def Reset(self):
        """
        @brief 記録と振る舞いを初期状態へ戻す
        """
        self._snapshots = []
        self._mutation_behavior = "ok"
        self._calls = []
        self._mutations = 0


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
    fake = FakeSnapperd(name, bus)
    # SIGTERMでメインループを止め、テスト終了時に子プロセスを残さない
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
    print("READY", flush=True)
    loop.run()
    del fake
    return 0


if __name__ == "__main__":
    sys.exit(main())
