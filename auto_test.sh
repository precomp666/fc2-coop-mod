#!/bin/bash
# Automated FC2 Co-op Mod Test Launcher
# Launches game via Lutris, runs test sequence, collects logs

set -euo pipefail

PROJECT_DIR="/home/host/Downloads/FC2-Coop-Project"
BIN_DIR="$PROJECT_DIR/bin"
LOG_FILE="$BIN_DIR/fc2_coop.log"
COOP_DEV="$PROJECT_DIR/coop_dev"
RESULTS_DIR="$PROJECT_DIR/test_results"

# Configuration
LUTRIS_GAME_SLUG="far-cry-2"
TEST_DURATION=300  # 5 minutes max test time
KEY_DELAY=2        # seconds between key presses

# Menu navigation delays (adjust if needed)
MENU_DELAY=3       # seconds after menu selection
LOADING_DELAY=5    # initial loading wait

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date +%H:%M:%S)] $*${NC}"; }
warn() { echo -e "${YELLOW}[$(date +%H:%M:%S)] $*${NC}"; }
error() { echo -e "${RED}[$(date +%H:%M:%S)] $*${NC}"; }

mkdir -p "$RESULTS_DIR"

# Check dependencies
check_deps() {
    local missing=()
    for cmd in lutris xdotool python3; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        error "Missing dependencies: ${missing[*]}"
        error "Install: sudo pacman -S ${missing[*]}  (or apt/dnf)"
        exit 1
    fi
}

# Build the mod
build_mod() {
    log "Building mod..."
    cd "$COOP_DEV"
    ./build.sh
    log "Build complete"
}

# Clear old log
clear_log() {
    if [ -f "$LOG_FILE" ]; then
        mv "$LOG_FILE" "$LOG_FILE.$(date +%s).bak"
    fi
}

# Launch game via Lutris
launch_game() {
    log "Launching Far Cry 2 via Lutris..."
    lutris "lutris:$LUTRIS_GAME_SLUG" >/dev/null 2>&1 &
    LUTRIS_PID=$!
    
    # Wait for game process to appear
    local timeout=60
    local start=$(date +%s)
    while [ $(($(date +%s) - start)) -lt $timeout ]; do
        if pgrep -f "FarCry2.exe" >/dev/null; then
            log "Game process detected"
            sleep 3  # Let it initialize
            return 0
        fi
        sleep 1
    done
    error "Game failed to start within ${timeout}s"
    return 1
}

# Navigate game menus to reach in-game
navigate_menus() {
    log "=== NAVIGATING GAME MENUS ==="
    
    # Wait for game window to be ready
    log "Waiting for game window..."
    local timeout=30
    local start=$(date +%s)
    while [ $(($(date +%s) - start)) -lt $timeout ]; do
        GAME_WINDOW=$(xdotool search --name "Far Cry 2" 2>/dev/null | head -1)
        if [ -n "$GAME_WINDOW" ]; then
            log "Game window found: $GAME_WINDOW"
            break
        fi
        sleep 1
    done
    
    if [ -z "$GAME_WINDOW" ]; then
        error "Game window not found!"
        return 1
    fi
    
    # Focus the window
    xdotool windowactivate "$GAME_WINDOW"
    sleep 1
    
    sleep "$LOADING_DELAY"
    
    # Generic menu navigation: press Enter multiple times for all screens
    # This handles: OK dialog → extra screen → Story Mode → Continue → loading
    log "Pressing Enter for all menu screens (up to 8 screens)..."
    for i in {1..8}; do
        log "Menu screen $i: pressing Enter on window $GAME_WINDOW"
        xdotool key --window "$GAME_WINDOW" --delay 100 Return
        sleep "$MENU_DELAY"
    done
    
    # Extra wait for loading screen
    log "Waiting for loading to complete..."
    sleep 15
    
    log "Menu navigation complete"
}

