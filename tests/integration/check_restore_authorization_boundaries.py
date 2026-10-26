#!/usr/bin/env python3
"""
@file check_restore_authorization_boundaries.py
@brief src/dbusservice/snapshotoperations*.cppの段階的復元認可境界が崩れておらず、
       削除済みの旧復元経路が再導入されていないことを構造的に検査する

存在理由 :
    qSnapperは段階的経路を通してのみファイルを復元する
    旧来 (LEGACY) のper-call経路 (RestoreFiles / RestoreFilesDirectをrestoreFilesImpl /
    restoreFilesAuthorized経由で呼ぶ) は、稼働中のinodeをその場で上書きし、クライアント申告の
    変更種別を信頼し、段階的実行に対する除外も持たなかったため削除された
    復活させてはならない

    STAGEDの経路 (BeginRestorePlan / StageRestoreEntries / CommitRestorePlan /
    ContinueRestorePlan / GetRestorePlanStatus / CancelRestorePlan) は、
    復元の実行をCommitRestorePlanの内部でちょうど1回だけ、計画の凍結より厳密に後に認可する
    StageRestoreEntriesとコミット後の制御メソッド (Continue/Status/Cancel) は決して認可してはならない
    認可すると、計画が不変になる前に認可を求めたり、実行中に再プロンプトしたりすることになり、
    いずれも「復元の実行をちょうど1回だけ認可する」というセキュリティ特性を損なう

    BeginRestorePlanだけは、計画の予算 (計画のために確保するエントリ数とpathバイト数の上限) を
    認可のない利用者に使い切らせないため、差分の閲覧と同じview-diffでちょうど1回認可する
    復元の実行の認可 (rollback-snapshot) を求めてはならず、計画の作成は認可後の本体
    (beginRestorePlanAuthorized) に置かなければならない

認可のプリミティブ :
    認可は非同期である
    AllowUserInteraction付きのブロッキングなpolkit呼び出しはタイムアウトを持たず、
    このrootサービスの単一スレッドQtイベントループ全体を凍結させるため、
    ローカルユーザは未応答のプロンプト1つでデーモンを無期限に停止させられる
    そこで本サービスはまず非対話の高速経路を実行し、polkitがchallengeを報告した場合にのみ
    遅延 (setDelayedReplyとpolkitのリクエスト単位の非同期API) する
    このゲートへの入口は2つある

    authorizeThen<T>(actionId, body)   -- 汎用スロット
    beginAuthorization(actionId, cont) -- CommitRestorePlan
                                          プロンプト後に計画状態を再検証してからマウントする必要がある

    以下の全ての境界検査では、この2つを認可とみなす

    非同期設計のためだけに成り立つ不変条件が2つある

    認可済み本体は、ゲートを通らずに到達できてはならず、二度目の認可もしてはならない
    (二度目は操作の途中で再プロンプトすることになる)
    QDBusContext::sendErrorReply()はスロットが戻った後では無意味であり、
    全エラー経路はreplyError()を通らなければならない
    sendErrorReply(はファイル内でreplyError()の内部にちょうど1回だけ現れてよい

    本スクリプトは実在のsnapshotoperations*.cppを
    (未コミットdiffが変動するため固定行番号ではなく括弧の対応で関数本体を切り出す) 解析し、
    これらの境界を機械的に検査する
    将来の編集が認可呼び出しを追加、削除、移動すると、
    セキュリティ挙動を静かに変えるのではなくここで失敗する

    加えて、復元の実行中に非復元操作を拒否する共通ゲート (snapshotoperations.hのauthorizeThen) が、
    認可の前と本体の直前の両方で確認していること、ゲートを外すメソッドが許可済みのものに限られることを検査する

    本スクリプトはPython 3標準ライブラリのみを使う

使い方 : check_restore_authorization_boundaries.py [--source FILE ...]
終了コード : 0 = 検査成功、1 = 違反ありまたはソースが見つからない
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# 削除済みの旧per-call復元経路の関数名
# いずれも再定義されてはならない
REMOVED_LEGACY_FUNCTIONS = (          # 復活してはならない旧経路の関数名
    "RestoreFiles",                   # 旧per-call復元の入口
    "RestoreFilesDirect",             # 旧per-call直接復元
    "restoreFilesImpl",               # 旧復元の実装本体
    "restoreFilesAuthorized",         # 旧復元の認可後本体
    "copyRegularFile",                # 通常ファイルのコピー
    "copySymlink",                    # シンボリックリンクのコピー
)

# 段階的単一認可経路の関数名
STAGE_BEGIN = "BeginRestorePlan"                                # 計画を作るメソッド (view-diffで認可する)
STAGE_BEGIN_ACTION = "com.presire.qsnapper.view-diff"           # 計画の作成に求めるアクション
STAGE_COMMIT_ACTION = "com.presire.qsnapper.rollback-snapshot"  # 復元の実行に求めるアクション
STAGE_BEGIN_BODY = "beginRestorePlanAuthorized"                 # 計画を作る認可後の本体
STAGE_PRE_FREEZE = ("StageRestoreEntries",)                     # 認可してはならない凍結前のメソッド
STAGE_COMMIT = "CommitRestorePlan"                              # 凍結と認可を行うメソッド
STAGE_POST_COMMIT_CONTROL = (                                   # コミット後の制御メソッド
    "ContinueRestorePlan",                                      # 実行を継続する
    "GetRestorePlanStatus",                                     # 状態を取得する
    "CancelRestorePlan",                                        # 実行をキャンセルする
)

# 現在正当に認可するSnapshotOperationsメソッドの基準集合
# この集合に無い新しい名前が現れた場合は未レビューの認可境界変更とみなし、静かに通過させず検査を失敗させる
# 逆にこの集合の名前が消えた場合 (現在リストにあるメソッドが認可しなくなった場合) も失敗させる
EXPECTED_AUTHORIZING_METHODS = frozenset({   # 認可ゲートの呼び出しを許可するメソッド
    "WriteSnapperConfig",
    "SetupQuota",
    "GetConfig",
    "GetTimerStates",
    "SetTimerEnabled",
    "CreateConfig",
    "DeleteConfig",
    "DeleteSnapshots",
    "GetFilesystemUsage",
    "ListSubvolumes",
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
    STAGE_BEGIN,
    STAGE_COMMIT,
    # QSNAPPER_ENABLE_TEST_HOOKS付きのビルドだけに存在し、例外がスロットの外へ漏れないことを単体テストで確かめるために認可ゲートを通す
    "authorizeThrowingBodyForTesting",
})

# 認可済みの本体を呼び出す式
# 例外をスロットの外へ出さないよう、authorizeThenとcompleteDeferredCallは本体をAUTHORIZED_BODY_CALLのラッパー経由で実行する
AUTHORIZED_BODY_CALL = "invokeAuthorizedBody<T>(body)"

FUNCTION_DEF_RE = re.compile(r"\bSnapshotOperations::(\w+)\s*\(")  # SnapshotOperations::name(の定義

# 実際の認可呼び出し位置
# 両方のゲート入口に一致し、authorizeThen<T>(...)の明示的なテンプレート引数も許容する
# (?<!::)のガードにより
# "SnapshotOperations::beginAuthorization("の定義が呼び出し位置として数えられるのを防ぐ
CALL_SITE_RE = re.compile(
    r"(?<!::)\b(?:authorizeThen\s*(?:<[^<>]*>)?|beginAuthorization)\s*\("
)
# CALL_SITE_REが数えるものの人が読める名前 (メッセージに使う)
AUTH_CALL_LABEL = "authorizeThen(/beginAuthorization("
# 認可ゲートを通過した後にのみ動作する本体
AUTHORIZED_BODY_DEF_RE = re.compile(r"\bSnapshotOperations::(\w+Authorized)\s*\(")  # 認可後本体の定義
# QDBusContext::sendErrorReply()は元のスロット呼び出しがまだスタック上にある間だけ有効である
# それ以外の全エラー経路はreplyError()を使わなければならない
RAW_ERROR_REPLY_RE = re.compile(r"(?<!::)\bsendErrorReply\s*\(")  # replyError()以外の生のエラー応答
RAW_ERROR_REPLY_OWNER = "replyError"                             # sendErrorReply(を許す唯一の関数

# 復元の実行中に非復元操作を拒否する共通ゲート
# authorizeThenとcompleteDeferredCallはテンプレートのためヘッダに定義されている
RESTORE_GATE_CALL = "rejectWhileRestoreRunning()"                                    # ゲートの呼び出し
RESTORE_GATE_EXEMPT_TOKEN = "AllowWhileRestoring"                                    # ゲートを外す指定
RESTORE_GATE_EXEMPT_METHODS = frozenset({"ListConfigs", "BeginRestorePlan"})         # 外してよいメソッド
AUTHORIZE_THEN_DEF_RE = re.compile(r"\bT\s+authorizeThen\s*\(")                      # authorizeThenの定義
COMPLETE_DEFERRED_DEF_RE = re.compile(r"\bvoid\s+completeDeferredCall\s*\(")         # completeDeferredCallの定義


def repo_root() -> str:
    """
    @brief 本スクリプトの位置からリポジトリルートを求める

    Returns:
        リポジトリの絶対パス
    """
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", ".."))


def default_source_paths() -> list[str]:
    """
    @brief src/dbusservice/snapshotoperations*.cppを全てソートして返す

    実装は責務ごとに複数ファイルへ分割されているため、以下の全検査は
    それらを連結したテキストに対して実行する
    ファイルが1つも見つからない場合は静かに通過させず、明確な失敗とする

    Returns:
        見つかったsnapshotoperations*.cppのソート済み絶対パスリスト
    """
    import glob

    pattern = os.path.join(repo_root(), "src", "dbusservice", "snapshotoperations*.cpp")
    return sorted(glob.glob(pattern))


def find_matching_paren(text: str, open_paren_index: int) -> int:
    """
    @brief open_paren_indexの'('に対応する')'の位置を返す

    Args:
        text: 走査対象のテキスト
        open_paren_index: '('の位置

    Returns:
        対応する')'の位置

    Raises:
        ValueError: 括弧の対応が取れない場合
    """
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
    """
    @brief name_match_endの直後から始まる引数リストを伴う関数本体の位置を返す

    Args:
        text: 走査対象のC++ソース全体
        name_match_end: 関数名の一致が終わる位置

    Returns:
        (brace_open, brace_close) の組
        本体を囲む波括弧の開始位置と終了位置

    Raises:
        ValueError: 関数本体の終端が見つからない場合

    Note:
        波括弧の対応を取る際は文字列、文字リテラル、//と/* */のコメントを読み飛ばすため、
        ログメッセージやコメント中に紛れ込んだ波括弧が走査を狂わせることはない
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


