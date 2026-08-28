#!/usr/bin/env python3
"""
@file check_restore_authorization_boundaries.py
@brief Structural proof that the legacy and staged restore authorization
       boundaries in src/dbusservice/snapshotoperations.cpp are unchanged.

Why this exists
----------------
qSnapper has two restore call paths that must never silently converge or
diverge in their PolicyKit authorization behaviour:

  - The LEGACY per-call path (RestoreFiles / RestoreFilesDirect, both thin
    wrappers over restoreFilesImpl) authorizes on EVERY call, unconditionally,
    independent of any staged-restore plan state.
  - The STAGED single-authorization path (BeginRestorePlan /
    StageRestoreEntries / CommitRestorePlan / ContinueRestorePlan /
    GetRestorePlanStatus / CancelRestorePlan) authorizes EXACTLY ONCE, inside
    CommitRestorePlan, strictly AFTER the plan is frozen. No pre-freeze method
    and no post-commit control method (Continue/Status/Cancel) may ever
    authorize -- doing so would either prompt for authorization before the
    plan is immutable, or re-prompt during execution, both of which defeat
    the "authorize exactly once" security property.

Authorization primitives
------------------------
Authorization is asynchronous. A blocking polkit call with
AllowUserInteraction has no timeout and freezes the whole single-threaded
Qt event loop of this root service, so any local user could stall the daemon
indefinitely with one unanswered prompt. The service therefore runs a
non-interactive fast path first and only defers (setDelayedReply + polkit's
per-request async API) when polkit reports "challenge". The two entry points
into that gate are:

  - authorizeThen<T>(actionId, body)   -- generic slots
  - beginAuthorization(actionId, cont) -- CommitRestorePlan, which must
                                          re-validate plan state after the
                                          prompt before it mounts anything

Both count as "authorizing" for every boundary assertion below.

Two invariants exist only because of the asynchronous design:

  - A *Authorized body must never be reachable without passing the gate, and
    must never authorize a second time (that would re-prompt mid-operation).
  - QDBusContext::sendErrorReply() is meaningless once a slot has returned,
    so every error path must go through replyError(); sendErrorReply( may
    appear exactly once in the file, inside replyError() itself.

This script parses the REAL snapshotoperations.cpp (extracting function
bodies by brace matching, not fixed line numbers, since the file carries an
evolving uncommitted diff) and asserts these boundaries mechanically, so a
future edit that adds/removes/moves an authorization call goes red here
instead of silently changing security behaviour.

Uses only the Python 3 standard library. Exits 0 on success, 1 on any
violation or parse error.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# Function names on the legacy per-call authorization path.
LEGACY_ENTRY_POINTS = ("RestoreFiles", "RestoreFilesDirect")
LEGACY_IMPL = "restoreFilesImpl"
# The part of the legacy path that runs after the authorization gate.
LEGACY_AUTHORIZED_IMPL = "restoreFilesAuthorized"

# Function names on the staged single-authorization path.
STAGE_PRE_FREEZE = ("BeginRestorePlan", "StageRestoreEntries")
STAGE_COMMIT = "CommitRestorePlan"
STAGE_POST_COMMIT_CONTROL = (
    "ContinueRestorePlan",
    "GetRestorePlanStatus",
    "CancelRestorePlan",
)
STAGED_CONTROL_METHODS = STAGE_PRE_FREEZE + STAGE_POST_COMMIT_CONTROL

# Identifiers that would indicate restoreFilesImpl started depending on the
# staged-restore plan state machine (it must not: it is the fully
# independent legacy path).
STAGED_PLAN_IDENTIFIERS = (
    "m_restoreRegistry",
    "m_restoreExecutor",
    "m_restorePlanOwners",
    "manifestId",
)

# Baseline set of SnapshotOperations methods that legitimately authorize
# today. A NEW name appearing here that is not in this set is treated as an
# unreviewed authorization-boundary change and fails the check (rather than
# silently passing); a name disappearing from this set (a currently-listed
# method that stops authorizing) also fails.
EXPECTED_AUTHORIZING_METHODS = frozenset({
    "WriteSnapperConfig",
    "SetupQuota",
    "ListConfigs",
    "ListSnapshots",
    "CreateSnapshot",
    "ModifySnapshot",
    "DeleteSnapshot",
    "RollbackSnapshot",
    "GetFileChanges",
    "GetFileChangesBetween",
    "GetFileDiffBetween",
    "GetFileDiffAndDetails",
    STAGE_COMMIT,
    LEGACY_IMPL,
})

FUNCTION_DEF_RE = re.compile(r"\bSnapshotOperations::(\w+)\s*\(")
# A real authorization call site. Matches both gate entry points and tolerates
# the explicit template argument on authorizeThen<T>(...). The (?<!::) guard
# keeps the "SnapshotOperations::beginAuthorization(" definition from counting
# as a call site.
CALL_SITE_RE = re.compile(
    r"(?<!::)\b(?:authorizeThen\s*(?:<[^<>]*>)?|beginAuthorization)\s*\("
)
# Human-readable name for the thing CALL_SITE_RE counts, used in messages.
AUTH_CALL_LABEL = "authorizeThen(/beginAuthorization("
# Bodies that run only after the authorization gate has passed.
AUTHORIZED_BODY_DEF_RE = re.compile(r"\bSnapshotOperations::(\w+Authorized)\s*\(")
# QDBusContext::sendErrorReply() is only valid while the original slot call is
# still on the stack. Every other error path must use replyError().
RAW_ERROR_REPLY_RE = re.compile(r"(?<!::)\bsendErrorReply\s*\(")
RAW_ERROR_REPLY_OWNER = "replyError"


def repo_root() -> str:
    """Return the repository root derived from this script's location."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", ".."))


