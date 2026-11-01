// Test helper: what Variable Rate Shading really produces, for a made-up scene.
//
// VRS keeps COVERAGE at full resolution (which object a pixel belongs to is decided per pixel, so
// outlines stay sharp) and only runs the pixel shader once per coarse block, per object. So each
// pixel gets its own object's colour, evaluated at the centre of its 2x2 / 4x4 block.
#pragma once
#include <vector>
#include "ovalffr_smooth.h"

namespace vrs_sim {

using ovalffr::RGBA;
using ovalffr::SmoothParams;

struct Scene {
    virtual ~Scene() {}
    virtual int surface(float u, float v) const = 0;                // which object covers this point
    virtual RGBA shade(int surface, float u, float v) const = 0;    // that object's colour at this point
};

inline std::vector<RGBA> render_full(const Scene& s, int w, int h) {
    std::vector<RGBA> img((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            img[(size_t)y * w + x] = s.shade(s.surface(u, v), u, v);
        }
    return img;
}

inline std::vector<RGBA> render_vrs(const Scene& s, const SmoothParams& p) {
    std::vector<RGBA> img((size_t)p.width * p.height);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            int n = 1 << ovalffr::smooth_rate_log2(p, x, y);
            int ax = (x / n) * n, ay = (y / n) * n;
            float u = (x + 0.5f) / p.width, v = (y + 0.5f) / p.height;                    // coverage: per pixel
            float cu = (ax + 0.5f * n) / p.width, cv = (ay + 0.5f * n) / p.height;         // shading: block centre
            img[(size_t)y * p.width + x] = s.shade(s.surface(u, v), cu, cv);
        }
    return img;
}

} // namespace vrs_sim
