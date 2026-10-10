#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <time.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <stdarg.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>

// =============================================================
//  Thread Safety & Synchronization Primitives
// =============================================================
static CRITICAL_SECTION g_cs;
static bool g_csInitialized = false;
static FILE* g_logFile = NULL;

// Thread-safe logging
void Log(const char* format, ...) {
    if (!g_csInitialized) {
        InitializeCriticalSection(&g_cs);
        g_csInitialized = true;
    }

    EnterCriticalSection(&g_cs);

    if (!g_logFile) {
        g_logFile = fopen("fc2_coop.log", "a");
    }
    if (g_logFile) {
        time_t now = time(NULL);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", localtime(&now));
        fprintf(g_logFile, "[%s] ", timeBuf);
        va_list args;
        va_start(args, format);
        vfprintf(g_logFile, format, args);
        va_end(args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
    }

    LeaveCriticalSection(&g_cs);
}

// Safe pointer validation using VirtualQuery (replaces IsBadReadPtr)
inline bool SafeReadPtr(const void* ptr, size_t size) {
    if (!ptr || size == 0) return false;
    
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size - 1;
    if (end < start) return false;
    
    if (!VirtualQuery((void*)end, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    
    return true;
}

// Safe pointer validation for single value reads
template<typename T>
inline bool SafeReadValue(const T* ptr, T& out) {
    if (!ptr) return false;
    if (!SafeReadPtr(ptr, sizeof(T))) return false;
    out = *ptr;
    return true;
}

// Overload for void** (reading a pointer to pointer)
inline bool SafeReadValue(const void* const* ptr, void*& out) {
    if (!ptr) return false;
    if (!SafeReadPtr(ptr, sizeof(void*))) return false;
    out = const_cast<void*>(*ptr);
    return true;
}

// =============================================================
//  Shared Math Types
// =============================================================
struct Vec3 { float x, y, z; };

// =============================================================
//  Network Protocol
// =============================================================
#define COOP_MAGIC     0x46433243  // 'FC2C'
#define COOP_PORT_TX   42069       // We SEND own position here
#define COOP_PORT_RX   42070       // We RECEIVE remote player here

#pragma pack(push, 1)
struct CoopPacket {
    uint32_t magic;
    uint32_t packetType;   // 1 = position
    uint32_t sequence;
    uint32_t timestamp;
    float x, y, z;
};
#pragma pack(pop)

static SOCKET g_txSocket = INVALID_SOCKET;
static sockaddr_in g_txBroadcast, g_txLocal;
static std::atomic<uint32_t> g_txSeq(0);

// Remote player state - atomic for thread safety
static std::atomic<float> g_remoteX(0), g_remoteY(0), g_remoteZ(0);
static std::atomic<DWORD> g_remoteLastTick(0);
static std::atomic<bool> g_hasRemotePlayer(false);

// Test marker state
static std::atomic<bool> g_testMarkerActive(true);
static Vec3 g_testMarkerPos = { 0, 0, 0 };
static std::atomic<bool> g_testMarkerInitialized(false);

// Tracked Player & Camera context
static std::atomic<uintptr_t> g_trackedPlayerAddr(0);
static std::atomic<bool> g_trackingEnabled(true);

// Lua callback registration - atomic to prevent race conditions
static std::atomic<bool> g_luaCallbacksRegistered(false);

// Buddy entity state
static std::atomic<uint64_t> g_p2EntityId(0);
static std::atomic<void*> g_p2EntityPtr(nullptr);
static std::atomic<bool> g_p2BuddySpawned(false);
static std::atomic<int> g_spawnGeneration(0);  // Increment on each spawn attempt

// Spawn/Teleport request flags (set from CoopThread, processed in HookedPresent)
static std::atomic<bool> g_reqSpawnBuddy(false);
static std::atomic<bool> g_reqTeleportBuddy(false);

// Selected archetype index
static std::atomic<int> g_selectedArchetypeIdx(0);

// Thread handles for cleanup
static HANDLE g_receiverThreadHandle = NULL;
static std::atomic<bool> g_shutdownRequested(false);

// =============================================================
//  Dunia Engine Script & Entity Interfaces (Option A)
// =============================================================
typedef int   (__cdecl    *fn_lua_dostring)(void* L, const char* str);
typedef void  (__cdecl    *fn_RegisterLuaGlobal)(const char* className, const char* funcName, void* fnHandler);
typedef const char* (__cdecl *fn_lua_tostring)(void* L, int index);
typedef void  (__thiscall *fn_InvalidateCache)(void* pEntity);
typedef void  (__thiscall *fn_SetPosition)(void* pEntity, const Vec3* pos, uint32_t flags);
typedef void  (__thiscall *fn_SetRotation)(void* pEntity, const Vec3* rot, uint32_t flags);
typedef void* (__thiscall *fn_GetEntityFromId)(void* thisMgr, void* outSmartPtr, uint32_t idLow, uint32_t idHigh);

static fn_lua_dostring      g_lua_dostring      = (fn_lua_dostring)0x102ab630;
static fn_RegisterLuaGlobal g_RegisterLuaGlobal  = (fn_RegisterLuaGlobal)0x102aa850;
static fn_lua_tostring      g_lua_tostring      = (fn_lua_tostring)0x102aad40;
static fn_InvalidateCache   g_InvalidateCache   = (fn_InvalidateCache)0x104dd4f0;
static fn_SetPosition       g_SetPosition       = (fn_SetPosition)0x104df700;
static fn_SetRotation       g_SetRotation       = (fn_SetRotation)0x104df7b0;
static fn_GetEntityFromId   g_GetEntityFromId   = (fn_GetEntityFromId)0x10055820;

// Buddy archetypes list
static const char* g_buddyArchetypes[] = {
    "BUDDY_Hakim_Echebbi",
    "BUDDY_Warren_Clyde",
    "BUDDY_Paul_Ferenc",
    "BUDDY_Marty_Alencar",
    "BUDDY_Josip_Idromeno",
    "BUDDY_Frank_Bilders",
    "BUDDY_Flora_Guillen",
    "BUDDY_Michele_Dachss",
    "BUDDY_Andre_Hyppolite",
    "BUDDY_Quarbani_Singh"
};
static const int g_numArchetypes = sizeof(g_buddyArchetypes) / sizeof(g_buddyArchetypes[0]);

void* GetLuaState() {
    uintptr_t scriptSysAddr = 0x11606728;
    uintptr_t pScriptSys = 0;
    if (!SafeReadValue((const uintptr_t*)scriptSysAddr, pScriptSys)) return nullptr;
    if (!pScriptSys) return nullptr;
    void* L = nullptr;
    if (!SafeReadValue((const void**)pScriptSys, L)) return nullptr;
    if (!L) return nullptr;
    return L;
}

static char g_localPlayerEntityName[128] = { 0 };

int __cdecl ModLogHandler(void* L) {
    if (!L) return 0;
    const char* msg = g_lua_tostring(L, 1);
    if (msg) Log("[LUA-LOG] %s", msg);
    return 0;
}

int __cdecl OnPlayerReportHandler(void* L) {
    if (!L) return 0;
    const char* strName = g_lua_tostring(L, 1);
    if (strName && strlen(strName) > 0 && strcmp(strName, "invalid id") != 0) {
        strncpy(g_localPlayerEntityName, strName, sizeof(g_localPlayerEntityName) - 1);
        Log("[PLAYER-INFO] Local Player Archetype/Name is: '%s'", strName);
    }
    return 0;
}

int __cdecl MyBuddySpawnedHandler(void* L) {
    if (!L) return 0;
    const char* strId = g_lua_tostring(L, 1);
    if (strId) {
        uint64_t id = 0;
        if (sscanf(strId, "%llu", &id) == 1 && id != 0 && id != 0xFFFFFFFFFFFFFFFFULL) {
            g_p2EntityId.store(id);
            Log("[BUDDY-SPAWN] Received Valid Spawn ID: %s (0x%016llX)", strId, id);

            void* pMgr = *(void**)0x11644E80;
            if (pMgr && SafeReadPtr(pMgr, sizeof(void*))) {
                uintptr_t smartPtr = 0;
                g_GetEntityFromId(pMgr, &smartPtr, (uint32_t)(id & 0xFFFFFFFF), (uint32_t)(id >> 32));
                if (smartPtr && SafeReadPtr((const void*)smartPtr, 0x10)) {
                    void* pEntity = nullptr;
                    if (SafeReadValue((const void**)(smartPtr + 0x0C), pEntity) && pEntity) {
                        g_p2EntityPtr.store(pEntity);
                        g_p2BuddySpawned.store(true);
                        Log("[BUDDY-SPAWN] SUCCESS! CEntity pointer resolved: 0x%08X", (uintptr_t)pEntity);
                    } else {
                        Log("[BUDDY-SPAWN] Warning: CEntity pointer in smartPtr was NULL");
                    }
                } else {
                    Log("[BUDDY-SPAWN] Warning: smartPtr NULL or invalid for id 0x%016llX", id);
                }
            } else {
                Log("[BUDDY-SPAWN] Warning: CEntityManager pointer was NULL at 0x11644E80");
            }
        } else {
            Log("[BUDDY-SPAWN] Rejected invalid ID: %s", strId ? strId : "null");
        }
    }
    return 0;
}

void ExecuteSpawnBuddy(float x, float y, float z) {
    void* L = GetLuaState();
    if (!L) {
        Log("[SPAWN] Lua state not available yet (are you in-game?)");
        return;
    }

    // Atomic callback registration to prevent race condition
    bool expected = false;
    if (g_luaCallbacksRegistered.compare_exchange_strong(expected, true)) {
        g_RegisterLuaGlobal(nullptr, "OnBuddySpawned", (void*)&MyBuddySpawnedHandler);
        g_RegisterLuaGlobal(nullptr, "OnPlayerReport", (void*)&OnPlayerReportHandler);
        g_RegisterLuaGlobal(nullptr, "ModLog", (void*)&ModLogHandler);
        Log("[LUA] Registered callbacks (OnBuddySpawned, OnPlayerReport, ModLog)");
    }

    // Increment spawn generation to invalidate any pending callbacks from previous spawns
    int currentGen = g_spawnGeneration.fetch_add(1) + 1;

    // 1. Query Local Player name if not yet discovered
    if (g_localPlayerEntityName[0] == '\0') {
        const char* qScript =
            "local myId = GetLocalPlayerId(); "
            "if myId and myId ~= '18446744073709551615' then "
            "  local myName = GetEntityName(myId); "
            "  OnPlayerReport(myName); "
            "end";
        int result = g_lua_dostring(L, qScript);
        if (result != 0) {
            const char* err = g_lua_tostring(L, -1);
            Log("[SPAWN] Lua error querying player name: %s", err ? err : "unknown");
        }
    }

    // 2. Remove previously spawned buddy entity if any
    if (g_p2BuddySpawned.load() && g_p2EntityId.load() != 0 && g_p2EntityId.load() != 0xFFFFFFFFFFFFFFFFULL) {
        char rmScript[128];
        snprintf(rmScript, sizeof(rmScript), "RemoveEntity('%llu');", g_p2EntityId.load());
        int result = g_lua_dostring(L, rmScript);
        if (result != 0) {
            const char* err = g_lua_tostring(L, -1);
            Log("[SPAWN] Lua error removing old entity: %s", err ? err : "unknown");
        }
        g_p2BuddySpawned.store(false);
        g_p2EntityPtr.store(nullptr);
        g_p2EntityId.store(0);
    }

    // 3. Try candidates until one spawns successfully
    const char* candidates[16];
    int candCount = 0;
    const int MAX_CANDIDATES = 16;

    if (g_localPlayerEntityName[0] != '\0' && candCount < MAX_CANDIDATES) {
        candidates[candCount++] = g_localPlayerEntityName;
    }
    // Only buddy archetypes - NO enemy archetypes!
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "Hakim_Echebbi";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "Marty_Alencar";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "Paul_Ferenc";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "Warren_Clyde";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "Josip_Idromeno";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "BUDDY_Hakim_Echebbi";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "BUDDY_Marty_Alencar";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "BUDDY_Paul_Ferenc";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "BUDDY_Warren_Clyde";
    if (candCount < MAX_CANDIDATES) candidates[candCount++] = "BUDDY_Josip_Idromeno";

    for (int i = 0; i < candCount && !g_p2BuddySpawned.load(); i++) {
        const char* arch = candidates[i];
        char script[256];
        snprintf(script, sizeof(script),
            "local eid = SpawnEntityFromArchetype('%s', %.2f, %.2f, %.2f, 0, 0, 0); "
            "if eid and eid ~= '18446744073709551615' then "
            "  OnBuddySpawned(eid); "
            "end",
            arch, x, y, z);

        Log("[SPAWN-TRY] Trying archetype '%s' at (%.1f, %.1f, %.1f)...", arch, x, y, z);
        int result = g_lua_dostring(L, script);
        if (result != 0) {
            const char* err = g_lua_tostring(L, -1);
            Log("[SPAWN] Lua error for archetype '%s': %s", arch, err ? err : "unknown");
        }
        if (g_p2BuddySpawned.load()) {
            Log("[SPAWN-SUCCESS] Archetype '%s' spawned successfully! EntityPtr=0x%08X", arch, (uintptr_t)g_p2EntityPtr.load());
            break;
        }
    }

    if (!g_p2BuddySpawned.load()) {
        Log("[SPAWN-NOTICE] Archetypes did not spawn immediately, trying CBuddiesManager...");
        const char* bmScript =
            "if CBuddiesManager and CBuddiesManager.SpawnPrimaryBuddy then "
            "  local bid = CBuddiesManager:SpawnPrimaryBuddy('default', 'none', %.2f, %.2f, %.2f); "
            "  if bid and bid ~= '18446744073709551615' then OnBuddySpawned(bid); end "
            "end";
        char bmBuf[256];
        snprintf(bmBuf, sizeof(bmBuf), bmScript, x, y, z);
        int result = g_lua_dostring(L, bmBuf);
        if (result != 0) {
            const char* err = g_lua_tostring(L, -1);
            Log("[SPAWN] Lua error in CBuddiesManager fallback: %s", err ? err : "unknown");
        }
    }
}
void SetEntityPositionAndRotation(void* pEntity, float x, float y, float z, float pitchRad = 0, float rollRad = 0, float yawRad = 0) {
    if (!pEntity || !SafeReadPtr(pEntity, 0x100)) {
        Log("[SETPOS] Invalid entity pointer: %p", pEntity);
        return;
    }

    // Additional validation: check if entity vtable is valid
    void** vtable = *(void***)pEntity;
    if (!vtable || !SafeReadPtr(vtable, sizeof(void*) * 20)) {
        Log("[SETPOS] Entity vtable invalid: %p", pEntity);
        if (g_p2EntityPtr.load() == pEntity) {
            g_p2BuddySpawned.store(false);
            g_p2EntityPtr.store(nullptr);
            g_p2EntityId.store(0);
        }
        return;
    }

    // Check entity state - look for "deleted" or "dead" flags
    // Common Dunia entity offsets: 0x8=flags, 0xC=state, 0x10=type
    uint8_t* entityFlags = (uint8_t*)((uintptr_t)pEntity + 0x8);
    if (SafeReadPtr(entityFlags, 1)) {
        uint8_t flags = *entityFlags;
        if (flags & 0x80) { // Common "deleted" flag
            Log("[SETPOS] Entity marked as deleted (flags=0x%02X), cleaning up", flags);
            if (g_p2EntityPtr.load() == pEntity) {
                g_p2BuddySpawned.store(false);
                g_p2EntityPtr.store(nullptr);
                g_p2EntityId.store(0);
            }
            return;
        }
    }

    // Call via entity's vtable directly - proper thiscall convention
    // Dunia entity vtable: InvalidateCache=8, SetPosition=12, SetRotation=13
    if (!SafeReadPtr(vtable, sizeof(void*) * 14)) {
        Log("[SETPOS] Vtable too small for SetPosition/SetRotation");
        if (g_p2EntityPtr.load() == pEntity) {
            g_p2BuddySpawned.store(false);
            g_p2EntityPtr.store(nullptr);
            g_p2EntityId.store(0);
        }
        return;
    }

    // Try multiple vtable index combinations (different entity types may have different layouts)
    struct VTableLayout {
        int invalidateCacheIdx;
        int setPositionIdx;
        int setRotationIdx;
        const char* name;
    };
    
    static const VTableLayout layouts[] = {
        {8, 12, 13, "standard"},
        {7, 11, 12, "alt1"},
        {9, 13, 14, "alt2"},
        {6, 10, 11, "alt3"},
        {10, 14, 15, "alt4"},
    };
    
    typedef void (__thiscall *fn_InvalidateCache_vt)(void*);
    typedef void (__thiscall *fn_SetPosition_vt)(void*, const Vec3*, uint32_t);
    typedef void (__thiscall *fn_SetRotation_vt)(void*, const Vec3*, uint32_t);
    
    fn_InvalidateCache_vt pInvalidateCache = nullptr;
    fn_SetPosition_vt pSetPosition = nullptr;
    fn_SetRotation_vt pSetRotation = nullptr;
    bool foundValidLayout = false;
    
    for (const auto& layout : layouts) {
        if (!SafeReadPtr(vtable, sizeof(void*) * (layout.setRotationIdx + 1))) continue;
        
        fn_InvalidateCache_vt ic = (fn_InvalidateCache_vt)vtable[layout.invalidateCacheIdx];
        fn_SetPosition_vt sp = (fn_SetPosition_vt)vtable[layout.setPositionIdx];
        fn_SetRotation_vt sr = (fn_SetRotation_vt)vtable[layout.setRotationIdx];
        
        if (ic && sp && sr &&
            SafeReadPtr((void*)ic, 1) &&
            SafeReadPtr((void*)sp, 1) &&
            SafeReadPtr((void*)sr, 1)) {
            pInvalidateCache = ic;
            pSetPosition = sp;
            pSetRotation = sr;
            Log("[SETPOS] Found valid vtable layout '%s': IC=%p SP=%p SR=%p", 
                layout.name, ic, sp, sr);
            foundValidLayout = true;
            break;
        }
    }
    
    if (!foundValidLayout) {
        Log("[SETPOS] No valid vtable layout found. Tried all known layouts.");
        Log("[SETPOS] Raw vtable[6-15]: %p %p %p %p %p %p %p %p %p %p",
            vtable[6], vtable[7], vtable[8], vtable[9], vtable[10], 
            vtable[11], vtable[12], vtable[13], vtable[14], vtable[15]);
        if (g_p2EntityPtr.load() == pEntity) {
            g_p2BuddySpawned.store(false);
            g_p2EntityPtr.store(nullptr);
            g_p2EntityId.store(0);
        }
        return;
    }

    Vec3 pos = { x, y, z };
    Vec3 rot = { pitchRad, rollRad, yawRad };

    // Check entity type - verify it's a pawn/character that supports position changes
    // Dunia pawn entities typically have type info at offset 0x10 or 0x14
    uint32_t* entityType = (uint32_t*)((uintptr_t)pEntity + 0x10);
    if (SafeReadPtr(entityType, sizeof(uint32_t))) {
        Log("[SETPOS] Entity type at +0x10: 0x%08X", *entityType);
    }
    entityType = (uint32_t*)((uintptr_t)pEntity + 0x14);
    if (SafeReadPtr(entityType, sizeof(uint32_t))) {
        Log("[SETPOS] Entity type at +0x14: 0x%08X", *entityType);
    }

    // Also try global function pointers with proper thiscall convention as fallback
    // These are the engine's global entity manipulation functions
    if (SafeReadPtr((void*)g_SetPosition, 1) && SafeReadPtr((void*)g_InvalidateCache, 1)) {
        Log("[SETPOS] Trying global function pointers as fallback...");
        Log("[SETPOS] Calling global InvalidateCache(%p)", pEntity);
        g_InvalidateCache(pEntity);
        Log("[SETPOS] Calling global SetPosition(%p) to (%.1f, %.1f, %.1f)", pEntity, x, y, z);
        g_SetPosition(pEntity, &pos, 0);
        Log("[SETPOS] Calling global SetRotation(%p)", pEntity);
        g_SetRotation(pEntity, &rot, 0);
        Log("[SETPOS] Global functions Success");
        return;
    }

    Log("[SETPOS] Calling InvalidateCache(vt[8]=%p) on %p", pInvalidateCache, pEntity);
    pInvalidateCache(pEntity);
    Log("[SETPOS] Calling SetPosition(vt[12]=%p) on %p to (%.1f, %.1f, %.1f)", pSetPosition, pEntity, x, y, z);
    pSetPosition(pEntity, &pos, 0);
    Log("[SETPOS] Calling SetRotation(vt[13]=%p) on %p", pSetRotation, pEntity);
    pSetRotation(pEntity, &rot, 0);
    Log("[SETPOS] Success");
}

bool IsValidCoord(float x, float y, float z) {
    if (std::isnan(x) || std::isnan(y) || std::isnan(z)) return false;
    if (std::isinf(x) || std::isinf(y) || std::isinf(z)) return false;
    if (x < 100.0f || x > 4000.0f || y < 100.0f || y > 4000.0f || z < -50.0f || z > 1000.0f) return false;
    return true;
}

// =============================================================
//  Auto-Locate Active Dunia Camera Context (SafeReadPtr-safe)
// =============================================================
bool AutoLocateCamera() {
    HANDLE hp = GetCurrentProcess();
    for (uint32_t page = 0x0100; page < 0x1000; page++) {
        uintptr_t addr = (page << 16) | 0x9C6C;
        float dims[2];
        SIZE_T br = 0;
        
        // Validate memory before reading
        if (!SafeReadPtr((void*)(addr - 0x20), sizeof(dims))) continue;
        if (!SafeReadPtr((void*)addr, sizeof(float) * 3)) continue;
        if (!SafeReadPtr((void*)(addr + 0x10), sizeof(float) * 3)) continue;
        
        if (ReadProcessMemory(hp, (LPCVOID)(addr - 0x20), dims, sizeof(dims), &br) && br == sizeof(dims)) {
            if (dims[0] >= 640.0f && dims[0] <= 7680.0f && dims[1] >= 480.0f && dims[1] <= 4320.0f) {
                float pos[3];
                if (ReadProcessMemory(hp, (LPCVOID)addr, pos, sizeof(pos), &br) && br == sizeof(pos)) {
                    if (IsValidCoord(pos[0], pos[1], pos[2])) {
                        float fwd[3];
                        if (ReadProcessMemory(hp, (LPCVOID)(addr + 0x10), fwd, sizeof(fwd), &br) && br == sizeof(fwd)) {
                            float flen = fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2];
                            if (flen >= 0.85f && flen <= 1.15f) {
                                g_trackedPlayerAddr.store(addr);
                                Log("[CAMERA-LOCK] Active camera found at 0x%08X: Pos (%.1f, %.1f, %.1f) Screen %.0fx%.0f",
                                    (DWORD)addr, pos[0], pos[1], pos[2], dims[0], dims[1]);
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// =============================================================
//  World-To-Screen Projection (SafeReadPtr-safe)
// =============================================================
bool WorldToScreen(float wx, float wy, float wz, float& sx, float& sy, float& dist, bool& isBehind) {
    uintptr_t addr = g_trackedPlayerAddr.load();
    if (!addr) {
        if (!AutoLocateCamera()) return false;
        addr = g_trackedPlayerAddr.load();
        if (!addr) return false;
    }

    HANDLE hp = GetCurrentProcess();
    SIZE_T br = 0;

    float camData[7]; // px, py, pz, proj_factor, fx, fy, fz
    if (!SafeReadPtr((void*)addr, sizeof(camData))) {
        g_trackedPlayerAddr.store(0);
        return false;
    }
    if (!ReadProcessMemory(hp, (LPCVOID)addr, camData, sizeof(camData), &br) || br != sizeof(camData)) {
        g_trackedPlayerAddr.store(0);
        return false;
    }

    float px = camData[0];
    float py = camData[1];
    float pz = camData[2];
    float pf = camData[3];
    Vec3 fwd = { camData[4], camData[5], camData[6] };

    if (!IsValidCoord(px, py, pz)) {
        g_trackedPlayerAddr.store(0);
        return false;
    }

    Vec3 right, up;
    if (!SafeReadPtr((void*)(addr + 0x80), sizeof(Vec3)) || !SafeReadPtr((void*)(addr + 0x90), sizeof(Vec3))) {
        return false;
    }
    if (!ReadProcessMemory(hp, (LPCVOID)(addr + 0x80), &right, sizeof(Vec3), &br) || br != sizeof(Vec3))
        return false;
    if (!ReadProcessMemory(hp, (LPCVOID)(addr + 0x90), &up, sizeof(Vec3), &br) || br != sizeof(Vec3))
        return false;

    float sw = 1920.0f, sh = 1080.0f;
    float dims[2];
    if (SafeReadPtr((void*)(addr - 0x20), sizeof(dims))) {
        if (ReadProcessMemory(hp, (LPCVOID)(addr - 0x20), dims, sizeof(dims), &br) && br == sizeof(dims)) {
            if (dims[0] > 200.0f && dims[1] > 200.0f) {
                sw = dims[0];
                sh = dims[1];
            }
        }
    }

    float dx = wx - px;
    float dy = wy - py;
    float dz = wz - pz;
    dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    float depth = dx * fwd.x + dy * fwd.y + dz * fwd.z;
    float side  = dx * right.x + dy * right.y + dz * right.z;
    float vert  = dx * up.x + dy * up.y + dz * up.z;

    isBehind = (depth <= 0.1f);

    float aspect = sw / sh;
    if (aspect <= 0.1f) aspect = 16.0f / 9.0f;

    if (isBehind) {
        float safeDepth = (depth < -0.1f) ? -depth : 0.1f;
        sx = (sw * 0.5f) * (1.0f - (side / safeDepth) * (pf / aspect));
        sy = (sh * 0.5f) * (1.0f + (vert / safeDepth) * pf);
        return false;
    }

    sx = (sw * 0.5f) * (1.0f + (side / depth) * (pf / aspect));
    sy = (sh * 0.5f) * (1.0f - (vert / depth) * pf);
    return true;
}

// =============================================================
//  D3D9 Overlay Rendering Primitives
// =============================================================
struct HudVert {
    float x, y, z, rhw;
    DWORD color;
};
#define HUD_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

void DrawTriangle(IDirect3DDevice9* dev, float x1, float y1, float x2, float y2, float x3, float y3, DWORD color) {
    HudVert v[3] = {
        { x1, y1, 0.0f, 1.0f, color },
        { x2, y2, 0.0f, 1.0f, color },
        { x3, y3, 0.0f, 1.0f, color }
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(HudVert));
}

void DrawRect(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD color) {
    HudVert v[6] = {
        { x,     y,     0.0f, 1.0f, color },
        { x + w, y,     0.0f, 1.0f, color },
        { x + w, y + h, 0.0f, 1.0f, color },
        { x,     y,     0.0f, 1.0f, color },
        { x + w, y + h, 0.0f, 1.0f, color },
        { x,     y + h, 0.0f, 1.0f, color }
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(HudVert));
}

void DrawDiamond(IDirect3DDevice9* dev, float sx, float sy, float size, DWORD fillCol, DWORD borderCol) {
    float b = size + 2.5f;
    float by = b * 1.35f;
    DrawTriangle(dev, sx, sy - by, sx + b, sy, sx - b, sy, borderCol);
    DrawTriangle(dev, sx, sy + by, sx - b, sy, sx + b, sy, borderCol);

    float s = size;
    float sy_h = s * 1.35f;
    DrawTriangle(dev, sx, sy - sy_h, sx + s, sy, sx - s, sy, fillCol);
    DrawTriangle(dev, sx, sy + sy_h, sx - s, sy, sx + s, sy, fillCol);
}

void DrawEdgePointer(IDirect3DDevice9* dev, float sx, float sy, float angle, float size, DWORD color) {
    float cosA = std::cos(angle);
    float sinA = std::sin(angle);

    float tipX = sx + cosA * size;
    float tipY = sy + sinA * size;

    float baseLen = size * 0.70f;
    float b1X = sx - cosA * (size * 0.5f) - sinA * baseLen;
    float b1Y = sy - sinA * (size * 0.5f) + cosA * baseLen;
    float b2X = sx - cosA * (size * 0.5f) + sinA * baseLen;
    float b2Y = sy - sinA * (size * 0.5f) - cosA * baseLen;

    DrawTriangle(dev, tipX, tipY, b1X, b1Y, b2X, b2Y, color);
}

void DrawHUDChar(IDirect3DDevice9* dev, float x, float y, char c, DWORD color) {
    float t = 1.6f;
    float w = 6.5f;
    float h = 10.0f;
    float h2 = 5.0f;

    auto H  = [&](float py) { DrawRect(dev, x, y + py, w, t, color); };
    auto VL = [&](float py, float len) { DrawRect(dev, x, y + py, t, len, color); };
    auto VR = [&](float py, float len) { DrawRect(dev, x + w - t, y + py, t, len, color); };

    switch (c) {
        case '0': H(0); H(h - t); VL(0, h); VR(0, h); break;
        case '1': VR(0, h); break;
        case '2': H(0); VR(0, h2); H(h2 - t*0.5f); VL(h2, h2); H(h - t); break;
        case '3': H(0); VR(0, h); H(h2 - t*0.5f); H(h - t); break;
        case '4': VL(0, h2); VR(0, h); H(h2 - t*0.5f); break;
        case '5': H(0); VL(0, h2); H(h2 - t*0.5f); VR(h2, h2); H(h - t); break;
        case '6': H(0); VL(0, h); H(h2 - t*0.5f); VR(h2, h2); H(h - t); break;
        case '7': H(0); VR(0, h); break;
        case '8': H(0); H(h2 - t*0.5f); H(h - t); VL(0, h); VR(0, h); break;
        case '9': H(0); H(h2 - t*0.5f); H(h - t); VL(0, h2); VR(0, h); break;
        case 'm': {
            float mw = 9.0f;
            float mh = 6.0f;
            float my = y + h - mh;
            DrawRect(dev, x, my, t, mh, color);
            DrawRect(dev, x + mw * 0.5f - t * 0.5f, my, t, mh, color);
            DrawRect(dev, x + mw - t, my, t, mh, color);
            DrawRect(dev, x, my, mw, t, color);
            break;
        }
        default: break;
    }
}

void DrawDistanceDigits(IDirect3DDevice9* dev, float cx, float cy, float dist, DWORD color) {
    char buf[16];
    int d = (int)std::round(dist);
    if (d < 0) d = 0;
    snprintf(buf, sizeof(buf), "%dm", d);

    int len = 0;
    while (buf[len]) len++;

    float charW = 7.5f;
    float spacing = 2.0f;
    float totalW = 0.0f;
    for (int i = 0; i < len; i++) {
        totalW += (buf[i] == 'm' ? 10.0f : charW) + spacing;
    }

    float startX = cx - totalW * 0.5f;
    float curX = startX;

    for (int i = 0; i < len; i++) {
        DrawHUDChar(dev, curX + 1.0f, cy + 1.0f, buf[i], 0xCC000000);
        curX += (buf[i] == 'm' ? 10.0f : charW) + spacing;
    }

    curX = startX;
    for (int i = 0; i < len; i++) {
        DrawHUDChar(dev, curX, cy, buf[i], color);
        curX += (buf[i] == 'm' ? 10.0f : charW) + spacing;
    }
}

void DrawDistanceBar(IDirect3DDevice9* dev, float sx, float sy, float dist, DWORD fillCol) {
    float barW = 46.0f;
    float barH = 4.0f;
    float barX = sx - barW * 0.5f;
    float barY = sy + 18.0f;

    DrawRect(dev, barX - 1.0f, barY - 1.0f, barW + 2.0f, barH + 2.0f, 0xCC111111);
    DrawRect(dev, barX, barY, barW, barH, 0x66000000);

    float ratio = dist / 100.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.05f) ratio = 0.05f;
    DrawRect(dev, barX + 1.0f, barY + 1.0f, (barW - 2.0f) * ratio, barH - 2.0f, fillCol);
}

void Render3DMarker(IDirect3DDevice9* dev, float wx, float wy, float wz, DWORD primaryColor, DWORD accentColor) {
    if (wx < 10.0f || wy < 10.0f) return;

    float sx, sy, dist = 0.0f;
    bool isBehind = false;
    bool onScreen = WorldToScreen(wx, wy, wz, sx, sy, dist, isBehind);

    if (dist < 0.5f || dist > 2500.0f) return;

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) {
        vp.Width = 1920; vp.Height = 1080; vp.X = 0; vp.Y = 0;
    }
    float sw = (float)vp.Width;
    float sh = (float)vp.Height;

    float margin = 50.0f;
    float cx = sw * 0.5f;
    float cy = sh * 0.5f;

    if (isBehind || sx < margin || sx > sw - margin || sy < margin || sy > sh - margin) {
        float dx = sx - cx;
        float dy = sy - cy;
        if (std::abs(dx) < 0.1f && std::abs(dy) < 0.1f) {
            dx = 0.0f; dy = 1.0f;
        }
        float angle = std::atan2(dy, dx);
        float scaleX = (dx != 0.0f) ? std::abs((cx - margin) / dx) : 999.0f;
        float scaleY = (dy != 0.0f) ? std::abs((cy - margin) / dy) : 999.0f;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        float edgeX = cx + dx * scale;
        float edgeY = cy + dy * scale;

        DrawEdgePointer(dev, edgeX, edgeY, angle, 16.0f, primaryColor);
        DrawDistanceDigits(dev, edgeX, edgeY + 14.0f, dist, 0xFFFFFFFF);
        return;
    }

    // On-screen: Render 3D Diamond, Numeric Meter Digits, and Proximity Bar
    float pulse = std::sin((float)GetTickCount() * 0.006f) * 1.5f;
    float baseSize = 15.0f * (80.0f / (dist + 60.0f)) + pulse;
    if (baseSize < 7.0f) baseSize = 7.0f;
    if (baseSize > 25.0f) baseSize = 25.0f;

    DrawDiamond(dev, sx, sy, baseSize, primaryColor, accentColor);
    DrawDistanceDigits(dev, sx, sy + baseSize * 1.35f + 4.0f, dist, 0xFFFFFFFF);
    DrawDistanceBar(dev, sx, sy + baseSize * 1.35f, dist, primaryColor);
}

// =============================================================
//  Master Overlay Render Routine (Called ONLY during Present)
// =============================================================
void RenderCoopOverlay(IDirect3DDevice9* dev) {
    if (!dev) return;

    uintptr_t addr = g_trackedPlayerAddr.load();
    if (!addr) {
        if (!AutoLocateCamera()) return;
        addr = g_trackedPlayerAddr.load();
        if (!addr) return;
    }

    // Auto-initialize test marker if not done yet
    if (g_testMarkerActive.load() && !g_testMarkerInitialized.load() && addr) {
        float cam[7]; SIZE_T brRead;
        if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, cam, sizeof(cam), &brRead) && brRead == sizeof(cam)) {
            if (IsValidCoord(cam[0], cam[1], cam[2])) {
                g_testMarkerPos.x = cam[0] + cam[4] * 15.0f;
                g_testMarkerPos.y = cam[1] + cam[5] * 15.0f;
                g_testMarkerPos.z = cam[2] + cam[6] * 15.0f;
                g_testMarkerInitialized.store(true);
                Log("[AUTO-INIT] Test marker placed 15m ahead at (%.1f, %.1f, %.1f)",
                    g_testMarkerPos.x, g_testMarkerPos.y, g_testMarkerPos.z);
            }
        }
    }

    if (g_hasRemotePlayer.load() && (GetTickCount() - g_remoteLastTick.load() > 5000)) {
        Log("[NET-RX] Remote player timed out.");
        g_hasRemotePlayer.store(false);
    }

    IDirect3DVertexShader9* oldVS = nullptr;
    IDirect3DPixelShader9* oldPS = nullptr;
    IDirect3DVertexDeclaration9* oldVDecl = nullptr;
    dev->GetVertexShader(&oldVS);
    dev->GetPixelShader(&oldPS);
    dev->GetVertexDeclaration(&oldVDecl);

    DWORD oldZEnable, oldAlphaBlend, oldLighting, oldSrcBlend, oldDestBlend, oldCull;
    dev->GetRenderState(D3DRS_ZENABLE, &oldZEnable);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    dev->GetRenderState(D3DRS_LIGHTING, &oldLighting);
    dev->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
    dev->GetRenderState(D3DRS_DESTBLEND, &oldDestBlend);
    dev->GetRenderState(D3DRS_CULLMODE, &oldCull);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0, nullptr);
    dev->SetFVF(HUD_FVF);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // 1. Remote Player 3D Waypoint (Cyan)
    if (g_hasRemotePlayer.load()) {
        Render3DMarker(dev, g_remoteX.load(), g_remoteY.load(), g_remoteZ.load() + 1.8f, 0xEE00E5FF, 0xFFFFFFFF);
    }

    // 2. Test Marker (Vibrant Lime Green: 0xEE00FF66)
    if (g_testMarkerActive.load() && g_testMarkerInitialized.load()) {
        Render3DMarker(dev, g_testMarkerPos.x, g_testMarkerPos.y, g_testMarkerPos.z, 0xEE00FF66, 0xFFFFFFFF);
    }

    dev->SetRenderState(D3DRS_ZENABLE, oldZEnable);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    dev->SetRenderState(D3DRS_LIGHTING, oldLighting);
    dev->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, oldDestBlend);
    dev->SetRenderState(D3DRS_CULLMODE, oldCull);

    dev->SetVertexShader(oldVS);
    if (oldVS) oldVS->Release();
    dev->SetPixelShader(oldPS);
    if (oldPS) oldPS->Release();
    dev->SetVertexDeclaration(oldVDecl);
    if (oldVDecl) oldVDecl->Release();
}

// =============================================================
//  D3D9 VTable Hooks (EndScene & Present) + Window Subclass
// =============================================================
typedef HRESULT (STDMETHODCALLTYPE *fn_EndScene)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *fn_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

static fn_EndScene g_origEndScene = nullptr;
static fn_Present  g_origPresent  = nullptr;
static void** g_gameDeviceVTable = nullptr;
static bool g_d3dHookActive = false;

static bool g_wndProcHooked = false;
static WNDPROC g_origWndProc = nullptr;

LRESULT CALLBACK GameWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    if (uMsg == WM_ACTIVATE || uMsg == WM_SETFOCUS) {
        SetCursor(NULL);
    }
    return CallWindowProc(g_origWndProc, hWnd, uMsg, wParam, lParam);
}

HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* pDev) {
    return g_origEndScene(pDev);
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDirect3DDevice9* pDev, const RECT* pSrc, const RECT* pDst, HWND hDst, const RGNDATA* pDirty) {
    if (!g_wndProcHooked && pDev) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(pDev->GetCreationParameters(&cp)) && cp.hFocusWindow) {
            g_origWndProc = (WNDPROC)GetWindowLongPtrA(cp.hFocusWindow, GWLP_WNDPROC);
            if (g_origWndProc) {
                SetWindowLongPtrA(cp.hFocusWindow, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
                g_wndProcHooked = true;
                Log("[INPUT] Subclassed game window (0x%08X) - system cursor suppressed!", (DWORD)cp.hFocusWindow);
            }
        }
    }

    // 1. Ensure Lua callback is registered once Level/Script system is ready (atomic)
    bool expected = false;
    if (g_luaCallbacksRegistered.compare_exchange_strong(expected, true)) {
        void* L = GetLuaState();
        if (L) {
            g_RegisterLuaGlobal(nullptr, "OnBuddySpawned", (void*)&MyBuddySpawnedHandler);
            g_RegisterLuaGlobal(nullptr, "OnPlayerReport", (void*)&OnPlayerReportHandler);
            g_RegisterLuaGlobal(nullptr, "ModLog", (void*)&ModLogHandler);
            Log("[LUA-INIT] Registered 'OnBuddySpawned' callback on render thread.");
        }
    }

    // 2. Process synchronous spawn request (from F7/Num7 key)
    if (g_reqSpawnBuddy.exchange(false)) {
        uintptr_t addr = g_trackedPlayerAddr.load();
        if (addr) {
            float cam[7]; SIZE_T br = 0;
            if (SafeReadPtr((void*)addr, sizeof(cam))) {
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, cam, sizeof(cam), &br) && br == sizeof(cam)) {
                    // Use FLAT forward vector (ignore pitch) for horizontal positioning
                    float fwdX = cam[4];
                    float fwdY = cam[5];
                    float fwdLen = sqrtf(fwdX*fwdX + fwdY*fwdY);
                    if (fwdLen > 0.001f) {
                        fwdX /= fwdLen;
                        fwdY /= fwdLen;
                    } else {
                        fwdX = 1.0f; fwdY = 0.0f;
                    }
                    
                    float sx = cam[0] + fwdX * 3.5f;
                    float sy = cam[1] + fwdY * 3.5f;
                    // Use camera height + small offset, will be corrected by game physics
                    float sz = cam[2] + 1.0f;  
                    ExecuteSpawnBuddy(sx, sy, sz);
                }
            } else {
                Log("[SPAWN] Camera read failed!");
            }
        } else {
            Log("[SPAWN] Camera not tracked yet!");
        }
    }

    // 3. Process synchronous teleport request (from F8/Num8/F6 key)
    if (g_reqTeleportBuddy.exchange(false)) {
        void* pEntity = g_p2EntityPtr.load();
        if (pEntity && SafeReadPtr(pEntity, 0x100)) {
            Vec3 pos = g_testMarkerPos;
            pos.z -= 1.5f;
            Log("[TELEPORT] Attempting teleport to (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
            SetEntityPositionAndRotation(pEntity, pos.x, pos.y, pos.z);
            Log("[TELEPORT] SUCCESS - Moved Player 2 Buddy to marker position (%.1f, %.1f, %.1f)",
                pos.x, pos.y, pos.z);
        } else {
            Log("[TELEPORT] Buddy entity invalid or dead! Press [F7] to respawn.");
            g_p2BuddySpawned.store(false);
            g_p2EntityPtr.store(nullptr);
            g_p2EntityId.store(0);
        }
    }

    // 4. Live update Player 2 position if remote player is active
    if (g_hasRemotePlayer.load()) {
        if (!g_p2BuddySpawned.load()) {
            // Auto-spawn buddy upon connecting
            ExecuteSpawnBuddy(g_remoteX.load(), g_remoteY.load(), g_remoteZ.load() - 1.8f);
        } else {
            void* pEntity = g_p2EntityPtr.load();
            if (pEntity && SafeReadPtr(pEntity, 0x100)) {
                SetEntityPositionAndRotation(pEntity, g_remoteX.load(), g_remoteY.load(), g_remoteZ.load() - 1.8f);
            } else {
                // Entity became invalid, reset state
                g_p2BuddySpawned.store(false);
                g_p2EntityPtr.store(nullptr);
                g_p2EntityId.store(0);
            }
        }
    }

    RenderCoopOverlay(pDev);
    return g_origPresent(pDev, pSrc, pDst, hDst, pDirty);
}