def default_source_path() -> str:
    return os.path.join(repo_root(), "src", "dbusservice", "snapshotoperations.cpp")


def find_matching_paren(text: str, open_paren_index: int) -> int:
    """Return the index of the ')' matching the '(' at open_paren_index."""
    depth = 0
    i = open_paren_index
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError(f"unmatched '(' at offset {open_paren_index}")


def find_function_body_span(text: str, name_match_end: int) -> tuple[int, int]:
    """Return (brace_open, brace_close) offsets of the function body that
    starts with the parameter list immediately following name_match_end.

    Brace matching skips over string/char literals and // and /* */
    comments so that a stray '{' or '}' inside a log message or comment
    cannot desynchronize the scan.
    """
    open_paren = text.index("(", name_match_end)
    close_paren = find_matching_paren(text, open_paren)
    brace_open = text.index("{", close_paren)

    depth = 0
    i = brace_open
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    while i < n:
        c = text[i]
        c2 = text[i + 1] if i + 1 < n else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and c2 == "/":
                in_block_comment = False
                i += 1
        elif in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif in_char:
            if c == "\\":
                i += 1
            elif c == "'":
                in_char = False
        else:
            if c == "/" and c2 == "/":
                in_line_comment = True
                i += 1
            elif c == "/" and c2 == "*":
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return brace_open, i
        i += 1
    raise ValueError(f"unterminated function body starting at offset {brace_open}")


def depth_at_offset(text: str, body_start: int, target_offset: int) -> int:
    """Return the brace depth (relative to body_start, which must be the
    function's opening '{') exactly at target_offset, skipping over
    string/char literals and comments the same way find_function_body_span
    does. depth == 1 means "directly inside the function's outer block, not
    nested in any if/for/while/try block".
    """
    depth = 0
    i = body_start
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    while i < target_offset:
        c = text[i]
        c2 = text[i + 1] if i + 1 < len(text) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and c2 == "/":
                in_block_comment = False
                i += 1
        elif in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif in_char:
            if c == "\\":
                i += 1
            elif c == "'":
                in_char = False
        else:
            if c == "/" and c2 == "/":
                in_line_comment = True
                i += 1
            elif c == "/" and c2 == "*":
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
        i += 1
    return depth


def collect_functions(text: str) -> dict[str, tuple[int, int, int]]:
    """Return {name: (def_start, brace_open, brace_close)} for every
    SnapshotOperations::<name>( ... ) { ... } definition in text.
    """
    functions: dict[str, tuple[int, int, int]] = {}
    for match in FUNCTION_DEF_RE.finditer(text):
        name = match.group(1)
        if name in functions:
            continue  # keep the first (definitions are unique per name here)
        try:
            brace_open, brace_close = find_function_body_span(text, match.end(1))
        except ValueError:
            continue
        functions[name] = (match.start(), brace_open, brace_close)
    return functions


