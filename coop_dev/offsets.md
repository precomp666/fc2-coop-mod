# Far Cry 2 Co-op Mod - Architecture & Memory Documentation

## Engine Overview & Verified Base Addresses
- **Game Executable**: `bin/FarCry2.exe` -> Base: `0x00400000` (Fixed, no ASLR)
- **Engine Core DLL**: `bin/Dunia.dll` -> Base: `0x10000000` (Fixed, no ASLR!)
- **Dedicated Server**: `bin/FC2ServerLauncher.exe`
- **Injection Method**: Proxy `bin/binkw32.dll` -> Forwards 71 exports to `bin/binkw32_orig.dll`.
- **Log File**: `fc2_coop.log` (generated in process root / working directory).

---

## Toolchain & Compilation
- **Platform**: Linux (Lutris / Wine) running 32-bit Windows binaries (`i686-w64-mingw32`).
- **Compiler**: Portable `llvm-mingw` in `~/tools/llvm-mingw*` (auto-detected by `build.sh`).
- **Build Script**: `bash build.sh` inside `coop_dev/`:
  - Generates `.def` via `generate_proxy.py`.
  - Compiles `main.cpp` + `binkw32.def` into `binkw32.dll` with `-O2 -lws2_32 -ld3d9`.
  - Automatically copies `binkw32.dll` to `../bin/binkw32.dll`.

---

## Critical Keybindings & Engine Conflicts
> [!IMPORTANT]
> **DO NOT USE `F5` OR `F9` FOR MOD HOTKEYS!**
> - **`F5`**: Far Cry 2 default hardcoded **Quick Save**.
> - **`F9`**: Far Cry 2 default hardcoded **Quick Load**.

### Active Mod Hotkeys
| Key | Action | Notes |
|---|---|---|
| **`[F3]`** | Toggle UDP TX | Pauses / resumes broadcasting own player coordinates |
| **`[F4]`** | Anchor 3D Marker | Anchors 3D diamond waypoint 15m ahead in camera direction |
| **`[F7]`** / **`[Num7]`** | Spawn Buddy (Player 2) | Discovers player model & spawns Entity via Lua 4.0 bridge |
| **`[F8]`** / **`[Num8]`** / **`[F6]`** | Teleport Buddy | Moves spawned Player 2 entity to current 3D Marker position |

---

## Direct3D 9 & Camera Projection System
- **Hook Method**: Virtual table hooking on `IDirect3DDevice9`:
  - `Present` (VTable index 17)
  - `EndScene` (VTable index 42)
  - Window subclass (`WM_SETCURSOR`) to eliminate double-cursor artifacts when Alt-Tabbing.
- **Dunia Live Camera Context**:
  - Found dynamically by scanning pages `0x0100` to `0x1000` matching `addr & 0xFFFF == 0x9C6C`:
    - `Base - 0x20`: Viewport Width & Height (`float[2]`, e.g. 1920.0, 1080.0)
    - `Base + 0x00`: Camera Position (`Vec3` in meters)
    - `Base + 0x0C`: Projection Scale `cot(fov_y / 2)` (`float`, e.g. 1.4192)
    - `Base + 0x10`: Forward Vector $\vec{F} = (f_x, f_y, f_z)$ (normalized unit vector)
    - `Base + 0x80`: Right Vector $\vec{R} = (r_x, r_y, r_z)$ (normalized unit vector)
    - `Base + 0x90`: Up Vector $\vec{U} = (u_x, u_y, u_z)$ (normalized unit vector)
- **Overlay Features**:
  - 3D pulsing diamond waypoint.
  - Dynamic ground anchor pin line.
  - On-screen distance meter digits & proximity bar.
  - 360-degree off-screen perimeter arrow pointing to target position.

---

## Dunia Script & Lua 4.0 Architecture
> [!WARNING]
> Dunia Engine embeds a modified **Lua 4.0** runtime (NOT Lua 5.x!):
> 1. **No `_G` table**: Globals are accessed directly (`myVar = 1`, NOT `_G.myVar = 1`).
> 2. Types: `LUA_TUSERDATA = 0`, `LUA_TNIL = 1`, `LUA_TNUMBER = 2`, `LUA_TSTRING = 3`, `LUA_TTABLE = 4`, `LUA_TFUNCTION = 5`.
> 3. Errors: When `lua_dostring` returns non-zero (`code 1`), the error message is on top of the stack and can be read with `lua_tostring(L, -1)`.

