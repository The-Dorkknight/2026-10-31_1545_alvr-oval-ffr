// Line-by-line C++ copy of layer/shaders/smooth.hlsl. Keep the two in step.
#include "ovalffr_smooth.h"
#include "ovalffr_font.h"

#include <algorithm>
#include <cmath>

namespace ovalffr {

static float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

SmoothParams smooth_params(const Config& in, int width, int height, int tile, float cx, float cy, int max_log2) {
    Config c = clamp(in);
    SmoothParams p;
    p.cx = std::isfinite(cx) ? cx : 0.5f;
    p.cy = std::isfinite(cy) ? cy : 0.5f;
    p.ha = c.h * 0.5f;
    p.hb = c.v * 0.5f;
    p.shape = c.shape;
    p.band = c.band;
    p.strength = c.smooth_strength;
    p.width = width;
    p.height = height;
    p.tile = tile < 1 ? 8 : tile;
    p.outer = std::min(c.outer, std::max(1, max_log2));   // same limit as make_mask()
    return p;
}

int smooth_rate_log2(const SmoothParams& p, int px, int py) {
    int tx = px / p.tile, ty = py / p.tile;
    float x = ((float)tx + 0.5f) * (float)p.tile / (float)p.width;
    float y = ((float)ty + 0.5f) * (float)p.tile / (float)p.height;
    float ux = std::fabs(x - p.cx) / p.ha, uy = std::fabs(y - p.cy) / p.hb;
    float s = std::pow(std::pow(ux, p.shape) + std::pow(uy, p.shape), 1.0f / p.shape);
    if (s <= 1.0f) return 0;
    if (s <= 1.0f + p.band) return 1;
    return p.outer;
}

static RGBA fetch(const SmoothParams& p, const std::vector<RGBA>& img, int x, int y) {
    x = std::min(std::max(x, 0), p.width - 1);
    y = std::min(std::max(y, 0), p.height - 1);
    return img[(size_t)y * p.width + x];
}

static float similar(const SmoothParams& p, const RGBA& a, const RGBA& b) {
    float d = std::max(std::fabs(a.r - b.r), std::max(std::fabs(a.g - b.g), std::fabs(a.b - b.b)));
    float m = std::max(std::max(a.r, b.r), std::max(std::max(a.g, b.g), std::max(a.b, b.b)));
    return saturate(1.0f - (d / std::max(m, 0.02f)) / p.edge);
}

static float maxdiff(const RGBA& a, const RGBA& b) {
    return std::max(std::max(std::fabs(a.r - b.r), std::fabs(a.g - b.g)), std::max(std::fabs(a.b - b.b), std::fabs(a.a - b.a)));
}

// all four corners of the block must match (CornersMatch in the shader)
static bool corners_match(const SmoothParams& p, const std::vector<RGBA>& src, int px, int py, int n, const RGBA& own) {
    int ax = (px / n) * n, ay = (py / n) * n;
    float d = std::max(std::max(maxdiff(own, fetch(p, src, ax, ay)), maxdiff(own, fetch(p, src, ax + n - 1, ay + n - 1))),
                       std::max(maxdiff(own, fetch(p, src, ax + n - 1, ay)), maxdiff(own, fetch(p, src, ax, ay + n - 1))));
    return d <= 1.0f / 512.0f;
}

std::vector<RGBA> smooth_image(const SmoothParams& p, const std::vector<RGBA>& src, long* changed) {
    std::vector<RGBA> out = src;
    long n_changed = 0;
    if (p.width <= 0 || p.height <= 0 || src.size() != (size_t)p.width * p.height) {
        if (changed) *changed = 0;
        return out;
    }
    for (int py = 0; py < p.height; ++py) {
        for (int px = 0; px < p.width; ++px) {
            int l2 = smooth_rate_log2(p, px, py);
            if (l2 == 0) continue;                                   // discard
            int n = 1 << l2;
            int ax = (px / n) * n, ay = (py / n) * n;
            RGBA own = fetch(p, src, px, py);
            if (!corners_match(p, src, px, py, n, own)) continue;    // discard: not a copied block
            float fx = ((float)(px - ax) + 0.5f) / (float)n - 0.5f;
            float fy = ((float)(py - ay) + 0.5f) / (float)n - 0.5f;
            int sx = fx < 0.0f ? -1 : 1, sy = fy < 0.0f ? -1 : 1;
            float wx = std::fabs(fx), wy = std::fabs(fy);
            RGBA cX = fetch(p, src, px + sx * n, py), cY = fetch(p, src, px, py + sy * n),
                 cXY = fetch(p, src, px + sx * n, py + sy * n);
            float w0 = (1 - wx) * (1 - wy);
            float w1 = wx * (1 - wy) * similar(p, own, cX);
            float w2 = (1 - wx) * wy * similar(p, own, cY);
            float w3 = wx * wy * similar(p, own, cXY);
            float ws = w0 + w1 + w2 + w3;
            RGBA r;
            r.r = own.r + ((own.r * w0 + cX.r * w1 + cY.r * w2 + cXY.r * w3) / ws - own.r) * p.strength;
            r.g = own.g + ((own.g * w0 + cX.g * w1 + cY.g * w2 + cXY.g * w3) / ws - own.g) * p.strength;
            r.b = own.b + ((own.b * w0 + cX.b * w1 + cY.b * w2 + cXY.b * w3) / ws - own.b) * p.strength;
            r.a = own.a;
            out[(size_t)py * p.width + px] = r;
            ++n_changed;
        }
    }
    if (changed) *changed = n_changed;
    return out;
}

VrsEvidence vrs_evidence(const SmoothParams& p, const std::vector<RGBA>& img) {
    VrsEvidence e;
    if (p.width <= 0 || p.height <= 0 || img.size() != (size_t)p.width * p.height) return e;
    for (int y = 0; y < p.height; y += 16)
        for (int x = 0; x < p.width; x += 16) {
            int l2 = smooth_rate_log2(p, x, y);
            if (l2 == 0) continue;
            int n = 1 << l2;
            RGBA c0 = fetch(p, img, x, y);
            bool same = true;
            for (int yy = 0; yy < n; ++yy)
                for (int xx = 0; xx < n; ++xx)
                    if (maxdiff(fetch(p, img, x + xx, y + yy), c0) > 1.0f / 512.0f) same = false;
            ++e.looked;
            if (same) ++e.copies;
        }
    return e;
}

int glyph_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int i = 0; i < kFontCount; ++i)
        if (kFontChars[i] == c) return i;
    for (int i = 0; i < kFontCount; ++i)
        if (kFontChars[i] == '?') return i;
    return 0;
}

