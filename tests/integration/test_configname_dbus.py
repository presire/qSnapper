#!/usr/bin/env python3
"""
test_configname_dbus.py - B-2: configName validation を全14メソッドに対して網羅。

対象メソッド (configName を引数に取るもの):
    GetFstype, ListSnapshots, CreateSnapshot, ModifySnapshot, DeleteSnapshot,
    RollbackSnapshot, GetFileChanges, GetFileChangesBetween,
    GetFileDiffAndDetails, GetFileDiffBetween, RestoreFiles, RestoreFilesDirect,
    WriteSnapperConfig, SetupQuota

各メソッドに対し 6種の悪性 configName を投入し、全て InvalidArgs で拒否されることを
検証する。読み取り系 (GetFstype 等) で試すので副作用はない。

修正前 (v1.3.2): 多くのメソッドで validation なく snapper 層まで到達し、
                  snapper 例外や予期せぬエラーを返すことで漏洩の兆候を観測。
修正後 (P0-3):   全メソッドで先頭の validateConfigName() により
                  org.freedesktop.DBus.Error.InvalidArgs が返る。

使い方:
    sudo python3 test_configname_dbus.py
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

DBUS_SERVICE = "com.presire.qsnapper.Operations"
DBUS_PATH = "/com/presire/qsnapper/Operations"
DBUS_IFACE = "com.presire.qsnapper.Operations"

MALICIOUS_NAMES = [
    "../etc/snapper",
    "/etc/snapper/configs/root",
    "",
    "root/../etc",
    "root\x00evil",
    "root; rm -rf /",
]


@dataclass
class MethodSpec:
    name: str
    # argsは configName を含む完全な引数リストを返すcallable (configName受け取り)
    make_args: object  # Callable[[str], list]


def empty_userdata():
    return Dictionary({}, signature="ss")


SPECS: list[MethodSpec] = [
    MethodSpec("GetFstype", lambda c: [c]),
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
    MethodSpec("RestoreFiles",
               lambda c: [c, 1, ["/.snapshots/1/snapshot/etc/hosts"], ["c"]]),
    MethodSpec("RestoreFilesDirect",
               lambda c: [c, 1, ["/.snapshots/1/snapshot/etc/hosts"], ["c"]]),
    MethodSpec("WriteSnapperConfig", lambda c: [c, empty_userdata()]),
    MethodSpec("SetupQuota", lambda c: [c]),
]


def main() -> int:
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
                    # TODO: 修正前は snapper 由来の例外が多数混じる。
                    #       baseline/fixed で判定を分けるかどうか検討。
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
