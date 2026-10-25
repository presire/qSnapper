#!/usr/bin/env python3
"""
@file test_vm_configs_btrfs.py
@brief 特権D-Busメソッド群を本物のloopback btrfs上で検証するVM専用の統合テスト

CreateConfig, DeleteConfig, DeleteSnapshots, SetupQuota, GetConfig,
GetFilesystemUsage, ListSubvolumesを、本物のヘルパー (com.presire.qsnapper.Operations)
と本物のsnapperdに対して呼び出し、システムバス越しの実際の結果を確認する
ctestやCIでは実行しない
設定ファイル (/etc/snapper/configs) と systemd や snapperd の状態を実際に書き換えるため、
使い捨てのVMでだけ実行すること

準備 : 1 GiBのスパースイメージをloopデバイスへ割り当ててbtrfsを作り、
       root所有0755のディレクトリへマウントしてサブボリューム vol1 と vol2 を作る
       ここで使う外部コマンド (losetup, mkfs.btrfs, mount, btrfs, snapper) は
       テスト側だけのものであり、ヘルパー側が外部コマンドを使わない方針とは無関係である

検証する場面 (各場面は PROOF_<NAME>: <結果> を出力する)
  a. CreateConfigの成功とGetConfigのSUBVOLUME, FSTYPE, .snapshotsの作成
  b. 同じsubvolumeへの重複作成が拒否され、設定が作られない
  c. .snapshotsが既にあるsubvolumeへの作成が拒否される
  d. symlink, root所有でない親, 通常ディレクトリ, 相対パス, 改行入りのパスが拒否される
  e. ルートの作成と削除が認可の前に拒否される (AccessDeniedまたはInvalidArgs)
  f. 全てのエラー本文に、設定名, マウントパス, スナップショット番号が含まれない
  g. スナップショットの作成と、重要フラグ, cleanupの反映
  h. DeleteSnapshotsの重複除去と、不正な入力で何も削除されないこと
  i. SetupQuotaの成功とGetConfigのQGROUP
  j. GetFilesystemUsageのCSV形式
  k. ListSubvolumesのCSV形式
  l. DeleteConfigの成功と設定ファイルの消滅

認可 : D-Bus呼び出しはPolkitの認可を要求する
       rootでの実行は通常ポリシーの規則で許可されるが、規則が無い環境では
       認証エージェントが認証を求めるため、呼び出しの待ち時間を最大600秒としている
       対話できるセッションで実行すること

後始末 : どの段階で失敗しても、作成した設定, マウント, loopデバイス, 一時イメージを必ず片付ける
         起動時に qsvmtest で始まる設定が既にある場合は、既存の設定に触れないよう実行を拒否する

使い方 : sudo env QSNAPPER_VM_TEST=1 python3 tests/integration/test_vm_configs_btrfs.py
         QSNAPPER_VM_TEST=1 を指定しない場合、またはrootでない場合は何もせず終了する
終了コード : 0 = 全て成功 (VM_CONFIGS_BTRFS: ALL_OK を出力)、
             1 = 失敗または後始末の失敗 (FAIL行を出力)、
             2 = python3-dbusが未導入、
             77 = オプトインが無い、またはroot権限が無いためスキップ
"""

from __future__ import annotations

import argparse
import csv
import importlib
import io
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any, NamedTuple, NoReturn

OPT_IN_ENV = "QSNAPPER_VM_TEST"                       # 実行を許可する環境変数名 (値は1)
EXIT_OK = 0                                           # 全て成功
EXIT_FAIL = 1                                         # 失敗または後始末の失敗
EXIT_NO_DBUS = 2                                      # python3-dbusが未導入
EXIT_SKIP = 77                                        # オプトインなしまたはroot権限なし

DBUS_SERVICE = "com.presire.qsnapper.Operations"      # 検査対象のD-Busサービス名
DBUS_PATH = "/com/presire/qsnapper/Operations"        # オブジェクトパス
DBUS_IFACE = "com.presire.qsnapper.Operations"        # インタフェース名
CALL_TIMEOUT_S = 600                                  # 認証エージェントの待ちを許す呼び出し時間 (秒)

NAME_PREFIX = "qsvmtest"                              # このテストが作る設定名の接頭辞
CONFIG_DIR = "/etc/snapper/configs"                   # snapperの設定ファイルの置き場
IMAGE_PARENT = "/var/tmp"                             # スパースイメージを置く親ディレクトリ
MOUNT_PARENT = "/mnt"                                 # マウント先を作る親ディレクトリ
IMAGE_BYTES = 1 << 30                                 # スパースイメージの大きさ (1 GiB)
NOBODY_UID = 65534                                    # root所有でない親の模擬に使うUID
NOBODY_GID = 65534                                    # root所有でない親の模擬に使うGID
SNAPSHOT_COUNT = 5                                    # 重要フラグなしで作るスナップショット数
UNIQUE_LIMIT = 100                                    # DeleteSnapshotsの重複除去後の上限
RAW_LIMIT = 1000                                      # DeleteSnapshotsの重複除去前の上限
MISSING_NUMBER = 99999                                # 存在しないスナップショット番号

