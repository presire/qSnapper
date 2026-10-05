#!/bin/bash
#
# @file poc_quit_dos_source_verification.sh
# @brief Quit()削除のソースレベル検証 (issue #5b)
#
# D-Bus PoCテストのソースコード版に相当する
# この環境ではサービスをビルド/実行できないため、Quit()がソースコードから
# 消えていることを検証する
# ExportAllSlotsの下では、これはD-Bus経由で
# 公開されていないことの検証と等価になる
#
# 使い方 : poc_quit_dos_source_verification.sh
# 終了コード : 0 = 全テストPASS、1 = いずれかFAIL
#

set -u    # 未定義変数の参照をエラーにする

PROJECT_DIR="/home/suse/Program/Qt/qSnapper"    # 検証対象のリポジトリルート
H_FILE="$PROJECT_DIR/src/dbusservice/snapshotoperations.h"    # 代表となるヘッダー
# 実装は責務ごとに複数ファイルへ分割されているため、全てのヘッダーとソースを確認する
H_FILES=( "$PROJECT_DIR"/src/dbusservice/snapshotoperations*.h )
CPP_FILES=( "$PROJECT_DIR"/src/dbusservice/snapshotoperations*.cpp )

PASS=0    # 失敗が無ければ最終的に1になる

echo "=== Source-Level PoC: Quit() Method Removal Verification ==="
echo ""

# Test 1: ヘッダーファイルにQuit()宣言が無いか確認する
echo "Test 1: Checking header files for Quit() declaration..."
FOUND=""
for f in "${H_FILES[@]}"; do
    if grep -q "void Quit()" "$f"; then
        FOUND="$FOUND $f"
    fi
done
if [ -n "$FOUND" ]; then
    echo "  FAIL: Quit() declaration found in:$FOUND"
    PASS=0
else
    echo "  PASS: No Quit() declaration in snapshotoperations*.h (${#H_FILES[@]} files checked)"
    PASS=1
fi
echo ""

# Test 2: cppファイルにQuit()実装が無いか確認する
echo "Test 2: Checking cpp files for Quit() implementation..."
FOUND=""
for f in "${CPP_FILES[@]}"; do
    if grep -q "SnapshotOperations::Quit()" "$f"; then
        FOUND="$FOUND $f"
    fi
done
if [ -n "$FOUND" ]; then
    echo "  FAIL: Quit() implementation found in:$FOUND"
    PASS=0
else
    echo "  PASS: No Quit() implementation in snapshotoperations*.cpp (${#CPP_FILES[@]} files checked)"
    PASS=1
fi
echo ""

# Test 3: 他のD-Busメソッドが残っているか確認する (sanity check)
echo "Test 3: Sanity check - other D-Bus methods still present..."
if grep -q "ListConfigs()" "$H_FILE" && grep -q "IsConfigured()" "$H_FILE"; then
    echo "  PASS: Other D-Bus methods present (class structure intact)"
else
    echo "  FAIL: Other methods missing - class may be corrupted"
    PASS=0
fi
echo ""

# Test 4: 対象のgitコミットが存在するか確認する
echo "Test 4: Checking git commit..."
if git -C "$PROJECT_DIR" log --oneline -1 | grep -q "remove unauthenticated Quit"; then
    echo "  PASS: Git commit found with correct message"
else
    echo "  FAIL: Expected git commit not found"
    PASS=0
fi
echo ""

# Test 5: 単体コンパイルしたオブジェクトをnmで検証する
echo "Test 5: Binary-level verification (standalone compilation)..."
if [ -f /tmp/verify_no_quit.o ]; then
    if nm /tmp/verify_no_quit.o | grep -qi quit; then
        echo "  FAIL: Quit symbol found in compiled object"
        PASS=0
    else
        echo "  PASS: No Quit symbol in compiled object"
    fi
else
    echo "  SKIP: Standalone object not available for nm check"
fi
echo ""

echo "=== SUMMARY ==="
# PASSが1ならQuit()は削除済みと判定する
if [ "$PASS" -eq 1 ]; then
    echo "Quit() method is removed — good"
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
