// Smoke test for the shim, run natively (arm64) with Steam running: exercises the same
// path as Steamworks.NET 11 (1.41 interface versions through the flat API).
// usage: cd <dir with steam_appid.txt> && ./smoke <path to shim libsteam_api.dylib>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define SYM(ret, name, args) ret (*name) args = (ret (*) args)dlsym(h, #name); if (!name) { printf("missing %s\n", #name); return 1; }

int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    SYM(bool, SteamAPI_Init, (void));
    SYM(void, SteamAPI_Shutdown, (void));
    SYM(void *, SteamClient, (void));
    SYM(int32_t, SteamAPI_GetHSteamUser, (void));
    SYM(int32_t, SteamAPI_GetHSteamPipe, (void));
    SYM(void *, SteamAPI_ISteamClient_GetISteamUser, (void *, int32_t, int32_t, const char *));
    SYM(void *, SteamAPI_ISteamClient_GetISteamApps, (void *, int32_t, int32_t, const char *));
    SYM(uint64_t, SteamAPI_ISteamUser_GetSteamID, (void *));
    SYM(bool, SteamAPI_ISteamUser_BLoggedOn, (void *));
    SYM(int, SteamAPI_ISteamApps_GetDLCCount, (void *));
    SYM(bool, SteamAPI_ISteamApps_BGetDLCDataByIndex, (void *, int, uint32_t *, bool *, char *, int));
    SYM(bool, SteamAPI_ISteamApps_BIsDlcInstalled, (void *, uint32_t));
    SYM(const char *, SteamAPI_ISteamApps_GetCurrentGameLanguage, (void *));

    if (!SteamAPI_Init()) { printf("SteamAPI_Init failed\n"); return 1; }
    void *client = SteamClient();
    int32_t user = SteamAPI_GetHSteamUser(), pipe = SteamAPI_GetHSteamPipe();
    printf("init ok; client=%p user=%d pipe=%d\n", client, user, pipe);
    void *su = SteamAPI_ISteamClient_GetISteamUser(client, user, pipe, "SteamUser019");
    void *sa = SteamAPI_ISteamClient_GetISteamApps(client, user, pipe, "STEAMAPPS_INTERFACE_VERSION008");
    printf("ISteamUser019=%p ISteamApps008=%p\n", su, sa);
    if (!su || !sa) return 1;
    printf("logged on: %d, steam id: %llu, language: %s\n", SteamAPI_ISteamUser_BLoggedOn(su),
           (unsigned long long)SteamAPI_ISteamUser_GetSteamID(su), SteamAPI_ISteamApps_GetCurrentGameLanguage(sa));
    int n = SteamAPI_ISteamApps_GetDLCCount(sa);
    printf("DLCs: %d\n", n);
    for (int i = 0; i < n; i++) {
        uint32_t id = 0; bool avail = false; char name[128] = {0};
        if (SteamAPI_ISteamApps_BGetDLCDataByIndex(sa, i, &id, &avail, name, sizeof name))
            printf("  %u owned=%d installed=%d %s\n", id, avail, SteamAPI_ISteamApps_BIsDlcInstalled(sa, id), name);
    }
    SteamAPI_Shutdown();
    printf("shutdown ok\n");
    return 0;
}
