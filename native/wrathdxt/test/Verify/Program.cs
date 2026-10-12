using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using Owlcat.Runtime.Visual.Dxt;
using StbDxtSharp;

// Checks libwrathdxt against the game's own compressor (DXTCompressor.dll), run by
// monohost on the game's own Mono so both sides see the same JIT. Commands:
//   tables                  lookup and midpoint tables, bit for bit
//   fuzz <images> <seed>    synthetic images of many block kinds, all 4 modes, DXT1 + DXT5
//   game <images> <seed>    through Request.Compress$BurstManaged, the game's exact entry
//   refine <vectors>        stb__RefineBlock on crafted blocks (test/refine_vectors.c) that
//                           tell apart how Mono rounds Quantize's argument; needs the
//                           -DWRATHDXT_TEST build for the native side
//   bench                   2048x2048, the game's entry vs native
// Exit code 0 only if everything matched.
internal static unsafe class Program
{
    [DllImport("libSystem.dylib")] private static extern IntPtr dlopen(string path, int mode);
    [DllImport("libSystem.dylib")] private static extern IntPtr dlsym(IntPtr handle, string name);
    [DllImport("libSystem.dylib")] private static extern IntPtr dlerror();

    private static delegate* unmanaged[Cdecl]<int, int, byte*, byte*, int, int, int> _native;
    private static delegate* unmanaged[Cdecl]<byte*, float*, float*, void> _nativeTables;
    private static delegate* unmanaged[Cdecl]<byte*, ushort*, ushort*, uint, int> _nativeRefine;

    private delegate void GameCompress(int width, int height, byte* inData, byte* outData, bool hasAlpha);
    private static GameCompress _game;

    private static DxtContext _ctx;
    private static int _mismatches, _printed;
    private static long _blocks;
    private static readonly Dictionary<string, int> MismatchByCase = new Dictionary<string, int>();

    private static int Main(string[] args)
    {
        if (args.Length < 2) return Usage();
        var lib = dlopen(args[0], 2 /* RTLD_NOW */);
        if (lib == IntPtr.Zero)
        {
            Console.Error.WriteLine("dlopen: " + Marshal.PtrToStringAnsi(dlerror()));
            return 2;
        }
        _native = (delegate* unmanaged[Cdecl]<int, int, byte*, byte*, int, int, int>)dlsym(lib, "wrathdxt_compress");
        _nativeTables = (delegate* unmanaged[Cdecl]<byte*, float*, float*, void>)dlsym(lib, "wrathdxt_tables");
        _nativeRefine = (delegate* unmanaged[Cdecl]<byte*, ushort*, ushort*, uint, int>)dlsym(lib, "wrathdxt_test_refine");
        if (_native == null || _nativeTables == null) return Fail("missing exports");

        var method = typeof(DxtCompressorService.Request).GetMethod("Compress$BurstManaged", BindingFlags.Public | BindingFlags.Static);
        if (method == null) return Fail("Request.Compress$BurstManaged not found");
        _game = (GameCompress)Delegate.CreateDelegate(typeof(GameCompress), method);
        _ctx.Init();

        Console.WriteLine($"runtime: {Type.GetType("Mono.Runtime")?.GetMethod("GetDisplayName", BindingFlags.NonPublic | BindingFlags.Static)?.Invoke(null, null)}, " +
                          $"{IntPtr.Size * 8}-bit, DXTCompressor {typeof(StbDxt).Assembly.Location}");
        switch (args[1])
        {
            case "tables": return Tables();
            case "fuzz" when args.Length == 4: return Fuzz(int.Parse(args[2]), int.Parse(args[3]), viaGame: false);
            case "game" when args.Length == 4: return Fuzz(int.Parse(args[2]), int.Parse(args[3]), viaGame: true);
            case "refine" when args.Length == 3: return Refine(args[2]);
            case "bench": return Bench();
            default: return Usage();
        }
    }

