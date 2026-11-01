#include "ovalffr_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace ovalffr {

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// NaN/inf (e.g. "h=nan" in a hand-edited config) must never reach the mask maths: every
// comparison with NaN is false, which would silently make the whole view the lowest rate.
static float fin(float v, float def) { return std::isfinite(v) ? v : def; }

Config clamp(const Config& in) {
    Config c = in;
    c.h = clampf(fin(c.h, 0.70f), 0.20f, 1.00f);
    c.v = clampf(fin(c.v, 0.70f), 0.20f, 1.00f);
    c.shape = clampf(fin(c.shape, 2.0f), 2.0f, 6.0f);
    c.band = clampf(fin(c.band, 0.50f), 0.0f, 2.0f);
    c.outer = c.outer < 1 ? 1 : (c.outer > 2 ? 2 : c.outer);
    c.auto_floor = clampf(fin(c.auto_floor, 0.60f), 0.40f, 1.00f);
    c.smooth_strength = clampf(fin(c.smooth_strength, 0.60f), 0.0f, 1.0f);
    return c;
}

Config parse_config(const std::string& text) {
    Config c;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "enabled") c.enabled = std::atoi(v.c_str()) != 0;
        else if (k == "h") c.h = (float)std::atof(v.c_str());
        else if (k == "v") c.v = (float)std::atof(v.c_str());
        else if (k == "shape") c.shape = (float)std::atof(v.c_str());
        else if (k == "band") c.band = (float)std::atof(v.c_str());
        else if (k == "outer") c.outer = std::atoi(v.c_str());
        else if (k == "centre_on_lens") c.centre_on_lens = std::atoi(v.c_str()) != 0;
        else if (k == "auto") c.auto_fov = std::atoi(v.c_str()) != 0;
        else if (k == "auto_floor") c.auto_floor = (float)std::atof(v.c_str());
        else if (k == "smooth") c.smooth = std::atoi(v.c_str()) != 0;
        else if (k == "smooth_strength") c.smooth_strength = (float)std::atof(v.c_str());
        else if (k == "overlay") c.overlay = std::atoi(v.c_str()) != 0;
    }
    return clamp(c);
}

std::string to_text(const Config& in) {
    Config c = clamp(in);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "# OvalFFR config (written by the UI, hot-reloaded by the layer)\n"
                  "enabled=%d\nh=%.4f\nv=%.4f\nshape=%.3f\nband=%.4f\nouter=%d\ncentre_on_lens=%d\n"
                  "auto=%d\nauto_floor=%.3f\nsmooth=%d\nsmooth_strength=%.3f\noverlay=%d\n",
                  c.enabled ? 1 : 0, c.h, c.v, c.shape, c.band, c.outer, c.centre_on_lens ? 1 : 0,
                  c.auto_fov ? 1 : 0, c.auto_floor, c.smooth ? 1 : 0, c.smooth_strength, c.overlay ? 1 : 0);
    return buf;
}

std::vector<uint8_t> make_mask(const Config& in, int width_px, int height_px, int tile,
                               float cx, float cy, int max_log2,
                               int* tiles_w, int* tiles_h) {
    Config c = clamp(in);
    if (tile < 1) tile = 8;
    cx = fin(cx, 0.5f);
    cy = fin(cy, 0.5f);
    if (width_px < 0) width_px = 0;
    if (height_px < 0) height_px = 0;
    long long tw64 = ((long long)width_px + tile - 1) / tile, th64 = ((long long)height_px + tile - 1) / tile;
    if (tw64 * th64 > 16LL * 1024 * 1024) { // absurd size: refuse rather than allocate gigabytes
        if (tiles_w) *tiles_w = 0;
        if (tiles_h) *tiles_h = 0;
        return std::vector<uint8_t>();
    }
    int tw = (int)tw64, th = (int)th64;
    if (tiles_w) *tiles_w = tw;
    if (tiles_h) *tiles_h = th;
    std::vector<uint8_t> m((size_t)tw * th, RATE_1X1);
    if (!c.enabled || width_px <= 0 || height_px <= 0) return m;

    int outer = std::min(c.outer, std::max(1, max_log2));
    uint8_t outer_code = (uint8_t)((outer << 2) | outer);
    uint8_t mid_code = RATE_2X2;
    const float a = c.h * 0.5f, b = c.v * 0.5f;
    for (int ty = 0; ty < th; ++ty) {
        float y = ((ty + 0.5f) * tile) / (float)height_px;
        for (int tx = 0; tx < tw; ++tx) {
            float x = ((tx + 0.5f) * tile) / (float)width_px;
            float ux = std::fabs(x - cx) / a, uy = std::fabs(y - cy) / b;
            float s = std::pow(std::pow(ux, c.shape) + std::pow(uy, c.shape), 1.0f / c.shape);
            uint8_t code = s <= 1.0f ? RATE_1X1 : (s <= 1.0f + c.band ? mid_code : outer_code);
            m[(size_t)ty * tw + tx] = code;
        }
    }
    return m;
}

double shading_cost(const std::vector<uint8_t>& mask) {
    if (mask.empty()) return 1.0;
    double sum = 0;
    for (uint8_t code : mask) {
        int lx = (code >> 2) & 3, ly = code & 3;
        sum += 1.0 / (double)((1 << lx) * (1 << ly));
    }
    return sum / (double)mask.size();
}

void centre_from_fov(float aL, float aR, float aU, float aD, float* cx, float* cy) {
    float tl = std::tan(aL), tr = std::tan(aR), tu = std::tan(aU), td = std::tan(aD);
    float w = tr - tl, h = tu - td;
    *cx = 0.5f; *cy = 0.5f;
    if (w > 1e-4f && h > 1e-4f) {
        *cx = clampf((0.0f - tl) / w, 0.2f, 0.8f);
        *cy = clampf(tu / h, 0.2f, 0.8f);
    }
}

std::string data_dir() {
    std::string dir;
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base) return "";
    dir = std::string(base) + "\\OvalFFR";
    _mkdir(dir.c_str());
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) return "";
    dir = std::string(home) + "/Library/Application Support/OvalFFR";
    mkdir((std::string(home) + "/Library").c_str(), 0755);
    mkdir((std::string(home) + "/Library/Application Support").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
#else
    const char* ovr = std::getenv("OVALFFR_DIR"); // used by the tests
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (ovr) dir = ovr;
    else if (xdg && *xdg) dir = std::string(xdg) + "/ovalffr";
    else if (home) dir = std::string(home) + "/.config/ovalffr";
    else return "";
    if (!xdg && home && !ovr) mkdir((std::string(home) + "/.config").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
#endif
    return dir;
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return (bool)f;
}

} // namespace ovalffr
