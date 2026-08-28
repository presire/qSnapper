#!/usr/bin/env python3
"""Lockstep checker for the qSnapper D-Bus contract and bus-policy allowlist.

Why this exists
---------------
dbus/com.presire.qsnapper.Operations.xml declares the public D-Bus methods of
the root qSnapper service. dbus/com.presire.qsnapper.Operations.conf is a
DENY-BY-DEFAULT bus policy: it denies every method_call to the service and then
re-allows exactly the public API members with <allow send_member="..."/>. If a
method is added to the XML but forgotten in the conf, legitimate clients get
org.freedesktop.DBus.Error.AccessDenied from the bus daemon itself. If a method
is removed from the XML but left in the conf, a dead allowlist entry widens the
attack surface for no benefit. Both files MUST stay in lockstep.

Additionally, the staged-restore contract establishes a security property:
after CommitRestorePlan freezes and authorizes a plan, no post-freeze method may
accept a path, change type, config name, snapshot number, or restore mode —
only a manifest identifier. This checker asserts that property mechanically so
a future edit that adds a mutable parameter to a post-freeze method goes red
here instead of silently weakening the boundary.

The script uses only the Python 3 standard library (xml.etree.ElementTree) so
it can run anywhere without the dbus module. It exits 0 on success and 1 on any
contract violation or parse error.
"""

from __future__ import annotations

import argparse
import os
import sys
import xml.etree.ElementTree as ET

INTERFACE = "com.presire.qsnapper.Operations"

# Post-freeze methods: after CommitRestorePlan authorizes a frozen plan, these
# must accept ONLY a manifest identifier (one "in" arg named manifestId, type s).
# If any of these grows a path/type/config/snapshot/mode argument, the frozen
# plan could be mutated after authorization — the core security boundary breaks.
POST_FREEZE_METHODS = (
    "CommitRestorePlan",
    "ContinueRestorePlan",
    "GetRestorePlanStatus",
    "CancelRestorePlan",
)


def repo_root() -> str:
    """Return the repository root derived from this script's location."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", ".."))


def default_xml_path() -> str:
    return os.path.join(repo_root(), "dbus", "com.presire.qsnapper.Operations.xml")


def default_conf_path() -> str:
    return os.path.join(repo_root(), "dbus", "com.presire.qsnapper.Operations.conf")


def parse_xml_methods(xml_path: str) -> dict[str, list[ET.Element]]:
    """Parse the introspection XML and return {method_name: [arg elements]}.

    Raises ET.ParseError on malformed XML. Only methods under the
    com.presire.qsnapper.Operations interface are returned.
    """
    tree = ET.parse(xml_path)  # may raise ET.ParseError
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
    """Parse the bus policy conf and return the set of send_member values
    whose send_interface is com.presire.qsnapper.Operations.

    Raises ET.ParseError on malformed XML. Standard-helper allows
    (Introspectable / Peer) are filtered out by the interface check.
    """
    tree = ET.parse(conf_path)  # may raise ET.ParseError
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
    """Assert each post-freeze method has EXACTLY ONE direction="in" arg,
    that it is named manifestId, and that its type is "s".

    Returns a list of human-readable failure messages (empty if all pass).
    Prints each assertion result line (OK or FAIL) for every method.
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

    # Parse both files fresh on every run (no caching).
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
