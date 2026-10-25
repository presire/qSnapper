#!/usr/bin/env python3
"""
@file test_configs_bulk.py
@brief 設定の作成と削除、スナップショットの一括削除、容量とサブボリュームの照会を、隔離したroot D-Bus上で検証する

tests/integration/isolated_root_dbus_env.shが起動する専用root D-Busバス上で、
本番のSnapshotOperationsをリンクしたqsnapper_restoregate_test_serviceを起動し、
新しい特権メソッドの入力検証、認可、排他、失敗時の本文を検査する
    - CreateConfig      : 設定名とsubvolumeの検証は認可の前、rootの拒否、認可拒否、復元中とsnapperd操作中の拒否、本文の失敗
    - DeleteConfig      : 設定名の検証は認可の前、rootの拒否、認可拒否、復元中とsnapperd操作中の拒否、本文の失敗
    - DeleteSnapshots   : 番号の検証と重複の除去は認可の前、認可拒否、復元中とsnapperd操作中の拒否、本文の失敗
    - GetFilesystemUsage: 設定名の検証は認可の前、認可拒否、復元中とsnapperd操作中の拒否、本文の失敗
    - ListSubvolumes    : 設定名の検証は認可の前、認可拒否、復元中とsnapperd操作中の拒否、本文の失敗

config名とpathと番号には、応答へ漏れていないことを確かめるための目印を混ぜる
実在するconfig (root など) は読み取りの対象にもせず、実システムの/etc/snapperへは触れない
本文が隔離環境で必ず失敗するのは、root所有の設定ファイルが利用者名前空間では別のuidに見えるためで、
これはfail-closedとして期待する結果である

tests/integration/run_isolated_restore_plan_test.sh経由でのみ実行すること
このランナーが、本スクリプトを起動する前に専用バスを起動し、QSNAPPER_TEST_READY_FILEを設定する

使い方 : python3 test_configs_bulk.py <qsnapper_restoregate_test_serviceのパス>
終了コード : 0 = 全検査成功、1 = 検査失敗、2 = python3-dbus未導入、64 = 引数不正
"""

from __future__ import annotations

import os
import subprocess
import sys

# 同じディレクトリの共通部品 (サービス制御、PROOF出力、遅延認可の呼び出し) を再利用する
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_restore_gate import (  # noqa: E402
    CALL_TIMEOUT_S, IFACE, PATH, RESTORE_IN_PROGRESS, RESTORE_IN_PROGRESS_TEXT,
    ServiceControl, call_deferred, fail, proof, wait_for_ready,
)

import dbus  # noqa: E402
from dbus.exceptions import DBusException  # noqa: E402

ERR_INVALID_ARGS = "org.freedesktop.DBus.Error.InvalidArgs"               # 入力検証の失敗
ERR_FAILED = "org.freedesktop.DBus.Error.Failed"                          # 本文の失敗
ERR_ACCESS_DENIED = "org.freedesktop.DBus.Error.AccessDenied"             # 認可の拒否
ERR_LIMITS = "org.freedesktop.DBus.Error.LimitsExceeded"                  # 件数上限の超過
ERR_NOT_SUPPORTED = "org.freedesktop.DBus.Error.NotSupported"             # 対象外の設定やファイルシステム
ERR_OPERATION_IN_PROGRESS = "com.presire.qsnapper.Error.OperationInProgress"   # 構成変更の排他
ERR_OPERATION_IN_PROGRESS_TEXT = "Another configuration change is in progress"  # 排他の本文

INVALID_NAME_TEXT = "Invalid configName"                                  # 設定名の検証失敗
INVALID_SUBVOLUME_TEXT = "Invalid subvolume path"                         # subvolumeの検証失敗
ROOT_CONFIG_TEXT = "The root configuration cannot be changed here"        # root設定の拒否
INVALID_NUMBERS_TEXT = "Invalid snapshot numbers"                         # 番号の検証失敗
TOO_MANY_NUMBERS_TEXT = "Too many snapshot numbers"                       # 番号の件数超過
CONFIG_FILES_TEXT = "The Snapper configuration files could not be verified"  # 設定ファイルの検証失敗
SNAPPER_ERROR_TEXT = "Snapper reported an error"                          # snapperの失敗
CONFIG_NOT_FOUND_TEXT = "Configuration not found"                         # 設定が見つからない
NOT_BTRFS_TEXT = "The filesystem is not Btrfs"                            # Btrfs以外のファイルシステム
AUTHORIZATION_TEXT = "Authorization failed"                               # 認可の拒否

