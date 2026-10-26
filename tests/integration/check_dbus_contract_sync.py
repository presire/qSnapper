#!/usr/bin/env python3
"""
@file check_dbus_contract_sync.py
@brief qSnapperのD-Bus契約とバスポリシー許可リストの同期を検査する

背景 :
    dbus/com.presire.qsnapper.Operations.xmlはrootサービスが公開するD-Busメソッドを宣言する
    dbus/com.presire.qsnapper.Operations.confは既定拒否のバスポリシーであり、サービスへの全method_callを拒否したうえで
    公開APIのメンバだけを<allow send_member="..."/>で再許可する
    XMLにメソッドを追加してconfへ反映し忘れると、正規のクライアントがバスデーモンから
    org.freedesktop.DBus.Error.AccessDeniedを受け取る
    XMLからメソッドを削除してconfに残すと、死んだ許可エントリが利益なく攻撃面を広げる
    両ファイルは常に同期していなければならない

さらに段階的復元の契約は次のセキュリティ特性を定める
    CommitRestorePlanが計画を凍結して認可した後は、凍結後のメソッドはパス、変更種別、設定名、スナップショット番号、復元モードを受け付けてはならない
    受け付けてよいのはマニフェスト識別子のみである
    本チェッカーはこの特性を機械的に検査し、凍結後のメソッドへ可変パラメータを追加する将来の変更が
    境界を静かに弱めるのではなくここで失敗するようにする

また削除済みのAuthenticateとQuitがXMLやconfへ再び現れないことも検査する
    実行時のPoCは既定拒否のバスポリシーによる拒否とメソッド不在を区別できないため、不在の保証はここで行う

本スクリプトはPython 3標準ライブラリ (xml.etree.ElementTree) のみを使うためdbusモジュールなしでどこでも実行できる

使い方 : check_dbus_contract_sync.py [--xml FILE] [--conf FILE]
終了コード : 0 = 契約違反なし、1 = 契約違反または解析エラー
"""

from __future__ import annotations

import argparse
import os
import sys
import xml.etree.ElementTree as ET

INTERFACE = "com.presire.qsnapper.Operations"   # 検査対象のD-Busインタフェース名

# 凍結後のメソッド : CommitRestorePlanが凍結済み計画を認可した後、これらは
# マニフェスト識別子のみ (direction="in"の引数1個、名前manifestId、型s) を受け付けてよい
# これらにパス、種別、設定、スナップショット、モードの引数が増えると、認可後に凍結済み計画を
# 変更できてしまい、中核のセキュリティ境界が崩れる
POST_FREEZE_METHODS = (           # 凍結後に可変引数を受け付けてはならないメソッド
    "CommitRestorePlan",          # 計画を凍結して認可する
    "ContinueRestorePlan",        # 実行を継続する
    "GetRestorePlanStatus",       # 状態を取得する
    "CancelRestorePlan",          # 実行をキャンセルする
)

# 削除済みのメソッド : 無認証で呼べる入口や認証状態の混同を招いたため廃止した
# XMLかconfへ再び現れた時点で、バス上の攻撃面が戻ったものとして失敗させる
FORBIDDEN_METHODS = (             # 再公開してはならないメソッド
    "Authenticate",               # 呼び出し元ごとの認証状態を保持し、アクションの取り違えを招いた
    "Quit",                       # 無認証でサービスを停止できた
)


def repo_root() -> str:
    """
    @brief 本スクリプトの位置からリポジトリルートを求める

    Returns:
        リポジトリの絶対パス
    """
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", ".."))


def default_xml_path() -> str:
    """
    @brief 既定のD-BusイントロスペクションXMLのパスを返す

    Returns:
        dbus/com.presire.qsnapper.Operations.xmlの絶対パス
    """
    return os.path.join(repo_root(), "dbus", "com.presire.qsnapper.Operations.xml")


def default_conf_path() -> str:
    """
    @brief 既定のバスポリシーconfのパスを返す

    Returns:
        dbus/com.presire.qsnapper.Operations.confの絶対パス
    """
    return os.path.join(repo_root(), "dbus", "com.presire.qsnapper.Operations.conf")


def parse_xml_methods(xml_path: str) -> dict[str, list[ET.Element]]:
    """
    @brief イントロスペクションXMLを解析し、メソッド名と引数要素の対応を返す

    Args:
        xml_path: 解析するイントロスペクションXMLのパス

    Returns:
        メソッド名をキー、そのarg要素のリストを値とする辞書
        com.presire.qsnapper.Operationsインタフェース配下のメソッドのみを対象とする

    Raises:
        ET.ParseError: XMLが不正な場合
    """
    tree = ET.parse(xml_path)  # ET.ParseErrorを送出する可能性がある
    root = tree.getroot()
    methods: dict[str, list[ET.Element]] = {}
    for iface in root.iter("interface"):
        if iface.get("name") != INTERFACE:
            continue
        for method in iface.findall("method"):
            name = method.get("name")
            if name is None:
                continue
            methods[name] = list(method.findall("arg"))
    return methods