bool SetupD3D9Hook() {
    Log("[D3D9] Scanning for game's IDirect3DDevice9...");

    // The game creates its device early. We need to find it by scanning memory
    // for the IDirect3DDevice9 vtable pattern, or hook the first Present call.
    // Better approach: scan for the device pointer in known locations.
    
    // For now, use the temporary device approach but with HARDWARE vertex processing
    // to match the game's actual device type
    
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        hD3D9 = LoadLibraryA("d3d9.dll");
    }
    if (!hD3D9) {
        Log("[D3D9] d3d9.dll not available in process!");
        return false;
    }

    typedef IDirect3D9* (WINAPI *fn_Create)(UINT);
    fn_Create pCreate = (fn_Create)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pCreate) {
        Log("[D3D9] Direct3DCreate9 export not found!");
        return false;
    }

    IDirect3D9* pD3D = pCreate(D3D_SDK_VERSION);
    if (!pD3D) {
        Log("[D3D9] Direct3DCreate9 returned NULL!");
        return false;
    }

    // Get the game's window handle
    HWND hGameWnd = FindWindowA(NULL, "Far Cry 2");
    if (!hGameWnd) {
        hGameWnd = FindWindowA("FarCry2", NULL);
    }
    
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "FC2CoopD3D";
    RegisterClassExA(&wc);
    HWND hWnd = hGameWnd ? hGameWnd : CreateWindowExA(0, "FC2CoopD3D", "", WS_POPUP, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hWnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    // Use HARDWARE vertex processing to match the game's actual device
    IDirect3DDevice9* pTmp = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &pTmp);
    if (FAILED(hr) || !pTmp) {
        // Try mixed vertex processing
        hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                 D3DCREATE_MIXED_VERTEXPROCESSING, &pp, &pTmp);
    }
    if (FAILED(hr) || !pTmp) {
        // Try software as last resort
        hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pTmp);
    }
    if (FAILED(hr) || !pTmp) {
        hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hWnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &pTmp);
    }
    if (FAILED(hr) || !pTmp) {
        Log("[D3D9] CreateDevice failed (hr: 0x%08X).", hr);
        pD3D->Release();
        if (!hGameWnd) DestroyWindow(hWnd);
        UnregisterClassA("FC2CoopD3D", wc.hInstance);
        return false;
    }

    void** vtable = *(void***)pTmp;
    g_origEndScene = (fn_EndScene)vtable[42];
    g_origPresent  = (fn_Present)vtable[17];
    Log("[D3D9] Original EndScene: 0x%08X | Present: 0x%08X", (DWORD)g_origEndScene, (DWORD)g_origPresent);

    // Store the vtable for cleanup
    g_gameDeviceVTable = vtable;

    DWORD oldProt;
    VirtualProtect(&vtable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    vtable[42] = (void*)&HookedEndScene;
    VirtualProtect(&vtable[42], sizeof(void*), oldProt, &oldProt);

    VirtualProtect(&vtable[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    vtable[17] = (void*)&HookedPresent;
    VirtualProtect(&vtable[17], sizeof(void*), oldProt, &oldProt);

    pTmp->Release();
    pD3D->Release();
    if (!hGameWnd) DestroyWindow(hWnd);
    UnregisterClassA("FC2CoopD3D", wc.hInstance);

    g_d3dHookActive = true;
    Log("[D3D9] Present hook active!");
    return true;
}

// =============================================================
//  Networking Setup
// =============================================================
bool InitNetworkSender() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    g_txSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_txSocket == INVALID_SOCKET) return false;

    BOOL bcast = TRUE;
    setsockopt(g_txSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));

    memset(&g_txLocal, 0, sizeof(g_txLocal));
    g_txLocal.sin_family = AF_INET;
    g_txLocal.sin_port = htons(COOP_PORT_TX);
    g_txLocal.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(&g_txBroadcast, 0, sizeof(g_txBroadcast));
    g_txBroadcast.sin_family = AF_INET;
    g_txBroadcast.sin_port = htons(COOP_PORT_TX);
    g_txBroadcast.sin_addr.s_addr = INADDR_BROADCAST;

    Log("[NET-TX] Broadcast sender ready on UDP :%d", COOP_PORT_TX);
    return true;
}