    private static int Usage()
    {
        Console.Error.WriteLine("usage: Verify.exe <libwrathdxt.dylib> tables | fuzz <images> <seed> | game <images> <seed> | refine <vectors> | bench");
        return 2;
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine("FAIL: " + message);
        return 1;
    }

    private static int Tables()
    {
        var ctx = new byte[sizeof(DxtContext)];
        var m5 = new float[32];
        var m6 = new float[64];
        fixed (byte* c = ctx) fixed (float* p5 = m5) fixed (float* p6 = m6) _nativeTables(c, p5, p6);

        var bad = 0;
        var managedCtx = _ctx;
        var mc = (byte*)&managedCtx;
        if (ctx.Length != 1664) bad++;
        for (var i = 0; i < ctx.Length; i++)
            if (ctx[i] != mc[i]) { if (bad++ < 10) Console.WriteLine($"context byte {i}: native {ctx[i]} managed {mc[i]}"); }
        bad += CompareFloats("midpoints5", m5, StbDxt.midpoints5);
        bad += CompareFloats("midpoints6", m6, StbDxt.midpoints6);
        Console.WriteLine(bad == 0 ? $"tables: OK (context {ctx.Length} bytes, midpoints 32 + 64, bit-identical)" : $"tables: {bad} differences");
        return bad == 0 ? 0 : 1;
    }

    private static int CompareFloats(string name, float[] native, float[] managed)
    {
        var bad = 0;
        if (native.Length != managed.Length) return 1;
        for (var i = 0; i < native.Length; i++)
        {
            var a = native[i];
            var b = managed[i];
            if (*(uint*)&a != *(uint*)&b && bad++ < 10) Console.WriteLine($"{name}[{i}]: native {*(uint*)&a:x8} managed {*(uint*)&b:x8}");
        }
        return bad;
    }

    // Each line: mask, 16 RGB pixels, which endpoint/channel, the expected 5/6-bit value if
    // Mono rounds Quantize's argument to float, and if it keeps it in double.
    private static int Refine(string path)
    {
        int rounded = 0, unrounded = 0, neither = 0, nativeBad = 0, total = 0;
        var block = new byte[64];
        foreach (var line in System.IO.File.ReadAllLines(path))
        {
            var f = line.Split(' ');
            if (f.Length != 20) continue;
            total++;
            var mask = Convert.ToUInt32(f[0], 16);
            for (var i = 0; i < 16; i++)
            {
                var rgb = Convert.ToInt32(f[1 + i], 16);
                block[i * 4] = (byte)(rgb >> 16);
                block[i * 4 + 1] = (byte)(rgb >> 8);
                block[i * 4 + 2] = (byte)rgb;
                block[i * 4 + 3] = 255;
            }
            int expectRounded = int.Parse(f[18]), expectUnrounded = int.Parse(f[19]);
            ushort max = 0, min = 0, nmax = 0, nmin = 0;
            fixed (byte* b = block)
            {
                StbDxt.stb__RefineBlock(_ctx, b, &max, &min, mask);
                if (_nativeRefine != null) _nativeRefine(b, &nmax, &nmin, mask);
            }
            var v = f[17].StartsWith("max") ? max : min;
            var got = f[17][3] == 'r' ? v >> 11 : (v >> 5) & 63;
            if (got == expectRounded) rounded++;
            else if (got == expectUnrounded) unrounded++;
            else neither++;
            if (_nativeRefine != null && (nmax != max || nmin != min) && nativeBad++ < 10)
                Console.WriteLine($"MISMATCH refine {line}: managed {max:x4}/{min:x4} native {nmax:x4}/{nmin:x4}");
        }
        Console.WriteLine($"refine: {total} vectors; Mono rounded the argument in {rounded}, kept double in {unrounded}, neither {neither}; " +
                          (_nativeRefine == null ? "native not checked (no test build)" : $"native mismatched {nativeBad}"));
        return total > 0 && neither == 0 && nativeBad == 0 && (rounded == total || unrounded == total) ? 0 : 1;
    }

