// Generates test vectors for `Verify.exe refine`: one 4x4 block + index mask for every
// input stb__RefineBlock can pass to Quantize5/6 where the result depends on whether Mono
// rounds the argument to float (a call) or keeps it in double (if it inlined the call).
// Random blocks almost never reach these, so fuzzing alone can't settle that question.
//
// Every possible input: Quantize gets (float)n * frb with frb = (1/85) / det, where det
// depends only on how many of the 16 pixels use each of the 4 indices.
//
// Output, one vector per line: mask, 16 RGB pixels (hex), endpoint+channel (maxr, ming,
// ...), the expected 5/6-bit value if the argument is rounded, and if it isn't.
#include <stdio.h>
#include "../wrathdxt.c" // the midpoint tables, exactly as shipped

static int q_rounded(float x, int n, const float *mid)
{
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    int q = (int)((double)x * n);
    return q + (x > mid[q]);
}

static int q_unrounded(double x, int n, const float *mid)
{
    x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    int q = (int)(x * n);
    return q + (x > (double)mid[q]);
}

// Per-index sums of one channel with at1 = 3*S0 + 2*S2 + S3 and S0+S1+S2+S3 = s.
static int solve(const int cnt[4], int at1, int s, int sums[4])
{
    for (int s0 = 0; s0 <= 255 * cnt[0]; s0++) {
        for (int s2 = 0; s2 <= 255 * cnt[2]; s2++) {
            int s3 = at1 - 3 * s0 - 2 * s2;
            if (s3 < 0) break;
            if (s3 > 255 * cnt[3]) continue;
            int s1 = s - s0 - s2 - s3;
            if (s1 < 0 || s1 > 255 * cnt[1]) continue;
            sums[0] = s0, sums[1] = s1, sums[2] = s2, sums[3] = s3;
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static char seen[1 << 16];
    int found = 0, missed = 0;
    for (int a = 0; a <= 16; a++)
    for (int b = 0; a + b <= 16; b++)
    for (int c = 0; a + b + c <= 16; c++) {
        int d = 16 - a - b - c, cnt[4] = {a, b, c, d};
        // w1/w2 per index: 0 (3,0), 1 (0,3), 2 (2,1), 3 (1,2); see prods[] in wrathdxt.c.
        int xx = 9 * a + 4 * c + d, yy = 9 * b + c + 4 * d, xy = 2 * c + 2 * d;
        int det = xx * yy - xy * xy;
        if (det <= 0 || seen[det]) continue;
        seen[det] = 1;
        float frb = 0.011764706f / (float)det;
        for (int n = -2; n <= 85 * det + 85; n++) { // x from just below 0 to just above 1
            double p = (double)(float)n * frb;      // exact
            for (int levels = 31; levels <= 63; levels += 32) {
                const float *mid = levels == 31 ? midpoints5 : midpoints6;
                int er = q_rounded((float)p, levels, mid), eu = q_unrounded(p, levels, mid);
                if (er == eu) continue;
                // max16: n = at1*yy - at2*xy; min16: n = at2*xx - at1*xy; at2 = 3S - at1.
                int sums[4], ok = 0, is_min = 0;
                for (int at1 = 0; at1 <= 3 * 255 * 16 && !ok; at1++) {
                    long m = (long)at1 * (yy + xy) - n; // = 3*S*xy
                    if (xy == 0) ok = m == 0 && solve(cnt, at1, at1 / 3, sums);
                    else ok = m >= 0 && m % (3L * xy) == 0 && solve(cnt, at1, (int)(m / (3L * xy)), sums);
                    if (!ok && xx > 0) {
                        long m2 = (long)n + (long)at1 * (xx + xy); // = 3*S*xx
                        ok = is_min = m2 >= 0 && m2 % (3L * xx) == 0 && solve(cnt, at1, (int)(m2 / (3L * xx)), sums);
                    }
                }
                if (!ok) {
                    missed++;
                    continue;
                }
                int idx[16], val[16], k = 0;
                for (int g = 0; g < 4; g++)
                    for (int i = 0; i < cnt[g]; i++, k++) {
                        idx[k] = g;
                        val[k] = sums[g] / cnt[g] + (i < sums[g] % cnt[g]);
                    }
                uint32_t mask = 0;
                for (int i = 0; i < 16; i++) mask |= (uint32_t)idx[i] << (2 * i);
                int green = levels == 63; // 5-bit test in red, 6-bit in green
                printf("%08x", mask);
                for (int i = 0; i < 16; i++) printf(" %02x%02x80", green ? 0x80 : val[i], green ? val[i] : 0x80);
                printf(" %s%c %d %d\n", is_min ? "min" : "max", green ? 'g' : 'r', er, eu);
                found++;
            }
        }
    }
    fprintf(stderr, "refine_vectors: %d vectors, %d inputs without a block\n", found, missed);
    return missed ? 1 : 0;
}