ERR_PREFIXES = ("com.presire.qsnapper.Error.",        # 本サービス固有のエラー名の接頭辞
                "org.freedesktop.DBus.Error.")        # 標準のD-Busエラー名の接頭辞
ERR_INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"        # 入力検証の拒否
ERR_ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"      # 認可または方針による拒否
ERR_LIMITS = "org.freedesktop.DBus.Error.LimitsExceeded"           # 件数上限の超過
ERR_FAILED = "org.freedesktop.DBus.Error.Failed"                   # 本体処理の失敗
INFRA_ERRORS = (                                      # 検証ではなく基盤の不具合を示すエラー名
    "org.freedesktop.DBus.Error.ServiceUnknown",
    "org.freedesktop.DBus.Error.NoReply",
    "org.freedesktop.DBus.Error.UnknownMethod",
    "org.freedesktop.DBus.Error.UnknownObject",
    "org.freedesktop.DBus.Error.UnknownInterface",
    "org.freedesktop.DBus.Error.NoServer",
    "org.freedesktop.DBus.Error.Disconnected",
    "org.freedesktop.DBus.Error.Spawn.",
)

USAGE_HEADER = "kind,flags,total,used"               # GetFilesystemUsageの列見出し
SUBVOL_FIRST_LINE = "truncated,0"                     # ListSubvolumesの先頭行 (打ち切りなし)
SUBVOL_HEADER = "id,parent_id,created,readonly,snapper,path"   # ListSubvolumesの列見出し


class TestFailure(Exception):
    """
    @brief 期待と異なる結果を表す例外

    Note:
        メッセージは FAIL 行としてそのまま出力される
    """


class ErrorInfo(NamedTuple):
    """
    @class ErrorInfo
    @brief D-Busエラー1件の名前と本文
    """
    name: str   # D-Busエラー名
    text: str   # エラー本文


class State:
    """
    @class State
    @brief テスト全体で共有する実行時の状態

    後始末が途中までの状態でも安全に動くよう、作成済みの資源だけを記録する
    """

    def __init__(self) -> None:
        self.image_dir: str | None = None     # スパースイメージの一時ディレクトリ
        self.image: str | None = None         # スパースイメージのパス
        self.loop: str | None = None          # 割り当てたloopデバイス
        self.mnt: str | None = None           # マウント先のディレクトリ
        self.mounted: bool = False            # マウント済みか
        self.attempted: list[str] = []        # 作成を試みた設定名 (後始末の対象)
        self.errors: list[ErrorInfo] = []     # 収集した全てのエラー
        self.numbers: set[int] = set()        # 使ったスナップショット番号 (漏洩検査用)
        self.cleanup_errors: list[str] = []   # 後始末で起きた問題


STATE = State()                  # 実行時の状態の唯一のインスタンス
DBUS_MOD: Any = None             # 遅延importしたdbusモジュール
DBUS_EXC: Any = None             # dbus.exceptions.DBusException
OPS: Any = None                  # 検査対象インタフェースのプロキシ


def fail(message: str) -> NoReturn:
    """
    @brief 失敗を報告して処理を中断する

    Args:
        message: FAIL 行に出力する理由

    Raises:
        TestFailure: 常に送出する
    """
    raise TestFailure(message)


def proof(name: str, result: str) -> None:
    """
    @brief 場面ごとの検証結果を PROOF 行として出力する

    Args:
        name: 場面の識別名
        result: 観測した結果
    """
    print(f"PROOF_{name}: {result}", flush=True)


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    """
    @brief 外部コマンドを実行する

    Args:
        cmd: コマンドと引数
        check: Trueのとき、終了コードが0以外なら失敗にする

    Returns:
        完了したプロセス (標準出力と標準エラーは文字列)

    Raises:
        TestFailure: checkがTrueで終了コードが0以外の場合

    Note:
        コマンドが見つからない場合もTestFailureにして、後始末を確実に走らせる
    """
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as exc:
        fail(f"cannot run {cmd[0]}: {exc}")
    if check and proc.returncode != 0:
        fail(f"command failed ({proc.returncode}): {' '.join(cmd)}: {proc.stderr.strip()}")
    return proc


def load_dbus() -> bool:
    """
    @brief python3-dbusを読み込む

    Returns:
        読み込めた場合True

    Note:
        オプトインの判定より前にimportすると、スキップすべき環境でも
        python3-dbusの有無で終了コードが変わってしまうため、判定後に呼ぶ
    """
    global DBUS_MOD, DBUS_EXC
    try:
        DBUS_MOD = importlib.import_module("dbus")
        DBUS_EXC = importlib.import_module("dbus.exceptions").DBusException
    except ImportError:
        return False
    return True


def connect() -> None:
    """
    @brief システムバス上の検査対象サービスへ接続する

    Note:
        接続によりD-Bus activationでヘルパーが起動する
        プロキシをOPSへ保存する
    """
    global OPS
    bus = DBUS_MOD.SystemBus()
    proxy = bus.get_object(DBUS_SERVICE, DBUS_PATH)
    OPS = DBUS_MOD.Interface(proxy, DBUS_IFACE)


