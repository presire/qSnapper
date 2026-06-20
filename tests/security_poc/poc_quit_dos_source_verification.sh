#!/bin/bash
# Source-level verification of Quit() removal (issue #5b)
# This is a source-code equivalent of the D-Bus PoC test.
# Since we cannot build/run the service in this environment,
# we verify that Quit() is absent from the source code,
# which under ExportAllSlots is equivalent to verifying
# it's not exposed via D-Bus.

set -u

PROJECT_DIR="/home/suse/Program/Qt/qSnapper"
H_FILE="$PROJECT_DIR/src/dbusservice/snapshotoperations.h"
CPP_FILE="$PROJECT_DIR/src/dbusservice/snapshotoperations.cpp"

PASS=0

echo "=== Source-Level PoC: Quit() Method Removal Verification ==="
echo ""

# Test 1: Check header file for Quit() declaration
echo "Test 1: Checking header file for Quit() declaration..."
if grep -q "void Quit()" "$H_FILE"; then
    echo "  FAIL: Quit() declaration found in snapshotoperations.h"
    PASS=0
else
    echo "  PASS: No Quit() declaration in snapshotoperations.h"
    PASS=1
fi
echo ""

# Test 2: Check cpp file for Quit() implementation
echo "Test 2: Checking cpp file for Quit() implementation..."
if grep -q "SnapshotOperations::Quit()" "$CPP_FILE"; then
    echo "  FAIL: Quit() implementation found in snapshotoperations.cpp"
    PASS=0
else
    echo "  PASS: No Quit() implementation in snapshotoperations.cpp"
    PASS=1
fi
echo ""

# Test 3: Verify the class still has other methods (sanity check)
echo "Test 3: Sanity check - other D-Bus methods still present..."
if grep -q "ListConfigs()" "$H_FILE" && grep -q "IsConfigured()" "$H_FILE"; then
    echo "  PASS: Other D-Bus methods present (class structure intact)"
else
    echo "  FAIL: Other methods missing - class may be corrupted"
    PASS=0
fi
echo ""

# Test 4: Check git commit
echo "Test 4: Checking git commit..."
if git -C "$PROJECT_DIR" log --oneline -1 | grep -q "remove unauthenticated Quit"; then
    echo "  PASS: Git commit found with correct message"
else
    echo "  FAIL: Expected git commit not found"
    PASS=0
fi
echo ""

# Test 5: nm verification on standalone compilation
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
if [ "$PASS" -eq 1 ]; then
    echo "Quit() method is removed — good"
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