void SendPosition(float x, float y, float z) {
    if (g_txSocket == INVALID_SOCKET) return;
    CoopPacket pkt = { COOP_MAGIC, 1, ++g_txSeq, (uint32_t)GetTickCount(), x, y, z };
    sendto(g_txSocket, (const char*)&pkt, sizeof(pkt), 0, (sockaddr*)&g_txLocal, sizeof(g_txLocal));
    sendto(g_txSocket, (const char*)&pkt, sizeof(pkt), 0, (sockaddr*)&g_txBroadcast, sizeof(g_txBroadcast));
}

DWORD WINAPI ReceiverThread(LPVOID) {
    SOCKET rxSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rxSock == INVALID_SOCKET) {
        Log("[NET-RX] Socket creation failed.");
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(rxSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(COOP_PORT_RX);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(rxSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("[NET-RX] Bind to :%d failed: %d", COOP_PORT_RX, WSAGetLastError());
        closesocket(rxSock);
        return 1;
    }
    Log("[NET-RX] Listening for remote partner packets on UDP :%d", COOP_PORT_RX);

    // Set socket to non-blocking for shutdown check
    u_long mode = 1;
    ioctlsocket(rxSock, FIONBIO, &mode);

    while (!g_shutdownRequested.load()) {
        char buf[256];
        sockaddr_in from;
        int fromLen = sizeof(from);
        int n = recvfrom(rxSock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n >= (int)sizeof(CoopPacket)) {
            CoopPacket* p = (CoopPacket*)buf;
            if (p->magic == COOP_MAGIC && p->packetType == 1) {
                g_remoteX.store(p->x);
                g_remoteY.store(p->y);
                g_remoteZ.store(p->z);
                g_remoteLastTick.store(GetTickCount());
                if (!g_hasRemotePlayer.exchange(true)) {
                    Log("[NET-RX] Remote Player connected at (%.1f, %.1f, %.1f)!", p->x, p->y, p->z);
                }
            }
        } else if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                Sleep(1); // Brief sleep on error
            }
        }
    }
    
    closesocket(rxSock);
    Log("[NET-RX] Receiver thread stopped.");
    return 0;
}