def call(method: str, *args: Any) -> Any:
    """
    @brief D-Busメソッドを呼び、成功を期待する

    Args:
        method: メソッド名
        *args: dbusの型に変換済みの引数

    Returns:
        メソッドの戻り値

    Raises:
        TestFailure: D-Busエラーが返った場合
    """
    try:
        return getattr(OPS, method)(*args, timeout=CALL_TIMEOUT_S)
    except DBUS_EXC as exc:
        fail(f"{method} raised {exc.get_dbus_name()}: {exc.get_dbus_message()}")


def is_dbus_true(value: Any) -> bool:
    """
    @brief D-Busのbool型の戻り値が真であるかを判定する

    Args:
        value: D-Busメソッドの戻り値

    Returns:
        bool型 (dbus.Boolean) の真であればTrue

    Note:
        python3-dbusはbを、intを継承したdbus.Booleanで返すため、
        Pythonのbool型の真との同一性 (is True) では成功しても一致しない
        1などの整数を誤って成功と扱わないよう、dbus.Booleanかbool型に限る
    """
    return isinstance(value, (bool, DBUS_MOD.Boolean)) and bool(value)


def expect_error(tag: str, method: str, *args: Any,
                 names: tuple[str, ...] | None = None) -> ErrorInfo:
    """
    @brief D-Busメソッドを呼び、エラーでの拒否を期待する

    Args:
        tag: 失敗時の識別用の名前
        method: メソッド名
        *args: dbusの型に変換済みの引数
        names: 許容するエラー名 (Noneの場合は本サービスまたは標準のD-Busエラーなら何でもよい)

    Returns:
        受け取ったエラー (STATE.errors にも追加する)

    Raises:
        TestFailure: 正常終了した場合、基盤の不具合を示すエラー名の場合、許容外のエラー名の場合

    Note:
        サービスが存在しない、メソッドが無い、応答が無いといった基盤の不具合を
        拒否と取り違えると、何も検証していないのに成功してしまうため区別する
    """
    try:
        getattr(OPS, method)(*args, timeout=CALL_TIMEOUT_S)
    except DBUS_EXC as exc:
        info = ErrorInfo(exc.get_dbus_name() or "", exc.get_dbus_message() or "")
        STATE.errors.append(info)
        if not info.name or any(info.name.startswith(p) for p in INFRA_ERRORS):
            fail(f"{tag}: infrastructure error {info.name}: {info.text}")
        if not info.name.startswith(ERR_PREFIXES):
            fail(f"{tag}: unexpected error name {info.name}")
        if names is not None and info.name not in names:
            fail(f"{tag}: error {info.name} not in {names}")
        return info
    fail(f"{tag}: {method} succeeded but a rejection was expected")


def ustr(value: str) -> Any:
    """
    @brief PythonのstrをD-Busのstring型へ変換する

    Args:
        value: 変換する文字列

    Returns:
        dbus.String
    """
    return DBUS_MOD.String(value)


def numbers_arg(values: list[int]) -> Any:
    """
    @brief 整数のリストをD-Busのau型へ変換する

    Args:
        values: スナップショット番号のリスト

    Returns:
        シグネチャuのdbus.Array
    """
    return DBUS_MOD.Array([DBUS_MOD.UInt32(v) for v in values], signature="u")


def empty_userdata() -> Any:
    """
    @brief 空のユーザデータ辞書を返す

    Returns:
        シグネチャssの空のdbus.Dictionary
    """
    return DBUS_MOD.Dictionary({}, signature="ss")


def list_configs() -> list[str]:
    """
    @brief snapperに登録済みの設定名を一覧する

    Returns:
        設定名のリスト

    Raises:
        TestFailure: snapperの実行または出力の解析に失敗した場合

    Note:
        ヘルパーの結果を信用せず、独立した経路 (snapper CLI) で確認するために使う
    """
    proc = run(["snapper", "--csvout", "list-configs"])
    reader = csv.DictReader(io.StringIO(proc.stdout))
    fields = {(f or "").strip().lower(): f for f in (reader.fieldnames or [])}
    if "config" not in fields:
        fail(f"unexpected list-configs output: {proc.stdout!r}")
    return [row[fields["config"]].strip() for row in reader if row[fields["config"]].strip()]


def snapshot_rows(config: str) -> dict[int, dict[str, str]]:
    """
    @brief 設定のスナップショットを番号ごとに一覧する

    Args:
        config: 設定名

    Returns:
        番号 (現在を表す0を除く) から、小文字の列名をキーにした行への辞書

    Raises:
        TestFailure: snapperの実行または出力の解析に失敗した場合

    Note:
        --no-dbusでsnapperdを経由せず、ディスク上の状態を読む
        snapperdは読み込んだconfigの一覧を保持し、ヘルパーがlibsnapperで直接作成したスナップショットを
        configを破棄するまで一覧に含めないため、snapperd経由では作成直後の結果を確認できない
    """
    proc = run(["snapper", "--no-dbus", "-c", config, "--csvout", "list",
                "--columns", "number,cleanup,userdata"])
    reader = csv.DictReader(io.StringIO(proc.stdout))
    rows: dict[int, dict[str, str]] = {}
    for raw in reader:
        row = {(k or "").strip().lower(): (v or "").strip() for k, v in raw.items()}
        if not row.get("number", "").isdigit():
            fail(f"unexpected snapshot list output: {proc.stdout!r}")
        number = int(row["number"])
        if number > 0:
            rows[number] = row
    return rows


