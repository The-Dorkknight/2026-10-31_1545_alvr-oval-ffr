// Makes the before/after picture in docs/smooth_demo.png (via tools/make_smooth_demo.py).
// Writes a binary PPM: three crops from the 4x4 zone of a synthetic scene, each magnified 4x:
//   [ full detail | what VRS gives the headset today | after Smooth periphery ]
// This is the C++ copy of the shader on a made-up picture, not a screenshot from a game.
#include "ovalffr_core.h"
#include "ovalffr_smooth.h"
#include "vrs_sim.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ovalffr;

// sky-to-ground gradient wall with soft waves, a lit ball and one dark post
struct Demo : vrs_sim::Scene {
    int surface(float u, float v) const override {
        if (u > 0.205f && u < 0.215f) return 2;
        float dx = u - 0.13f, dy = v - 0.25f;
        return dx * dx * 1.8f + dy * dy < 0.010f ? 1 : 0;
    }
    RGBA shade(int s, float u, float v) const override {
        if (s == 2) return {0.05f, 0.05f, 0.06f, 1.0f};
        if (s == 1) {
            float dx = u - 0.13f, dy = v - 0.25f, d2 = dx * dx * 1.8f + dy * dy;
            float l = 0.35f + 0.65f * std::sqrt(std::fmax(0.0f, 1.0f - d2 / 0.010f));
            return {0.95f * l, 0.55f * l, 0.20f * l, 1.0f};
        }
        float wave = 0.08f * std::sin(u * 60.0f + v * 20.0f);
        return {0.25f + 0.5f * v + wave, 0.35f + 0.35f * v + wave * 0.8f, 0.75f - 0.4f * v + wave * 0.5f, 1.0f};
    }
};

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "smooth_demo.ppm";
    const int W = 1024, H = 768;
    Config c; c.h = 0.55f; c.v = 0.55f; c.band = 0.30f; c.outer = 2; c.smooth = true; c.smooth_strength = 0.6f;
    SmoothParams p = smooth_params(c, W, H, 16, 0.5f, 0.5f, 2);
    Demo scene;
    auto full = vrs_sim::render_full(scene, W, H);
    auto coarse = vrs_sim::render_vrs(scene, p);
    long changed = 0;
    auto sm = smooth_image(p, coarse, &changed);

    const int CX = 64, CY = 112, CW = 192, CH = 160, Z = 3;   // crop inside the 4x4 zone
    const int OW = CW * Z * 3 + 2 * 8, OH = CH * Z;
    FILE* f = std::fopen(out, "wb");
    if (!f) { std::perror(out); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", OW, OH);
    const std::vector<RGBA>* imgs[3] = {&full, &coarse, &sm};
    for (int oy = 0; oy < OH; ++oy)
        for (int ox = 0; ox < OW; ++ox) {
            int panel = ox / (CW * Z + 8), px = ox % (CW * Z + 8);
            unsigned char rgb[3] = {255, 255, 255};
            if (panel < 3 && px < CW * Z) {
                const RGBA& q = (*imgs[panel])[(size_t)(CY + oy / Z) * W + (CX + px / Z)];
                float ch[3] = {q.r, q.g, q.b};
                for (int k = 0; k < 3; ++k) rgb[k] = (unsigned char)std::lround(255.0f * std::fmin(1.0f, std::fmax(0.0f, ch[k])));
            }
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
    int l2 = smooth_rate_log2(p, CX, CY);
    std::printf("wrote %s (crop is in the %dx%d zone; %ld pixels smoothed in the whole frame)\n", out, 1 << l2, 1 << l2, changed);
    return 0;
}
