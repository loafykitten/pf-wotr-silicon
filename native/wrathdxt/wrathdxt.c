// stb_dxt's DXT1/DXT5 compressor in C, matched byte for byte to the copy the game ships.
// Wrath compresses its character atlases with StbDxtSharp (Managed/DXTCompressor.dll), a
// C# port of stb_dxt.h v1.10. The arm64 build has no lib_burst_generated, so the game runs
// it as Mono-compiled C#; this is the same algorithm compiled for the CPU.
//
// Credits: stb_dxt.h by Fabian "ryg" Giesen and Sean Barrett (public domain or MIT);
// StbDxtSharp by Roman Shapiro (github.com/StbSharp/StbDxtSharp, public domain), whose
// float tables are reproduced exactly below. This follows the v1.10 port the game uses,
// not current upstream stb_dxt, which has changed since. The output must match the
// game's compressor byte for byte, so:
//   - floats follow Mono's rules, not C's. The game's Mono (Unity's 5.11, float32
//     optimization off) evaluates float expressions in double precision and rounds to
//     float only when a value is stored (a local, an array element) or converted. So a
//     multi-term float expression is computed in double here and rounded once on
//     assignment, and a float->int conversion truncates the unrounded double. Single
//     float operations give the same result either way and are left as plain float.
//     Strict float32 code mismatched about 1 block in 25,000 on noisy input;
//   - -ffp-contract=off: a fused multiply-add rounds differently. Never -ffast-math;
//   - integer arithmetic wraps like C# (-fwrapv), though nothing here should overflow;
//   - the float tables are the DLL's exact bit patterns (test/verify.sh tables checks).
// test/verify.sh checks it against the game's own DXTCompressor.dll on the game's Mono.

#include <stdint.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

// Bumped whenever the output could change; WrathPerf logs it.
#define WRATHDXT_VERSION 1

// DxtContext, initialised once (the C# rebuilds it per call; it's a pure function of
// nothing). Same layout as the C# struct, so the harness can compare it byte for byte.
typedef struct {
    uint8_t expand5[32];
    uint8_t expand6[64];
    uint8_t omatch5[512];
    uint8_t omatch6[512];
    uint8_t quant_rb[272];
    uint8_t quant_g[272];
} dxt_context;

static dxt_context ctx;

// StbDxt.midpoints5 / midpoints6, as stored in DXTCompressor.dll.
static const uint32_t midpoints5_bits[32] = {
    0x3c807fed, 0x3d40c0f0, 0x3da0a06f, 0x3de4e50c, 0x3e149492, 0x3e34b4d0, 0x3e54d4cc,
    0x3e76f6d7, 0x3e8c8c93, 0x3e9c9c91, 0x3eacacb0, 0x3ebdbdb6, 0x3ececedd, 0x3edededb,
    0x3eeeeefa, 0x3f000000, 0x3f088883, 0x3f109093, 0x3f189891, 0x3f212125, 0x3f29a9a8,
    0x3f31b1b8, 0x3f39b9b6, 0x3f42424a, 0x3f4acacd, 0x3f52d2cc, 0x3f5adadc, 0x3f63635e,
    0x3f6bebf2, 0x3f73f3f1, 0x3f7bfc01, 0x3f800000,
};
static const uint32_t midpoints6_bits[64] = {
    0x3c007fed, 0x3cc0bfe4, 0x3d20a0f5, 0x3d60e0eb, 0x3d909071, 0x3db0b0f2, 0x3dd0d0ee,
    0x3df0f0e9, 0x3e088872, 0x3e1898b3, 0x3e28a8b1, 0x3e38b8ae, 0x3e48c8ac, 0x3e58d8ed,
    0x3e68e8ea, 0x3e7afaf8, 0x3e868683, 0x3e8e8e82, 0x3e9696a2, 0x3e9e9ea1, 0x3ea6a6a0,
    0x3eaeae9f, 0x3eb6b6bf, 0x3ebebebe, 0x3ec6c6bd, 0x3ececedd, 0x3ed6d6dc, 0x3edededb,
    0x3ee6e6da, 0x3eeeeefa, 0x3ef6f6f9, 0x3f000000, 0x3f048484, 0x3f088883, 0x3f0c8c93,
    0x3f109093, 0x3f149492, 0x3f189891, 0x3f1c9ca2, 0x3f20a0a1, 0x3f24a4a0, 0x3f28a8b1,
    0x3f2cacb0, 0x3f30b0af, 0x3f34b4af, 0x3f38b8bf, 0x3f3cbcbe, 0x3f414142, 0x3f45c5c5,
    0x3f49c9c5, 0x3f4dcdd5, 0x3f51d1d4, 0x3f55d5d4, 0x3f59d9d3, 0x3f5ddde3, 0x3f61e1e3,
    0x3f65e5e2, 0x3f69e9e2, 0x3f6dedf2, 0x3f71f1f1, 0x3f75f5f1, 0x3f79fa01, 0x3f7dfe00,
    0x3f800000,
};