def line_of(text: str, offset: int) -> int:
    """Return the 1-based line number containing offset."""
    return text.count("\n", 0, offset) + 1


def find_non_code_ranges(text: str) -> list[tuple[int, int]]:
    """Return a list of [start, end) offset ranges that are inside a //
    line comment, a /* */ block comment, a "string", or a 'char' literal.

    Used to filter out authorization-call occurrences that only appear in
    prose (e.g. a Doxygen comment mentioning the function by name) so they
    are never mistaken for a real call site.
    """
    ranges: list[tuple[int, int]] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        c2 = text[i + 1] if i + 1 < n else ""
        if c == "/" and c2 == "/":
            start = i
            while i < n and text[i] != "\n":
                i += 1
            ranges.append((start, i))
        elif c == "/" and c2 == "*":
            start = i
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
            ranges.append((start, i))
        elif c == '"':
            start = i
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i = min(i + 1, n)
            ranges.append((start, i))
        elif c == "'":
            start = i
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i = min(i + 1, n)
            ranges.append((start, i))
        else:
            i += 1
    return ranges


def is_in_non_code_range(offset: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= offset < end for start, end in ranges)


def real_call_offsets(text: str, ranges: list[tuple[int, int]]) -> list[int]:
    """Return the offsets of every authorization call (CALL_SITE_RE) in
    text that is real code (not inside a comment/string/char literal) and
    not the "SnapshotOperations::beginAuthorization(" definition itself.
    """
    return [m.start() for m in CALL_SITE_RE.finditer(text)
            if not is_in_non_code_range(m.start(), ranges)]