def config_file(name: str) -> str:
    """
    @brief 設定名に対応する設定ファイルのパスを返す

    Args:
        name: 設定名

    Returns:
        設定ファイルの絶対パス
    """
    return os.path.join(CONFIG_DIR, name)


def track(name: str) -> str:
    """
    @brief 作成を試みる設定名を後始末の対象として記録する

    Args:
        name: 設定名

    Returns:
        引数のname

    Note:
        拒否されるはずの作成が誤って成功した場合でも、後始末で確実に消せるよう、
        呼び出しの前に記録する
    """
    if name not in STATE.attempted:
        STATE.attempted.append(name)
    return name


def assert_no_config(name: str) -> None:
    """
    @brief 設定が作られていないことを、snapperと設定ファイルの両方で確認する

    Args:
        name: 設定名

    Raises:
        TestFailure: 設定が存在する場合
    """
    if name in list_configs() or os.path.exists(config_file(name)):
        fail(f"configuration {name} exists but must not")


def assert_root_chain(path: str) -> None:
    """
    @brief パスの全ての親がroot所有で、他者に書き込まれないことを確認する

    Args:
        path: 確認する絶対パス

    Raises:
        TestFailure: root所有でない、またはgroupかotherに書き込める場合

    Note:
        信頼できない親を持つパスの拒否を検証する前提として、
        正常系の親が確かに信頼できる状態であることを保証する
    """
    current = path
    while True:
        info = os.lstat(current)
        if info.st_uid != 0 or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            fail(f"parent is not root-owned and private: {current}")
        parent = os.path.dirname(current)
        if parent == current:
            return
        current = parent


def vol(name: str) -> str:
    """
    @brief マウント先の下のパスを返す

    Args:
        name: マウント先からの相対名

    Returns:
        絶対パス
    """
    return os.path.join(STATE.mnt or "", name)


def setup_filesystem() -> None:
    """
    @brief loopback btrfsを作り、検査用のサブボリュームを用意する

    Raises:
        TestFailure: 外部コマンドが失敗した場合

    Note:
        mkdtempは0700で作るため、全ての親をroot所有0755へ明示的に揃える
        STATEへ資源を1つ作るたびに記録し、途中で失敗しても後始末が追えるようにする
    """
    os.makedirs(MOUNT_PARENT, mode=0o755, exist_ok=True)
    STATE.image_dir = tempfile.mkdtemp(prefix="qsvmtest-img-", dir=IMAGE_PARENT)
    os.chmod(STATE.image_dir, 0o755)
    STATE.image = os.path.join(STATE.image_dir, "disk.img")
    with open(STATE.image, "wb") as handle:
        handle.truncate(IMAGE_BYTES)

    STATE.loop = run(["losetup", "--find", "--show", STATE.image]).stdout.strip()
    run(["mkfs.btrfs", "-f", "-q", STATE.loop])

    STATE.mnt = tempfile.mkdtemp(prefix="qsvmtest-", dir=MOUNT_PARENT)
    os.chmod(STATE.mnt, 0o755)
    run(["mount", "-t", "btrfs", STATE.loop, STATE.mnt])
    STATE.mounted = True

    # マウント後のルートはmkfs由来の所有者と権限になるため、改めて揃える
    os.chown(STATE.mnt, 0, 0)
    os.chmod(STATE.mnt, 0o755)
    run(["btrfs", "subvolume", "create", vol("vol1")])
    run(["btrfs", "subvolume", "create", vol("vol2")])
    assert_root_chain(vol("vol1"))
    assert_root_chain(vol("vol2"))
    proof("SETUP", f"loop={STATE.loop} mount={STATE.mnt}")


def scenario_a_create() -> None:
    """
    @brief CreateConfigの成功と、結果の読み戻しを検証する

    Raises:
        TestFailure: 期待と異なる場合
    """
    name = track("qsvmtest1")
    result = call("CreateConfig", ustr(name), ustr(vol("vol1")))
    if not is_dbus_true(result):
        fail(f"CreateConfig returned {result!r}")
    if name not in list_configs():
        fail("snapper list-configs does not list qsvmtest1")
    settings = {str(k): str(v) for k, v in call("GetConfig", ustr(name)).items()}
    if settings.get("SUBVOLUME") != vol("vol1"):
        fail(f"SUBVOLUME mismatch: {settings.get('SUBVOLUME')!r}")
    if settings.get("FSTYPE") != "btrfs":
        fail(f"FSTYPE mismatch: {settings.get('FSTYPE')!r}")
    if not os.path.isdir(os.path.join(vol("vol1"), ".snapshots")):
        fail(".snapshots was not created")
    proof("CREATE_CONFIG", f"SUBVOLUME={settings['SUBVOLUME']} FSTYPE={settings['FSTYPE']}")