def parse_conf_members(conf_path: str) -> set[str]:
    """
    @brief バスポリシーconfを解析し、対象インタフェースのsend_member集合を返す

    Args:
        conf_path: 解析するバスポリシーconfのパス

    Returns:
        send_interfaceがcom.presire.qsnapper.Operationsであるsend_memberの集合
        標準ヘルパー向けの許可 (Introspectable/Peer) はインタフェース検査で除外される

    Raises:
        ET.ParseError: XMLが不正な場合
    """
    tree = ET.parse(conf_path)  # ET.ParseErrorを送出する可能性がある
    root = tree.getroot()
    members: set[str] = set()
    for allow in root.iter("allow"):
        if allow.get("send_interface") != INTERFACE:
            continue
        member = allow.get("send_member")
        if member:
            members.add(member)
    return members


def check_post_freeze_args(
    methods: dict[str, list[ET.Element]],
) -> list[str]:
    """
    @brief 各凍結後メソッドがmanifestId (型s) のin引数を1個だけ持つことを検査する

    Args:
        methods: メソッド名をキー、そのarg要素のリストを値とする辞書

    Returns:
        人が読める失敗メッセージのリスト (全て成功なら空)
        各メソッドについて検査結果の行 (OKまたはFAIL) を出力する
    """
    failures: list[str] = []
    for method_name in POST_FREEZE_METHODS:
        args = methods.get(method_name)
        if args is None:
            msg = f"POST-FREEZE '{method_name}': MISSING from XML"
            print(msg)
            failures.append(msg)
            continue
        in_args = [a for a in args if a.get("direction") == "in"]
        if len(in_args) != 1:
            msg = (
                f"POST-FREEZE '{method_name}': expected exactly 1 "
                f'direction="in" arg, got {len(in_args)}'
            )
            print(msg)
            failures.append(msg)
            continue
        arg = in_args[0]
        arg_name = arg.get("name")
        arg_type = arg.get("type")
        ok = arg_name == "manifestId" and arg_type == "s"
        status = "OK" if ok else "FAIL"
        detail = f"name={arg_name!r} type={arg_type!r}"
        print(f"POST-FREEZE '{method_name}': {status} (single in-arg {detail})")
        if not ok:
            failures.append(
                f"POST-FREEZE '{method_name}': in-arg must be "
                f"name='manifestId' type='s', got name={arg_name!r} "
                f"type={arg_type!r}"
            )
    return failures


def main(argv: list[str] | None = None) -> int:
    """
    @brief XMLとconfの同期および凍結後メソッドの引数を検査する

    Args:
        argv: コマンドライン引数 (Noneの場合はsys.argvを使用)

    Returns:
        0 = 契約違反なし、1 = 契約違反または解析エラー
    """
    parser = argparse.ArgumentParser(
        description="Check qSnapper D-Bus XML/conf contract lockstep."
    )
    parser.add_argument(
        "--xml",
        default=default_xml_path(),
        help="Path to com.presire.qsnapper.Operations.xml",
    )
    parser.add_argument(
        "--conf",
        default=default_conf_path(),
        help="Path to com.presire.qsnapper.Operations.conf",
    )
    args = parser.parse_args(argv)

    failures: list[str] = []

    # 毎回両ファイルを新規に解析する (キャッシュしない)
    try:
        print(f"XML:  {args.xml}")
        xml_methods = parse_xml_methods(args.xml)
    except ET.ParseError as exc:
        msg = f"XML parse error: {exc}"
        print(msg)
        failures.append(msg)
        xml_methods = None
    except OSError as exc:
        msg = f"XML read error: {exc}"
        print(msg)
        failures.append(msg)
        xml_methods = None

    try:
        print(f"CONF: {args.conf}")
        conf_members = parse_conf_members(args.conf)
    except ET.ParseError as exc:
        msg = f"CONF parse error: {exc}"
        print(msg)
        failures.append(msg)
        conf_members = None
    except OSError as exc:
        msg = f"CONF read error: {exc}"
        print(msg)
        failures.append(msg)
        conf_members = None

    if xml_methods is not None and conf_members is not None:
        xml_method_names = set(xml_methods.keys())

        only_in_xml = sorted(xml_method_names - conf_members)
        only_in_conf = sorted(conf_members - xml_method_names)

        print(f"XML methods ({len(xml_method_names)}): {sorted(xml_method_names)}")
        print(f"CONF allowlist ({len(conf_members)}): {sorted(conf_members)}")

        if only_in_xml:
            msg = f"only in XML: {only_in_xml}"
            print(msg)
            failures.append(msg)
        if only_in_conf:
            msg = f"only in conf: {only_in_conf}"
            print(msg)
            failures.append(msg)
        if not only_in_xml and not only_in_conf:
            print("SET SYNC: OK (XML methods == conf allowlist)")

        reintroduced = sorted(
            set(FORBIDDEN_METHODS) & (xml_method_names | conf_members)
        )
        print(f"FORBIDDEN methods present: {reintroduced if reintroduced else 'none'}")
        if reintroduced:
            failures.append(f"removed methods were reintroduced: {reintroduced}")

        # Post-freeze security-property assertions.
        post_freeze_failures = check_post_freeze_args(xml_methods)
        failures.extend(post_freeze_failures)

    print()
    if failures:
        print("CONTRACT SYNC: FAILED")
        print(f"  ({len(failures)} failure(s))")
        return 1
    print("CONTRACT SYNC: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