// =============================================================
//  Main Co-op Orchestrator Thread
// =============================================================
DWORD WINAPI CoopThread(LPVOID) {
    Sleep(2000);
    Log("=================================================");
    Log("Far Cry 2 Co-op Mod - Milestone 4: Entity Spawning");
    Log("Keys: [F3] Toggle TX | [F4] Anchor Marker");
    Log("       [F7 / Num7] Spawn Buddy | [F8 / Num8] Teleport Buddy");
    Log("=================================================");

    InitNetworkSender();
    g_receiverThreadHandle = CreateThread(NULL, 0, ReceiverThread, NULL, 0, NULL);
    if (!g_receiverThreadHandle) {
        Log("[ERROR] Failed to create receiver thread!");
    }

    HMODULE hDunia = NULL;
    for (int i = 0; i < 30 && !hDunia; i++) {
        hDunia = GetModuleHandleA("Dunia.dll");
        if (!hDunia) Sleep(500);
    }
    if (hDunia) Log("[SUCCESS] Dunia.dll located at 0x%08X", (DWORD)hDunia);

    AutoLocateCamera();

    Sleep(4000);
    if (!SetupD3D9Hook()) {
        Log("[D3D9] Hook installation deferred or failed.");
    }

    bool f3d = false, f4d = false, f7d = false, f8d = false;
    int tick = 0;

    while (1) {
        Sleep(50);
        tick++;

        if (!g_trackedPlayerAddr || (tick % 60 == 0)) {
            AutoLocateCamera();
        }

        bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !f3d) {
            g_trackingEnabled.store(!g_trackingEnabled.load());
            Log("[TX] Transmission %s", g_trackingEnabled.load() ? "RESUMED" : "PAUSED");
        }
        f3d = f3;

        bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4 && !f4d) {
            uintptr_t addr = g_trackedPlayerAddr.load();
            if (!addr) AutoLocateCamera();
            addr = g_trackedPlayerAddr.load();

            if (addr) {
                float cam[7]; SIZE_T brRead;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, cam, sizeof(cam), &brRead) && brRead == sizeof(cam)) {
                    if (IsValidCoord(cam[0], cam[1], cam[2])) {
                        g_testMarkerPos.x = cam[0] + cam[4] * 15.0f;
                        g_testMarkerPos.y = cam[1] + cam[5] * 15.0f;
                        g_testMarkerPos.z = cam[2] + cam[6] * 15.0f;
                        g_testMarkerActive.store(true);
                        g_testMarkerInitialized.store(true);
                        Log("[F4] Anchored 3D marker 15m ahead at (%.1f, %.1f, %.1f) from (%.1f, %.1f, %.1f)",
                            g_testMarkerPos.x, g_testMarkerPos.y, g_testMarkerPos.z, cam[0], cam[1], cam[2]);
                    } else {
                        Log("[F4] Player coordinates not valid yet.");
                    }
                }
            } else {
                Log("[F4] Camera context not found yet.");
            }
        }
        f4d = f4;

        // [F7 / Num7] Spawn Buddy NPC 3.5m ahead of player (avoids F5 QuickSave conflict)
        bool f7 = ((GetAsyncKeyState(VK_F7) & 0x8000) != 0) || ((GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0);
        if (f7 && !f7d) {
            g_reqSpawnBuddy.store(true);
            Log("[SPAWN-KEY] Spawn buddy requested (archetype: %s)", g_buddyArchetypes[g_selectedArchetypeIdx.load()]);
        }
        f7d = f7;

        // [F8 / Num8 / F6] Teleport buddy to current marker position
        bool f8 = ((GetAsyncKeyState(VK_F8) & 0x8000) != 0) || ((GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0) || ((GetAsyncKeyState(VK_F6) & 0x8000) != 0);
        if (f8 && !f8d) {
            if (g_testMarkerInitialized.load()) {
                g_reqTeleportBuddy.store(true);
                Log("[TELEPORT-KEY] Teleport buddy requested to (%.1f, %.1f, %.1f)",
                    g_testMarkerPos.x, g_testMarkerPos.y, g_testMarkerPos.z);
            } else {
                Log("[TELEPORT-KEY] No marker set yet! Press [F4] first.");
            }
        }
        f8d = f8;

        // Broadcast own position at 20 Hz
        if (g_trackingEnabled.load() && g_trackedPlayerAddr.load()) {
            uintptr_t addr = g_trackedPlayerAddr.load();
            Vec3 lp; SIZE_T brRead;
            if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &lp, sizeof(Vec3), &brRead) && brRead == sizeof(Vec3) && IsValidCoord(lp.x, lp.y, lp.z)) {
                SendPosition(lp.x, lp.y, lp.z);
                if (tick % 100 == 0) {
                    Log("[UDP TX #%u] Pos: (%.1f, %.1f, %.1f) -> Sent to UDP :%d", g_txSeq.load(), lp.x, lp.y, lp.z, COOP_PORT_TX);
                }
            }
        }
        // =============================================================
        //  Milestone 5 (Animation & Action Replication - Co-op Testing)
        // =============================================================

        // Player actions state tracking
        struct ActionState {
            bool isFiringWeapon;
            bool isCrouching;
            bool isDead;
        } g_actionState = {};

        // Initialize action state (read from memory offsets if needed)
        if (!g_actionState.isFiringWeapon && !g_actionState.isCrouching) {
            g_actionState.isFiringWeapon = false;  // Default: not doing anything
        }

        // Check for weapon fire event
        static bool g_lastWasFiring = false;
        static bool g_isDead = false;
        uintptr_t addr = g_trackedPlayerAddr.load();
        if (addr) {
            uint8_t* pFlags = (uint8_t*)addr;  // Byte pointer for flag bits
            bool currentlyFiring = (*pFlags & 0x01) != 0;  // Check weapon fire flag
            bool isCrouching = (*pFlags & 0x02) != 0;      // Check crouch flag
            if (currentlyFiring && !g_lastWasFiring) {
                Log("[WEAPON_FIRE] Weapon fired!");
                g_actionState.isFiringWeapon = true;
            } else if (!currentlyFiring && g_lastWasFiring) {
                g_actionState.isFiringWeapon = false;
            }
            g_lastWasFiring = currentlyFiring;

            // Check death state
            bool currentlyDead = (*pFlags & 0x10) != 0;
            if (currentlyDead && !g_isDead) {
                Log("[DEATH] Player died! Sending DEATH flag to remote.");
                g_actionState.isDead = true;
            } else if (!currentlyDead && g_isDead) {
                Log("[REVIVE] Player revived! Resetting action flags.");
                g_actionState.isDead = false;
                g_actionState.isFiringWeapon = false;
                g_actionState.isCrouching = false;
            }
            g_isDead = currentlyDead;

            // Check crouch state
            if (isCrouching && !g_actionState.isCrouching) {
                Log("[STANCE] Going prone!");
                g_actionState.isCrouching = true;
            } else if (!isCrouching && g_actionState.isCrouching) {
                g_actionState.isCrouching = false;
            }
            g_actionState.isCrouching = isCrouching;
        }

        // Action event log thread - separate from position sync thread
        static volatile bool g_reqLogActionEvent = false;
        static char g_lastLoggedAction[128] = { 0 };
        
        // Send action state to remote (every frame, with debounce)
        if (tick % 5 == 0 && g_trackedPlayerAddr) {
            // Build action packet: Flags | Frame Counter | Timestamp
            uint32_t actionFlags = 0;
            if (g_actionState.isFiringWeapon) actionFlags |= 0x01;
            if (g_actionState.isCrouching)    actionFlags |= 0x02;
            if (g_actionState.isDead)               actionFlags |= 0x04;

            SOCKET rxSock = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in remoteAddr;
            memset(&remoteAddr, 0, sizeof(remoteAddr));
            remoteAddr.sin_family = AF_INET;
            remoteAddr.sin_port = htons(COOP_PORT_RX);

            char buffer[128];
            snprintf(buffer, sizeof(buffer), "ACTION:%u|%u|%.2f", actionFlags, (unsigned int)(tick / 60), (double)tick / 1000.0);
            int len = strlen(buffer);

            sendto(rxSock, buffer, len, 0, (sockaddr*)&remoteAddr, sizeof(remoteAddr));
            closesocket(rxSock);

            Log("[ACTION TX] Synced to remote: flags=%u frame=%u time=%.2f", actionFlags, tick / 60, tick / 1000.0);
            
            // Store last logged action for debouncing
            char temp[32];
            snprintf(temp, sizeof(temp), "ACTION:%u", actionFlags);
            strncpy(g_lastLoggedAction, temp, 31);
        }
    }
    return 0;
}