    // Block kinds, chosen per 4x4 block to reach every branch of the compressor.
    private static readonly string[] Kinds =
    {
        "noise", "two-color", "gradient", "solid", "near-solid", "low-variance", "gray",
        "extremes", "palette", "one-channel", "near-gradient",
    };

    private static void FillBlock(Random rng, byte[] img, int width, int bx, int by, int kind, int alphaKind)
    {
        var c0 = new[] { rng.Next(256), rng.Next(256), rng.Next(256) };
        var c1 = new[] { rng.Next(256), rng.Next(256), rng.Next(256) };
        var a0 = rng.Next(256);
        var a1 = rng.Next(256);
        var flip = rng.Next(16);
        for (var y = 0; y < 4; y++)
        for (var x = 0; x < 4; x++)
        {
            var p = ((by + y) * width + bx + x) * 4;
            var i = y * 4 + x;
            var t = i / 15f;
            for (var ch = 0; ch < 3; ch++)
            {
                int v;
                switch (kind)
                {
                    case 0: v = rng.Next(256); break;
                    case 1: v = rng.Next(2) == 0 ? c0[ch] : c1[ch]; break;
                    case 2: v = (int)(c0[ch] + (c1[ch] - c0[ch]) * t) + rng.Next(-6, 7); break;
                    case 3: v = c0[ch]; break;
                    case 4: v = c0[ch] + (i == flip ? rng.Next(-3, 4) : 0); break;
                    case 5: v = c0[ch] + rng.Next(-2, 3); break;
                    case 6: v = ch == 0 ? rng.Next(256) : img[p]; break;
                    case 7: v = rng.Next(2) * 255; break;
                    case 8: { var k = rng.Next(4); v = (c0[ch] * (3 - k) + c1[ch] * k) / 3; break; }
                    case 9: v = ch == flip % 3 ? rng.Next(256) : c0[ch]; break;
                    default: v = (int)(c0[ch] + (c1[ch] - c0[ch]) * t); break;
                }
                img[p + ch] = (byte)Math.Max(0, Math.Min(255, v));
            }
            int a;
            switch (alphaKind)
            {
                case 0: a = 255; break;
                case 1: a = rng.Next(256); break;
                case 2: a = rng.Next(2) == 0 ? a0 : a1; break;
                case 3: a = (int)(a0 + (a1 - a0) * t); break;
                case 4: a = a0; break;
                case 5: a = a0 + rng.Next(-4, 5); break;
                default: a = 0; break;
            }
            img[p + 3] = (byte)Math.Max(0, Math.Min(255, a));
        }
    }

    private static int Fuzz(int images, int seed, bool viaGame)
    {
        var rng = new Random(seed);
        var sw = Stopwatch.StartNew();
        for (var n = 0; n < images; n++)
        {
            // Mostly small images (many blocks per second), sometimes atlas-sized rows.
            var w = 4 * (1 + rng.Next(rng.Next(8) == 0 ? 128 : 16));
            var h = 4 * (1 + rng.Next(16));
            var img = new byte[w * h * 4];
            var kinds = new int[(w / 4) * (h / 4)];
            var bi = 0;
            for (var by = 0; by < h; by += 4)
            for (var bx = 0; bx < w; bx += 4)
            {
                var kind = rng.Next(Kinds.Length);
                kinds[bi++] = kind;
                FillBlock(rng, img, w, bx, by, kind, rng.Next(7));
            }
            if (viaGame)
            {
                // The game always uses mode None; hasAlpha picks DXT5 (atlases) or DXT1.
                Check(img, w, h, true, 0, kinds, true);
                Check(img, w, h, false, 0, kinds, true);
            }
            else
            {
                for (var mode = 0; mode < 4; mode++)
                {
                    Check(img, w, h, true, mode, kinds, false);
                    Check(img, w, h, false, mode, kinds, false);
                }
            }
        }
        Console.WriteLine($"{(viaGame ? "game" : "fuzz")} seed {seed}: {images} images, {_blocks} blocks compared, " +
                          $"{_mismatches} mismatched ({sw.Elapsed.TotalSeconds:0.0} s)");
        foreach (var kv in MismatchByCase) Console.WriteLine($"  {kv.Key}: {kv.Value}");
        return _mismatches == 0 ? 0 : 1;
    }

