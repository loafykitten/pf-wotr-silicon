// Callback smoke test: register a 1.41 CCallback for UserStatsReceived_t through the shim,
// request stats with SteamUserStats011 via the flat API, pump SteamAPI_RunCallbacks.
// usage: cd <dir with steam_appid.txt> && ./callbacks
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include "steam/steam_api.h"

extern "C" void *SteamAPI_ISteamClient_GetISteamUserStats(intptr_t, HSteamUser, HSteamPipe, const char *);
extern "C" bool SteamAPI_ISteamUserStats_RequestCurrentStats(intptr_t);
extern "C" uint32 SteamAPI_ISteamUserStats_GetNumAchievements(intptr_t);

struct Listener {
    bool done = false;
    void OnStats(UserStatsReceived_t *p);
    CCallback<Listener, UserStatsReceived_t, false> cb{this, &Listener::OnStats};
};

void Listener::OnStats(UserStatsReceived_t *p)
{
    printf("UserStatsReceived_t: game=%llu result=%d\n", (unsigned long long)p->m_nGameID, (int)p->m_eResult);
    done = true;
}

int main()
{
    if (!SteamAPI_Init()) { printf("init failed\n"); return 1; }
    Listener l;
    auto steamClient = (void *(*)())dlsym(RTLD_DEFAULT, "SteamClient");
    intptr_t stats = (intptr_t)SteamAPI_ISteamClient_GetISteamUserStats(
        (intptr_t)steamClient(), SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(), "STEAMUSERSTATS_INTERFACE_VERSION011");
    printf("ISteamUserStats011=%p request=%d\n", (void *)stats, SteamAPI_ISteamUserStats_RequestCurrentStats(stats));
    for (int i = 0; i < 100 && !l.done; i++) { SteamAPI_RunCallbacks(); usleep(50000); }
    printf("callback %s; achievements defined: %u\n", l.done ? "RECEIVED" : "NOT received", SteamAPI_ISteamUserStats_GetNumAchievements(stats));
    SteamAPI_Shutdown();
    return l.done ? 0 : 2;
}
