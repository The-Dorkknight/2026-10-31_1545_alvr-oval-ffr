// Property/fuzz test for the portable core. Deterministic (fixed seed) so failures reproduce.
// Invariants checked on every random case:
//   * mask codes are only 1x1 / 2x2 / 4x4, and never 4x4 when the GPU or the config forbids it
//   * mask size is exactly ceil(w/tile) x ceil(h/tile)
//   * saving is within [0, 15/16]; a disabled config saves nothing
//   * growing the sharp area never increases the saving (monotonic)
//   * mirroring the centre mirrors the mask (when the width is a whole number of tiles)
//   * NaN / inf / garbage anywhere never escapes: the result is always finite and in range
#include "ovalffr_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace ovalffr;

static unsigned long long rng_state = 0x9E3779B97F4A7C15ULL;
static unsigned long long rnd() {   // xorshift64*
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}
static double unit() { return (double)(rnd() >> 11) / 9007199254740992.0; }
static double range(double lo, double hi) { return lo + unit() * (hi - lo); }

static int failures = 0;
#define CHECK(cond, ...)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            if (failures < 15) { std::printf("FAIL line %d: %s  ", __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static float weird(float normal) {
    switch (rnd() % 12) {
        case 0: return std::numeric_limits<float>::quiet_NaN();
        case 1: return std::numeric_limits<float>::infinity();
        case 2: return -std::numeric_limits<float>::infinity();
        case 3: return 0.0f;
        case 4: return -1.0f;
        case 5: return 1e30f;
        default: return normal;
    }
}

