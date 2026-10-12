using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using HarmonyLib;
using Owlcat.Runtime.Visual.Dxt;
using StbDxtSharp;

namespace WrathPerf;

// Character atlases are DXT-compressed by DxtCompressorService.Request.Compress on worker
// threads. Without lib_burst_generated (the arm64 build has none) that runs the C# as
// compiled by Mono: about 10.8 s of CPU per load. libwrathdxt (native/wrathdxt) is the
// same algorithm compiled for arm64, checked to give byte-identical output.
//
// The game's own compressor stays the fallback: it's used whenever libwrathdxt isn't
// loaded (an x86_64 player, a missing file), fails the self-test at startup, is switched
// off, or (with VerifyDxt) ever produces different bytes, which also turns it off for the
// rest of the session.
internal static unsafe class NativeDxt
{
    private const string Library = "libwrathdxt.dylib";
    private const int ExpectedVersion = 1;

    private static delegate* unmanaged[Cdecl]<int, int, byte*, byte*, int, int, int> _compress;
    private delegate void ManagedCompress(int width, int height, byte* inData, byte* outData, bool hasAlpha);
    private static ManagedCompress _game; // Request.Compress$BurstManaged: the game's own code

    private static volatile bool _usable; // loaded and passed the self-test
    private static volatile bool _failed; // a verify mismatch turned it off for this session

    // The library's state; Describe() adds whether the setting has it switched on.
    internal static string Status { get; private set; } = "not loaded";
    internal static long Calls, VerifiedCalls, Mismatches;
    private static long _seenCalls, _reportedCalls;
    private static float _changedAt;

    internal static bool Active => _usable && !_failed && Main.Enabled && Main.Settings.NativeDxt;

    internal static string Describe() =>
        _usable && !_failed && !Main.Settings.NativeDxt ? "off in settings; " + Status.Replace("active (", "ready (") : Status;

    [DllImport("libSystem.dylib")] private static extern IntPtr dlopen(string path, int mode);
    [DllImport("libSystem.dylib")] private static extern IntPtr dlsym(IntPtr handle, string symbol);
    [DllImport("libSystem.dylib")] private static extern IntPtr dlerror();

    internal static void Init(string modDir)
    {
        try
        {
            Status = Load(modDir);
        }
        catch (Exception e)
        {
            Status = "error, using the game's compressor: " + e.Message;
            Main.Mod.Logger.Error(e.ToString());
        }
        Main.Mod.Logger.Log("native dxt: " + Status);
    }

    private static string Load(string modDir)
    {
        var method = AccessTools.Method(typeof(DxtCompressorService.Request), "Compress$BurstManaged");
        if (method == null) return "the game's compressor wasn't found (game update?); not patched";
        _game = (ManagedCompress)Delegate.CreateDelegate(typeof(ManagedCompress), method);

        var path = Path.Combine(modDir, Library);
        var lib = dlopen(path, 2 | 4); // RTLD_NOW | RTLD_LOCAL; fails on an x86_64 player
        if (lib == IntPtr.Zero) return $"{Library} not loaded ({Marshal.PtrToStringAnsi(dlerror())}); using the game's compressor";
        var version = (delegate* unmanaged[Cdecl]<int>)dlsym(lib, "wrathdxt_version");
        var tables = (delegate* unmanaged[Cdecl]<byte*, float*, float*, void>)dlsym(lib, "wrathdxt_tables");
        _compress = (delegate* unmanaged[Cdecl]<int, int, byte*, byte*, int, int, int>)dlsym(lib, "wrathdxt_compress");
        if (version == null || tables == null || _compress == null) return $"{Library} is missing exports; using the game's compressor";
        if (version() != ExpectedVersion) return $"{Library} is version {version()}, expected {ExpectedVersion}; using the game's compressor";

        var error = SelfTest(tables);
        if (error != null) return "self-test failed, using the game's compressor: " + error;
        _usable = true;
        return $"active ({Library} v{ExpectedVersion}, self-test passed)";
    }

