# Far Cry 2 Co-op Mod 🎮

[![GitHub Remote](https://img.shields.io/badge/Remote-GitHub-green)](https://github.com/precomp666/fc2-coop-mod)  
[![Status](https://img.shields.io/badge/Status-Active-brightgreen)]()  

## 🚀 Quick Start - První spuštění na NOVÉM PC:
```bash
# 1. Klonování z GitHubu
git clone https://github.com/precomp666/fc2-coop-mod.git FC2-Coop-Project
cd FC2-Coop-Project

# 2. Nastavení Git identity (POVINNÉ!)
git config --global user.name "VÁŠ_NÁZEV"
git config --global user.email "váš@email.com"

# 3. Aktualizace z hlavního repozitáře
git pull origin master

# 4. Build a kompilace
bash coop_dev/build.sh

# 5. Spuštění hry
./bin/FC2ServerLauncher.exe

## 📁 Project Structure
```
├── coop_dev/           # Vývojový zdrojový kód
│   ├── main.cpp        # Hlavní injection DLL entry point
│   ├── offsets.md      # Memory offsets & Lua API documentation
│   ├── build.sh        # Build script (Linux/Wine)
│   └── *.def           # Proxy definition files
├── bin/                # Game executables (ignored in Git)
├── Data_Win32/         # Game data (ignored in Git)
├── fc2_coop.log        # Runtime log file
├── .gitignore          # Git ignore patterns
└── README.md           # This file
```

---

## 🎯 Progress Milestones - Current State

- ✅ **Milestone 1:** DLL Proxying & Process Attachment  
  → `binkw32.dll` → `binkw32_orig.dll`

- ✅ **Milestone 2:** Coordinate Tracking & UDP Transmission (20 Hz loop)  

---

## 🔧 Technical Architecture

### Engine Overview & Base Addresses
- **Game Executable**: `bin/FarCry2.exe` → Base: `0x00400000` (Fixed, no ASLR)
- **Engine Core DLL**: `bin/Dunia.dll` → Base: `0x10000000` (Fixed, no ASLR!)
- **Injection Method**: Proxy `bin/binkw32.dll` → Forwards 71 exports to `bin/binkw32_orig.dll`
- **Log File**: `fc2_coop.log`

### Toolchain & Compilation
- **Platform**: Linux (Lutris / Wine) running 32-bit Windows binaries (`i686-w64-mingw32`)
- **Compiler**: Portable `llvm-mingw` in `~/tools/llvm-mingw*`
- **Build Script**: `bash coop_dev/build.sh`

### Critical Keybindings ⚠️

> [!IMPORTANT]  
> **DO NOT USE `F5` OR `F9` FOR MOD HOTKEYS!**

| Key | Default Game Action | Mod Hotkey Action |
|-----|---------------------|-------------------|
| **`F5`** | Quick Save | ❌ **UNAVAILABLE** - Conflict! |
| **`F9`** | Quick Load | ❌ **UNAVAILABLE** - Conflict! |
| **`[F3]`** | — | Toggle UDP TX (broadcast position) |
| **`[F4]`** | — | Anchor 3D Marker waypoint |
| **`[F7]/[Num7]`** | — | Spawn Buddy (Player 2) |
| **`[F8]/[Num8]/[F6]`** | — | Teleport Buddy to position |

- ✅ **Milestone 3:** Direct3D 9 Waypoint Overlay & 3D Projection  
  → `[F4]` anchor marker, 360° edge pointer

- ✅ **Milestone 4:** Entity Spawning & Representation  
  → Rebound keys to conflict-free `[F7]` (Spawn) and `[F8]` (Teleport)  
  → Lua 4.0 runtime compatibility fixes (removed `_G`)  
  → Added query for local player archetype

- ⏳ **Milestone 5 (Option C):** Animation & Action Replication (TO DO)  
  → Weapon fire sync, stance sync, death/revive state
```
```ost@ondrej-pc:~/Downloads/Far Cry 2

---

## 💡 Lua 4.0 Scripting API (Dunia Engine)

> [!WARNING]  
> **Dunia Engine embeds a modified Lua 4.0 runtime (NOT Lua 5.x)!**
> 1. No `_G` table: Globals accessed directly (`myVar = 1`, NOT `_G.myVar = 1`)
> 2. Types: `LUA_TUSERDATA = 0`, `LUA_TNIL = 1`, `LUA_TNUMBER = 2`, `LUA_TSTRING = 3`, `LUA_TTABLE = 4`, `LUA_TFUNCTION = 5`

### Verified Dunia Engine API Offsets

| Function | Address | Description |
|----------|---------|-------------|
| **Script System Singleton** | `0x11606728` | Points to `CLuaState*` |
| **`lua_dostring`** | `0x102ab630` | Execute string (`int __cdecl (void* L, const char* str)`) |
| **`lua_tostring`** | `0x102aad40` | Get string from stack (`const char* __cdecl (void* L, int index)`) |
| **`RegisterLuaGlobal`** | `0x102aa850` | Register Lua function globally |
| **`CEntityManager`** | `0x11644E80` | Entity manager singleton |
| **`GetEntityFromId`** | `0x10055820` | Get entity by ID |

#### Entity Transform Functions
- `g_InvalidateCache`: `0x104dd4f0` (`void __thiscall (void* pEntity)`)
- `g_SetPosition`: `0x104df700` (`void __thiscall (void* pEntity, const Vec3* pos, uint32_t flags)`)
- `g_SetRotation`: `0x104df7b0` (`void __thiscall (void* pEntity, const Vec3* rot, uint32_t flags)`)

### Registered Lua Functions for Entities

```lua
-- Get local player ID (returns 64-bit string like "9223372036854775807")
local playerId = GetLocalPlayerId()

-- Get entity name by ID
local name = GetEntityName(entityIdStr)

-- Spawn entity from archetype (7 args: name + 6 numbers)
local newId = SpawnEntityFromArchetype("Paul_Ferenc", x, y, z, rx, ry, rz)

-- Remove entity from world
RemoveEntity(entityIdStr)

-- Teleport entity
TeleportEntity(entityIdStr, anchorOrTargetStr, flags)
```

### Direct3D 9 Camera Projection System

**Hook Method**: Virtual table hooking on `IDirect3DDevice9`:
- `Present` (VTable index 17)
- `EndScene` (VTable index 42)
- Window subclass (`WM_SETCURSOR`) to eliminate double-cursor artifacts

**Dunia Live Camera Context** (scanned pages `0x0100` to `0x1000` matching `addr & 0xFFFF == 0x9C6C`):
- `Base - 0x20`: Viewport Width & Height (`float[2]`)
- `Base + 0x00`: Camera Position (`Vec3` in meters)
- `Base + 0x0C`: Projection Scale `cot(fov_y / 2)`
- `Base + 0x10`: Forward Vector $\vec{F} = (f_x, f_y, f_z)$
- `Base + 0x80`: Right Vector $\vec{R} = (r_x, r_y, r_z)$
- `Base + 0x90`: Up Vector $\vec{U} = (u_x, u_y, u_z)$

**Overlay Features**:
- 3D pulsing diamond waypoint
- Dynamic ground anchor pin line
- On-screen distance meter digits & proximity bar
- 360-degree off-screen perimeter arrow pointing to target position
*Last updated: 2026-10-08*