def scenario_b_duplicate() -> None:
    """
    @brief 同じsubvolumeへの重複作成が拒否されることを検証する

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        重複を許すと1つのsubvolumeに複数の設定が付き、タイマーやクリーンアップが
        同じスナップショット群を互いに削除し合うため
    """
    name = track("qsvmtest2")
    info = expect_error("duplicate", "CreateConfig", ustr(name), ustr(vol("vol1")))
    assert_no_config(name)
    if "qsvmtest1" not in list_configs():
        fail("qsvmtest1 disappeared after the duplicate attempt")
    proof("DUPLICATE", f"rejected {info.name}")


def scenario_c_existing_snapshots_dir() -> None:
    """
    @brief .snapshotsが既にあるsubvolumeへの作成が拒否されることを検証する

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        既存の .snapshots を流用すると、利用者または別の主体が置いた
        内容をsnapperの管理下へ取り込んでしまうため
    """
    name = track("qsvmtest3")
    snapshots_dir = os.path.join(vol("vol2"), ".snapshots")
    os.mkdir(snapshots_dir)
    try:
        info = expect_error("existing .snapshots", "CreateConfig", ustr(name), ustr(vol("vol2")))
        assert_no_config(name)
    finally:
        # 後続の場面でvol2を再利用するため、成否にかかわらず取り除く
        if os.path.isdir(snapshots_dir):
            os.rmdir(snapshots_dir)
    proof("EXISTING_SNAPSHOTS_DIR", f"rejected {info.name}")


def scenario_d_untrusted_paths() -> None:
    """
    @brief 信頼できないパスの指定が全て拒否されることを検証する

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        symlinkや非root所有の親を許すと、一般ユーザが差し替えた先のsubvolumeへ
        rootの権限で設定とスナップショットを作らせることができるため
    """
    os.symlink("vol2", vol("link"))
    userdir = vol("userdir")
    os.mkdir(userdir, 0o755)
    run(["btrfs", "subvolume", "create", os.path.join(userdir, "sv")])
    os.chown(userdir, NOBODY_UID, NOBODY_GID)
    os.mkdir(vol("plaindir"), 0o755)

    cases = [
        ("qsvmtest4", vol("link"), "symlink"),
        ("qsvmtest5", os.path.join(userdir, "sv"), "parent not root-owned"),
        ("qsvmtest7", vol("plaindir"), "plain directory"),
        ("qsvmtest8", "vol1", "relative path"),
        ("qsvmtest9", vol("vol1") + "\n", "newline in path"),
    ]
    results = []
    for name, path, label in cases:
        track(name)
        info = expect_error(label, "CreateConfig", ustr(name), ustr(path))
        assert_no_config(name)
        results.append(f"{label}={info.name.rsplit('.', 1)[-1]}")
    proof("UNTRUSTED_PATHS", ", ".join(results))


def scenario_e_root() -> None:
    """
    @brief rootの設定の作成と削除が認可の前に拒否されることを検証する

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        rootの設定が消えると復元の拠り所を失い取り返しがつかないため、
        認可を問い合わせる前にAccessDeniedまたはInvalidArgsで拒否される必要がある
        万一成功した場合は取り消せないので、その時点で失敗として中断する
    """
    before = sorted(list_configs())
    track("qsvmtest6")
    create = expect_error("create root", "CreateConfig", ustr("qsvmtest6"), ustr("/"))
    # 名前rootは空の名前の解決先でありDeleteConfigで消せないため、ルート以外のサブボリュームでも作成を拒否する
    # 後始末の対象に加えると既存のrootの設定を消すおそれがあるため、trackしない
    create_name = expect_error("create root name", "CreateConfig", ustr("root"), ustr(vol("vol2")),
                               names=(ERR_ACCESS_DENIED, ERR_INVALID_ARGS))
    delete = expect_error("delete root", "DeleteConfig", ustr("root"),
                          names=(ERR_ACCESS_DENIED, ERR_INVALID_ARGS))
    assert_no_config("qsvmtest6")
    if sorted(list_configs()) != before:
        fail("the configuration list changed during the root scenario")
    proof("ROOT", f"create={create.name} create_name={create_name.name} delete={delete.name}")


def check_error_bodies(label: str) -> None:
    """
    @brief 収集した全てのエラー本文に、秘匿すべき値が含まれないことを検証する

    Args:
        label: PROOF 行の識別名

    Raises:
        TestFailure: 本文に設定名, マウントパス, スナップショット番号が含まれる場合

    Note:
        エラー本文は呼び出し元へ返る固定文であるべきで、内部の設定名や
        パス、番号が混ざると、権限のない呼び出し元への情報漏洩になる
        番号は単語として一致した場合だけを漏洩とみなす
    """
    if not STATE.errors:
        fail("no error body was collected")
    mnt = STATE.mnt or ""
    secrets = [NAME_PREFIX, mnt, os.path.basename(mnt) if mnt else "",
               STATE.image_dir, STATE.image, STATE.loop]
    for info in STATE.errors:
        for secret in secrets:
            if secret and secret in info.text:
                fail(f"error body leaks {secret!r}: {info.name}: {info.text!r}")
        for number in STATE.numbers:
            if re.search(rf"\b{number}\b", info.text):
                fail(f"error body leaks snapshot number {number}: {info.name}: {info.text!r}")
    proof(label, f"{len(STATE.errors)} error bodies checked")