OverlayText encode_text(const std::string& l1, const std::string& l2) {
    OverlayText t;
    const std::string* lines[2] = {&l1, &l2};
    for (int ln = 0; ln < 2; ++ln) {
        int len = (int)std::min(lines[ln]->size(), (size_t)kTextCols);
        while (len > 0 && (*lines[ln])[len - 1] == ' ') --len;   // trailing spaces only widen the box
        if (ln == 0) t.len1 = len; else t.len2 = len;
        for (int c = 0; c < len; ++c) {
            int i = ln * kTextCols + c;
            t.words[i / 4] |= (uint32_t)glyph_index((*lines[ln])[c]) << ((i % 4) * 8);
        }
    }
    return t;
}

// Below the lens centre, in the part of the view that is still clear through the lens. Each eye
// uses its own lens centre, so both eyes see the box at the same place (it appears far away).
void text_position(float cx, float cy, float* x, float* y) {
    *x = std::isfinite(cx) ? cx : 0.5f;
    *y = (std::isfinite(cy) ? cy : 0.5f) + 0.13f;
}

int text_scale(int width) { return std::max(2, width / 450); }

static bool glyph_pixel(int g, int gx, int gy) {
    if (g < 0 || g >= kFontCount) g = 0;
    int bit = gy * 5 + gx;
    return gy < 4 ? ((kFont[g][0] >> bit) & 1) != 0 : ((kFont[g][1] >> (bit - 20)) & 1) != 0;
}

static bool text_pixel(const SmoothParams& p, const OverlayText& t, float tx, float ty, int s, int px, int py, RGBA* out) {
    int lines = t.len2 > 0 ? 2 : 1;
    s = std::max(s, 1);
    int boxW = std::max(t.len1, t.len2) * 6 * s + 5 * s;
    int boxH = lines * 9 * s + 4 * s;
    int ox = (int)(tx * (float)p.width) - boxW / 2, oy = (int)(ty * (float)p.height);
    int qx = px - ox, qy = py - oy;
    if (qx < 0 || qy < 0 || qx >= boxW || qy >= boxH) return false;
    *out = {0.02f, 0.02f, 0.03f, 0.70f};
    int x = qx - 3 * s, y = qy - 3 * s;
    if (x < 0 || y < 0) return true;
    int row = y / (9 * s), col = x / (6 * s), gx = (x % (6 * s)) / s, gy = (y % (9 * s)) / s;
    int len = row == 0 ? t.len1 : t.len2;
    if (row < lines && col < len && gx < 5 && gy < 7) {
        int i = row * kTextCols + col;
        int g = (int)((t.words[i / 4] >> ((i % 4) * 8)) & 0xFF);
        if (glyph_pixel(g, gx, gy)) *out = {1.0f, 1.0f, 1.0f, 1.0f};
    }
    return true;
}

std::vector<RGBA> overlay_image(const SmoothParams& p, const std::vector<RGBA>& img, const std::vector<RGBA>& src,
                                const OverlayText& t, unsigned flags, float tx, float ty, int scale) {
    std::vector<RGBA> out = img;
    if (p.width <= 0 || p.height <= 0 || img.size() != (size_t)p.width * p.height ||
        ((flags & kOvHaveCopy) && src.size() != img.size()))
        return out;
    for (int py = 0; py < p.height; ++py)
        for (int px = 0; px < p.width; ++px) {
            RGBA c;
            bool have = (flags & kOvText) && text_pixel(p, t, tx, ty, scale, px, py, &c);
            if (!have && (flags & kOvZones)) {
                int l2 = smooth_rate_log2(p, px, py);
                if (l2 > 0) {
                    bool real = !(flags & kOvHaveCopy) || corners_match(p, src, px, py, 1 << l2, fetch(p, src, px, py));
                    c = !real ? RGBA{0.70f, 0.20f, 0.95f, 0.35f}
                              : (l2 == 1 ? RGBA{0.96f, 0.76f, 0.26f, 0.35f} : RGBA{0.86f, 0.24f, 0.30f, 0.35f});
                    have = true;
                }
            }
            if (!have) continue;   // discard
            RGBA& d = out[(size_t)py * p.width + px];   // blend: SRC_ALPHA / INV_SRC_ALPHA, alpha kept
            d.r = c.r * c.a + d.r * (1.0f - c.a);
            d.g = c.g * c.a + d.g * (1.0f - c.a);
            d.b = c.b * c.a + d.b * (1.0f - c.a);
        }
    return out;
}

} // namespace ovalffr