# 応答へ漏れていないことを確かめるための目印
# 設定名とsubvolumeと番号のすべてに現れる文字列を禁じ、どこか1箇所でも転送されれば検出できるようにする
SENTINEL = "SENTINELcfg"
SENTINEL_NUMBER = 4242
FORBIDDEN_TEXT = (SENTINEL, str(SENTINEL_NUMBER))

NONEXISTENT_CONFIG = SENTINEL                                             # 実在しないconfig名として使う目印
SUBVOLUME = "/var/lib/" + SENTINEL                                        # 検証を通るが本文では使われないpath
VALID_NAME = "qsnapper-" + SENTINEL                                       # 検証を通る設定名
SHORT_TIMEOUT_S = 5.0                                                     # 認可へ進んだことを短時間で見抜く待ち時間


def rpc(obj, method: str, args: list, timeout: float = CALL_TIMEOUT_S) -> tuple:
    """
    @brief D-Busメソッドを同期で呼び、成功値かエラー名と本文を返す

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        method: 呼び出すメソッド名
        args: メソッドへ渡す引数
        timeout: 応答を待つ秒数

    Returns:
        成功した場合は ("OK", 戻り値)、失敗した場合は (エラー名, エラー本文)
    """
    try:
        result = getattr(obj, method)(*args, dbus_interface=IFACE, timeout=timeout)
        return ("OK", result)
    except DBusException as exc:
        return (exc.get_dbus_name() or "", exc.get_dbus_message() or "")


def contains_forbidden(result: tuple) -> bool:
    """
    @brief エラーの本文に目印が含まれていないかを調べる

    Args:
        result: rpcの戻り値

    Returns:
        目印が1つでも含まれていればTrue
    """
    text = str(result[1])
    return any(word in text for word in FORBIDDEN_TEXT)


def check_error(tag: str, result: tuple, name: str, text: str) -> None:
    """
    @brief エラー名と本文が期待どおりで、目印を含まないことを必須とする

    Args:
        tag: 検査を特定する表示名
        result: rpcの戻り値
        name: 期待するエラー名
        text: 期待するエラー本文
    """
    if result[0] != name or result[1] != text:
        fail(f"{tag}: expected ({name}, {text!r}) but got {result!r}")
    # 入力値がエラー本文へ混ざると、呼び出し元へ内部の名前やpathが漏れるため
    if contains_forbidden(result):
        fail(f"{tag}: error text leaked an input value: {result[1]!r}")
    proof(f"{tag}_error", result[0])
    proof(f"{tag}_text", result[1])


def check_error_name(tag: str, result: tuple, name: str, texts: tuple) -> None:
    """
    @brief エラー名が期待どおりで、本文が許容した候補のいずれかであることを必須とする

    Args:
        tag: 検査を特定する表示名
        result: rpcの戻り値
        name: 期待するエラー名
        texts: 許容するエラー本文の候補
    """
    if result[0] != name or result[1] not in texts:
        fail(f"{tag}: expected ({name}, one of {texts!r}) but got {result!r}")
    if contains_forbidden(result):
        fail(f"{tag}: error text leaked an input value: {result[1]!r}")
    proof(f"{tag}_error", result[0])
    proof(f"{tag}_text", result[1])