// =============================================================
//  DLL Entry Point
// =============================================================
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        Log("[INIT] binkw32 proxy attached (PID: %d)", GetCurrentProcessId());
        CreateThread(NULL, 0, CoopThread, NULL, 0, NULL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        Log("[SHUTDOWN] Proxy detaching - cleaning up...");
        
        // Signal shutdown to all threads
        g_shutdownRequested.store(true);
        
        // Restore D3D9 vtable hooks
        if (g_gameDeviceVTable && g_origPresent && g_origEndScene) {
            DWORD oldProt;
            VirtualProtect(&g_gameDeviceVTable[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_gameDeviceVTable[42] = (void*)g_origEndScene;
            VirtualProtect(&g_gameDeviceVTable[42], sizeof(void*), oldProt, &oldProt);
            
            VirtualProtect(&g_gameDeviceVTable[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_gameDeviceVTable[17] = (void*)g_origPresent;
            VirtualProtect(&g_gameDeviceVTable[17], sizeof(void*), oldProt, &oldProt);
            Log("[SHUTDOWN] D3D9 hooks restored.");
        }
        
        // Wait for receiver thread to finish
        if (g_receiverThreadHandle) {
            WaitForSingleObject(g_receiverThreadHandle, 2000);
            CloseHandle(g_receiverThreadHandle);
            g_receiverThreadHandle = NULL;
        }
        
        // Cleanup networking
        if (g_txSocket != INVALID_SOCKET) {
            closesocket(g_txSocket);
            g_txSocket = INVALID_SOCKET;
        }
        WSACleanup();
        
        // Restore window procedure
        if (g_wndProcHooked && g_origWndProc) {
            // Note: We don't have the window handle here, but the hook will be gone with D3D9 cleanup
        }
        
        // Close log file
        EnterCriticalSection(&g_cs);
        if (g_logFile) { 
            fclose(g_logFile); 
            g_logFile = NULL; 
        }
        LeaveCriticalSection(&g_cs);
        
        if (g_csInitialized) {
            DeleteCriticalSection(&g_cs);
            g_csInitialized = false;
        }
        
        Log("[SHUTDOWN] Cleanup complete.");
    }
    return TRUE;
}
