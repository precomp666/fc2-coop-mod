#!/bin/bash
# Quick Launch - Build and launch game for manual testing

set -euo pipefail

PROJECT_DIR="/home/host/Downloads/FC2-Coop-Project"
COOP_DEV="$PROJECT_DIR/coop_dev"
BIN_DIR="$PROJECT_DIR/bin"

echo "=== FC2 Co-op Mod Quick Launch ==="
echo ""

# 1. Build
echo "[1/3] Building mod..."
cd "$COOP_DEV"
./build.sh

# 2. Clear old log
echo "[2/3] Clearing old log..."
rm -f "$BIN_DIR/fc2_coop.log"

# 3. Launch game
echo "[3/3] Launching Far Cry 2 via Lutris..."
echo "   (Game will start in background)"
lutris "lutris:far-cry-2" >/dev/null 2>&1 &

echo ""
echo "=== LAUNCHED ==="
echo "Game starting... Check logs with:"
echo "  tail -f $BIN_DIR/fc2_coop.log"
echo ""
echo "MENU NAVIGATION (manual - press ENTER for each screen):"
echo "  1. OK dialog         -> press ENTER"
echo "  2. Extra screen      -> press ENTER"
echo "  3. Story Mode        -> press ENTER"
echo "  4. Continue          -> press ENTER"
echo "  5. Wait for loading  -> wait for '[STATE] In-game state confirmed'"
echo "  (Press ENTER 5-8 times total to get through all menus)"
echo ""
echo "Hotkeys (once in-game):"
echo "  F3 - Toggle TX"
echo "  F4 - Anchor marker"
echo "  F7 / Num7 - Spawn buddy (press multiple times to cycle archetypes)"
echo "  F8 / Num8 / F6 - Teleport buddy"
echo ""
echo "Verify after session:"
echo "  python3 $PROJECT_DIR/verify_log.py"