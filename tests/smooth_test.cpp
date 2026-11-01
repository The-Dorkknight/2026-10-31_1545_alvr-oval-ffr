// Tests for "Smooth periphery" (the C++ copy of layer/shaders/smooth.hlsl).
// What this proves: the maths. What it cannot prove: that the HLSL behaves the same on a real GPU,
// or that it looks good in a headset.
#include "ovalffr_core.h"
#include "ovalffr_smooth.h"
#include "vrs_sim.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ovalffr;

static int failures = 0;
#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        if (!(cond)) { std::printf("FAIL line %d: %s  ", __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } \
    } while (0)

static unsigned long long rs = 12345;
static double rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs % 1000000) / 1000000.0; }

// A smooth, detailed test scene: soft gradients plus medium-frequency waves (like a lit wall).
struct Waves : vrs_sim::Scene {
    int surface(float, float) const override { return 0; }
    RGBA shade(int, float u, float v) const override {
        return {0.5f + 0.35f * std::sin(u * 23.0f + v * 7.0f), 0.4f + 0.3f * std::sin(v * 19.0f - u * 5.0f),
                0.3f + 0.25f * std::cos((u + v) * 15.0f), 1.0f};
    }
};
// Black and white squares: every square is its own object, so the edges between them are hard.
struct Squares : vrs_sim::Scene {
    int w, h;
    Squares(int W, int H) : w(W), h(H) {}
    int surface(float u, float v) const override { return (((int)(u * w) / 32 + (int)(v * h) / 32) & 1); }
    RGBA shade(int s, float, float) const override { float x = s ? 1.0f : 0.02f; return {x, x, x, 1.0f}; }
};
// A lit ball in front of a wavy wall: outlines are full resolution, shading is coarse.
struct Ball : vrs_sim::Scene {
    int surface(float u, float v) const override { float dx = u - 0.2f, dy = v - 0.3f; return dx * dx + dy * dy < 0.01f; }
    RGBA shade(int s, float u, float v) const override {
        if (s) { float l = 0.4f + 0.6f * std::fmax(0.0f, 1.0f - ((u - 0.17f) * (u - 0.17f) + (v - 0.27f) * (v - 0.27f)) / 0.02f);
                 return {0.95f * l, 0.55f * l, 0.2f * l, 1.0f}; }
        return {0.3f + 0.1f * std::sin(u * 40.0f), 0.4f, 0.7f - 0.3f * v, 1.0f};
    }
};

static double coarse_error(const SmoothParams& p, const std::vector<RGBA>& a, const std::vector<RGBA>& ref) {
    double e = 0; long n = 0;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            if (smooth_rate_log2(p, x, y) == 0) continue;
            const RGBA &q = a[(size_t)y * p.width + x], &r = ref[(size_t)y * p.width + x];
            e += std::fabs(q.r - r.r) + std::fabs(q.g - r.g) + std::fabs(q.b - r.b); ++n;
        }
    return n ? e / n : 0.0;
}

// true when `out` is `in` with the purple "not applied" tint blended over it
static bool is_purple(const RGBA& out, const RGBA& in) {
    auto bl = [](float c, float d) { return c * 0.35f + d * 0.65f; };
    return std::fabs(out.r - bl(0.70f, in.r)) < 1e-4f && std::fabs(out.g - bl(0.20f, in.g)) < 1e-4f &&
           std::fabs(out.b - bl(0.95f, in.b)) < 1e-4f;
}