def reject_before_auth(obj, control: ServiceControl, method: str, cases: list,
                       expected_name: str, expected_text: str, tag: str) -> None:
    """
    @brief 不正な入力が認可を1件も要求せずに拒否されることを必須とする

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
        method: 呼び出すメソッド名
        cases: (表示名, 引数) の並び
        expected_name: 期待するエラー名
        expected_text: 期待するエラー本文
        tag: 検査を特定する表示名
    """
    # 認可を保留させたまま呼び出し、保留件数が増えないことで認可の前の拒否を確かめる
    control.expect_ok("auth defer")
    try:
        for label, args in cases:
            result = rpc(obj, method, args, SHORT_TIMEOUT_S)
            pending = control.pending()
            # 認可へ進んだ呼び出しは保留が1件残るため、拒否より後に検証しているとここで分かる
            if pending != 0:
                fail(f"{tag}/{label}: reached authorization (pending={pending})")
            check_error(f"{tag}/{label}", result, expected_name, expected_text)
    finally:
        # 途中で失敗しても保留を残さない
        control.expect_ok("auth grant")


def check_denied(obj, control: ServiceControl, method: str, args: list, tag: str) -> None:
    """
    @brief 認可を拒否した場合にAccessDeniedで終わり、本文へ入力値が漏れないことを必須とする

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
        method: 呼び出すメソッド名
        args: メソッドへ渡す引数
        tag: 検査を特定する表示名
    """
    result = call_deferred(obj, control, method, args, [], "deny")
    check_error(tag, result, ERR_ACCESS_DENIED, AUTHORIZATION_TEXT)


def check_restore_blocks(obj, control: ServiceControl, method: str, args: list,
                         tag: str) -> None:
    """
    @brief 認可待ちの間に復元が始まった場合にRestoreInProgressで終わることを必須とする

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
        method: 呼び出すメソッド名
        args: メソッドへ渡す引数
        tag: 検査を特定する表示名
    """
    try:
        # 復元は認可待ちに入ってから始める
        # 認可要求より先に復元を始めると復元ゲートが認可自体を止めてしまい
        # 認可待ちの間に復元が始まった場合の検証にならない
        result = call_deferred(obj, control, method, args,
                               ["restore begin " + VALID_NAME], "grant")
        check_error(tag, result, RESTORE_IN_PROGRESS, RESTORE_IN_PROGRESS_TEXT)
    finally:
        # 例外で抜けても次の検査へ復元状態を持ち越さない
        control.expect_ok("restore end")


def test_bad_names(obj, control: ServiceControl) -> None:
    """
    @brief 設定名の不正な文字と長さが認可の前に拒否されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    # どちらの検証でも危険な文字と要素は共通して拒否する
    common_bad_names = [
        ("empty", ""),
        ("leading-hyphen", "-" + SENTINEL),
        ("dot", "."),
        ("dotdot", ".."),
        ("slash", SENTINEL + "/x"),
        ("space", SENTINEL + " x"),
        ("newline", SENTINEL + "\n"),
        ("crlf", SENTINEL + "\r\n"),
        ("double-quote", SENTINEL + '"x'),
        ("single-quote", SENTINEL + "'x"),
        ("dollar", SENTINEL + "$x"),
        ("backslash", SENTINEL + "\\x"),
        ("control", SENTINEL + "\x01x"),
        ("non-ascii", SENTINEL + "\u00e9"),
    ]
    # 新規設定名は設定ファイル名にもなるため先頭underscoreとdotを拒否し64文字までに制限する
    new_name_cases = common_bad_names + [
        ("leading-underscore", "_" + SENTINEL),
        ("dot-inside", SENTINEL + ".x"),
        ("length-65", "a" * 65),
    ]
    cases = [(label, [name, SUBVOLUME]) for label, name in new_name_cases]
    reject_before_auth(obj, control, "CreateConfig", cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "CreateConfig/name")

    # 既存設定名の検証はsnapperの設定名規則に合わせて255文字まで許容する
    config_bad_names = common_bad_names + [("length-256", "a" * 256)]
    # DeleteConfigは空文字をrootへ読み替えないため空文字も拒否対象に残す
    delete_cases = [(label, [name]) for label, name in config_bad_names]
    reject_before_auth(obj, control, "DeleteConfig", delete_cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "DeleteConfig/name")

    # 設定を受け取る読み取り系は空文字をrootとして解決するため空文字を除外する
    resolved_cases = [(label, [name]) for label, name in config_bad_names if label != "empty"]
    reject_before_auth(obj, control, "GetFilesystemUsage", resolved_cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "GetFilesystemUsage/name")
    reject_before_auth(obj, control, "ListSubvolumes", resolved_cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "ListSubvolumes/name")

    number_cases = [(label, [name, dbus.Array([SENTINEL_NUMBER], signature="u")])
                    for label, name in config_bad_names if label != "empty"]
    reject_before_auth(obj, control, "DeleteSnapshots", number_cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "DeleteSnapshots/name")


def test_bad_subvolume(obj, control: ServiceControl) -> None:
    """
    @brief subvolumeのpathとして不正な値が認可の前に拒否されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    bad_paths = [
        ("empty", ""),
        ("relative", "home"),
        ("double-slash", "//x"),
        ("trailing-slash", "/x/"),
        ("dot-component", "/x/./y"),
        ("dotdot-component", "/x/../y"),
        ("space", "/x y"),
        ("double-quote", '/x"'),
        ("single-quote", "/x'"),
        ("dollar", "/x$y"),
        ("backslash", "/x\\y"),
        ("control", "/x\x01y"),
        ("too-many-bytes", "/" + "a" * 4095),
        ("too-long-component", "/" + "a" * 256),
    ]
    cases = [(label, [VALID_NAME, path]) for label, path in bad_paths]
    reject_before_auth(obj, control, "CreateConfig", cases,
                       ERR_INVALID_ARGS, INVALID_SUBVOLUME_TEXT, "CreateConfig/subvolume")