def create_snapshot(config: str, important: bool) -> int:
    """
    @brief スナップショットを1つ作り、その番号を返す

    Args:
        config: 設定名
        important: 重要フラグ

    Returns:
        新しく作られたスナップショットの番号

    Raises:
        TestFailure: 作成に失敗した、または新しい番号を特定できない場合

    Note:
        戻り値の本文は解析せず、作成前後のsnapperの一覧の差から番号を求める
        ヘルパーの応答形式に依存せず、実際に作られたものだけを確認できるため
    """
    before = set(snapshot_rows(config))
    call("CreateSnapshot", ustr(config), ustr("single"), ustr("vmtest"),
         DBUS_MOD.Int32(0), ustr("number"), empty_userdata(), DBUS_MOD.Boolean(important))
    created = set(snapshot_rows(config)) - before
    if len(created) != 1:
        fail(f"expected one new snapshot, got {sorted(created)}")
    number = created.pop()
    STATE.numbers.add(number)
    return number


def scenario_g_snapshots(config: str) -> tuple[list[int], int]:
    """
    @brief スナップショットを作成し、重要フラグとcleanupの反映を検証する

    Args:
        config: 設定名

    Returns:
        重要でないスナップショット番号のリストと、重要なスナップショットの番号

    Raises:
        TestFailure: 期待と異なる場合
    """
    plain = [create_snapshot(config, False) for _ in range(SNAPSHOT_COUNT)]
    important = create_snapshot(config, True)
    rows = snapshot_rows(config)
    row = rows[important]
    if "important=yes" not in row.get("userdata", ""):
        fail(f"important flag missing: {row!r}")
    if row.get("cleanup") != "number":
        fail(f"cleanup mismatch: {row!r}")
    for number in plain:
        if "important" in rows[number].get("userdata", ""):
            fail(f"snapshot {number} must not be important")
    proof("SNAPSHOTS", f"created={len(plain) + 1} important_userdata=yes cleanup=number")
    return plain, important


def scenario_h_delete(config: str, plain: list[int], important: int) -> list[int]:
    """
    @brief DeleteSnapshotsの重複除去と、不正入力で何も削除されないことを検証する

    Args:
        config: 設定名
        plain: 重要でないスナップショット番号 (5件以上)
        important: 重要なスナップショットの番号

    Returns:
        削除後に残っているスナップショット番号のリスト

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        一括削除が途中で止まった場合や不正入力の場合に、一部だけ削除された状態を
        残さないことが目的であるため、各拒否の後で一覧が変化していないことを確認する
    """
    n1, n2, n3 = plain[0], plain[1], plain[2]
    keep = plain[3]
    result = [int(x) for x in call("DeleteSnapshots", ustr(config),
                                    numbers_arg([n1, n2, n2, n3]))]
    if result != sorted({n1, n2, n3}):
        fail(f"DeleteSnapshots returned {result}, expected {sorted({n1, n2, n3})}")
    remaining = set(snapshot_rows(config))
    if remaining & {n1, n2, n3}:
        fail(f"deleted snapshots still listed: {sorted(remaining & {n1, n2, n3})}")
    expected = set(plain[3:]) | {important}
    if remaining != expected:
        fail(f"unexpected remaining snapshots: {sorted(remaining)} != {sorted(expected)}")

    unique_over = list(range(1000, 1000 + UNIQUE_LIMIT + 1))
    raw_over = [keep] * (RAW_LIMIT + 1)
    STATE.numbers.update({UNIQUE_LIMIT + 1, RAW_LIMIT + 1, MISSING_NUMBER})
    bad_cases = [
        ("zero", [0], (ERR_INVALID_ARGS,)),
        ("101 unique", unique_over, (ERR_LIMITS,)),
        ("1001 raw", raw_over, (ERR_LIMITS,)),
        ("nonexistent", [keep, MISSING_NUMBER], (ERR_FAILED,)),
    ]
    names = []
    for label, values, allowed in bad_cases:
        info = expect_error(f"DeleteSnapshots {label}", "DeleteSnapshots",
                            ustr(config), numbers_arg(values), names=allowed)
        if set(snapshot_rows(config)) != remaining:
            fail(f"snapshots changed after the rejected case: {label}")
        names.append(f"{label}={info.name.rsplit('.', 1)[-1]}")
    proof("DELETE_SNAPSHOTS", f"returned={result} " + ", ".join(names))
    return sorted(remaining)


