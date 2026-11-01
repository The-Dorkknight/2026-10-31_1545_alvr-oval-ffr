// OvalFFR core: platform-independent mask maths, config and savings estimate.
// No GPU or OpenXR dependency, so it builds and is unit-tested on any machine.
// The Python UI (src/ovalffr/mask.py) mirrors this file; tests/test_mask_parity.py
// checks the two give identical masks.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#define OVALFFR_VERSION "0.5.0"

namespace ovalffr {

struct Config {
    bool  enabled   = true;
    float h         = 0.70f; // full-quality oval width,  fraction of view width  (0.20..1.00)
    float v         = 0.70f; // full-quality oval height, fraction of view height (0.20..1.00)
    float shape     = 2.0f;  // superellipse exponent: 2 = oval, 4 = slot / rounded-rectangle
    float band      = 0.50f; // middle ring thickness as a fraction of the inner oval size
    int   outer     = 2;     // log2 of the outer rate: 1 = 2x2, 2 = 4x4
    bool  centre_on_lens = true; // centre on the optical axis from the runtime FOV
    bool  auto_fov  = false; // Auto foveation: shrink the sharp area only while frames are being missed
    float auto_floor = 0.60f; // Auto never goes below this fraction of h and v (0.40..1.00)
    bool  smooth    = false; // Smooth periphery: blend the blocky low-res pixels after the game draws (off by default)
    float smooth_strength = 0.60f; // 0..1, how far each pixel moves towards the smooth (upscaled) value
    bool  overlay   = false; // small fps box in the headset (Ctrl+Alt+O)
};

// Shading-rate codes are the D3D12 encoding: (log2(x) << 2) | log2(y).
// 0 = 1x1, 5 = 2x2, 10 = 4x4.
constexpr uint8_t RATE_1X1 = 0, RATE_2X2 = 5, RATE_4X4 = 10;

Config clamp(const Config& c);
Config parse_config(const std::string& text);        // key=value lines, '#' comments
std::string to_text(const Config& c);

// Tile grid for a view of width_px x height_px with square tiles of `tile` pixels.
// (cx, cy) is the oval centre in 0..1 view coordinates (y down).
// `max_log2` limits the coarsest rate the GPU supports (1 = 2x2, 2 = 4x4).
std::vector<uint8_t> make_mask(const Config& c, int width_px, int height_px, int tile,
                               float cx, float cy, int max_log2,
                               int* tiles_w, int* tiles_h);

// Fraction of pixel-shader invocations the mask still runs (1.0 = no saving).
double shading_cost(const std::vector<uint8_t>& mask);
inline double estimated_saving(const std::vector<uint8_t>& mask) { return 1.0 - shading_cost(mask); }

// Optical-axis position inside the view image from OpenXR FOV half-angles (radians,
// angleLeft/angleDown negative). Result is 0..1 with y down. Falls back to 0.5.
void centre_from_fov(float angleLeft, float angleRight, float angleUp, float angleDown,
                     float* cx, float* cy);

// Per-user config/log folder (created if missing). Empty string if it cannot be made.
std::string data_dir();
bool read_file(const std::string& path, std::string* out);
bool write_file(const std::string& path, const std::string& text);

} // namespace ovalffr
