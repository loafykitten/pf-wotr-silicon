// Core (non-interface) functions of Steamworks SDK 1.41's libsteam_api, implemented on top
// of a current universal libsteam_api (shipped next to this library as
// libsteam_api_modern.dylib, loaded at runtime).
//
// Why: the game's Steamworks.NET 11.0.0 needs SDK 1.41's exports, but Valve's 1.41 library
// is x86_64-only. The interface wrappers (flat_gen.cpp) just call into steamclient's C++
// interfaces; only the core below needs a real Steam API implementation, and the modern
// library provides it. Old core functions the modern library no longer exports are
// implemented here.
//
// Signatures follow the SDK 1.41 headers (steam_api.h, steam_api_internal.h,
// steam_gameserver.h). This file deliberately doesn't include them: steam_api.h declares
// an inline SteamClient() that clashes with the exported one below.
#include <dlfcn.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>

#define S_API extern "C" __attribute__((visibility("default")))

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint64_t SteamAPICall_t;

// SteamClient017 is the ISteamClient version SDK 1.41 (and so Steamworks.NET 11) uses;
// every interface the game asks for later is obtained through it.
static const char kSteamClientVersion[] = "SteamClient017";

static void Log(const char *msg, const char *detail = "")
{
    fprintf(stderr, "[steamshim] %s%s\n", msg, detail ? detail : "");
}

static void *Modern()
{
    static void *handle = [] {
        Dl_info info;
        if (!dladdr((void *)&Modern, &info) || !info.dli_fname) {
            Log("can't locate own library path");
            return (void *)nullptr;
        }
        std::string self(info.dli_fname);
        std::string dir = self.substr(0, self.find_last_of('/'));
        std::string path = dir + "/libsteam_api_modern.dylib";
        void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) Log("can't load ", dlerror());
        return h;
    }();
    return handle;
}

template <class F>
static F Sym(const char *name)
{
    void *h = Modern();
    return h ? reinterpret_cast<F>(dlsym(h, name)) : nullptr;
}

