// OvalFFR GPU passes: portable C++ copy of layer/shaders/smooth.hlsl (smoothing, VRS check,
// in-headset text and zone highlight).
// The layer runs the HLSL on the GPU; this copy exists so the maths can be unit-tested and a
// before/after picture can be made on any machine (tests/smooth_test.cpp, tests/smooth_demo.cpp).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "ovalffr_core.h"

namespace ovalffr {

struct SmoothParams {
    float cx = 0.5f, cy = 0.5f;   // oval centre
    float ha = 0.35f, hb = 0.35f; // half sizes (h/2, v/2 of the effective config)
    float shape = 2.0f, band = 0.5f;
    float strength = 0.6f;
    float edge = 0.5f;            // relative brightness difference treated as a hard edge
    int width = 0, height = 0;
    int tile = 16;
    int outer = 2;                // log2 of the outer rate, already limited by the GPU
};

// Build the shader constants from a config, exactly as the D3D12 backend does.
SmoothParams smooth_params(const Config& cfg, int width, int height, int tile, float cx, float cy, int max_log2);

// log2 of the shading rate the mask gives pixel (px, py): 0 = 1x1, 1 = 2x2, 2 = 4x4.
int smooth_rate_log2(const SmoothParams& p, int px, int py);

struct RGBA { float r, g, b, a; };

// Run the pass over one image (row-major, width*height). Returns the new image.
// `changed`, if given, receives how many pixels were written.
std::vector<RGBA> smooth_image(const SmoothParams& p, const std::vector<RGBA>& src, long* changed = nullptr);

// ---- VRS check (CSCount) -------------------------------------------------------------------
struct VrsEvidence { long looked = 0, copies = 0; };
// One low-res block per 16x16 pixel cell: is it an exact copy (= VRS really happened there)?
VrsEvidence vrs_evidence(const SmoothParams& p, const std::vector<RGBA>& img);

// ---- In-headset text and zone highlight (PSOverlay) ----------------------------------------
constexpr int kTextCols = 32;   // characters per line; 2 lines
struct OverlayText {
    std::array<uint32_t, 16> words{};   // 2 x 32 glyph indices, 4 per word, lowest byte first
    int len1 = 0, len2 = 0;
};
int glyph_index(char c);                                      // unknown -> '?', lower case -> upper
OverlayText encode_text(const std::string& line1, const std::string& line2);
// 0..1 of the view: where the box goes for an eye whose lens centre is (cx, cy).
void text_position(float cx, float cy, float* x, float* y);
int text_scale(int width);                                    // font pixel size for a view this wide

enum OverlayFlags { kOvText = 1, kOvZones = 2, kOvHaveCopy = 4 };
// Alpha-blends the overlay onto `img` (`src` is the unmodified copy the zone check reads).
std::vector<RGBA> overlay_image(const SmoothParams& p, const std::vector<RGBA>& img, const std::vector<RGBA>& src,
                                const OverlayText& t, unsigned flags, float text_x, float text_y, int scale);


} // namespace ovalffr