def scenario_i_quota(config: str) -> None:
    """
    @brief SetupQuotaの成功とQGROUPの反映を検証する

    Args:
        config: 設定名

    Raises:
        TestFailure: 期待と異なる場合
    """
    result = call("SetupQuota", ustr(config))
    if not is_dbus_true(result):
        fail(f"SetupQuota returned {result!r}")
    settings = {str(k): str(v) for k, v in call("GetConfig", ustr(config)).items()}
    if not settings.get("QGROUP"):
        fail("QGROUP is empty after SetupQuota")
    proof("SETUP_QUOTA", f"QGROUP={settings['QGROUP']}")


def scenario_j_usage(config: str) -> None:
    """
    @brief GetFilesystemUsageのCSV形式を検証する

    Args:
        config: 設定名

    Raises:
        TestFailure: 期待と異なる場合
    """
    lines = str(call("GetFilesystemUsage", ustr(config))).split("\n")
    if lines[0] != USAGE_HEADER:
        fail(f"usage header mismatch: {lines[0]!r}")
    kinds = [line.split(",", 1)[0] for line in lines[1:]]
    for kind in ("info", "space", "device"):
        if kind not in kinds:
            fail(f"usage CSV has no {kind} row")
    device_rows = [line.split(",") for line in lines[1:] if line.startswith("device,")]
    total = int(device_rows[0][2])
    if not IMAGE_BYTES * 0.9 <= total <= IMAGE_BYTES * 1.1:
        fail(f"device total {total} is not about {IMAGE_BYTES}")
    proof("FILESYSTEM_USAGE", f"rows={len(lines) - 1} device_total={total}")


def scenario_k_subvolumes(config: str, remaining: list[int]) -> None:
    """
    @brief ListSubvolumesのCSV形式と、スナップショットの判別を検証する

    Args:
        config: 設定名
        remaining: 残っているスナップショット番号

    Raises:
        TestFailure: 期待と異なる場合
    """
    lines = str(call("ListSubvolumes", ustr(config))).split("\n")
    if lines[0] != SUBVOL_FIRST_LINE:
        fail(f"first line mismatch: {lines[0]!r}")
    if lines[1] != SUBVOL_HEADER:
        fail(f"header mismatch: {lines[1]!r}")
    rows = list(csv.DictReader(io.StringIO("\n".join(lines[1:]))))
    paths = [row["path"] for row in rows]
    for expected in ("vol1", "vol2"):
        if expected not in paths:
            fail(f"{expected} is missing from the subvolume list: {paths}")
    snapper_rows = [row for row in rows if row["snapper"] == "1"]
    if not snapper_rows:
        fail("no row is marked as a snapper snapshot")
    proof("LIST_SUBVOLUMES",
          f"rows={len(rows)} snapper_rows={len(snapper_rows)} remaining_snapshots={len(remaining)}")


def scenario_l_delete_config(config: str, remaining: list[int]) -> None:
    """
    @brief DeleteConfigの成功と、設定の消滅を検証する

    Args:
        config: 設定名
        remaining: 残っているスナップショット番号

    Raises:
        TestFailure: 期待と異なる場合

    Note:
        スナップショットが残ったままの設定削除は、snapperが.snapshotsを消せずに
        失敗し得るため、先にDeleteSnapshotsで空にしてから削除する
    """
    deleted = [int(x) for x in call("DeleteSnapshots", ustr(config), numbers_arg(remaining))]
    if deleted != sorted(remaining):
        fail(f"final DeleteSnapshots returned {deleted}, expected {sorted(remaining)}")
    result = call("DeleteConfig", ustr(config))
    if not is_dbus_true(result):
        fail(f"DeleteConfig returned {result!r}")
    if config in list_configs():
        fail("snapper list-configs still lists the deleted configuration")
    if os.path.exists(config_file(config)):
        fail("the configuration file still exists")
    proof("DELETE_CONFIG", "removed")


def cleanup_configs() -> None:
    """
    @brief 作成を試みた設定のうち、残っているものを片付ける

    Note:
        既存の設定には触れないよう、このテストが作成を試みた名前だけを対象にする
        スナップショットが残ると設定を削除できないため、先にスナップショットを消す
        失敗はSTATE.cleanup_errorsへ記録し、残りの後始末を続ける
    """
    try:
        existing = set(list_configs())
    except TestFailure as exc:
        STATE.cleanup_errors.append(f"cannot list configs: {exc}")
        return
    for name in STATE.attempted:
        if name not in existing:
            continue
        try:
            rows = snapshot_rows(name)
            if rows:
                run(["snapper", "-c", name, "delete"] + [str(n) for n in sorted(rows)])
            run(["snapper", "-c", name, "delete-config"])
        except TestFailure as exc:
            STATE.cleanup_errors.append(f"cannot remove config {name}: {exc}")