### Verified Dunia Engine Export / Memory Offsets
- **Script System Singleton**: `0x11606728` -> Points to `CLuaState*`.
- **`lua_dostring`**: `0x102ab630` (`int __cdecl (void* L, const char* str)`)
- **`lua_tostring`**: `0x102aad40` (`const char* __cdecl (void* L, int index)`)
- **`RegisterLuaGlobal`**: `0x102aa850` (`void __cdecl (const char* className, const char* funcName, void* fnHandler)`)
- **`CEntityManager` Singleton**: `0x11644E80`
- **`CEntityManager::GetEntityFromId`**: `0x10055820` (`void* __thiscall (void* thisMgr, void* outSmartPtr, uint32_t idLow, uint32_t idHigh)`)
- **Entity Transform Functions**:
  - `g_InvalidateCache`: `0x104dd4f0` (`void __thiscall (void* pEntity)`)
  - `g_SetPosition`: `0x104df700` (`void __thiscall (void* pEntity, const Vec3* pos, uint32_t flags)`)
  - `g_SetRotation`: `0x104df7b0` (`void __thiscall (void* pEntity, const Vec3* rot, uint32_t flags)`)

### Registered Dunia Lua Functions for Entities
- **`GetLocalPlayerId()`** (`0x1058fa00`):
  - Returns 64-bit local player ID as formatted string (`"%llu"`).
- **`GetEntityName(entityIdStr)`** (`0x10590c20`):
  - Returns the entity's archetype name string (e.g. `"Paul_Ferenc"`, `"Marty_Alencar"`).
- **`SpawnEntityFromArchetype(name, x, y, z, rx, ry, rz)`** (`0x10591100`):
  - Requires 7 arguments: 1 string + 6 numbers.
  - Returns newly created 64-bit Entity ID as string, or `"18446744073709551615"` (`0xFFFFFFFFFFFFFFFF` = `INVALID_ENTITY_ID`) if archetype is not found.
- **`RemoveEntity(entityIdStr)`** (`0x105911b7`):
  - Despawns entity from the world.
- **`TeleportEntity(entityIdStr, anchorOrTargetStr, flags)`** (`0x109fa080`).

---

## UDP Network Protocol
- **TX Port**: `42069` (Broadcast own position at 20 Hz)
- **RX Port**: `42070` (Listen for remote co-op partner)
- **Packet Structure** (`#pragma pack(push, 1)`):
  ```cpp
  struct CoopPacket {
      uint32_t magic;       // 0x46433243 ('FC2C')
      uint32_t packetType;  // 1 = Position update
      uint32_t sequence;
      uint32_t timestamp;
      float x, y, z;
  };
  ```

---

## Progress Milestones & Current State
- [x] **Milestone 1:** DLL Proxying & Process Attachment (`binkw32.dll` -> `binkw32_orig.dll`).
- [x] **Milestone 2:** Coordinate Tracking & UDP Transmission (20 Hz loop).
- [x] **Milestone 3:** Direct3D 9 Waypoint Overlay & 3D Projection (`[F4]` anchor marker, 360° edge pointer).
- [x] **Milestone 4:** Entity Spawning & Representation:
  - Rebound keys to conflict-free `[F7]` (Spawn) and `[F8]` (Teleport).
  - Lua 4.0 runtime compatibility fixes (removed `_G`).
  - Added query for local player archetype (`GetLocalPlayerId()` -> `GetEntityName(...)`).
  - Implemented multi-archetype probe fallback loop in `ExecuteSpawnBuddy`.
  - Added `INVALID_ENTITY_ID` (`0xFFFFFFFFFFFFFFFF`) rejection check.
- [ ] **Milestone 5 (Option C):** Animation & Action Replication (Weapon fire sync, stance sync, death/revive state).