def call_offsets_in_span(offsets: list[int], span_start: int, span_end: int) -> list[int]:
    return [off for off in offsets if span_start <= off <= span_end]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prove the legacy and staged restore authorization "
                     "boundaries in snapshotoperations.cpp are unchanged."
    )
    parser.add_argument("--source", default=default_source_path(),
                        help="Path to src/dbusservice/snapshotoperations.cpp")
    args = parser.parse_args()

    print(f"SOURCE: {args.source}")
    try:
        with open(args.source, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        print(f"FAIL: cannot read source: {exc}")
        return 1

    functions = collect_functions(text)
    print(f"Parsed {len(functions)} SnapshotOperations:: function definitions "
          f"by brace matching.")

    non_code_ranges = find_non_code_ranges(text)
    call_offsets = real_call_offsets(text, non_code_ranges)
    print(f"Found {len(call_offsets)} real (non-comment/string) "
          f"{AUTH_CALL_LABEL} occurrences in the whole file.")

    failures: list[str] = []

    def require_function(name: str) -> str | None:
        if name not in functions:
            failures.append(f"function not found by brace matching: {name}")
            return None
        _, brace_open, brace_close = functions[name]
        return text[brace_open:brace_close + 1]

    def calls_in_function(name: str) -> list[int]:
        """Return absolute-offset real call sites within name's body span."""
        if name not in functions:
            return []
        _, brace_open, brace_close = functions[name]
        return call_offsets_in_span(call_offsets, brace_open, brace_close)

    # --- 1: RestoreFiles / RestoreFilesDirect delegate to restoreFilesImpl ---
    for name in LEGACY_ENTRY_POINTS:
        body = require_function(name)
        if body is None:
            continue
        delegates = f"{LEGACY_IMPL}(" in body
        status = "OK" if delegates else "FAIL"
        print(f"[1] {name}: {status} (delegates to {LEGACY_IMPL})")
        if not delegates:
            failures.append(f"{name} does not call {LEGACY_IMPL}(")

    # --- 2: restoreFilesImpl authorizes exactly once, unconditionally,
    # independent of staged-plan state ---
    impl_body = require_function(LEGACY_IMPL)
    if impl_body is not None:
        _, impl_brace_open, _ = functions[LEGACY_IMPL]
        impl_calls = calls_in_function(LEGACY_IMPL)
        call_count = len(impl_calls)
        print(f"[2] {LEGACY_IMPL}: {AUTH_CALL_LABEL} call count = {call_count}")
        if call_count != 1:
            failures.append(
                f"{LEGACY_IMPL} must call {AUTH_CALL_LABEL} exactly once, "
                f"found {call_count}"
            )
        else:
            absolute_offset = impl_calls[0]
            depth = depth_at_offset(text, impl_brace_open, absolute_offset)
            print(f"[2] {LEGACY_IMPL}: {AUTH_CALL_LABEL} brace depth at call "
                  f"site = {depth} (line {line_of(text, absolute_offset)})")
            if depth != 1:
                failures.append(
                    f"{LEGACY_IMPL}'s {AUTH_CALL_LABEL} call is nested at "
                    f"brace depth {depth} (expected 1 == unconditional on the "
                    f"function's main path, not guarded by an extra if/for/"
                    f"while/try block)"
                )
        # The legacy path's work moved behind the authorization gate into
        # restoreFilesAuthorized, so both halves must stay free of staged-plan
        # state for the independence property to still mean anything.
        for legacy_part in (LEGACY_IMPL, LEGACY_AUTHORIZED_IMPL):
            part_body = require_function(legacy_part)
            if part_body is None:
                continue
            staged_refs = [ident for ident in STAGED_PLAN_IDENTIFIERS
                           if ident in part_body]
            print(f"[2] {legacy_part}: staged-plan identifiers referenced = "
                  f"{staged_refs if staged_refs else 'none'}")
            if staged_refs:
                failures.append(
                    f"{legacy_part} references staged-restore-plan identifiers "
                    f"{staged_refs} -- the legacy per-call authorization must "
                    f"stay independent of manifest/plan state"
                )

    # --- 3: BeginRestorePlan / StageRestoreEntries authorize zero times ---
    for name in STAGE_PRE_FREEZE:
        body = require_function(name)
        if body is None:
            continue
        call_count = len(calls_in_function(name))
        print(f"[3] {name}: {AUTH_CALL_LABEL} call count = {call_count} "
              f"(pre-freeze, must be 0)")
        if call_count != 0:
            failures.append(
                f"{name} (pre-freeze staged-restore method) must call "
                f"{AUTH_CALL_LABEL} zero times, found {call_count}"
            )

    # --- 4: CommitRestorePlan authorizes exactly once, AFTER freeze( ---
    commit_body = require_function(STAGE_COMMIT)
    if commit_body is not None:
        _, commit_brace_open, _ = functions[STAGE_COMMIT]
        commit_calls = calls_in_function(STAGE_COMMIT)
        call_count = len(commit_calls)
        print(f"[4] {STAGE_COMMIT}: {AUTH_CALL_LABEL} call count = {call_count}")
        if call_count != 1:
            failures.append(
                f"{STAGE_COMMIT} must call {AUTH_CALL_LABEL} exactly once, "
                f"found {call_count}"
            )
        freeze_offset_in_body = commit_body.find("m_restoreRegistry.freeze(")
        freeze_offset = (commit_brace_open + freeze_offset_in_body
                         if freeze_offset_in_body >= 0 else -1)
        auth_offset = commit_calls[0] if commit_calls else -1
        print(f"[4] {STAGE_COMMIT}: m_restoreRegistry.freeze( at file-offset "
              f"{freeze_offset} (line {line_of(text, freeze_offset) if freeze_offset >= 0 else '?'}), "
              f"{AUTH_CALL_LABEL} at file-offset {auth_offset} "
              f"(line {line_of(text, auth_offset) if auth_offset >= 0 else '?'})")
        if freeze_offset < 0:
            failures.append(f"{STAGE_COMMIT} no longer calls m_restoreRegistry.freeze(")
        elif auth_offset < 0:
            failures.append(f"{STAGE_COMMIT} has no {AUTH_CALL_LABEL} call to order")
        elif not (freeze_offset < auth_offset):
            failures.append(
                f"{STAGE_COMMIT} authorizes (offset {auth_offset}) before/at "
                f"freeze (offset {freeze_offset}) -- authorization must happen "
                f"strictly AFTER the plan is frozen"
            )

    # --- 5: post-commit control methods authorize zero times ---
    for name in STAGE_POST_COMMIT_CONTROL:
        body = require_function(name)
        if body is None:
            continue
        call_count = len(calls_in_function(name))
        print(f"[5] {name}: {AUTH_CALL_LABEL} call count = {call_count} "
              f"(post-commit control, must be 0 -- no re-prompt)")
        if call_count != 0:
            failures.append(
                f"{name} (post-commit staged-restore control method) must "
                f"call {AUTH_CALL_LABEL} zero times, found {call_count}"
            )

    # --- 6: StageRestoreEntries rejects an over-capacity chunk with
    # InvalidArgs and returns BEFORE ever calling m_restoreRegistry.stageEntries( ---
    stage_body = require_function("StageRestoreEntries")
    if stage_body is not None:
        idx_cap = stage_body.find("kMaxEntriesPerStageChunk")
        idx_invalid = (stage_body.find("QDBusError::InvalidArgs", idx_cap, idx_cap + 300)
                       if idx_cap >= 0 else -1)
        idx_msg = (stage_body.find("Restore entry chunk is too large", idx_cap, idx_cap + 400)
                   if idx_cap >= 0 else -1)
        idx_return = (stage_body.find("return false", idx_invalid, idx_invalid + 150)
                      if idx_invalid >= 0 else -1)
        idx_stage_call = stage_body.find("m_restoreRegistry.stageEntries(")
        print(f"[6] StageRestoreEntries: kMaxEntriesPerStageChunk@{idx_cap} "
              f"InvalidArgs@{idx_invalid} msg@{idx_msg} return@{idx_return} "
              f"stageEntries(@{idx_stage_call}")
        if idx_cap < 0:
            failures.append(
                "StageRestoreEntries no longer references "
                "kMaxEntriesPerStageChunk (capacity guard missing)"
            )
        elif idx_invalid < 0:
            failures.append(
                "StageRestoreEntries: no QDBusError::InvalidArgs found near "
                "the kMaxEntriesPerStageChunk capacity check"
            )
        elif idx_msg < 0:
            failures.append(
                "StageRestoreEntries: capacity guard no longer sends the "
                "'Restore entry chunk is too large' message"
            )
        elif idx_return < 0:
            failures.append(
                "StageRestoreEntries: capacity guard does not 'return false' "
                "right after sending the InvalidArgs error"
            )
        elif idx_stage_call < 0:
            failures.append(
                "StageRestoreEntries no longer calls "
                "m_restoreRegistry.stageEntries( at all"
            )
        elif not (idx_return < idx_stage_call):
            failures.append(
                "StageRestoreEntries: the over-capacity rejection (return "
                f"false at offset {idx_return}) does not happen BEFORE "
                f"m_restoreRegistry.stageEntries( (offset {idx_stage_call}) "
                "-- an over-capacity chunk could reach the registry"
            )

    # --- 7: enumerate every real authorization call site in the whole
    # file, print its enclosing function, and fail if a staged-restore
    # control method appears, or if the enclosing-function set drifts from
    # the reviewed baseline. ---
    print()
    print(f"{AUTH_CALL_LABEL} call site enumeration (whole file):")
    sites: list[tuple[int, str]] = []
    for offset in call_offsets:
        enclosing = None
        for name, (_, brace_open, brace_close) in functions.items():
            if brace_open <= offset <= brace_close:
                enclosing = name
                break
        line = line_of(text, offset)
        sites.append((line, enclosing or "<UNRESOLVED>"))

    for line, name in sorted(sites):
        print(f"    line {line}: SnapshotOperations::{name}(...)")

    found_names = {name for _, name in sites}
    print(f"Total {AUTH_CALL_LABEL} call sites: {len(sites)}")
    print(f"Distinct enclosing methods: {sorted(found_names)}")

    staged_violations = found_names & set(STAGED_CONTROL_METHODS)
    if staged_violations:
        failures.append(
            f"staged-restore control method(s) now call {AUTH_CALL_LABEL}: "
            f"{sorted(staged_violations)} -- this breaks the single-"
            "authorization guarantee"
        )

    unresolved = [f"line {line}" for line, name in sites if name == "<UNRESOLVED>"]
    if unresolved:
        failures.append(
            f"{AUTH_CALL_LABEL} call site(s) outside any known function "
            f"body: {unresolved}"
        )

    missing_from_baseline = EXPECTED_AUTHORIZING_METHODS - found_names
    grown_beyond_baseline = found_names - EXPECTED_AUTHORIZING_METHODS
    if missing_from_baseline:
        failures.append(
            "method(s) expected to authorize no longer do: "
            f"{sorted(missing_from_baseline)}"
        )
    if grown_beyond_baseline:
        failures.append(
            "method(s) NOT in the reviewed authorizing-method baseline now "
            f"call {AUTH_CALL_LABEL}: {sorted(grown_beyond_baseline)} -- "
            "review and, if intentional, add to EXPECTED_AUTHORIZING_METHODS"
        )

    # --- 8: every *Authorized body sits strictly behind the gate ---
    # An authorized body must be unreachable except from a function that
    # authorizes, and must never authorize again itself (a second prompt
    # mid-operation would break the "authorize exactly once" property just as
    # badly as authorizing in a post-commit control method).
    print()
    authorized_bodies = sorted({m.group(1)
                                for m in AUTHORIZED_BODY_DEF_RE.finditer(text)})
    print(f"[8] authorized bodies found: {authorized_bodies}")
    if not authorized_bodies:
        failures.append(
            "no SnapshotOperations::<name>Authorized( definitions found -- the "
            "authorization gate structure this check relies on is gone"
        )
    for name in authorized_bodies:
        own_calls = len(calls_in_function(name))
        if own_calls != 0:
            failures.append(
                f"{name} (runs after the gate) authorizes {own_calls} time(s) "
                f"-- an authorized body must never re-prompt"
            )

        callers: set[str] = set()
        pattern = re.compile(r"(?<!::)\b" + re.escape(name) + r"\s*\(")
        for match in pattern.finditer(text):
            offset = match.start()
            if is_in_non_code_range(offset, non_code_ranges):
                continue
            for other, (_, brace_open, brace_close) in functions.items():
                if other != name and brace_open <= offset <= brace_close:
                    callers.add(other)
                    break
        ungated = sorted(c for c in callers if not calls_in_function(c))
        print(f"[8] {name}: authorizes {own_calls} time(s), "
              f"called from {sorted(callers) if callers else 'nowhere'}")
        if not callers:
            failures.append(
                f"{name} has no caller -- an authorized body must be reachable "
                f"exactly through its authorization gate"
            )
        if ungated:
            failures.append(
                f"{name} is called from {ungated}, which do(es) not authorize "
                f"-- an authorized body must never be reachable without "
                f"passing the authorization gate"
            )

    # --- 9: sendErrorReply( is confined to replyError() ---
    # Once a slot has returned (which it does while a polkit prompt is open),
    # QDBusContext::sendErrorReply() has no call context left and silently
    # drops the error. replyError() is the only place allowed to touch it; it
    # routes deferred replies through the captured QDBusMessage instead.
    print()
    raw_sites: list[tuple[int, str]] = []
    for match in RAW_ERROR_REPLY_RE.finditer(text):
        offset = match.start()
        if is_in_non_code_range(offset, non_code_ranges):
            continue
        enclosing = "<UNRESOLVED>"
        for other, (_, brace_open, brace_close) in functions.items():
            if brace_open <= offset <= brace_close:
                enclosing = other
                break
        raw_sites.append((line_of(text, offset), enclosing))

    print(f"[9] sendErrorReply( call sites: "
          f"{[f'line {line}: {name}' for line, name in sorted(raw_sites)]}")
    stray = sorted({name for _, name in raw_sites
                    if name != RAW_ERROR_REPLY_OWNER})
    if len(raw_sites) != 1 or stray:
        failures.append(
            f"sendErrorReply( must appear exactly once, inside "
            f"{RAW_ERROR_REPLY_OWNER}(); found {len(raw_sites)} site(s)"
            + (f" in {stray}" if stray else "")
            + " -- error paths that can run after a delayed reply must use "
              "replyError()"
        )

    print()
    if failures:
        print("RESTORE AUTHORIZATION BOUNDARIES: FAILED")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("RESTORE AUTHORIZATION BOUNDARIES: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