// Forward to the modern library's export of the same name, or return `fallback`.
#define FORWARD(ret, name, params, args, fallback)                     \
    S_API ret name params                                              \
    {                                                                  \
        static auto fn = Sym<ret(*) params>(#name);                    \
        return fn ? fn args : (fallback);                              \
    }

#define FORWARD_VOID(name, params, args)                               \
    S_API void name params                                             \
    {                                                                  \
        static auto fn = Sym<void(*) params>(#name);                   \
        if (fn) fn args;                                               \
    }

// --- client -----------------------------------------------------------------------------

// SDK 1.41 SteamAPI_Init() returned bool. Current SDKs implement it inline over
// SteamAPI_InitFlat, which returns an ESteamAPIInitResult (0 = OK) and an error message.
S_API bool SteamAPI_Init()
{
    static auto initFlat = Sym<int (*)(char *)>("SteamAPI_InitFlat");
    if (!initFlat) {
        Log("SteamAPI_InitFlat missing from libsteam_api_modern.dylib");
        return false;
    }
    char err[1024] = {0};
    int result = initFlat(err);
    if (result != 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "SteamAPI_InitFlat failed (%d): ", result);
        Log(buf, err);
    }
    return result == 0;
}

S_API bool SteamAPI_InitSafe() { return SteamAPI_Init(); }

FORWARD_VOID(SteamAPI_Shutdown, (), ())
FORWARD(bool, SteamAPI_RestartAppIfNecessary, (uint32_t unOwnAppID), (unOwnAppID), false)
FORWARD_VOID(SteamAPI_ReleaseCurrentThreadMemory, (), ())
FORWARD_VOID(SteamAPI_WriteMiniDump, (uint32_t code, void *info, uint32_t build), (code, info, build))
FORWARD_VOID(SteamAPI_SetMiniDumpComment, (const char *msg), (msg))
FORWARD_VOID(SteamAPI_RunCallbacks, (), ())
FORWARD_VOID(SteamAPI_RegisterCallback, (void *callback, int id), (callback, id))
FORWARD_VOID(SteamAPI_UnregisterCallback, (void *callback), (callback))
FORWARD_VOID(SteamAPI_RegisterCallResult, (void *callback, SteamAPICall_t call), (callback, call))
FORWARD_VOID(SteamAPI_UnregisterCallResult, (void *callback, SteamAPICall_t call), (callback, call))
FORWARD(bool, SteamAPI_IsSteamRunning, (), (), false)
FORWARD(const char *, SteamAPI_GetSteamInstallPath, (), (), "")
FORWARD(HSteamPipe, SteamAPI_GetHSteamPipe, (), (), 0)
FORWARD(HSteamUser, SteamAPI_GetHSteamUser, (), (), 0)
FORWARD(HSteamPipe, GetHSteamPipe, (), (), 0)
FORWARD(HSteamUser, GetHSteamUser, (), (), 0)
FORWARD_VOID(SteamAPI_SetTryCatchCallbacks, (bool enabled), (enabled))
FORWARD_VOID(SteamAPI_UseBreakpadCrashHandler,
             (const char *version, const char *date, const char *time, bool fullDumps, void *ctx, void *preDump),
             (version, date, time, fullDumps, ctx, preDump))
FORWARD_VOID(SteamAPI_SetBreakpadAppID, (uint32_t appId), (appId))
FORWARD(void *, SteamInternal_ContextInit, (void *data), (data), nullptr)
FORWARD(void *, SteamInternal_CreateInterface, (const char *ver), (ver), nullptr)

// 1.41 exported SteamClient() returning the global ISteamClient (SteamClient017). Ask
// steamclient for exactly that version so the 1.41 wrappers' vtable layout matches.
S_API void *SteamClient()
{
    // Cache once found; keep retrying while null (before SteamAPI_Init succeeds). A racing
    // duplicate CreateInterface call is harmless (steamclient returns the same object).
    static std::atomic<void *> client{nullptr};
    void *c = client.load(std::memory_order_acquire);
    if (!c) {
        c = SteamInternal_CreateInterface(kSteamClientVersion);
        if (c) client.store(c, std::memory_order_release);
    }
    return c;
}

// Pre-1.41 legacy exports that current SDKs dropped.
S_API void Steam_RunCallbacks(HSteamPipe, bool gameServerCallbacks);
S_API void Steam_RegisterInterfaceFuncs(void *) {}
S_API HSteamUser Steam_GetHSteamUserCurrent() { return SteamAPI_GetHSteamUser(); }

// --- game server (the game never runs one; init refuses, the rest are inert) ---------------

FORWARD_VOID(SteamGameServer_Shutdown, (), ())
FORWARD_VOID(SteamGameServer_RunCallbacks, (), ())
FORWARD(bool, SteamGameServer_BSecure, (), (), false)
FORWARD(uint64_t, SteamGameServer_GetSteamID, (), (), 0)
FORWARD(HSteamPipe, SteamGameServer_GetHSteamPipe, (), (), 0)
FORWARD(HSteamUser, SteamGameServer_GetHSteamUser, (), (), 0)
FORWARD(uint32_t, SteamGameServer_GetIPCCallCount, (), (), 0)

S_API void Steam_RunCallbacks(HSteamPipe, bool gameServerCallbacks)
{
    if (gameServerCallbacks) SteamGameServer_RunCallbacks();
    else SteamAPI_RunCallbacks();
}

// Game-server mode isn't supported: Wrath never runs a server, and the current SDK's
// SteamInternal_GameServer_Init_V2 also takes an interface-version list that would have to
// match the modern library's own server interfaces. Fail cleanly instead of guessing.
S_API bool SteamInternal_GameServer_Init(uint32_t, uint16_t, uint16_t, uint16_t, int, const char *)
{
    Log("game server init isn't supported by the shim");
    return false;
}

S_API bool SteamGameServer_InitSafe(uint32_t ip, uint16_t steamPort, uint16_t gamePort, uint16_t queryPort, int mode,
                                    const char *version)
{
    return SteamInternal_GameServer_Init(ip, steamPort, gamePort, queryPort, mode, version);
}

// Inline-only in 1.41 headers, but Steamworks.NET imports them.
S_API void SteamGameServer_ReleaseCurrentThreadMemory() { SteamAPI_ReleaseCurrentThreadMemory(); }
S_API void *SteamGameServerClient() { return nullptr; }