def collect_functions(text: str) -> dict[str, tuple[int, int, int]]:
    """
    @brief text中の全SnapshotOperations::<name>( ... ) { ... }定義を収集する

    Args:
        text: 走査対象のC++ソース全体

    Returns:
        名前をキー、(def_start, brace_open, brace_close) を値とする辞書
    """
    functions: dict[str, tuple[int, int, int]] = {}
    for match in FUNCTION_DEF_RE.finditer(text):
        name = match.group(1)
        if name in functions:
            continue  # 最初の出現を採用する (このコードでは名前ごとに定義は一意)
        try:
            brace_open, brace_close = find_function_body_span(text, match.end(1))
        except ValueError:
            continue
        functions[name] = (match.start(), brace_open, brace_close)
    return functions


def line_of(text: str, offset: int) -> int:
    """
    @brief offsetを含む1始まりの行番号を返す

    Args:
        text: 走査対象のテキスト
        offset: 位置

    Returns:
        1始まりの行番号
    """
    return text.count("\n", 0, offset) + 1


def find_non_code_ranges(text: str) -> list[tuple[int, int]]:
    """
    @brief コメントまたは文字列リテラル内側の位置範囲を返す

    Args:
        text: 走査対象のC++ソース全体

    Returns:
        //行コメント、/* */ブロックコメント、"文字列"、'文字'リテラルの内側にある
        [start, end) 形式の位置範囲リスト

    Note:
        文章中にのみ現れる認可呼び出し (例 : 関数名に言及したDoxygenコメント) を除外し、
        実際の呼び出し位置と誤認しないために使う
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
    """
    @brief 指定位置が非コード範囲のいずれかに含まれるかを返す

    Args:
        offset: 判定する位置
        ranges: find_non_code_ranges()が返す位置範囲リスト

    Returns:
        offsetが範囲内ならTrue
    """
    return any(start <= offset < end for start, end in ranges)


def real_call_offsets(text: str, ranges: list[tuple[int, int]]) -> list[int]:
    """
    @brief 実コード中の認可呼び出しの位置を全て返す

    Args:
        text: 走査対象のC++ソース全体
        ranges: 非コード範囲のリスト

    Returns:
        CALL_SITE_REに一致し、コメント、文字列、文字リテラルの内側ではなく、
        "SnapshotOperations::beginAuthorization("の定義自体でもない位置のリスト
    """
    return [m.start() for m in CALL_SITE_RE.finditer(text)
            if not is_in_non_code_range(m.start(), ranges)]


def call_offsets_in_span(offsets: list[int], span_start: int, span_end: int) -> list[int]:
    """
    @brief 指定範囲内にある呼び出し位置だけを返す

    Args:
        offsets: 呼び出し位置のリスト
        span_start: 範囲の開始位置
        span_end: 範囲の終了位置

    Returns:
        span_start以上span_end以下にある位置のリスト
    """
    return [off for off in offsets if span_start <= off <= span_end]


def check_restore_gate(text: str,
                       functions: dict[str, tuple[int, int, int]],
                       non_code_ranges: list[tuple[int, int]],
                       failures: list[str]) -> None:
    """
    @brief 復元の実行中に非復元操作を拒否する共通ゲートが崩れていないことを検査する

    ゲートはヘッダのテンプレート (authorizeThenとcompleteDeferredCall) にあるため、
    snapshotoperations.hを読み、括弧の対応で本体を切り出して呼び出しの順序を確かめる

    Args:
        text: 連結済みのsnapshotoperations*.cpp全体
        functions: collect_functions()の結果
        non_code_ranges: textのコメントと文字列の範囲
        failures: 違反を追記するリスト (本関数が変更する)
    """
    header_path = os.path.join(repo_root(), "src", "dbusservice", "snapshotoperations.h")
    try:
        with open(header_path, "r", encoding="utf-8") as handle:
            header = handle.read()
    except OSError as exc:
        failures.append(f"cannot read {header_path}: {exc}")
        return

    def template_body(pattern: re.Pattern[str], label: str) -> str | None:
        """
        @brief ヘッダからテンプレート関数の本体を切り出す

        Args:
            pattern: 関数定義に一致する正規表現
            label: 失敗時に表示する関数名

        Returns:
            波括弧を含む関数本体、見つからない場合はNone
        """
        match = pattern.search(header)
        if match is None:
            failures.append(f"{label} definition not found in snapshotoperations.h")
            return None
        try:
            brace_open, brace_close = find_function_body_span(header, match.end() - 1)
        except ValueError:
            failures.append(f"{label} body not found by brace matching")
            return None
        return header[brace_open:brace_close + 1]

    authorize_then = template_body(AUTHORIZE_THEN_DEF_RE, "authorizeThen")
    if authorize_then is not None:
        gate_offsets = [m.start() for m in re.finditer(re.escape(RESTORE_GATE_CALL), authorize_then)]
        auth_offset = authorize_then.find("beginAuthorization(")
        body_offset = authorize_then.rfind(f"return {AUTHORIZED_BODY_CALL};")
        print(f"[10] authorizeThen gate offsets {gate_offsets}, "
              f"beginAuthorization at {auth_offset}, last body() at {body_offset}")
        if len(gate_offsets) < 2:
            failures.append(
                f"authorizeThen must call {RESTORE_GATE_CALL} before authorization "
                f"and again before running the body"
            )
        elif not (gate_offsets[0] < auth_offset < gate_offsets[-1] < body_offset):
            failures.append(
                f"authorizeThen must call {RESTORE_GATE_CALL} before beginAuthorization( "
                f"and again between authorization and return {AUTHORIZED_BODY_CALL};"
            )

    complete_deferred = template_body(COMPLETE_DEFERRED_DEF_RE, "completeDeferredCall")
    if complete_deferred is not None:
        gate_offset = complete_deferred.find(RESTORE_GATE_CALL)
        body_offset = complete_deferred.find(AUTHORIZED_BODY_CALL)
        print(f"[10] completeDeferredCall gate at {gate_offset}, body() at {body_offset}")
        if gate_offset < 0 or body_offset < 0 or gate_offset > body_offset:
            failures.append(
                f"completeDeferredCall must call {RESTORE_GATE_CALL} before {AUTHORIZED_BODY_CALL} "
                f"(a restore can start while the Polkit prompt is pending)"
            )

    gate_impl = functions.get("rejectWhileRestoreRunning")
    if gate_impl is None:
        failures.append("SnapshotOperations::rejectWhileRestoreRunning definition not found")
    elif "m_restoreExecutions.isEmpty()" not in text[gate_impl[1]:gate_impl[2] + 1]:
        failures.append("rejectWhileRestoreRunning no longer checks m_restoreExecutions")

    exempt_sites: set[str] = set()
    for match in re.finditer(RESTORE_GATE_EXEMPT_TOKEN, text):
        if is_in_non_code_range(match.start(), non_code_ranges):
            continue
        enclosing = "<UNRESOLVED>"
        for name, (_, brace_open, brace_close) in functions.items():
            if brace_open <= match.start() <= brace_close:
                enclosing = name
                break
        exempt_sites.add(enclosing)
    print(f"[10] methods exempt from the restore gate: {sorted(exempt_sites)}")
    unexpected = sorted(exempt_sites - RESTORE_GATE_EXEMPT_METHODS)
    if unexpected:
        failures.append(
            f"{RESTORE_GATE_EXEMPT_TOKEN} used outside {sorted(RESTORE_GATE_EXEMPT_METHODS)}: "
            f"{unexpected} -- only methods that never touch the shared Snapper instance, "
            f"comparison cache or mount state may run during a restore"
        )


def main() -> int:
    """
    @brief 旧経路と段階的経路の認可境界を解析して検査する

    Returns:
        0 = 検査成功、1 = 違反ありまたはソースが見つからない
    """
    parser = argparse.ArgumentParser(
                 description="Prove the legacy and staged restore authorization "
                             "boundaries in snapshotoperations*.cpp are unchanged."
    )
    parser.add_argument("--source", nargs="*", default=None,
                        help="Paths to src/dbusservice/snapshotoperations*.cpp "
                             "(default: all of them)")
    args = parser.parse_args()

    sources: list[str] = list(args.source) if args.source else default_source_paths()
    if not sources:
        print("FAIL: no src/dbusservice/snapshotoperations*.cpp files found")
        return 1

    parts: list[str] = []
    line_base = 1
    for source in sources:
        print(f"SOURCE: {source}")
        try:
            with open(source, "r", encoding="utf-8") as handle:
                content = handle.read()
        except OSError as exc:
            print(f"FAIL: cannot read source: {exc}")
            return 1
        print(f"  lines 1-{content.count(chr(10)) + 1} "
              f"mapped to combined lines {line_base}-"
              f"{line_base + content.count(chr(10))}")
        line_base += content.count("\n") + 2
        parts.append(f"// ===== boundary-check source: {source} =====\n{content}")
    text = "\n".join(parts)

    functions = collect_functions(text)
    print(f"Parsed {len(functions)} SnapshotOperations:: function definitions "
          f"by brace matching.")

    non_code_ranges = find_non_code_ranges(text)
    call_offsets = real_call_offsets(text, non_code_ranges)
    print(f"Found {len(call_offsets)} real (non-comment/string) "
          f"{AUTH_CALL_LABEL} occurrences in the whole file.")

    failures: list[str] = []

    def require_function(name: str) -> str | None:
        """
        @brief 名前に対応する関数本体を取得し、無ければ失敗を記録する

        Args:
            name: 取得する関数名

        Returns:
            関数本体のテキスト、見つからない場合はNone
        """
        if name not in functions:
            failures.append(f"function not found by brace matching: {name}")
            return None
        _, brace_open, brace_close = functions[name]
        return text[brace_open:brace_close + 1]

    def calls_in_function(name: str) -> list[int]:
        """
        @brief 名前の本体範囲内にある実呼び出し位置 (絶対オフセット) を返す

        Args:
            name: 対象の関数名

        Returns:
            本体範囲内の呼び出し位置のリスト、関数が無い場合は空リスト
        """
        if name not in functions:
            return []
        _, brace_open, brace_close = functions[name]
        return call_offsets_in_span(call_offsets, brace_open, brace_close)

    # --- 1: 削除済みの旧復元経路が再定義されていないこと ---
    reintroduced = [name for name in REMOVED_LEGACY_FUNCTIONS if name in functions]
    print(f"[1] removed legacy restore functions defined = "
          f"{reintroduced if reintroduced else 'none'}")
    if reintroduced:
        failures.append(
            f"removed legacy restore functions were reintroduced: {reintroduced}"
            f" -- restore must go through the staged single-authorization path"
        )

    # --- 3: StageRestoreEntriesの認可は0回、BeginRestorePlanはview-diffでちょうど1回であること ---
    begin_body = require_function(STAGE_BEGIN)
    if begin_body is not None:
        call_count = len(calls_in_function(STAGE_BEGIN))
        print(f"[3] {STAGE_BEGIN}: {AUTH_CALL_LABEL} call count = {call_count} "
              f"(must be 1, action {STAGE_BEGIN_ACTION})")
        if call_count != 1:
            failures.append(
                f"{STAGE_BEGIN} must call {AUTH_CALL_LABEL} exactly once, "
                f"found {call_count}"
            )
        if STAGE_BEGIN_ACTION not in begin_body:
            failures.append(f"{STAGE_BEGIN} must authorize with {STAGE_BEGIN_ACTION}")
        if STAGE_COMMIT_ACTION in begin_body:
            failures.append(
                f"{STAGE_BEGIN} must not request {STAGE_COMMIT_ACTION}; "
                f"the restore is authorized only by {STAGE_COMMIT}"
            )
        if "m_restoreRegistry.createStaging(" in begin_body:
            failures.append(
                f"{STAGE_BEGIN} must create the plan only inside {STAGE_BEGIN_BODY} "
                f"(after authorization)"
            )
    if STAGE_BEGIN_BODY in functions:
        _, body_open, body_close = functions[STAGE_BEGIN_BODY]
        if "m_restoreRegistry.createStaging(" not in text[body_open:body_close]:
            failures.append(f"{STAGE_BEGIN_BODY} no longer creates the staging plan")
    else:
        failures.append(f"{STAGE_BEGIN_BODY} definition was not found")

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

    # --- 4: CommitRestorePlanの認可はfreeze(の後にちょうど1回であること ---
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

    # --- 5: コミット後の制御メソッドの認可は0回であること ---
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

    # --- 6: StageRestoreEntriesは容量超過のチャンクをInvalidArgsで拒否し、
    # m_restoreRegistry.stageEntries(を呼ぶ前に戻ること ---
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

    # --- 7: ファイル全体の実認可呼び出し位置を列挙し、その所属関数を出力する
    # 所属関数の集合がレビュー済み基準からずれた場合、または所属関数を特定できない
    # 呼び出しがある場合は失敗させる
    # 段階的復元の制御メソッドの認可0回は、[3]と[5]が同じ呼び出し位置に対して既に検査している
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

    # --- 8: 全ての*Authorized本体がゲートの厳密に後ろにあること ---
    # 認可済み本体は認可する関数から以外は到達できてはならず、自身も二度目の認可をしてはならない
    # 操作の途中での二度目のプロンプトは、「ちょうど1回だけ認可する」特性を
    # コミット後の制御メソッドでの認可と同じくらい損なう
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

    # --- 9: sendErrorReply(はreplyError()に限定されること ---
    # スロットが戻った後 (polkitプロンプトが開いている間はそうなる) では、
    # QDBusContext::sendErrorReply()には呼び出しコンテキストが残っておらず、エラーを静かに捨てる
    # これに触れてよい唯一の場所はreplyError()であり、そこでは捕捉済みのQDBusMessageを通して
    # 遅延応答を処理する
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

    # [10] 復元の実行中に非復元操作を拒否する共通ゲート
    # 共有のSnapperインスタンスを差し替える操作が復元中に走ると、実行中の復元が壊れる
    # 認可待ちの間に復元が始まり得るため、認可前に加えて本体の直前でも再確認していることを検査する
    print()
    check_restore_gate(text, functions, non_code_ranges, failures)

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
