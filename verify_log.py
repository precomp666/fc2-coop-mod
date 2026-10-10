#!/usr/bin/env python3
"""
Log verification script - run after manual test session.
Analyzes fc2_coop.log for expected patterns and reports issues.
"""

import sys
import re
from pathlib import Path

LOG_FILE = Path("/home/host/Downloads/FC2-Coop-Project/bin/fc2_coop.log")

PATTERNS = {
    "init": [
        r"\[INIT\] binkw32 proxy attached",
        r"\[SUCCESS\] Dunia\.dll located at 0x[0-9A-F]+",
        r"\[NET-TX\] Sender ready",
        r"\[NET-RX\] Listening",
        r"\[D3D9\] Present hook active",
    ],
    "in_game": [
        r"\[STATE\] In-game state confirmed",
    ],
    "camera": [
        r"\[CAMERA-LOCK\] Active camera found at 0x[0-9A-F]+",
    ],
    "f4_marker": [
        r"\[F4\] Anchored 3D marker",
        r"\[AUTO-INIT\] Test marker placed",
    ],
    "f7_spawn": [
        r"\[SPAWN-KEY\] Spawn buddy requested",
        r"\[SPAWN-TRY\] Trying archetype",
        r"\[SPAWN-SUCCESS\] Archetype",
    ],
    "f8_teleport": [
        r"\[TELEPORT-KEY\] Teleport buddy requested",
        r"\[TELEPORT-KEY\] Teleport queued for main thread",
        r"\[TELEPORT\] Processing teleport request",
        r"\[TELEPORT\] SUCCESS",
    ],
    "udp": [
        r"\[UDP TX #\d+\] Pos:",
    ],
    "shutdown": [
        r"\[SHUTDOWN\] Proxy detaching",
    ],
    "errors": [
        r"\[SPAWN\] Lua error",
        r"EXCEPTION",
        r"Invalid entity pointer",
        r"crash",
    ],
    "bad_spawn": [
        r"enemy_archetypes",  # Should not spawn enemies as buddies
    ]
}

def check_patterns(log_content, category, patterns):
    found = []
    missing = []
    for p in patterns:
        matches = re.findall(p, log_content)
        if matches:
            found.append(f"  ✓ {p} ({len(matches)}x)")
        else:
            missing.append(f"  ✗ {p}")
    return found, missing

def main():
    if not LOG_FILE.exists():
        print(f"ERROR: Log file not found: {LOG_FILE}")
        return 1
    
    with open(LOG_FILE, 'r') as f:
        content = f.read()
    
    print("=" * 60)
    print("FAR CRY 2 CO-OP MOD - LOG VERIFICATION")
    print("=" * 60)
    
    all_ok = True
    
    for category, patterns in PATTERNS.items():
        found, missing = check_patterns(content, category, patterns)
        status = "OK" if not missing else "ISSUES"
        if missing:
            all_ok = False
        print(f"\n[{category.upper()}] {status}")
        for f in found:
            print(f)
        for m in missing:
            print(m)
    
    # Summary
    print("\n" + "=" * 60)
    if all_ok:
        print("RESULT: ALL CHECKS PASSED ✓")
        return 0
    else:
        print("RESULT: ISSUES FOUND ✗")
        return 1

if __name__ == "__main__":
    sys.exit(main())