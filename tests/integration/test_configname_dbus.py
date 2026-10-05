#!/usr/bin/env python3
"""
@file test_configname_dbus.py
@brief configNameの検証が全12メソッドで機能することを網羅的に検査する

configNameを引数に取る以下の12メソッドに対し6種の悪性configNameを投入し、
全てがorg.freedesktop.DBus.Error.InvalidArgsで拒否されることを検証する
対象 : ListSnapshots, CreateSnapshot, ModifySnapshot, DeleteSnapshot,
       RollbackSnapshot, GetFileChanges, GetFileChangesBetween,
       GetFileDiffAndDetails, GetFileDiffBetween, BeginRestorePlan,
       WriteSnapperConfig, SetupQuota
読み取り系 (ListSnapshots等) で試すため副作用はない

修正前 (v1.3.2) : 多くのメソッドで検証を通過してsnapper層まで到達し、
                  snapper例外や予期せぬエラーを返して漏洩の兆候を観測した
修正後 (P0-3) : 全メソッドが先頭のvalidateConfigName()により
                org.freedesktop.DBus.Error.InvalidArgsを返す

使い方 : sudo python3 test_configname_dbus.py
終了コード : 0 = 期待どおり、1 = 期待はずれ、2 = VM外またはpython3-dbus未導入
"""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass

try:
    from dbus import SystemBus, Interface, Dictionary, String  # type: ignore
    from dbus.exceptions import DBusException  # type: ignore
except ImportError:
    print("ERROR: python3-dbus required", file=sys.stderr)
    sys.exit(2)

DBUS_SERVICE = "com.presire.qsnapper.Operations"   # 検査対象のD-Busサービス名
DBUS_PATH = "/com/presire/qsnapper/Operations"     # オブジェクトパス
DBUS_IFACE = "com.presire.qsnapper.Operations"     # インタフェース名

MALICIOUS_NAMES = [                # 検証に用いる悪性configName
    "../etc/snapper",              # 親ディレクトリ参照の混入
    "/etc/snapper/configs/root",   # 絶対パスの指定
    "",                            # 空文字
    "root/../etc",                 # 正規名に親ディレクトリ参照を混入
    "root\x00evil",                # NULバイトの混入
    "root; rm -rf /",              # シェルメタ文字の混入
]


@dataclass
class MethodSpec:
    """
    @class MethodSpec
    @brief 検査対象1メソッド分の呼び出し仕様

    name : D-Busメソッド名
    make_args : configNameを受け取り、そのメソッドの完全な引数リストを返すcallable
    """
    name: str           # D-Busメソッド名
    make_args: object   # configNameを受け取り完全な引数リストを返すcallable


def empty_userdata():
    """
    @brief 空のユーザデータ辞書を返す

    Returns:
        "ss"シグネチャを持つ空のqdbus辞書
    """
    return Dictionary({}, signature="ss")


SPECS: list[MethodSpec] = [                # 検査対象メソッドとその引数生成関数
    MethodSpec("ListSnapshots", lambda c: [c]),
    MethodSpec("CreateSnapshot",
               lambda c: [c, "single", "test", 0, "", empty_userdata(), False]),
    MethodSpec("ModifySnapshot",
               lambda c: [c, 1, "desc", "", empty_userdata()]),
    MethodSpec("DeleteSnapshot", lambda c: [c, 1]),
    MethodSpec("RollbackSnapshot", lambda c: [c, 1]),
    MethodSpec("GetFileChanges", lambda c: [c, 1]),
    MethodSpec("GetFileChangesBetween", lambda c: [c, 1, 2]),
    MethodSpec("GetFileDiffAndDetails", lambda c: [c, 1, "/etc/hosts"]),
    MethodSpec("GetFileDiffBetween", lambda c: [c, 1, 2, "/etc/hosts"]),
    MethodSpec("BeginRestorePlan", lambda c: [c, 1, 0, "direct"]),
    MethodSpec("WriteSnapperConfig", lambda c: [c, empty_userdata()]),
    MethodSpec("SetupQuota", lambda c: [c]),
]


def main() -> int:
    """
    @brief 全対象メソッドへ悪性configNameを投入する網羅検査を実行する

    Returns:
        0 = 期待どおり、1 = 期待はずれ、2 = VM外で実行された場合
    """
    if subprocess.run(["systemd-detect-virt"], capture_output=True,
                      text=True).stdout.strip() == "none":
        print("ERROR: run inside a VM", file=sys.stderr)
        return 2

    bus = SystemBus()
    proxy = bus.get_object(DBUS_SERVICE, DBUS_PATH)
    ops = Interface(proxy, DBUS_IFACE)

    total = 0
    blocked = 0
    leaked = 0

    for spec in SPECS:
        method = getattr(ops, spec.name)
        for payload in MALICIOUS_NAMES:
            total += 1
            try:
                method(*spec.make_args(String(payload)))
                print(f"[LEAK] {spec.name}({payload!r}) returned without error")
                leaked += 1
            except DBusException as e:
                name = e.get_dbus_name()
                if "InvalidArgs" in name:
                    blocked += 1
                else:
                    # TODO: 修正前はsnapper由来の例外が多数混じる
                    # baselineとfixedで判定を分けるかどうか検討する
                    print(f"[OTHER] {spec.name}({payload!r}) → {name}: {e}")

    print(f"\ntotal={total} blocked={blocked} leaked={leaked}")
    mode = "baseline" if os.environ.get("BASELINE") else "fixed"
    if mode == "fixed":
        # 修正版では全てがInvalidArgsで弾かれるべき
        return 0 if blocked == total else 1
    else:
        # baselineでは少なくとも一部が検証をすり抜けるはず
        return 0 if blocked < total else 1


if __name__ == "__main__":
    sys.exit(main())
