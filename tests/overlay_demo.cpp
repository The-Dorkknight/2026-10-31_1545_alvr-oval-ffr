// Makes docs/overlay_demo.png (via tools/make_smooth_demo.py --overlay): what the in-headset fps box
// and the zone highlight (Ctrl+Alt+Z) look like, using the C++ copy of the shader on a made-up
// scene. Two eye images side by side:
//   left : VRS really reached the image -> yellow (2x2) and red (4x4) zones
//   right: the game drew this image at full detail (VRS did not reach it) -> purple
#include "ovalffr_core.h"
#include "ovalffr_smooth.h"
#include "vrs_sim.h"

#include <cmath>
#include <cstdio>

using namespace ovalffr;

struct Room : vrs_sim::Scene {
    int surface(float u, float v) const override {
        float dx = u - 0.30f, dy = v - 0.42f;
        if (dx * dx + dy * dy < 0.012f) return 1;     // a ball
        if (v > 0.70f) return 2;                     // floor
        return 0;                                    // wall
    }
    RGBA shade(int s, float u, float v) const override {
        if (s == 1) {
            float l = 0.35f + 0.65f * std::fmax(0.0f, 1.0f - ((u - 0.27f) * (u - 0.27f) + (v - 0.39f) * (v - 0.39f)) / 0.02f);
            return {0.95f * l, 0.55f * l, 0.2f * l, 1.0f};
        }
        if (s == 2) { float k = 0.35f + 0.1f * std::sin(u * 50.0f) * std::sin(v * 50.0f); return {k, k * 0.9f, k * 0.8f, 1.0f}; }
        float w = 0.06f * std::sin(u * 35.0f + v * 9.0f);
        return {0.55f + w, 0.6f + w, 0.68f + w, 1.0f};
    }
};

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "overlay_demo.ppm";
    const int W = 720, H = 720;
    Config c; c.h = 0.60f; c.v = 0.55f; c.band = 0.40f; c.outer = 2;
    SmoothParams p = smooth_params(c, W, H, 16, 0.5f, 0.5f, 2);
    Room room;
    auto vrs = vrs_sim::render_vrs(room, p);       // VRS reached the image
    auto full = vrs_sim::render_full(room, W, H);  // it did not
    float tx, ty;
    text_position(0.5f, 0.5f, &tx, &ty);
    OverlayText t = encode_text("72 FPS / 90 HZ  FFR ON", "YELLOW 2X2 RED 4X4 PURPLE=FULL");
    unsigned fl = kOvText | kOvZones | kOvHaveCopy;
    auto a = overlay_image(p, vrs, vrs, t, fl, tx, ty, text_scale(W));
    OverlayText t2 = encode_text("72 FPS / 90 HZ  FFR ON", "YELLOW 2X2 RED 4X4 PURPLE=FULL");
    auto b = overlay_image(p, full, full, t2, fl, tx, ty, text_scale(W));
    auto ev = vrs_evidence(p, vrs), ev2 = vrs_evidence(p, full);

    const int GAP = 12, OW = W * 2 + GAP;
    FILE* f = std::fopen(out, "wb");
    if (!f) { std::perror(out); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", OW, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < OW; ++x) {
            unsigned char rgb[3] = {255, 255, 255};
            const std::vector<RGBA>* img = x < W ? &a : (x >= W + GAP ? &b : nullptr);
            if (img) {
                const RGBA& q = (*img)[(size_t)y * W + (x < W ? x : x - W - GAP)];
                float ch[3] = {q.r, q.g, q.b};
                for (int k = 0; k < 3; ++k) rgb[k] = (unsigned char)std::lround(255.0f * std::fmin(1.0f, std::fmax(0.0f, ch[k])));
            }
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
    std::printf("wrote %s; VRS check would read %.0f%% (left) and %.0f%% (right)\n", out,
                100.0 * ev.copies / (ev.looked ? ev.looked : 1), 100.0 * ev2.copies / (ev2.looked ? ev2.looked : 1));
    return 0;
}
