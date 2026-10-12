using System;
using System.Collections.Concurrent;
using HarmonyLib;
using UnityEngine;
using UnityModManagerNet;

namespace WrathPerf;

public sealed class Settings : UnityModManager.ModSettings
{
    // Compress character atlases with libwrathdxt instead of the game's Mono-run C#.
    public bool NativeDxt = true;
    // Also run the game's compressor on every call and compare (slow; for testing).
    public bool VerifyDxt;
    // Frames waited before each loading cleanup pass (the game's default is 100).
    public int CleanupWaitFrames = CleanupWait.Recommended;

    public override void Save(UnityModManager.ModEntry modEntry) => Save(this, modEntry);
}

// Load-time improvements, each with its own toggle so it can be A/B tested:
//   - NativeDxt: the game's DXT compressor compiled for arm64, byte-identical output;
//   - CleanupWait: a shorter fixed wait before each of the two cleanup passes per load.
public static class Main
{
    internal static UnityModManager.ModEntry Mod;
    internal static Settings Settings;
    // The mod's on/off state from UMM. Not Mod.Active: UMM sets that only after OnToggle
    // returns, so it still reads false while the mod is being switched on.
    internal static volatile bool Enabled;
    private static readonly ConcurrentQueue<string> Pending = new ConcurrentQueue<string>();
    private static string _framesText;

    public static bool Load(UnityModManager.ModEntry modEntry)
    {
        Mod = modEntry;
        Settings = UnityModManager.ModSettings.Load<Settings>(modEntry);
        _framesText = Settings.CleanupWaitFrames.ToString();
        modEntry.OnGUI = OnGUI;
        modEntry.OnSaveGUI = entry => Settings.Save(entry);
        // UMM calls this after Load for an enabled mod, and from its UI.
        modEntry.OnToggle = (_, active) =>
        {
            Enabled = active;
            CleanupWait.Apply();
            Note(active
                ? $"on; native dxt: {NativeDxt.Describe()}, verify {(Settings.VerifyDxt ? "on" : "off")}; cleanup wait: {CleanupWait.Status}"
                : "off (game's compressor and cleanup wait)");
            return true;
        };

        var host = new GameObject("WrathPerf");
        UnityEngine.Object.DontDestroyOnLoad(host);
        host.hideFlags = HideFlags.HideAndDontSave;
        host.AddComponent<Host>();

        NativeDxt.Init(modEntry.Path);
        new Harmony(modEntry.Info.Id).PatchAll(typeof(Main).Assembly);
        return true;
    }

    // Any thread. Written to the UMM log (on the main thread) and, if WrathStats is
    // installed, to its session events, so benchmark runs record the configuration.
    internal static void Note(string text)
    {
        Pending.Enqueue(text);
        NoteToWrathStats?.Invoke(null, new object[] { "wrathperf: " + text });
    }

    private static readonly System.Reflection.MethodInfo NoteToWrathStats =
        AccessTools.TypeByName("WrathStats.WrathStatsApi")?.GetMethod("Note", new[] { typeof(string) });

    private sealed class Host : MonoBehaviour
    {
        private void Update()
        {
            while (Pending.TryDequeue(out var text)) Mod.Logger.Log(text);
            NativeDxt.ReportIfIdle(Time.realtimeSinceStartup);
        }
    }

    private static void OnGUI(UnityModManager.ModEntry modEntry)
    {
        Settings.NativeDxt = GUILayout.Toggle(Settings.NativeDxt, " Native texture compressor (faster character atlases)");
        GUILayout.Label($"    {NativeDxt.Describe()}");
        Settings.VerifyDxt = GUILayout.Toggle(Settings.VerifyDxt,
            " Check every compression against the game's compressor (slow; for testing)");
        if (NativeDxt.Calls > 0)
            GUILayout.Label($"    This session: {NativeDxt.Calls} compressions native, {NativeDxt.VerifiedCalls} checked, " +
                            $"{NativeDxt.Mismatches} mismatches");

        GUILayout.Space(8);
        GUILayout.BeginHorizontal();
        GUILayout.Label($"Frames to wait before each loading cleanup pass ({CleanupWait.Min}–{CleanupWait.GameDefault}, game: {CleanupWait.GameDefault}):",
            GUILayout.ExpandWidth(false));
        _framesText = GUILayout.TextField(_framesText, GUILayout.Width(50));
        if (int.TryParse(_framesText, out var frames) && frames != Settings.CleanupWaitFrames &&
            frames >= CleanupWait.Min && frames <= CleanupWait.GameDefault)
        {
            Settings.CleanupWaitFrames = frames;
            CleanupWait.Apply();
        }
        GUILayout.EndHorizontal();
        GUILayout.Label($"    {CleanupWait.Status}");
    }
}