static float midpoints5[32], midpoints6[64];

static const int w1tab[4] = {3, 0, 2, 1};
static const int prods[4] = {589824, 2304, 262402, 66562};

enum { MODE_DITHER = 1, MODE_HIGHQUAL = 2 };

static int mul8bit(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void from16bit(uint8_t *out, uint16_t v)
{
    int rv = (v & 0xf800) >> 11;
    int gv = (v & 0x07e0) >> 5;
    int bv = v & 0x001f;
    out[0] = ctx.expand5[rv];
    out[1] = ctx.expand6[gv];
    out[2] = ctx.expand5[bv];
    out[3] = 0;
}

static uint16_t as16bit(int r, int g, int b)
{
    return (uint16_t)((mul8bit(r, 31) << 11) + (mul8bit(g, 63) << 5) + mul8bit(b, 31));
}

static int lerp13(int a, int b) { return (2 * a + b) / 3; }

static void lerp13rgb(uint8_t *out, const uint8_t *p1, const uint8_t *p2)
{
    out[0] = (uint8_t)lerp13(p1[0], p2[0]);
    out[1] = (uint8_t)lerp13(p1[1], p2[1]);
    out[2] = (uint8_t)lerp13(p1[2], p2[2]);
}

static void prepare_opt_table(uint8_t *table, const uint8_t *expand, int size)
{
    for (int i = 0; i < 256; i++) {
        int best_err = 256;
        for (int mn = 0; mn < size; mn++) {
            for (int mx = 0; mx < size; mx++) {
                int mine = expand[mn];
                int maxe = expand[mx];
                int err = iabs(lerp13(maxe, mine) - i);
                err += iabs(maxe - mine) * 3 / 100;
                if (err < best_err) {
                    table[i * 2] = (uint8_t)mx;
                    table[i * 2 + 1] = (uint8_t)mn;
                    best_err = err;
                }
            }
        }
    }
}

// DxtContext.Init().
__attribute__((constructor)) static void init_tables(void)
{
    for (int i = 0; i < 32; i++) ctx.expand5[i] = (uint8_t)((i << 3) | (i >> 2));
    for (int i = 0; i < 64; i++) ctx.expand6[i] = (uint8_t)((i << 2) | (i >> 4));
    for (int i = 0; i < 272; i++) {
        int v = i - 8 >= 0 ? (i - 8 > 255 ? 255 : i - 8) : 0;
        ctx.quant_rb[i] = ctx.expand5[mul8bit(v, 31)];
        ctx.quant_g[i] = ctx.expand6[mul8bit(v, 63)];
    }
    prepare_opt_table(ctx.omatch5, ctx.expand5, 32);
    prepare_opt_table(ctx.omatch6, ctx.expand6, 64);
    memcpy(midpoints5, midpoints5_bits, sizeof midpoints5);
    memcpy(midpoints6, midpoints6_bits, sizeof midpoints6);
}

static void eval_colors(uint8_t *color, uint16_t c0, uint16_t c1)
{
    from16bit(color, c0);
    from16bit(color + 4, c1);
    lerp13rgb(color + 8, color, color + 4);
    lerp13rgb(color + 12, color + 4, color);
}

static uint32_t match_colors_block(const uint8_t *block, const uint8_t *color, int dither)
{
    uint32_t mask = 0;
    int dirr = color[0] - color[4];
    int dirg = color[1] - color[5];
    int dirb = color[2] - color[6];
    int dots[16], stops[4];

    for (int i = 0; i < 16; i++)
        dots[i] = block[i * 4] * dirr + block[i * 4 + 1] * dirg + block[i * 4 + 2] * dirb;
    for (int i = 0; i < 4; i++)
        stops[i] = color[i * 4] * dirr + color[i * 4 + 1] * dirg + color[i * 4 + 2] * dirb;

    int c0point = stops[1] + stops[3];
    int halfpoint = stops[3] + stops[2];
    int c3point = stops[2] + stops[0];

    if (!dither) {
        for (int i = 15; i >= 0; i--) {
            int dot = dots[i] * 2;
            mask <<= 2;
            if (dot < halfpoint)
                mask |= dot < c0point ? 1 : 3;
            else
                mask |= dot < c3point ? 2 : 0;
        }
    } else {
        int err[8];
        int *ep1 = err, *ep2 = err + 4;
        const int *dp = dots;
        c0point <<= 3;
        halfpoint <<= 3;
        c3point <<= 3;
        for (int i = 0; i < 8; i++) err[i] = 0;
        for (int y = 0; y < 4; y++) {
            int dot, lmask, step;
            dot = (dp[0] << 4) + 3 * ep2[1] + 5 * ep2[0];
            step = dot < halfpoint ? (dot < c0point ? 1 : 3) : (dot < c3point ? 2 : 0);
            ep1[0] = dp[0] - stops[step];
            lmask = step;

            dot = (dp[1] << 4) + 7 * ep1[0] + 3 * ep2[2] + 5 * ep2[1] + ep2[0];
            step = dot < halfpoint ? (dot < c0point ? 1 : 3) : (dot < c3point ? 2 : 0);
            ep1[1] = dp[1] - stops[step];
            lmask |= step << 2;

            dot = (dp[2] << 4) + 7 * ep1[1] + 3 * ep2[3] + 5 * ep2[2] + ep2[1];
            step = dot < halfpoint ? (dot < c0point ? 1 : 3) : (dot < c3point ? 2 : 0);
            ep1[2] = dp[2] - stops[step];
            lmask |= step << 4;

            dot = (dp[3] << 4) + 7 * ep1[2] + 5 * ep2[3] + ep2[2];
            step = dot < halfpoint ? (dot < c0point ? 1 : 3) : (dot < c3point ? 2 : 0);
            ep1[3] = dp[3] - stops[step];
            lmask |= step << 6;

            dp += 4;
            mask |= (uint32_t)(lmask << (y * 8));
            int *t = ep1;
            ep1 = ep2;
            ep2 = t;
        }
    }
    return mask;
}

static void optimize_colors_block(const uint8_t *block, uint16_t *pmax16, uint16_t *pmin16)
{
    int mind = 0x7fffffff, maxd = -0x7fffffff;
    const uint8_t *minp = NULL, *maxp = NULL;
    double magn;
    int v_r, v_g, v_b;
    float covf[6], vfr, vfg, vfb;
    int cov[6];
    int mu[3], min[3], max[3];

    for (int ch = 0; ch < 3; ch++) {
        const uint8_t *bp = block + ch;
        int muv, minv, maxv;
        muv = minv = maxv = bp[0];
        for (int i = 4; i < 64; i += 4) {
            muv += bp[i];
            if (bp[i] < minv)
                minv = bp[i];
            else if (bp[i] > maxv)
                maxv = bp[i];
        }
        mu[ch] = (muv + 8) >> 4;
        min[ch] = minv;
        max[ch] = maxv;
    }

    for (int i = 0; i < 6; i++) cov[i] = 0;
    for (int i = 0; i < 16; i++) {
        int r = block[i * 4] - mu[0];
        int g = block[i * 4 + 1] - mu[1];
        int b = block[i * 4 + 2] - mu[2];
        cov[0] += r * r;
        cov[1] += r * g;
        cov[2] += r * b;
        cov[3] += g * g;
        cov[4] += g * b;
        cov[5] += b * b;
    }

    for (int i = 0; i < 6; i++) covf[i] = (float)cov[i] / 255.0f;

    vfr = (float)(max[0] - min[0]);
    vfg = (float)(max[1] - min[1]);
    vfb = (float)(max[2] - min[2]);

    for (int iter = 0; iter < 4; iter++) { // StbDxt.nIterPower
        // Mono: three products and two sums in double, rounded once on assignment.
        float r = (float)((double)vfr * covf[0] + (double)vfg * covf[1] + (double)vfb * covf[2]);
        float g = (float)((double)vfr * covf[1] + (double)vfg * covf[3] + (double)vfb * covf[4]);
        float b = (float)((double)vfr * covf[2] + (double)vfg * covf[4] + (double)vfb * covf[5]);
        vfr = r;
        vfg = g;
        vfb = b;
    }

    // CRuntime.fabs(double) returns float, then widens to double.
    magn = (double)__builtin_fabsf(vfr);
    if ((double)__builtin_fabsf(vfg) > magn) magn = (double)__builtin_fabsf(vfg);
    if ((double)__builtin_fabsf(vfb) > magn) magn = (double)__builtin_fabsf(vfb);

    if (magn < 4.0) {
        v_r = 299;
        v_g = 587;
        v_b = 114;
    } else {
        magn = 512.0 / magn;
        v_r = (int)((double)vfr * magn);
        v_g = (int)((double)vfg * magn);
        v_b = (int)((double)vfb * magn);
    }

    for (int i = 0; i < 16; i++) {
        int dot = block[i * 4] * v_r + block[i * 4 + 1] * v_g + block[i * 4 + 2] * v_b;
        if (dot < mind) {
            mind = dot;
            minp = block + i * 4;
        }
        if (dot > maxd) {
            maxd = dot;
            maxp = block + i * 4;
        }
    }

    *pmax16 = as16bit(maxp[0], maxp[1], maxp[2]);
    *pmin16 = as16bit(minp[0], minp[1], minp[2]);
}

// Mono: x * 31f stays in double (exact), and the ushort conversion truncates that, so
// a product just below an integer isn't rounded up to it first.
static uint16_t quantize5(float x)
{
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    uint16_t q = (uint16_t)((double)x * 31.0f);
    return (uint16_t)(q + (x > midpoints5[q] ? 1 : 0));
}

static uint16_t quantize6(float x)
{
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    uint16_t q = (uint16_t)((double)x * 63.0f);
    return (uint16_t)(q + (x > midpoints6[q] ? 1 : 0));
}

static int refine_block(const uint8_t *block, uint16_t *pmax16, uint16_t *pmin16, uint32_t mask)
{
    float frb;
    uint16_t oldmin = *pmin16, oldmax = *pmax16, min16, max16;
    int akku = 0, xx, xy, yy;
    int at1_r = 0, at1_g = 0, at1_b = 0;
    int at2_r = 0, at2_g = 0, at2_b = 0;
    uint32_t cm = mask;

    if ((mask ^ (mask << 2)) < 4) { // all pixels have the same index
        int r = 8, g = 8, b = 8;
        for (int i = 0; i < 16; i++) {
            r += block[i * 4];
            g += block[i * 4 + 1];
            b += block[i * 4 + 2];
        }
        r >>= 4;
        g >>= 4;
        b >>= 4;
        max16 = (uint16_t)((ctx.omatch5[r * 2] << 11) | (ctx.omatch6[g * 2] << 5) | ctx.omatch5[b * 2]);
        min16 = (uint16_t)((ctx.omatch5[r * 2 + 1] << 11) | (ctx.omatch6[g * 2 + 1] << 5) | ctx.omatch5[b * 2 + 1]);
    } else {
        for (int i = 0; i < 16; i++, cm >>= 2) {
            int step = (int)(cm & 3);
            int w1 = w1tab[step];
            int r = block[i * 4];
            int g = block[i * 4 + 1];
            int b = block[i * 4 + 2];
            akku += prods[step];
            at1_r += w1 * r;
            at1_g += w1 * g;
            at1_b += w1 * b;
            at2_r += r;
            at2_g += g;
            at2_b += b;
        }
        at2_r = 3 * at2_r - at1_r;
        at2_g = 3 * at2_g - at1_g;
        at2_b = 3 * at2_b - at1_b;

        xx = akku >> 16;
        yy = (akku >> 8) & 0xff;
        xy = akku & 0xff;

        // The C# compiler folds 1f / 85f into this constant.
        frb = 0.011764706f / (float)(xx * yy - xy * xy);

        max16 = (uint16_t)(quantize5((float)(at1_r * yy - at2_r * xy) * frb) << 11);
        max16 |= (uint16_t)(quantize6((float)(at1_g * yy - at2_g * xy) * frb) << 5);
        max16 |= quantize5((float)(at1_b * yy - at2_b * xy) * frb);

        min16 = (uint16_t)(quantize5((float)(at2_r * xx - at1_r * xy) * frb) << 11);
        min16 |= (uint16_t)(quantize6((float)(at2_g * xx - at1_g * xy) * frb) << 5);
        min16 |= quantize5((float)(at2_b * xx - at1_b * xy) * frb);
    }

    *pmin16 = min16;
    *pmax16 = max16;
    return oldmin != min16 || oldmax != max16;
}

static void dither_block(uint8_t *dest, const uint8_t *block)
{
    int err[8], *ep1 = err, *ep2 = err + 4;
    for (int ch = 0; ch < 3; ch++) {
        const uint8_t *bp = block + ch;
        uint8_t *dp = dest + ch;
        const uint8_t *quant = ch == 1 ? ctx.quant_g : ctx.quant_rb;
        memset(err, 0, sizeof err);
        for (int y = 0; y < 4; y++) {
            dp[0] = quant[bp[0] + ((3 * ep2[1] + 5 * ep2[0]) >> 4)];
            ep1[0] = bp[0] - dp[0];
            dp[4] = quant[bp[4] + ((7 * ep1[0] + 3 * ep2[2] + 5 * ep2[1] + ep2[0]) >> 4)];
            ep1[1] = bp[4] - dp[4];
            dp[8] = quant[bp[8] + ((7 * ep1[1] + 3 * ep2[3] + 5 * ep2[2] + ep2[1]) >> 4)];
            ep1[2] = bp[8] - dp[8];
            dp[12] = quant[bp[12] + ((7 * ep1[2] + 5 * ep2[3] + ep2[2]) >> 4)];
            ep1[3] = bp[12] - dp[12];
            bp += 16;
            dp += 16;
            int *t = ep1;
            ep1 = ep2;
            ep2 = t;
        }
    }
}

static void compress_color_block(uint8_t *dest, const uint8_t *block, int mode)
{
    uint32_t mask;
    int i;
    uint16_t max16, min16;
    uint8_t dblock[64], color[16];
    int dither = mode & MODE_DITHER;
    int refinecount = (mode & MODE_HIGHQUAL) ? 2 : 1;

    // Same 32-bit compare as the C# ((uint*)block)[i]; memcpy avoids aliasing UB.
    uint32_t first, cur;
    memcpy(&first, block, 4);
    for (i = 1; i < 16; i++) {
        memcpy(&cur, block + i * 4, 4);
        if (cur != first) break;
    }

    if (i == 16) { // constant color
        int r = block[0], g = block[1], b = block[2];
        mask = 0xaaaaaaaa;
        max16 = (uint16_t)((ctx.omatch5[r * 2] << 11) | (ctx.omatch6[g * 2] << 5) | ctx.omatch5[b * 2]);
        min16 = (uint16_t)((ctx.omatch5[r * 2 + 1] << 11) | (ctx.omatch6[g * 2 + 1] << 5) | ctx.omatch5[b * 2 + 1]);
    } else {
        if (dither) dither_block(dblock, block);
        optimize_colors_block(dither ? dblock : block, &max16, &min16);
        if (max16 != min16) {
            eval_colors(color, max16, min16);
            mask = match_colors_block(block, color, dither);
        } else {
            mask = 0;
        }
        for (i = 0; i < refinecount; i++) {
            uint32_t lastmask = mask;
            if (refine_block(dither ? dblock : block, &max16, &min16, mask)) {
                if (max16 == min16) {
                    mask = 0;
                    break;
                }
                eval_colors(color, max16, min16);
                mask = match_colors_block(block, color, dither);
            }
            if (mask == lastmask) break;
        }
    }

    if (max16 < min16) {
        uint16_t t = min16;
        min16 = max16;
        max16 = t;
        mask ^= 0x55555555;
    }

    dest[0] = (uint8_t)max16;
    dest[1] = (uint8_t)(max16 >> 8);
    dest[2] = (uint8_t)min16;
    dest[3] = (uint8_t)(min16 >> 8);
    dest[4] = (uint8_t)mask;
    dest[5] = (uint8_t)(mask >> 8);
    dest[6] = (uint8_t)(mask >> 16);
    dest[7] = (uint8_t)(mask >> 24);
}

static void compress_alpha_block(uint8_t *dest, const uint8_t *src, int stride)
{
    int mn, mx, dist, bias, dist4, dist2, bits, mask;

    mn = mx = src[0];
    for (int i = 1; i < 16; i++) {
        if (src[i * stride] < mn)
            mn = src[i * stride];
        else if (src[i * stride] > mx)
            mx = src[i * stride];
    }

    dest[0] = (uint8_t)mx;
    dest[1] = (uint8_t)mn;
    dest += 2;

    dist = mx - mn;
    dist4 = dist * 4;
    dist2 = dist * 2;
    bias = dist < 8 ? dist - 1 : dist / 2 + 2;
    bias -= mn * 7;
    bits = 0;
    mask = 0;

    for (int i = 0; i < 16; i++) {
        int a = src[i * stride] * 7 + bias;
        int ind, t;
        t = a >= dist4 ? -1 : 0;
        ind = t & 4;
        a -= dist4 & t;
        t = a >= dist2 ? -1 : 0;
        ind += t & 2;
        a -= dist2 & t;
        ind += a >= dist;
        ind = -ind & 7;
        ind ^= 2 > ind;
        mask |= ind << bits;
        if ((bits += 3) >= 8) {
            *dest++ = (uint8_t)mask;
            mask >>= 8;
            bits -= 8;
        }
    }
}

static void compress_dxt_block(uint8_t *dest, const uint8_t *src, int alpha, int mode)
{
    uint8_t data[64];
    if (alpha) {
        compress_alpha_block(dest, src + 3, 4);
        dest += 8;
        memcpy(data, src, 64);
        for (int i = 0; i < 16; i++) data[i * 4 + 3] = 255;
        src = data;
    }
    compress_color_block(dest, src, mode);
}

// StbDxt.CompressDxt: RGBA32 in, DXT1 (8 bytes per 4x4 block) or DXT5 (16) out. Returns
// 0, or 1 (Error.Value.ERROR_DIMENSIONS) if width or height isn't a multiple of 4.
EXPORT int wrathdxt_compress(int width, int height, const uint8_t *data, uint8_t *dest, int has_alpha, int mode)
{
    if (width % 4 != 0 || height % 4 != 0) return 1;
    uint8_t block[64];
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            for (int row = 0; row < 4 && y + row < height; row++)
                memcpy(block + row * 16, data + (long)width * 4 * (y + row) + (long)x * 4, 16);
            compress_dxt_block(dest, block, has_alpha ? 1 : 0, mode);
            dest += has_alpha ? 16 : 8;
        }
    }
    return 0;
}

// For WrathPerf's startup check and the test harness: the lookup tables in the C#
// DxtContext layout (1664 bytes) and the two midpoint tables.
EXPORT void wrathdxt_tables(uint8_t *context_out, float *midpoints5_out, float *midpoints6_out)
{
    memcpy(context_out, &ctx, sizeof ctx);
    memcpy(midpoints5_out, midpoints5, sizeof midpoints5);
    memcpy(midpoints6_out, midpoints6, sizeof midpoints6);
}

EXPORT int wrathdxt_version(void) { return WRATHDXT_VERSION; }

#ifdef WRATHDXT_TEST
// test/verify.sh builds a copy with this, for `Verify.exe refine`.
EXPORT int wrathdxt_test_refine(const uint8_t *block, uint16_t *pmax16, uint16_t *pmin16, uint32_t mask)
{
    return refine_block(block, pmax16, pmin16, mask);
}
#endif

_Static_assert(sizeof(dxt_context) == 1664, "matches StbDxtSharp.DxtContext");