int main() {
    // 1. The shader's per-pixel rate equals the shading-rate mask the layer uploads.
    {
        int bad = 0, cases = 0;
        for (int i = 0; i < 300; ++i) {
            Config c;
            c.h = (float)(0.2 + 0.8 * rnd()); c.v = (float)(0.2 + 0.8 * rnd());
            c.shape = (float)(2.0 + 4.0 * rnd()); c.band = (float)(2.0 * rnd()); c.outer = 1 + (int)(rnd() * 2);
            int w = 200 + (int)(rnd() * 900), h = 200 + (int)(rnd() * 900);
            int tile = (int[]){8, 16, 32}[i % 3];
            int maxl = 1 + (i % 2);
            float cx = (float)(0.3 + 0.4 * rnd()), cy = (float)(0.3 + 0.4 * rnd());
            int tw, th;
            auto m = make_mask(c, w, h, tile, cx, cy, maxl, &tw, &th);
            SmoothParams p = smooth_params(c, w, h, tile, cx, cy, maxl);
            for (int k = 0; k < 400; ++k) {
                int x = (int)(rnd() * w), y = (int)(rnd() * h);
                uint8_t code = m[(size_t)(y / tile) * tw + (x / tile)];
                int want = code == RATE_1X1 ? 0 : (code == RATE_2X2 ? 1 : 2);
                bad += smooth_rate_log2(p, x, y) != want;
                ++cases;
            }
        }
        CHECK(bad == 0, "%d of %d pixels disagree with make_mask", bad, cases);
    }

    const int W = 640, H = 480;
    Config c; c.h = 0.5f; c.v = 0.5f; c.band = 0.4f; c.outer = 2; c.smooth = true; c.smooth_strength = 1.0f;
    SmoothParams p = smooth_params(c, W, H, 16, 0.5f, 0.5f, 2);
    Waves waves;
    auto full = vrs_sim::render_full(waves, W, H);
    auto coarse = vrs_sim::render_vrs(waves, p);

    // 2. Smoothing brings the coarse zones closer to the full-detail picture.
    {
        long changed = 0;
        auto sm = smooth_image(p, coarse, &changed);
        double e0 = coarse_error(p, coarse, full), e1 = coarse_error(p, sm, full);
        std::printf("blocky error %.4f -> smoothed %.4f (%.0f%% lower), %ld pixels written\n", e0, e1,
                    100.0 * (1.0 - e1 / e0), changed);
        CHECK(e1 < e0 * 0.85, "smoothing should cut the error by at least 15%% (%.4f -> %.4f)", e0, e1);
        CHECK(changed > 0, "nothing was smoothed");
        SmoothParams p6 = p; p6.strength = 0.6f;
        double e6 = coarse_error(p6, smooth_image(p6, coarse), full);
        CHECK(e6 < e0 && e6 > e1, "strength 0.6 should land between (%.4f %.4f %.4f)", e0, e6, e1);
    }

    // 3. The full-quality zone is never touched, bit for bit.
    {
        auto sm = smooth_image(p, coarse);
        long diff = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (smooth_rate_log2(p, x, y) == 0) {
                    const RGBA &a = sm[(size_t)y * W + x], &b = coarse[(size_t)y * W + x];
                    diff += a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
                }
        CHECK(diff == 0, "%ld sharp-zone pixels changed", diff);
    }

    // 4. A picture that was NOT rendered coarse (game drew elsewhere, or added grain) is left alone.
    {
        std::vector<RGBA> noise((size_t)W * H);
        for (auto& q : noise) q = {(float)rnd(), (float)rnd(), (float)rnd(), 1.0f};
        long changed = 0;
        smooth_image(p, noise, &changed);
        CHECK(changed < (long)(W * H) / 1000, "%ld pixels of a full-detail image were changed", changed);
        long changed2 = 0;
        smooth_image(p, full, &changed2);   // smooth full-res scene: blocks are not copies either
        std::printf("full-detail inputs: %ld (noise) and %ld (smooth scene) of %d pixels written\n", changed, changed2, W * H);
        CHECK(changed2 < (long)(W * H) / 100, "%ld pixels of the full-detail scene were changed", changed2);
    }

    // 5. Hard edges stay hard: black and white squares, and a ball's outline against the wall.
    {
        Squares sq(W, H);
        auto chk = vrs_sim::render_full(sq, W, H);
        auto sm = smooth_image(p, vrs_sim::render_vrs(sq, p));
        float worst = 0;
        for (size_t i = 0; i < sm.size(); ++i) worst = std::fmax(worst, std::fabs(sm[i].r - chk[i].r));
        CHECK(worst < 0.02f, "a hard edge was blurred by %.3f", worst);

        Ball ball;
        auto bc = vrs_sim::render_vrs(ball, p);
        auto bs = smooth_image(p, bc);
        // Promise 1: a block that an outline runs THROUGH is never touched.
        // Promise 2: an outline that runs ALONG a block border is only softened a little (like a
        // stretched low-res image would be), never moved by more than a fraction of the contrast.
        long through_changed = 0, along = 0;
        float worst_along = 0;
        auto su = [&](int xx, int yy) { return ball.surface((xx + 0.5f) / W, (yy + 0.5f) / H); };
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                int n = 1 << smooth_rate_log2(p, x, y);
                if (n == 1) continue;
                int ax = (x / n) * n, ay = (y / n) * n;
                bool mixed = false;
                for (int yy = ay; yy < ay + n && yy < H; ++yy)
                    for (int xx = ax; xx < ax + n && xx < W; ++xx) mixed |= su(xx, yy) != su(x, y);
                const RGBA &q = bs[(size_t)y * W + x], &r = bc[(size_t)y * W + x];
                float d = std::fmax(std::fabs(q.r - r.r), std::fmax(std::fabs(q.g - r.g), std::fabs(q.b - r.b)));
                if (mixed) { through_changed += d > 0.0f; continue; }
                bool edge = su(x, y) != su(x + 1, y) || su(x, y) != su(x, y + 1) || su(x, y) != su(x - 1, y) || su(x, y) != su(x, y - 1);
                if (edge) { ++along; worst_along = std::fmax(worst_along, d); }
            }
        std::printf("ball outline: %ld pixels in blocks it runs through changed; along block borders worst change %.3f (%ld px)\n",
                    through_changed, worst_along, along);
        CHECK(through_changed == 0, "%ld pixels in outline blocks were changed", through_changed);
        CHECK(worst_along < 0.12f, "an outline along a block border moved by %.3f", worst_along);
        double e0 = coarse_error(p, bc, vrs_sim::render_full(ball, W, H)), e1 = coarse_error(p, bs, vrs_sim::render_full(ball, W, H));
        std::printf("ball scene: blocky error %.4f -> smoothed %.4f\n", e0, e1);
        CHECK(e1 < e0, "ball scene got worse (%.4f -> %.4f)", e0, e1);
    }

    // 6. Strength 0 changes nothing; alpha is always kept.
    {
        SmoothParams p0 = p; p0.strength = 0.0f;
        auto sm = smooth_image(p0, coarse);
        double d = 0;
        for (size_t i = 0; i < sm.size(); ++i) d += std::fabs(sm[i].r - coarse[i].r) + std::fabs(sm[i].a - coarse[i].a);
        CHECK(d == 0.0, "strength 0 changed the image (%g)", d);
    }

    // 7. Odd inputs do not crash: zero size, mismatched buffer, 2x2-only GPU, tiny image.
    {
        SmoothParams z = p; z.width = 0;
        CHECK(smooth_image(z, coarse).size() == coarse.size(), "zero width");
        std::vector<RGBA> tiny(3);
        CHECK(smooth_image(p, tiny).size() == 3, "mismatched buffer");
        SmoothParams t = smooth_params(c, 5, 3, 16, 0.5f, 0.5f, 1);
        std::vector<RGBA> small(15, RGBA{0.5f, 0.5f, 0.5f, 1.0f});
        CHECK(smooth_image(t, small).size() == 15, "tiny image");
        CHECK(t.outer == 1, "2x2-only GPU must cap the outer rate");
        Config nanc = c; nanc.smooth_strength = std::nanf("");
        CHECK(clamp(nanc).smooth_strength == 0.60f, "NaN strength");
    }

    // 8. Config keys round-trip and default to off.
    {
        CHECK(!Config().smooth, "smooth must be off by default");
        CHECK(!parse_config(to_text(Config())).smooth, "default text turns it on");
        Config on; on.smooth = true; on.smooth_strength = 0.35f;
        Config back = parse_config(to_text(on));
        CHECK(back.smooth && std::fabs(back.smooth_strength - 0.35f) < 1e-3, "round trip");
        CHECK(to_text(Config()).find("smooth=0\nsmooth_strength=0.600\n") != std::string::npos, "text matches Python");
        CHECK(parse_config("smooth_strength=7").smooth_strength == 1.0f, "clamped high");
        CHECK(parse_config("smooth_strength=-1").smooth_strength == 0.0f, "clamped low");
    }

    // 9. VRS check: high on a VRS-rendered picture, near zero on a full-detail one.
    {
        Ball ball;
        auto ev_vrs = vrs_evidence(p, vrs_sim::render_vrs(ball, p));
        auto ev_full = vrs_evidence(p, vrs_sim::render_full(ball, W, H));
        auto ev_waves = vrs_evidence(p, full);
        double a = 100.0 * ev_vrs.copies / std::max(1L, ev_vrs.looked), b = 100.0 * ev_full.copies / std::max(1L, ev_full.looked),
               c3 = 100.0 * ev_waves.copies / std::max(1L, ev_waves.looked);
        std::printf("VRS check: %.1f%% of %ld blocks with VRS, %.1f%% (ball) and %.1f%% (waves) without\n", a, ev_vrs.looked, b, c3);
        CHECK(ev_vrs.looked > 100, "too few blocks looked at (%ld)", ev_vrs.looked);
        CHECK(a >= 90.0, "VRS picture should read >= 90%% (%.1f)", a);
        CHECK(c3 <= 5.0, "full-detail wavy picture should read near 0%% (%.1f)", c3);
        // The ball scene's wall is smooth but not flat, so it should also read low.
        CHECK(b <= 20.0, "full-detail ball scene should read low (%.1f)", b);
        SmoothParams z = p; z.width = 0;
        CHECK(vrs_evidence(z, full).looked == 0, "zero size");
    }

    // 10. Text: encoding, unknown characters, length limits, and the font itself.
    {
        CHECK(glyph_index('a') == glyph_index('A'), "lower case maps to upper case");
        CHECK(glyph_index('@') == glyph_index('?'), "unknown character shows as '?'");
        CHECK(glyph_index(' ') == 0, "space is glyph 0");
        OverlayText t = encode_text("71 FPS / 90 HZ   ", std::string(50, 'X'));
        CHECK(t.len1 == 14, "trailing spaces trimmed (%d)", t.len1);
        CHECK(t.len2 == kTextCols, "line 2 cut to %d (%d)", kTextCols, t.len2);
        CHECK(((t.words[0] >> 0) & 0xFF) == (uint32_t)glyph_index('7') && ((t.words[0] >> 8) & 0xFF) == (uint32_t)glyph_index('1'),
              "glyphs packed lowest byte first");
        CHECK(((t.words[8] >> 0) & 0xFF) == (uint32_t)glyph_index('X'), "line 2 starts at character 32");
        // Draw "H" and "0" big and compare with the hand-drawn rows.
        const char* want[2][7] = {{"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
                                  {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}};
        const char* text[2] = {"H", "0"};
        for (int k = 0; k < 2; ++k) {
            SmoothParams q = p; q.width = 64; q.height = 32;
            std::vector<RGBA> black((size_t)q.width * q.height, RGBA{0, 0, 0, 1});
            OverlayText ot = encode_text(text[k], "");
            // box centred at x = 32 px, top at y = 0; scale 1: glyph pixels start 3 px in
            auto img = overlay_image(q, black, black, ot, kOvText, 0.5f, 0.0f, 1);
            int boxW = 1 * 6 + 5, ox = 32 - boxW / 2;
            int bad = 0;
            for (int gy = 0; gy < 7; ++gy)
                for (int gx = 0; gx < 5; ++gx) {
                    bool on = img[(size_t)(3 + gy) * q.width + ox + 3 + gx].r > 0.5f;
                    bad += on != (want[k][gy][gx] == '#');
                }
            CHECK(bad == 0, "glyph '%s' drawn wrong in %d pixels", text[k], bad);
        }
        float x, y;
        text_position(0.55f, 0.5f, &x, &y);
        CHECK(std::fabs(x - 0.55f) < 1e-6 && y > 0.5f && y < 0.7f, "text sits below the lens centre");
        CHECK(text_scale(2000) >= 4 && text_scale(100) == 2, "text scale");
    }

    // 11. Zone highlight: sharp area untouched, colours by zone, purple where the image is full detail.
    {
        OverlayText none;
        auto vrs_img = vrs_sim::render_vrs(Waves(), p);
        auto hz = overlay_image(p, vrs_img, vrs_img, none, kOvZones | kOvHaveCopy, 0.5f, 0.6f, 4);
        long sharp_changed = 0, purple = 0, coarse = 0;
        auto hz_full = overlay_image(p, full, full, none, kOvZones | kOvHaveCopy, 0.5f, 0.6f, 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = (size_t)y * W + x;
                if (smooth_rate_log2(p, x, y) == 0) { sharp_changed += hz[i].r != vrs_img[i].r || hz[i].b != vrs_img[i].b; continue; }
                ++coarse;
                purple += is_purple(hz_full[i], full[i]);
            }
        CHECK(sharp_changed == 0, "zone highlight touched %ld sharp pixels", sharp_changed);
        CHECK(purple > coarse * 9 / 10, "full-detail picture should show purple almost everywhere (%ld of %ld)", purple, coarse);
        long purple_vrs = 0;
        for (size_t i = 0; i < hz.size(); ++i) purple_vrs += is_purple(hz[i], vrs_img[i]);
        CHECK(purple_vrs < coarse / 20, "VRS picture should show almost no purple (%ld)", purple_vrs);
        for (size_t i = 0; i < hz.size(); ++i) if (hz[i].a != vrs_img[i].a) { CHECK(false, "alpha changed"); break; }
    }

    if (failures) { std::printf("smooth_test: %d failure(s)\n", failures); return 1; }
    std::printf("smooth_test: all passed\n");
    return 0;
}