int main() {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        Config c;
        c.enabled = (rnd() % 10) != 0;
        c.h = weird((float)range(0.0, 1.5)); c.v = weird((float)range(0.0, 1.5));
        c.shape = weird((float)range(1.0, 8.0)); c.band = weird((float)range(-0.5, 2.5));
        c.outer = (int)(rnd() % 6) - 1;
        int w = (int)(rnd() % 1500) - 20, h = (int)(rnd() % 1500) - 20, tile = (int)(rnd() % 40) - 3;   // big frames: tested once below
        float cx = weird((float)range(-0.2, 1.2)), cy = weird((float)range(-0.2, 1.2));
        int max_log2 = 1 + (int)(rnd() % 2);

        Config k = clamp(c);
        CHECK(std::isfinite(k.h) && k.h >= 0.20f && k.h <= 1.0f, "h=%g", k.h);
        CHECK(std::isfinite(k.v) && k.v >= 0.20f && k.v <= 1.0f, "v=%g", k.v);
        CHECK(std::isfinite(k.shape) && k.shape >= 2.0f && k.shape <= 6.0f, "shape=%g", k.shape);
        CHECK(std::isfinite(k.band) && k.band >= 0.0f && k.band <= 2.0f, "band=%g", k.band);
        CHECK(k.outer == 1 || k.outer == 2, "outer=%d", k.outer);

        int tw = -1, th = -1;
        auto m = make_mask(c, w, h, tile, cx, cy, max_log2, &tw, &th);
        int t = tile < 1 ? 8 : tile;
        int ww = w < 0 ? 0 : w, hh = h < 0 ? 0 : h;
        CHECK(tw == (ww + t - 1) / t && th == (hh + t - 1) / t, "size %dx%d tile %d -> %dx%d", ww, hh, t, tw, th);
        CHECK(m.size() == (size_t)tw * th, "mask vector size");
        bool any4 = false;
        for (unsigned char code : m) {
            CHECK(code == 0 || code == 5 || code == 10, "bad code %d", code);
            any4 |= (code == 10);
        }
        if (max_log2 == 1 || k.outer == 1) CHECK(!any4, "4x4 emitted although forbidden");
        double sv = estimated_saving(m);
        CHECK(sv >= 0.0 && sv <= 0.9375 + 1e-9, "saving %g", sv);
        if (!c.enabled) CHECK(sv == 0.0, "disabled config saved %g", sv);

        // monotonic in h and v (same centre, same everything else)
        if (c.enabled && w > 16 && h > 16) {
            Config a = k, b = k;
            b.h = std::fmin(1.0f, a.h + 0.1f); b.v = std::fmin(1.0f, a.v + 0.1f);
            int tw2, th2;
            double s1 = estimated_saving(make_mask(a, w, h, t, 0.5f, 0.5f, max_log2, &tw2, &th2));
            double s2 = estimated_saving(make_mask(b, w, h, t, 0.5f, 0.5f, max_log2, &tw2, &th2));
            CHECK(s2 <= s1 + 1e-12, "bigger sharp area saved more: %g -> %g", s1, s2);
        }
        // mirror symmetry when the width is a whole number of tiles
        if (c.enabled && w > 0 && h > 0 && w % t == 0 && std::isfinite(cx)) {
            int ta, tb, tc, td;
            float fx = (float)range(0.3, 0.7);
            auto m1 = make_mask(k, w, h, t, fx, 0.5f, max_log2, &ta, &tb);
            auto m2 = make_mask(k, w, h, t, 1.0f - fx, 0.5f, max_log2, &tc, &td);
            int diff = 0;
            for (int y = 0; y < tb; ++y) for (int x = 0; x < ta; ++x) diff += m1[(size_t)y * ta + x] != m2[(size_t)y * ta + (ta - 1 - x)];
            // float rounding may flip a tile exactly on a boundary; allow a tiny fraction
            CHECK(diff <= (int)(ta * tb) / 400 + 2, "mirror differs in %d of %d tiles", diff, ta * tb);
        }
    }

    // one realistic big frame at the finest tile size, and an over-limit one that must be refused
    { int a, b; auto g = make_mask(Config(), 4000, 4000, 1, 0.5f, 0.5f, 2, &a, &b);
      CHECK(g.size() == 16000000u && a == 4000 && b == 4000, "4000x4000 tile 1"); }
    { int a, b; auto g = make_mask(Config(), 4200, 4200, 1, 0.5f, 0.5f, 2, &a, &b);
      CHECK(g.empty() && a == 0 && b == 0, "over-limit mask was built"); }
    // absurd sizes are refused, not allocated
    int tw = -1, th = -1;
    auto big = make_mask(Config(), 2000000000, 2000000000, 1, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(big.empty() && tw == 0 && th == 0, "huge size was not refused");

    // garbage config text never crashes or escapes the valid range
    const char* junk[] = {"", "\n\n", "h=", "=5", "h=nan\nv=inf", "h=1e999\nv=-1e999", "h=abc", "outer=999999999999",
                          "enabled=maybe", "\xff\xfe\x00h=0.5", "h=0.5\r\nv=0.4\r\n", "# only a comment"};
    for (const char* j : junk) {
        Config c = parse_config(j);
        CHECK(std::isfinite(c.h) && c.h >= 0.2f && c.h <= 1.0f && std::isfinite(c.v) && c.v >= 0.2f && c.v <= 1.0f,
              "junk config produced h=%g v=%g", c.h, c.v);
    }
    for (int i = 0; i < 2000; ++i) {   // random bytes
        std::string s;
        int len = (int)(rnd() % 120);
        for (int k = 0; k < len; ++k) s.push_back((char)(rnd() % 256));
        Config c = parse_config(s);
        CHECK(std::isfinite(c.h) && std::isfinite(c.v) && std::isfinite(c.shape) && std::isfinite(c.band), "random bytes escaped");
    }

    // FOV helper: never NaN, always inside 0.2..0.8
    for (int i = 0; i < 5000; ++i) {
        float cx, cy;
        centre_from_fov(weird((float)range(-1.5, 0.2)), weird((float)range(-0.2, 1.5)), weird((float)range(-0.2, 1.5)),
                        weird((float)range(-1.5, 0.2)), &cx, &cy);
        CHECK(std::isfinite(cx) && std::isfinite(cy) && cx >= 0.2f && cx <= 0.8f && cy >= 0.2f && cy <= 0.8f,
              "centre %g %g", cx, cy);
    }

    if (failures) { std::printf("fuzz_test: %d failure(s)\n", failures); return 1; }
    std::printf("fuzz_test: %d random cases + junk configs + FOV cases passed\n", N);
    return 0;
}