def test_root_refusal(obj, control: ServiceControl) -> None:
    """
    @brief rootの設定をこの経路から変更できないことを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    # 空の設定名はrootへ読み替えられるため、明示したrootだけを拒否する
    # rootという名前は、別のsubvolumeを指定しても作成させない
    cases = [
        ("root-subvolume", [VALID_NAME, "/"]),
        ("root-name", ["root", "/srv/qsnapper-test-subvolume"]),
    ]
    reject_before_auth(obj, control, "CreateConfig", cases,
                       ERR_ACCESS_DENIED, ROOT_CONFIG_TEXT, "CreateConfig/root")

    cases = [("root-name", ["root"])]
    reject_before_auth(obj, control, "DeleteConfig", cases,
                       ERR_ACCESS_DENIED, ROOT_CONFIG_TEXT, "DeleteConfig/root")


def test_bad_numbers(obj, control: ServiceControl) -> None:
    """
    @brief スナップショット番号の不正値と件数上限が認可の前に拒否されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    def numbers(values: list) -> dbus.Array:
        """
        @brief 32bit符号なし整数の配列を作る

        Args:
            values: 番号の並び

        Returns:
            auシグネチャのD-Bus配列
        """
        return dbus.Array(values, signature="u")

    invalid = [
        ("empty", []),
        ("zero", [0]),
        ("zero-mixed", [SENTINEL_NUMBER, 0]),
        # auは符号なしのため負値はD-Busの移送で拒否されサービスまで届かない
    ]
    cases = [(label, [NONEXISTENT_CONFIG, numbers(values)]) for label, values in invalid]
    reject_before_auth(obj, control, "DeleteSnapshots", cases,
                       ERR_INVALID_ARGS, INVALID_NUMBERS_TEXT, "DeleteSnapshots/numbers")

    too_many = [
        ("raw-over-limit", list(range(1, 1002))),
        ("unique-over-limit", list(range(1, 102))),
    ]
    cases = [(label, [NONEXISTENT_CONFIG, numbers(values)]) for label, values in too_many]
    reject_before_auth(obj, control, "DeleteSnapshots", cases,
                       ERR_LIMITS, TOO_MANY_NUMBERS_TEXT, "DeleteSnapshots/limits")