def cleanup_filesystem() -> None:
    """
    @brief アンマウント, loopデバイスの解放, 一時ファイルの削除を行う

    Note:
        通常のumountが失敗した場合は遅延アンマウントへ切り替える
        loopデバイスを使い続けたまま残すと、後続の実行でデバイスを消費し続けるため
        失敗はSTATE.cleanup_errorsへ記録し、残りの後始末を続ける
    """
    mnt = STATE.mnt
    loop = STATE.loop
    if STATE.mounted and mnt:
        if run(["umount", mnt], check=False).returncode == 0:
            STATE.mounted = False
        elif run(["umount", "-l", mnt], check=False).returncode == 0:
            STATE.mounted = False
            STATE.cleanup_errors.append(f"lazy unmount was needed for {mnt}")
        else:
            STATE.cleanup_errors.append(f"cannot unmount {mnt}")
    if loop:
        if run(["losetup", "-d", loop], check=False).returncode != 0:
            STATE.cleanup_errors.append(f"cannot detach {loop}")
    if STATE.image_dir:
        shutil.rmtree(STATE.image_dir, ignore_errors=True)
        if os.path.exists(STATE.image_dir):
            STATE.cleanup_errors.append(f"cannot remove {STATE.image_dir}")
    # マウントが残っている間はrmdirしない (マウント先の中身を消す事故を避ける)
    if mnt and not STATE.mounted:
        try:
            os.rmdir(mnt)
        except OSError as exc:
            STATE.cleanup_errors.append(f"cannot remove {mnt}: {exc}")


def cleanup() -> bool:
    """
    @brief 全ての後始末を行い、問題の有無を返す

    Returns:
        後始末が全て成功した場合True
    """
    for step in (cleanup_configs, cleanup_filesystem):
        try:
            step()
        except Exception as exc:  # 後始末は途中で止めず、残りの段階を続ける
            STATE.cleanup_errors.append(f"{step.__name__}: {exc}")
    for problem in STATE.cleanup_errors:
        print(f"CLEANUP_PROBLEM: {problem}", file=sys.stderr)
    return not STATE.cleanup_errors


def check_environment() -> str | None:
    """
    @brief 実行に必要な外部コマンドが揃っているか確認する

    Returns:
        不足がある場合は不足したコマンド名、全て揃っていればNone
    """
    for tool in ("losetup", "mkfs.btrfs", "mount", "umount", "btrfs", "snapper"):
        if shutil.which(tool) is None:
            return tool
    return None


def run_scenarios() -> None:
    """
    @brief 全ての場面を順に実行する

    Raises:
        TestFailure: いずれかの場面が期待と異なる場合
    """
    config = "qsvmtest1"
    setup_filesystem()
    connect()
    scenario_a_create()
    scenario_b_duplicate()
    scenario_c_existing_snapshots_dir()
    scenario_d_untrusted_paths()
    scenario_e_root()
    check_error_bodies("ERROR_BODIES")
    plain, important = scenario_g_snapshots(config)
    remaining = scenario_h_delete(config, plain, important)
    scenario_i_quota(config)
    scenario_j_usage(config)
    scenario_k_subvolumes(config, remaining)
    scenario_l_delete_config(config, remaining)
    check_error_bodies("ERROR_BODIES_ALL")


def main(argv: list[str] | None = None) -> int:
    """
    @brief オプトインを確認し、場面を実行して後始末する

    Args:
        argv: コマンドライン引数 (Noneの場合はsys.argvを使う)

    Returns:
        終了コード (0, 1, 2, 77)

    Note:
        既存の設定を壊さないため、qsvmtest で始まる設定がある場合は
        後始末の対象を作る前に実行を拒否する
    """
    parser = argparse.ArgumentParser(
        description="Run the privileged configuration tests on a real loopback btrfs "
                    "(VM only, never run by ctest)",
        epilog=f"Requires {OPT_IN_ENV}=1 and root. Exit codes: 0 ok, 1 failure, "
               "2 python3-dbus missing, 77 skipped")
    parser.parse_args(argv)

    if os.environ.get(OPT_IN_ENV) != "1":
        print(f"SKIP: set {OPT_IN_ENV}=1 to run this destructive VM-only test")
        return EXIT_SKIP
    if os.geteuid() != 0:
        print("SKIP: root privileges are required")
        return EXIT_SKIP
    if not load_dbus():
        print("ERROR: python3-dbus required", file=sys.stderr)
        return EXIT_NO_DBUS

    try:
        missing = check_environment()
        if missing:
            fail(f"required command not found: {missing}")
        existing = [c for c in list_configs() if c.startswith(NAME_PREFIX)]
        if existing:
            fail(f"refusing to run: configurations already exist: {existing}")
    except TestFailure as exc:
        print(f"FAIL: {exc}")
        return EXIT_FAIL

    ok = False
    try:
        run_scenarios()
        ok = True
    except TestFailure as exc:
        print(f"FAIL: {exc}")
    except KeyboardInterrupt:
        print("FAIL: interrupted")
    except Exception as exc:  # 想定外の例外でも後始末を確実に実行して報告する
        print(f"FAIL: unexpected {type(exc).__name__}: {exc}")
    finally:
        cleaned = cleanup()

    if ok and cleaned:
        print("VM_CONFIGS_BTRFS: ALL_OK")
        return EXIT_OK
    if ok:
        print("FAIL: cleanup did not complete")
    return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main())
