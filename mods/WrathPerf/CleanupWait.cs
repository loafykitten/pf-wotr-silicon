using System.Reflection;
using HarmonyLib;
using Kingmaker;

namespace WrathPerf;

// Game.UnloadUnusedAssetsCoroutine runs twice per load and first waits Game.m_WaitOnStart
// frames, 100 by default (a dev cheat value, "wait_on_start_uuac"), before its cleanup.
// The cleanup only needs a frame boundary for deferred Destroy, handle releases and audio
// callbacks to settle, so 10 is plenty. Both passes still run. m_WaitOnStart is a plain
// static property, read each frame of the wait, so setting it is enough: no patch.
internal static class CleanupWait
{
    internal const int GameDefault = 100;
    internal const int Recommended = 10;
    internal const int Min = 1;

    private static readonly PropertyInfo Property = AccessTools.Property(typeof(Game), "m_WaitOnStart");

    internal static string Status { get; private set; } = "not applied";

    // Main thread. Applies the setting, or restores the game's default when the mod is off.
    internal static void Apply()
    {
        if (Property == null || Property.PropertyType != typeof(int))
        {
            Status = "Game.m_WaitOnStart not found (game update?); the game's own wait is in use";
            return;
        }
        var frames = Main.Enabled ? Clamp(Main.Settings.CleanupWaitFrames) : GameDefault;
        Property.SetValue(null, frames);
        var now = (int)Property.GetValue(null);
        Status = now == GameDefault ? $"{now} frames (the game's default)" : $"{now} frames (game: {GameDefault})";
    }

    private static int Clamp(int frames) => frames < Min ? Min : frames > GameDefault ? GameDefault : frames;
}