def test_duplicates_accepted(obj, control: ServiceControl) -> None:
    """
    @brief 重複を除いた件数が上限以内なら認可まで進むことを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    # 101件でも重複を除いて100件なら受け付け、認可の拒否で止まることを確かめる
    values = dbus.Array(list(range(1, 101)) + [1], signature="u")
    result = call_deferred(obj, control, "DeleteSnapshots",
                           [NONEXISTENT_CONFIG, values], [], "deny")
    check_error("DeleteSnapshots/duplicates", result, ERR_ACCESS_DENIED, AUTHORIZATION_TEXT)


def test_denied(obj, control: ServiceControl) -> None:
    """
    @brief 5つのメソッドが認可の拒否でAccessDeniedになることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    check_denied(obj, control, "CreateConfig", [VALID_NAME, SUBVOLUME], "CreateConfig/deny")
    check_denied(obj, control, "DeleteConfig", [VALID_NAME], "DeleteConfig/deny")
    check_denied(obj, control, "DeleteSnapshots",
                 [NONEXISTENT_CONFIG, dbus.Array([SENTINEL_NUMBER], signature="u")],
                 "DeleteSnapshots/deny")
    check_denied(obj, control, "GetFilesystemUsage", [NONEXISTENT_CONFIG],
                 "GetFilesystemUsage/deny")
    check_denied(obj, control, "ListSubvolumes", [NONEXISTENT_CONFIG],
                 "ListSubvolumes/deny")


def test_restore_pending(obj, control: ServiceControl) -> None:
    """
    @brief 認可待ちの間に復元が始まると5つのメソッドが中断されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    check_restore_blocks(obj, control, "CreateConfig", [VALID_NAME, SUBVOLUME],
                         "CreateConfig/restore")
    check_restore_blocks(obj, control, "DeleteConfig", [VALID_NAME],
                         "DeleteConfig/restore")
    check_restore_blocks(obj, control, "DeleteSnapshots",
                         [NONEXISTENT_CONFIG, dbus.Array([SENTINEL_NUMBER], signature="u")],
                         "DeleteSnapshots/restore")
    check_restore_blocks(obj, control, "GetFilesystemUsage", [NONEXISTENT_CONFIG],
                         "GetFilesystemUsage/restore")
    check_restore_blocks(obj, control, "ListSubvolumes", [NONEXISTENT_CONFIG],
                         "ListSubvolumes/restore")


def snapperd_blocked_calls() -> list:
    """
    @brief snapperd操作中に拒否されるべき呼び出しの一覧を返す

    Returns:
        (表示名, メソッド名, 引数) の並び
    """
    return [
        ("CreateConfig", "CreateConfig", [VALID_NAME, SUBVOLUME]),
        ("DeleteConfig", "DeleteConfig", [VALID_NAME]),
        ("DeleteSnapshots", "DeleteSnapshots",
         [NONEXISTENT_CONFIG, dbus.Array([SENTINEL_NUMBER], signature="u")]),
        ("GetFilesystemUsage", "GetFilesystemUsage", [NONEXISTENT_CONFIG]),
        ("ListSubvolumes", "ListSubvolumes", [NONEXISTENT_CONFIG]),
        ("ListSnapshots", "ListSnapshots", [NONEXISTENT_CONFIG]),
        ("GetConfig", "GetConfig", [NONEXISTENT_CONFIG]),
        ("CreateSnapshot", "CreateSnapshot",
         [NONEXISTENT_CONFIG, "single", "bulk test", 0, "number",
          dbus.Dictionary({}, signature="ss"), True]),
        ("BeginRestorePlan", "BeginRestorePlan", [NONEXISTENT_CONFIG, SENTINEL_NUMBER, 0, "all"]),
        ("SetTimerEnabled", "SetTimerEnabled", ["snapper-timeline.timer", True]),
    ]


def test_snapperd_in_progress(obj, control: ServiceControl) -> None:
    """
    @brief snapperd操作中は構成を変える呼び出しが認可の前に拒否されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    control.expect_ok("snapperd begin")
    try:
        # 認可を保留させ、拒否が認可より前であることを保留件数で確かめる
        control.expect_ok("auth defer")
        try:
            for label, method, args in snapperd_blocked_calls():
                result = rpc(obj, method, args, SHORT_TIMEOUT_S)
                pending = control.pending()
                if pending != 0:
                    fail(f"snapperd/{label}: reached authorization (pending={pending})")
                check_error(f"snapperd/{label}", result,
                            ERR_OPERATION_IN_PROGRESS, ERR_OPERATION_IN_PROGRESS_TEXT)
        finally:
            control.expect_ok("auth grant")

        # ListConfigsは排他の対象外であり、構成の一覧は常に読める必要がある
        result = rpc(obj, "ListConfigs", [])
        if result[0] != "OK":
            fail(f"ListConfigs while snapperd is busy failed: {result!r}")
        proof("snapperd/ListConfigs", result[0])
    finally:
        control.expect_ok("snapperd end")