    private static void Check(byte[] img, int w, int h, bool alpha, int mode, int[] kinds, bool viaGame)
    {
        var blockBytes = alpha ? 16 : 8;
        var size = (w / 4) * (h / 4) * blockBytes;
        var managed = new byte[size];
        var native = new byte[size];
        fixed (byte* src = img) fixed (byte* m = managed) fixed (byte* nat = native)
        {
            if (viaGame) _game(w, h, src, m, alpha);
            else StbDxt.CompressDxt(_ctx, w, h, src, m, alpha, (CompressionMode)mode);
            var rc = _native(w, h, src, nat, alpha ? 1 : 0, mode);
            if (rc != 0) throw new Exception($"native returned {rc} for {w}x{h}");
        }
        var blocks = size / blockBytes;
        _blocks += blocks;
        for (var b = 0; b < blocks; b++)
        {
            var same = true;
            for (var k = 0; k < blockBytes; k++) same &= managed[b * blockBytes + k] == native[b * blockBytes + k];
            if (same) continue;
            _mismatches++;
            var key = $"{(alpha ? "dxt5" : "dxt1")} mode {mode} {Kinds[kinds[b]]}";
            MismatchByCase[key] = MismatchByCase.TryGetValue(key, out var c) ? c + 1 : 1;
            if (_printed++ >= 10) continue;
            int bx = b % (w / 4) * 4, by = b / (w / 4) * 4;
            var input = new System.Text.StringBuilder();
            for (var y = 0; y < 4; y++) input.Append(BitConverter.ToString(img, ((by + y) * w + bx) * 4, 16)).Append(' ');
            Console.WriteLine($"MISMATCH {key} block ({bx},{by}) of {w}x{h}\n  in      {input}\n" +
                              $"  managed {BitConverter.ToString(managed, b * blockBytes, blockBytes)}\n" +
                              $"  native  {BitConverter.ToString(native, b * blockBytes, blockBytes)}");
        }
    }

    // Atlas-like content: smooth regions with mild noise, some flat and some detailed.
    private static int Bench()
    {
        const int size = 2048;
        var rng = new Random(1);
        var img = new byte[size * size * 4];
        for (var by = 0; by < size; by += 4)
        for (var bx = 0; bx < size; bx += 4)
        {
            var r = rng.Next(10);
            FillBlock(rng, img, size, bx, by, r < 5 ? 2 : r < 7 ? 5 : r < 8 ? 3 : 0, r < 6 ? 0 : 3);
        }
        var bad = 0;
        foreach (var alpha in new[] { true, false })
        {
            var outSize = size / 4 * (size / 4) * (alpha ? 16 : 8);
            var managed = new byte[outSize];
            var native = new byte[outSize];
            double tm, tn;
            fixed (byte* src = img) fixed (byte* m = managed) fixed (byte* nat = native)
            {
                var sw = Stopwatch.StartNew();
                _game(size, size, src, m, alpha);
                tm = sw.Elapsed.TotalMilliseconds;
                sw.Restart();
                _native(size, size, src, nat, alpha ? 1 : 0, 0);
                tn = sw.Elapsed.TotalMilliseconds;
            }
            var same = true;
            for (var i = 0; i < outSize; i++) same &= managed[i] == native[i];
            if (!same) bad++;
            Console.WriteLine($"bench {size}x{size} {(alpha ? "DXT5" : "DXT1")}: game {tm:0} ms, native {tn:0.0} ms " +
                              $"({tm / tn:0.0}x), output {(same ? "identical" : "DIFFERENT")}");
        }
        return bad == 0 ? 0 : 1;
    }
}