# Wait for in-game state (via mod log)
wait_for_ingame() {
    log "Waiting for in-game state confirmation from mod..."
    local timeout=120
    local start=$(date +%s)
    
    while [ $(($(date +%s) - start)) -lt $timeout ]; do
        if [ -f "$LOG_FILE" ] && grep -q "In-game state confirmed" "$LOG_FILE"; then
            log "In-game state confirmed by mod!"
            return 0
        fi
        sleep 1
    done
    error "Timeout waiting for in-game state"
    return 1
}

# Global game window ID
GAME_WINDOW=""

# Send key press to game window
send_key() {
    local key=$1
    local desc=$2
    if [ -z "$GAME_WINDOW" ]; then
        GAME_WINDOW=$(xdotool search --name "Far Cry 2" 2>/dev/null | head -1)
    fi
    if [ -n "$GAME_WINDOW" ]; then
        log "Sending $desc ($key) to window $GAME_WINDOW"
        xdotool key --window "$GAME_WINDOW" --delay 100 "$key"
    else
        warn "Game window not found, sending to active window"
        xdotool key --delay 100 "$key"
    fi
    sleep "$KEY_DELAY"
}

# Run test sequence
run_tests() {
    log "=== STARTING TEST SEQUENCE ==="
    
    # Test 1: F4 - Anchor marker
    send_key "F4" "Anchor Marker"
    sleep 1
    
    # Test 2: F3 - Toggle TX
    send_key "F3" "Toggle TX"
    sleep 1
    
    # Test 3: F7 - Spawn buddy (may need multiple presses)
    for i in {1..5}; do
        send_key "F7" "Spawn Buddy (attempt $i)"
        sleep 2
        if grep -q "SPAWN-SUCCESS" "$LOG_FILE" 2>/dev/null; then
            log "Buddy spawned successfully!"
            break
        fi
    done
    
    # Test 4: F8 - Teleport buddy
    send_key "F8" "Teleport Buddy"
    sleep 2
    
    # Test 5: F6 - Alt teleport
    send_key "F6" "Alt Teleport"
    sleep 1
    
    # Test 6: Num7/Num8
    send_key "KP_7" "Num7 Spawn"
    sleep 2
    send_key "KP_8" "Num8 Teleport"
    sleep 1
    
    log "=== TEST SEQUENCE COMPLETE ==="
}

# Monitor for crashes
monitor_crashes() {
    while pgrep -f "FarCry2.exe" >/dev/null; do
        if [ -f "$LOG_FILE" ] && grep -q "EXCEPTION\|crash\|Invalid entity pointer" "$LOG_FILE" 2>/dev/null; then
            error "Crash detected in log!"
            return 1
        fi
        sleep 1
    done
    return 0
}

# Collect results
collect_results() {
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local result_file="$RESULTS_DIR/test_$timestamp.log"
    
    log "Collecting results..."
    
    # Copy log
    cp "$LOG_FILE" "$result_file" 2>/dev/null || true
    
    # Run verification
    python3 "$PROJECT_DIR/verify_log.py" > "$result_file.verification" 2>&1 || true
    
    # Summary
    echo ""
    log "=== TEST SUMMARY ==="
    if [ -f "$result_file.verification" ]; then
        cat "$result_file.verification"
    fi
    
    log "Results saved to: $result_file"
}

# Cleanup
cleanup() {
    log "Cleaning up..."
    pkill -f "FarCry2.exe" 2>/dev/null || true
    pkill -f "lutris" 2>/dev/null || true
    sleep 2
}

# Main
main() {
    log "=== FC2 Co-op Mod Automated Test ==="
    
    check_deps
    build_mod
    clear_log
    
    trap cleanup EXIT
    
    if ! launch_game; then
        exit 1
    fi
    
    # Navigate menus to reach in-game
    if ! navigate_menus; then
        error "Menu navigation failed"
        exit 1
    fi
    
    # Wait for mod to confirm in-game state
    if ! wait_for_ingame; then
        exit 1
    fi
    
    run_tests
    
    # Wait a bit more for any delayed logs
    sleep 5
    
    collect_results
    log "Test complete!"
}

main "$@"