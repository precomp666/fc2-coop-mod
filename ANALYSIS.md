# Far Cry 2 Co-op Mod - Code Analysis & Issue Report

## Project Overview
Reverse engineering project implementing a complete co-op campaign for Far Cry 2 single player. Uses DLL proxy injection (`binkw32.dll` → `binkw32_orig.dll`) with:
- UDP networking (ports 42069/42070) for position synchronization
- D3D9 vtable hooks for 3D waypoint overlay rendering
- Lua 4.0 bridge for entity spawning/manipulation via Dunia engine
- Camera context auto-discovery for world-to-screen projection

## Current State: Milestone 4 Complete ✓
- DLL proxying & process attachment
- Coordinate tracking & UDP transmission (20 Hz)
- D3D9 waypoint overlay & 3D projection
- Entity spawning with multi-archetype fallback & Lua 4.0 compatibility

## Critical Issues Found

### 1. Missing Header Includes (Compilation Risk)
```cpp
// main.cpp uses va_list, va_start, va_end, vfprintf but missing:
#include <stdarg.h>
#include <cstring>  // for strlen, strcmp, strncpy
#include <cstdio>   // for vsnprintf
```

### 2. Thread Safety Violations (Crash Risk)
**Problem**: Multiple threads access shared globals with only `volatile` (insufficient for C++ memory model)
- `CoopThread` (main loop), `ReceiverThread` (UDP), `HookedPresent` (render thread)
- Shared: `g_remoteX/Y/Z`, `g_hasRemotePlayer`, `g_p2EntityPtr`, `g_trackedPlayerAddr`, `g_luaCallbacksRegistered`

**Fix Required**: Use `std::atomic` or proper mutex protection (Windows `CRITICAL_SECTION`)

### 3. Deprecated `IsBadReadPtr` Usage (Unreliable)
**Problem**: `IsBadReadPtr` is deprecated, causes false positives/negatives, can crash the process
- Used in: `GetLuaState()`, `SetEntityPositionAndRotation()`, `MyBuddySpawnedHandler()`

**Fix**: Replace with `__try/__except` structured exception handling or validate pointers via known-good ranges

### 4. Hardcoded Memory Offsets (Version Fragility)
All offsets are for a specific game build (likely 1.03 patch):
```
Dunia.dll base:       0x10000000
Script System:        0x11606728
lua_dostring:         0x102ab630
lua_tostring:         0x102aad40
RegisterLuaGlobal:    0x102aa850
CEntityManager:       0x11644E80
GetEntityFromId:      0x10055820
InvalidateCache:      0x104dd4f0
SetPosition:          0x104df700
SetRotation:          0x104df7b0
GetLocalPlayerId:     0x1058fa00
GetEntityName:        0x10590c20
SpawnEntityFromArchetype: 0x10591100
RemoveEntity:         0x105911b7
TeleportEntity:       0x109fa080
```

**Risk**: Any game update, different regional version, or Steam/GOG difference breaks the mod.

### 5. D3D9 Hook Installed on Temporary Device (Potential Instability)
```cpp
// Creates temp device with SOFTWARE_VERTEX_PROCESSING
// But game uses HARDWARE vertex processing
// VTable indices 17 (Present) and 42 (EndScene) may differ!
```

**Fix**: Hook the game's actual device by finding it via pattern scanning or waiting for `Present` call

### 6. No D3D Hook Cleanup on Detach (Resource Leak)
- VTable hooks not restored in `DLL_PROCESS_DETACH`
- Receiver thread not terminated
- Log file handle cleanup only in detach path

### 7. Lua Error Handling Missing
```cpp
// lua_dostring returns non-zero on error, error message on stack
// Current code ignores return value entirely
g_lua_dostring(L, script);  // No error check!
```

### 8. Camera Scanning Safety Issue
```cpp
// Scans 0x0100-0x1000 pages with ReadProcessMemory on CURRENT process
// Can trigger access violations on guard pages / non-committed memory
for (uint32_t page = 0x0100; page < 0x1000; page++) {
    uintptr_t addr = (page << 16) | 0x9C6C;
    ReadProcessMemory(hp, (LPCVOID)(addr - 0x20), ...)
}
```

### 9. UDP Broadcast Design Flaw
```cpp
// Sends to BOTH 127.0.0.1 AND INADDR_BROADCAST
// Broadcast (255.255.255.255) only works on local subnet
// No support for cross-internet co-op (needs STUN/relay or manual IP config)
```

### 10. Race Condition in Lua Callback Registration
```cpp
// Both CoopThread (via HookedPresent) and ExecuteSpawnBuddy() register callbacks
if (!g_luaCallbacksRegistered) {  // Not atomic!
    g_RegisterLuaGlobal(...);
    g_luaCallbacksRegistered = true;
}
```

### 11. Entity Pointer Validation Insufficient
```cpp
// Only checks 0x100 bytes readable
if (!pEntity || IsBadReadPtr(pEntity, 0x100)) return;
// But entity structure may be larger, or pointer stale after level transition
```

### 12. No Level Transition Handling
- Camera address, entity pointers, Lua state all become invalid on map change
- No detection of level load/unload events

### 13. Magic Numbers Should Be Constants
```cpp
// 0xFFFFFFFFFFFFFFFFULL used directly instead of named constant
// 15.0f, 3.5f, 1.8f distances hardcoded
```

### 14. Receiver Thread Never Cleaned Up
- `CreateThread` for `ReceiverThread` in `CoopThread`
- No `WaitForSingleObject` or termination signal on detach

### 15. Potential Buffer Overflow in Candidate Array
```cpp
const char* candidates[16];  // Fixed size
int candCount = 0;
// ... adds up to 10 candidates currently, but no bounds check in loop
```

## Recommended Fixes Priority

### HIGH (Stability/Crashes)
1. Add missing headers
2. Replace `IsBadReadPtr` with SEH
3. Add thread synchronization (critical sections)
4. Fix D3D hook to target game's actual device
5. Add D3D hook cleanup on detach
6. Add Lua error checking
7. Fix camera scanning with SEH

### MEDIUM (Correctness/Robustness)
8. Add level transition detection
9. Improve entity pointer validation
10. Fix UDP for cross-network play (configurable target IP)
11. Make magic numbers into constants
12. Add bounds checking to candidate array
13. Proper receiver thread cleanup

### LOW (Quality of Life)
14. Precise timing for 20Hz send
15. Dynamic resolution detection (already partially done)
16. Configuration file for offsets/keys/IPs
17. Better logging with levels (debug/info/warn/error)

## Architecture Notes for Milestone 5 (Animation & Action Replication)
- Need to hook animation state replication
- Weapon fire sync requires networked event packets (not just position)
- Stance (crouch/prone/stand) sync
- Health/damage sync for revive mechanics
- Vehicle synchronization
- Mission/objective state synchronization

## Testing Checklist
- [ ] Clean compile with `-Wall -Wextra -Werror`
- [ ] Load in Lutris/Wine without crashes
- [ ] Verify log output matches expected initialization sequence
- [ ] Test F3/F4/F7/F8/F6 keybindings in-game
- [ ] Verify UDP packets received by `udp_receiver.py`
- [ ] Test entity spawning with all buddy archetypes
- [ ] Test teleport to marker
- [ ] Test level transition (map change)
- [ ] Test Alt-Tab (cursor suppression)
- [ ] Test clean exit (no crashes on close)