def test_snapperd_after_grant(obj, control: ServiceControl) -> None:
    """
    @brief 認可待ちの間にsnapperd操作が始まると、認可が下りても拒否されることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    try:
        # snapperd操作は認可の解決後も続けておく
        # 認可が下りた時点で動作中でなければ拒否を検証できない
        result = call_deferred(obj, control, "CreateConfig",
                               [VALID_NAME, SUBVOLUME], ["snapperd begin"], "grant")
        check_error("snapperd/after-grant", result,
                    ERR_OPERATION_IN_PROGRESS, ERR_OPERATION_IN_PROGRESS_TEXT)
    finally:
        control.expect_ok("snapperd end")


def test_snapperd_end_restores(obj, control: ServiceControl) -> None:
    """
    @brief snapperd操作の終了後に通常の検証と認可の経路へ戻ることを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    cases = [("after-end", [SENTINEL + "\n", SUBVOLUME])]
    reject_before_auth(obj, control, "CreateConfig", cases,
                       ERR_INVALID_ARGS, INVALID_NAME_TEXT, "snapperd/ended")
    check_denied(obj, control, "ListSubvolumes", [NONEXISTENT_CONFIG],
                 "snapperd/ended-auth")


def test_post_auth_failures(obj, control: ServiceControl) -> None:
    """
    @brief 認可後の本文が隔離環境でfail-closedに失敗することを検証する

    Args:
        obj: 呼び出し対象のD-Busオブジェクト
        control: テスト用サービスの制御
    """
    control.expect_ok("auth grant")
    result = rpc(obj, "CreateConfig", [VALID_NAME, SUBVOLUME])
    check_error("CreateConfig/post-auth", result, ERR_FAILED, CONFIG_FILES_TEXT)

    result = rpc(obj, "DeleteConfig", [VALID_NAME])
    check_error("DeleteConfig/post-auth", result, ERR_FAILED, CONFIG_FILES_TEXT)

    result = rpc(obj, "DeleteSnapshots",
                 [NONEXISTENT_CONFIG, dbus.Array([SENTINEL_NUMBER], signature="u")])
    check_error("DeleteSnapshots/post-auth", result, ERR_FAILED, SNAPPER_ERROR_TEXT)

    # configが存在しないため、Btrfsの判定より前に設定の読み込みで失敗する
    result = rpc(obj, "GetFilesystemUsage", [NONEXISTENT_CONFIG])
    check_error_name("GetFilesystemUsage/post-auth", result, ERR_NOT_SUPPORTED,
                     (CONFIG_NOT_FOUND_TEXT, NOT_BTRFS_TEXT))

    result = rpc(obj, "ListSubvolumes", [NONEXISTENT_CONFIG])
    check_error_name("ListSubvolumes/post-auth", result, ERR_NOT_SUPPORTED,
                     (CONFIG_NOT_FOUND_TEXT, NOT_BTRFS_TEXT))


def main() -> int:
    """
    @brief 新しい特権メソッドの検査シナリオを実行する

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
        print("SETUP: configuration bulk service is READY")
        control = ServiceControl(service)

        bus = dbus.SystemBus(private=True)
        obj = bus.get_object(IFACE, PATH)

        test_bad_names(obj, control)
        test_bad_subvolume(obj, control)
        test_root_refusal(obj, control)
        test_bad_numbers(obj, control)
        test_duplicates_accepted(obj, control)
        test_denied(obj, control)
        test_restore_pending(obj, control)
        test_snapperd_in_progress(obj, control)
        test_snapperd_after_grant(obj, control)
        test_snapperd_end_restores(obj, control)
        test_post_auth_failures(obj, control)

        print("CONFIGS_BULK: ALL_OK")
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
