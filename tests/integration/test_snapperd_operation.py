#!/usr/bin/env python3
"""
@file test_snapperd_operation.py
@brief SnapperdOperationの転送、事前確認、中断、結果不明、応答期限を、偽のsnapperdを使って検証する

tests/integration/isolated_root_dbus_env.shが起動する専用root D-Busバス上で、
偽のsnapperd (fake_snapperd.py) とsnapperdop_test_driverを起動し、次の結果を検査する
    - Replied      : 一覧の確認と変更の転送が成功する
    - Rejected     : snapperdが未知のエラー名で応答する
    - NotForwarded : snapperdが居ないため変更を転送しない
    - Outdated     : 一覧に無い番号を求められたため変更を転送しない
    - Interrupted  : 変更の直前のガードが偽を返したため変更を転送しない
    - Unknown      : snapperdが応答せずに終了したため結果が分からない
    - deadline     : 応答の期限が完了より先に来て、その後に完了が1回だけ届く
    - Settled      : 変更の応答が転送の期限より遅れ、同じsnapperdへの照会で処理の終わりを確かめた
    - ownerswap    : 変更を受け取ったsnapperdが終了して別のsnapperdが名前を取得しても、新しい方へは何も送らない
    - barrierdenied: バスが完了の確認の照会を拒否しても、snapperdの接続が終了するまで完了しない
呼び出し元の切断によるInterruptedは、切断後に結果を返す接続が残らないためドライバでは再現しない

実snapperdは起動せず、変更要求は隔離バス上の偽物だけが受け取る

tests/integration/run_isolated_restore_plan_test.sh経由でのみ実行すること

使い方 : python3 test_snapperd_operation.py <snapperdop_test_driverのパス>
終了コード : 0 = 全検査成功、1 = 検査失敗、2 = python3-dbus未導入、64 = 引数不正
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

# 同じディレクトリの共通部品 (失敗の報告とPROOF出力) を再利用する
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_restore_gate import fail, proof  # noqa: E402

import dbus  # noqa: E402

BUS_NAME = "org.opensuse.Snapper"                                  # 偽のsnapperdが所有するバス名
SNAPPER_PATH = "/org/opensuse/Snapper"                             # 偽のsnapperdのオブジェクトパス
CONTROL_IFACE = "com.presire.qsnapper.Test.FakeSnapperd"           # 制御用のインタフェース
FAKE_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_snapperd.py")  # 偽のsnapperd
MUTATION_FILE_ENV = "QSNAPPER_FAKE_SNAPPERD_MUTATION_FILE"         # 変更要求の受信を伝えるファイル
REQUIRED_NUMBER = 4242                                             # ドライバが要求する番号
DRIVER_TIMEOUT_S = 40                                              # ドライバ1回の待ち時間


class FakeSnapperd:
    """
    @brief 偽のsnapperdを起動し、制御インタフェースを通じて振る舞いを切り替える
    """

    def __init__(self, bus, mutation_file: str):
        """
        @brief 偽のsnapperdを起動し、READYを待って制御対象を取得する

        Args:
            bus: 隔離されたシステムバス
            mutation_file: 変更要求の受信を伝えるファイルのパス

        Raises:
            SystemExit: READYが得られない場合
        """
        self._mutation_file = mutation_file       # 受信を伝えるファイル
        env = dict(os.environ)                    # 偽物だけに渡す環境変数
        env[MUTATION_FILE_ENV] = mutation_file
        self._process = subprocess.Popen([sys.executable, FAKE_SCRIPT],
                                         stdout=subprocess.PIPE, text=True, env=env)
        line = self._process.stdout.readline() if self._process.stdout is not None else ""
        if line.strip() != "READY":
            self.stop()
            fail(f"fake snapperd did not become READY (got {line!r})")
        obj = bus.get_object(BUS_NAME, SNAPPER_PATH)
        self._control = dbus.Interface(obj, CONTROL_IFACE)

    def configure(self, numbers: list, behavior: str) -> None:
        """
        @brief 一覧として返す番号と変更要求の振る舞いを設定する

        Args:
            numbers: ListSnapshotsが返す番号
            behavior: DeleteSnapshotsの振る舞い
        """
        self._control.SetSnapshots(dbus.Array(numbers, signature="u"))
        self._control.SetMutationBehavior(behavior)

    def mutations(self) -> int:
        """
        @brief 変更要求を受け取った回数を返す

        Returns:
            受信回数
        """
        return int(self._control.GetMutations())

    def calls(self) -> list:
        """
        @brief 受け取った呼び出しの記録を返す

        Returns:
            表示名の並び
        """
        return [str(item) for item in self._control.GetCalls()]

    def stop(self) -> None:
        """
        @brief 偽のsnapperdを停止し、子プロセスを残さない
        """
        if self._process.poll() is None:
            self._process.terminate()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()


def run_driver(driver: str, scenario: str) -> tuple:
    """
    @brief ドライバを1回起動し、標準出力を返す

    Args:
        driver: ドライバのパス
        scenario: シナリオ名

    Returns:
        (完了の名前, 期限切れの回数, 完了の回数, 標準出力全体)

    Raises:
        SystemExit: ドライバが起動できないか、期限内に終わらない場合
    """
    completed = None
    try:
        completed = subprocess.run([driver, scenario], capture_output=True,
                                   text=True, timeout=DRIVER_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        fail(f"driver scenario '{scenario}' did not finish")
    if completed is None:
        fail(f"driver scenario '{scenario}' did not run")
    output = completed.stdout
    completion = ""
    deadlines = 0
    completions = 0
    for line in output.splitlines():
        if line.startswith("COMPLETION "):
            completion = line.split(" ", 1)[1]
        elif line == "DEADLINE":
            deadlines += 1
        elif line.startswith("SUMMARY "):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            deadlines = int(fields.get("deadline", -1))
            completions = int(fields.get("completion", -1))
    if completions != 1:
        fail(f"driver scenario {scenario!r} reported {completions} completions: {output!r} stderr={completed.stderr!r}")
    # 完了が2回通知されてもアプリは先に終了するため、行の重複でも検出できるようにする
    if output.count("COMPLETION ") != 1 or output.count("DEADLINE") > 1:
        fail(f"driver scenario '{scenario}' reported duplicate notifications: {output!r}")
    return (completion, deadlines, completions, output)


def read_marker(path: str) -> bool:
    """
    @brief 変更要求の受信を伝えるファイルに内容があるかを返す

    Args:
        path: ファイルのパス

    Returns:
        内容があればTrue
    """
    try:
        with open(path, encoding="utf-8") as handle:
            return bool(handle.read().strip())
    except FileNotFoundError:
        return False


def reset_marker(path: str) -> None:
    """
    @brief 受信を伝えるファイルを空にする

    Args:
        path: ファイルのパス
    """
    try:
        os.remove(path)
    except FileNotFoundError:
        pass


def check(result: tuple, expected: str, deadlines: int, tag: str) -> None:
    """
    @brief ドライバの結果が期待どおりであることを必須とする

    Args:
        result: run_driverの戻り値
        expected: 期待する完了の名前
        deadlines: 期待する期限切れの回数
        tag: 検査を特定する表示名
    """
    if result[0] != expected:
        fail(f"{tag}: expected completion {expected} but got {result[0]}")
    if result[1] != deadlines:
        fail(f"{tag}: expected {deadlines} deadlines but got {result[1]}")
    proof(f"{tag}_completion", result[0])
    proof(f"{tag}_deadlines", str(result[1]))


def main() -> int:
    """
    @brief snapperd操作の検査シナリオを実行する

    Returns:
        0 = 全検査成功、64 = 引数不正
    """
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <driver-binary-path>", file=sys.stderr)
        return 64

    driver = sys.argv[1]
    if not os.path.isfile(driver) or not os.access(driver, os.X_OK):
        fail(f"driver binary not found or not executable: {driver}")

    tmpdir = os.environ.get("ISOLATED_DBUS_TMPROOT", "/tmp")
    mutation_file = os.path.join(tmpdir, "fixtures", "snapperd-mutations.txt")
    os.makedirs(os.path.dirname(mutation_file), exist_ok=True)

    bus = dbus.SystemBus(private=True)

    # snapperdが居ない状態では、一覧の照会から転送せずにNotForwardedで終わる
    reset_marker(mutation_file)
    result = run_driver(driver, "replied")
    check(result, "NotForwarded", 0, "notforwarded")
    proof("notforwarded_marker", str(read_marker(mutation_file)))

    fake = None
    try:
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "ok")
        reset_marker(mutation_file)
        result = run_driver(driver, "replied")
        check(result, "Replied", 0, "replied")
        if fake.mutations() != 1:
            fail(f"replied: expected 1 forwarded mutation but got {fake.mutations()}")
        proof("replied_mutations", str(fake.mutations()))
        fake.stop()
        time.sleep(0.2)

        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "error")
        reset_marker(mutation_file)
        result = run_driver(driver, "rejected")
        check(result, "Rejected", 0, "rejected")
        if fake.mutations() != 1:
            fail(f"rejected: expected 1 forwarded mutation but got {fake.mutations()}")
        proof("rejected_mutations", str(fake.mutations()))
        fake.stop()
        time.sleep(0.2)

        # 一覧は返るが要求された番号が無いため、変更は転送されない
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([1, 2], "ok")
        reset_marker(mutation_file)
        result = run_driver(driver, "outdated")
        check(result, "Outdated", 0, "outdated")
        if fake.mutations() != 0:
            fail(f"outdated: mutation was forwarded {fake.mutations()} times")
        if not any(call.startswith("ListSnapshots") for call in fake.calls()):
            fail(f"outdated: ListSnapshots was not queried: {fake.calls()!r}")
        proof("outdated_mutations", "0")
        fake.stop()
        time.sleep(0.2)

        # ガードが偽を返すため、一覧の照会の後に変更を止める
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "ok")
        reset_marker(mutation_file)
        result = run_driver(driver, "interrupted")
        check(result, "Interrupted", 0, "interrupted")
        if fake.mutations() != 0:
            fail(f"interrupted: mutation was forwarded {fake.mutations()} times")
        proof("interrupted_mutations", "0")
        fake.stop()
        time.sleep(0.2)

        # 応答を返さずに終了するため、変更が行われたか分からない
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "exit")
        reset_marker(mutation_file)
        result = run_driver(driver, "unknown")
        check(result, "Unknown", 0, "unknown")
        if not read_marker(mutation_file):
            fail("unknown: mutation was not observed before the fake exited")
        proof("unknown_marker", "observed")
        fake.stop()
        time.sleep(0.2)

        # 期限の後に応答するため、期限切れの通知と完了が1回ずつ届く
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "slow")
        reset_marker(mutation_file)
        result = run_driver(driver, "deadline")
        check(result, "Replied", 1, "deadline")
        if not read_marker(mutation_file):
            fail("deadline: mutation was not observed")
        proof("deadline_marker", "observed")
        fake.stop()
        time.sleep(0.2)

        # 変更の応答が転送の期限より遅れても、同じsnapperdへの照会で処理の終わりを確かめてから完了する
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "slow")
        reset_marker(mutation_file)
        result = run_driver(driver, "settled")
        check(result, "Settled", 0, "settled")
        calls = fake.calls()
        if fake.mutations() != 1 or "ListConfigs" not in calls:
            fail(f"settled: expected one mutation followed by ListConfigs: {calls!r}")
        mutation_index = next(index for index, call in enumerate(calls) if call.startswith("DeleteSnapshots"))
        if calls.index("ListConfigs") < mutation_index:
            fail(f"settled: ListConfigs was sent before the mutation: {calls!r}")
        proof("settled_barrier", "after_mutation")
        fake.stop()
        time.sleep(0.2)

        # 変更を受け取ったsnapperdが応答せずに終了し、別のsnapperdが同じ名前を取得しても、
        # 新しい方へ変更も照会も送らず、結果が分からないまま完了する
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "hang")
        reset_marker(mutation_file)
        process = subprocess.Popen([driver, "ownerswap"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        waited = 0.0
        while not read_marker(mutation_file) and waited < 10.0:
            time.sleep(0.1)
            waited += 0.1
        if not read_marker(mutation_file):
            process.kill()
            fail("ownerswap: mutation was not observed")
        # 転送の期限 (1秒) を過ぎ、完了の確認の照会が古いsnapperdに届くまで待つ
        time.sleep(1.5)
        fake.stop()
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "ok")
        output = ""
        try:
            output, _ = process.communicate(timeout=DRIVER_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            process.kill()
            fail("ownerswap: driver did not finish")
        if output.count("COMPLETION ") != 1 or "COMPLETION Unknown" not in output:
            fail(f"ownerswap: expected a single Unknown completion: {output!r}")
        if fake.mutations() != 0 or fake.calls():
            fail(f"ownerswap: the new owner received calls: {fake.calls()!r}")
        proof("ownerswap_completion", "Unknown")
        proof("ownerswap_new_owner_calls", "0")
        fake.stop()
        time.sleep(0.2)

        # バスが完了の確認の照会を拒否しても、snapperdの処理中に完了せず、接続の終了を確かめてから完了する
        fake = FakeSnapperd(bus, mutation_file)
        fake.configure([REQUIRED_NUMBER], "hang")
        reset_marker(mutation_file)
        process = subprocess.Popen([driver, "barrierdenied"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        waited = 0.0
        while not read_marker(mutation_file) and waited < 10.0:
            time.sleep(0.1)
            waited += 0.1
        if not read_marker(mutation_file):
            process.kill()
            fail("barrierdenied: mutation was not observed")
        # 転送の期限 (1秒) と、拒否された照会の送り直しを1回以上含む時間だけ待つ
        time.sleep(7.0)
        if process.poll() is not None:
            output, _ = process.communicate()
            fail(f"barrierdenied: completed while snapperd was still processing: {output!r}")
        fake.stop()
        output = ""
        try:
            output, _ = process.communicate(timeout=DRIVER_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            process.kill()
            fail("barrierdenied: driver did not finish after snapperd exited")
        if output.count("COMPLETION ") != 1 or "COMPLETION Unknown" not in output:
            fail(f"barrierdenied: expected a single Unknown completion: {output!r}")
        proof("barrierdenied_held", "yes")
        proof("barrierdenied_completion", "Unknown")
    finally:
        if fake is not None:
            fake.stop()

    print("SNAPPERD_OPERATION: ALL_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
