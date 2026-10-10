# FC2 Co-op Mod - Automated Testing Workflow

## Quick Start

### 1. Build & Verify
```bash
cd /home/host/Downloads/FC2-Coop-Project
./build_and_verify.sh
```

### 2. Manual Test Session
1. Launch Far Cry 2 via Lutris
2. Navigate menus (press ENTER for each screen):
   - OK dialog → ENTER
   - Extra screen → ENTER  
   - Story Mode → ENTER
   - Continue → ENTER
   - (Press ENTER 5-8 times total)
3. Wait for `[STATE] In-game state confirmed` in log
4. Test hotkeys:
   - **F4** - Anchor marker (should log `[F4] Anchored 3D marker`)
   - **F7** - Spawn buddy (should log `[SPAWN-KEY]` → `[SPAWN] Main thread...` → `[SPAWN-SUCCESS]`)
   - **F8** - Teleport buddy (should log `[TELEPORT] Processing...` → `[TELEPORT] SUCCESS`)
   - **F3** - Toggle TX (should log `[TX] Transmission PAUSED/RESUMED`)
   - **Check log for `[KEY-DEBUG]` every 10s to verify key detection**
5. Exit game cleanly

### 3. Verify Log
```bash
python3 verify_log.py
```

## Automated Test (requires xdotool)

```bash
# Install dependencies
sudo pacman -S xdotool  # or apt install xdotool

# Run full automated suite
python3 test_coop.py
```

## Test Coverage

| Test | What It Verifies |
|------|------------------|
| Initialization | Proxy attaches, Dunia found, D3D9 hook active, UDP ready |
| In-Game State | Camera stability detection works (2s stable = in-game) |
| F4 Marker | Marker anchors at camera forward position |
| F7 Spawn | Buddy spawns (BUDDY_ archetype, not enemy) |
| F8 Teleport | Teleport processes on main thread, no crash |
| UDP TX | Position broadcasts at 20Hz |
| Clean Shutdown | DLL detaches cleanly, no crash on exit |

## Key Log Patterns to Watch

### Good (Expected)
```
[INIT] binkw32 proxy attached
[SUCCESS] Dunia.dll located at 0x10000000
[STATE] In-game state confirmed (stable camera for 2s)
[CAMERA-LOCK] Active camera found at 0x04D79C6C
[F4] Anchored 3D marker 15.0m ahead at (...)
[SPAWN-KEY] Spawn buddy requested (archetype: BUDDY_Hakim_Echebbi)
[SPAWN-SUCCESS] Archetype 'BUDDY_Hakim_Echebbi' spawned!
[TELEPORT] Processing teleport request, entity=0x197CB7C0
[TELEPORT] SUCCESS: Moved Player 2 Buddy to marker position
[UDP TX #400] Pos: (3286.0, 1526.1, 22.8)
[SHUTDOWN] Proxy detaching
```

### Bad (Issues)
```
[SPAWN-SUCCESS] Archetype 'enemy_archetypes.Red_Faction...'  # Wrong archetype
[TELEPORT] EXCEPTION during SetPosition...                    # Crash
[SETPOS] Invalid entity pointer...                            # Stale pointer
IsBadReadPtr...                                               # Deprecated API
__try/__except...                                             # SEH (clang compat)
```

## Troubleshooting

### F7 Crashes Game
- **Cause**: CBuddiesManager Lua calls crash engine
- **Fix**: Archetype loop is now primary; CBuddiesManager only as last resort

### F8 Crashes Game  
- **Cause**: Render thread calling engine functions
- **Fix**: Teleport queued to main thread via `g_teleportPending`

### Marker Flickers in Loading
- **Cause**: Camera context invalid during loading screens
- **Fix**: `g_inGame` state gates all rendering/hotkeys

### Buddy Spawns as Enemy
- **Cause**: BUDDY_ archetypes not tried first
- **Fix**: Candidate list prioritizes all 10 BUDDY_ archetypes

### F3 Spam
- **Cause**: No key debouncing
- **Fix**: 300ms debounce on all hotkeys

## Architecture Notes for Future Work

### Thread Safety
- All Dunia engine calls (`SetPosition`, `SetRotation`, `SpawnEntity`) → **main thread only** (`CoopThread`)
- Render thread (`HookedPresent`) only queues requests via atomics
- Shared state protected by `std::atomic<>` + `CRITICAL_SECTION`

### Memory Safety
- `SafeReadPtr()` uses `VirtualQuery` instead of deprecated `IsBadReadPtr`
- All pointer dereferences validated before engine calls
- Entity pointers re-validated on each use

### Milestone 5 Preparation (Animation/Action Sync)
- Need to hook animation state, weapon fire events
- Network protocol needs event packets (not just position)
- Consider dedicated packet types: `WEAPON_FIRE`, `STANCE_CHANGE`, `HEALTH_UPDATE`