    // Lookup tables bit for bit, then a test image through both compressors, DXT5 and DXT1.
    private static string SelfTest(delegate* unmanaged[Cdecl]<byte*, float*, float*, void> tables)
    {
        var ctx = default(DxtContext);
        ctx.Init();
        var nativeCtx = new byte[sizeof(DxtContext)];
        var mid5 = new float[StbDxt.midpoints5.Length];
        var mid6 = new float[StbDxt.midpoints6.Length];
        fixed (byte* c = nativeCtx) fixed (float* m5 = mid5) fixed (float* m6 = mid6) tables(c, m5, m6);
        var managedCtx = (byte*)&ctx;
        for (var i = 0; i < nativeCtx.Length; i++)
            if (nativeCtx[i] != managedCtx[i]) return $"lookup table byte {i} differs";
        for (var i = 0; i < mid5.Length; i++)
            if (Bits(mid5[i]) != Bits(StbDxt.midpoints5[i])) return $"midpoints5[{i}] differs";
        for (var i = 0; i < mid6.Length; i++)
            if (Bits(mid6[i]) != Bits(StbDxt.midpoints6[i])) return $"midpoints6[{i}] differs";

        var image = TestImage(128);
        foreach (var alpha in new[] { true, false })
        {
            var size = 128 / 4 * (128 / 4) * (alpha ? 16 : 8);
            var expected = new byte[size];
            var actual = new byte[size];
            fixed (byte* src = image) fixed (byte* e = expected) fixed (byte* a = actual)
            {
                _game(128, 128, src, e, alpha);
                if (_compress(128, 128, src, a, alpha ? 1 : 0, 0) != 0) return "native compressor returned an error";
            }
            for (var i = 0; i < size; i++)
                if (expected[i] != actual[i]) return $"{(alpha ? "DXT5" : "DXT1")} output differs at byte {i}";
        }
        return null;
    }

    private static int Bits(float f) => *(int*)&f;

    // Noise, gradients, flat and near-flat blocks, 0/255 extremes, and varied alpha.
    private static byte[] TestImage(int size)
    {
        var rng = new Random(20261010);
        var img = new byte[size * size * 4];
        for (var by = 0; by < size; by += 4)
        for (var bx = 0; bx < size; bx += 4)
        {
            var kind = rng.Next(5);
            int r0 = rng.Next(256), g0 = rng.Next(256), b0 = rng.Next(256), a0 = rng.Next(256);
            for (var y = 0; y < 4; y++)
            for (var x = 0; x < 4; x++)
            {
                var p = ((by + y) * size + bx + x) * 4;
                var t = (y * 4 + x) * 17;
                for (var ch = 0; ch < 4; ch++)
                {
                    var c0 = ch == 0 ? r0 : ch == 1 ? g0 : ch == 2 ? b0 : a0;
                    img[p + ch] = kind switch
                    {
                        0 => (byte)rng.Next(256),
                        1 => (byte)((c0 + t) / 2),
                        2 => (byte)c0,
                        3 => (byte)Math.Min(255, c0 + rng.Next(3)),
                        _ => (byte)(rng.Next(2) * 255),
                    };
                }
            }
        }
        return img;
    }

    // From the Harmony prefix, on the game's worker threads. False: let the game run its own.
    internal static bool TryCompress(int width, int height, byte* inData, byte* outData, bool hasAlpha)
    {
        if (!Active) return false;
        if (_compress(width, height, inData, outData, hasAlpha ? 1 : 0, 0) != 0) return false; // the game's own error path
        Interlocked.Increment(ref Calls);
        if (Main.Settings.VerifyDxt) Verify(width, height, inData, outData, hasAlpha);
        return true;
    }

    private static void Verify(int width, int height, byte* inData, byte* outData, bool hasAlpha)
    {
        var size = width / 4 * (height / 4) * (hasAlpha ? 16 : 8);
        var expected = new byte[size];
        fixed (byte* e = expected) _game(width, height, inData, e, hasAlpha);
        Interlocked.Increment(ref VerifiedCalls);
        for (var i = 0; i < size; i++)
        {
            if (expected[i] == outData[i]) continue;
            Marshal.Copy(expected, 0, (IntPtr)outData, size); // keep the game's output
            Interlocked.Increment(ref Mismatches);
            _failed = true;
            Status = "turned off after a mismatch; using the game's compressor";
            Main.Note($"native dxt MISMATCH ({width}x{height} {(hasAlpha ? "DXT5" : "DXT1")}, first at byte {i}); " +
                      "kept the game's output and switched to the game's compressor");
            return;
        }
    }

    // Main thread, every frame: one log line per burst of compressions (e.g. per load).
    internal static void ReportIfIdle(float now)
    {
        var calls = Interlocked.Read(ref Calls);
        if (calls != _seenCalls)
        {
            _seenCalls = calls;
            _changedAt = now;
            return;
        }
        if (calls == _reportedCalls || now - _changedAt < 2f) return;
        Main.Note($"native dxt: {calls - _reportedCalls} compressions" +
                  (Main.Settings.VerifyDxt ? $"; session: {Interlocked.Read(ref VerifiedCalls)} checked, {Interlocked.Read(ref Mismatches)} mismatches" : ""));
        _reportedCalls = calls;
    }
}

[HarmonyPatch]
internal static class CompressPatch
{
    private static MethodBase TargetMethod() =>
        AccessTools.Method(typeof(DxtCompressorService.Request), "Compress",
            new[] { typeof(int), typeof(int), typeof(byte*), typeof(byte*), typeof(bool) });

    private static unsafe bool Prefix(int width, int height, byte* inData, byte* outData, bool hasAlpha) =>
        !NativeDxt.TryCompress(width, height, inData, outData, hasAlpha);
}
