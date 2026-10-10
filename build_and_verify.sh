#!/bin/bash
# Build and verification script for FC2 Co-op Mod

set -e

PROJECT_DIR="/home/host/Downloads/FC2-Coop-Project"
COOP_DEV="$PROJECT_DIR/coop_dev"
BIN_DIR="$PROJECT_DIR/bin"

echo "=== FC2 Co-op Mod Build & Verify ==="
echo ""

# 1. Build
echo "[1/4] Building proxy DLL..."
cd "$COOP_DEV"
./build.sh

# 2. Verify DLL exists and has correct size
echo ""
echo "[2/4] Verifying DLL..."
DLL="$BIN_DIR/binkw32.dll"
ORIG="$BIN_DIR/binkw32_orig.dll"

if [ ! -f "$DLL" ]; then
    echo "ERROR: DLL not found at $DLL"
    exit 1
fi

SIZE=$(stat -c%s "$DLL")
echo "  DLL size: $SIZE bytes"

if [ ! -f "$ORIG" ]; then
    echo "WARNING: Original DLL backup not found at $ORIG"
fi

# 3. Verify exports (should forward all 71)
echo ""
echo "[3/4] Checking exports..."
EXPORTS=$(objdump -p "$DLL" | grep -A 100 "Export Address Table" | grep "Forwarder RVA" | wc -l)
echo "  Forwarded exports: $EXPORTS"

if [ "$EXPORTS" -ne 71 ]; then
    echo "WARNING: Expected 71 forwarded exports, got $EXPORTS"
fi

# Check imports
echo ""
echo "  Key imports:"
objdump -p "$DLL" | grep "DLL Name:" | head -10

# 4. Quick syntax check on main.cpp
echo ""
echo "[4/4] Checking source..."
if grep -q "__try\|__except" "$COOP_DEV/main.cpp"; then
    echo "  WARNING: SEH (__try/__except) found in main.cpp - may not compile on clang"
else
    echo "  OK: No SEH in main.cpp"
fi

if grep -q "IsBadReadPtr" "$COOP_DEV/main.cpp"; then
    echo "  WARNING: IsBadReadPtr found - deprecated"
else
    echo "  OK: No IsBadReadPtr"
fi

if grep -q "std::atomic" "$COOP_DEV/main.cpp"; then
    echo "  OK: Thread-safe atomics used"
else
    echo "  WARNING: No std::atomic found"
fi

echo ""
echo "=== Build verification complete ==="
echo "DLL ready at: $DLL"
echo "Run game via Lutris to test."
echo "Verify log with: python3 $PROJECT_DIR/verify_log.py"