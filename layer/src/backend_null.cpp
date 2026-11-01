// Non-Windows build: no GPU backend yet (Vulkan is on the roadmap), so the layer is a
// pass-through. For tests, OVALFFR_MOCK_BACKEND=1 selects a mock that records what the
// layer core tells a backend, so the plumbing can be verified without a GPU or a headset.
#include "backend.h"

#include <cstdio>
#include <cstdlib>
#include <map>

#ifndef _WIN32
namespace ovalffr {

namespace {
struct MockBackend : Backend {
    Config cfg;
    int tw = 0, th = 0;
    float cx[2] = {0.5f, 0.5f}, cy[2] = {0.5f, 0.5f};
    uint32_t w = 0, h = 0;
    int configs = 0, images = 0, rebuilds = 0;
    int acquired = 0, released = 0, smooth_runs = 0;
    long long format = 0;
    uint32_t last_index = 0;
    std::map<XrSwapchain, uint32_t> eye;
    double saving = 0;

    const char* name() const override { return "mock"; }
    void onSwapchainImages(XrSwapchain, uint32_t W, uint32_t H, uint32_t, const XrSwapchainImageBaseHeader*,
                           uint32_t count) override { w = W; h = H; images += (int)count; rebuild(); }
    void onSwapchainDestroyed(XrSwapchain sc) override { eye.erase(sc); }
    void onSwapchainEye(XrSwapchain sc, uint32_t e) override { eye[sc] = e; if (w && h) write_state(); }
    void onEyeCentre(uint32_t e, float x, float y) override { cx[e] = x; cy[e] = y; rebuild(); }
    void onConfig(const Config& c) override { cfg = c; ++configs; rebuild(); }
    void onSwapchainFormat(XrSwapchain, int64_t f, uint32_t) override { format = f; }
    void onAcquire(XrSwapchain, uint32_t i) override { ++acquired; last_index = i; }
    void onRelease(XrSwapchain) override {
        ++released;
        if (cfg.enabled && cfg.smooth) ++smooth_runs;   // what the D3D12 backend would queue
        write_state();
    }
    Overlay ov;
    void onOverlay(const Overlay& o) override { ov = o; write_state(); }
    double takeVrsSeen() override {
        const char* v = std::getenv("OVALFFR_TEST_VRS_SEEN");   // tests: pretend the GPU check measured this
        return v ? std::atof(v) : -1.0;
    }
    std::string smoothStatus() const override { return cfg.smooth ? (smooth_runs ? "running" : "waiting for the first frame") : "off"; }
    void rebuild() {
        if (!w || !h) return;
        auto m = make_mask(cfg, (int)w, (int)h, 16, cx[0], cy[0], 2, &tw, &th);
        saving = estimated_saving(m);
        ++rebuilds;
        write_state();
    }
    void write_state() {
        char hex[32];
        std::string eyes;
        for (auto& kv : eye) {
            std::snprintf(hex, sizeof hex, "%llx", (unsigned long long)(uintptr_t)kv.first);
            eyes += std::string("eye_") + hex + "=" + std::to_string(kv.second) + "\n";
        }
        char num[256];
        std::snprintf(num, sizeof num, "cx0=%.6f\ncx1=%.6f\nsaving=%.6f\nh=%.4f\nv=%.4f\n", cx[0], cx[1], saving,
                      cfg.h, cfg.v);
        write_file(data_dir() + "/mock_state.txt",
                   "configs=" + std::to_string(configs) + "\nimages=" + std::to_string(images) +
                   "\nrebuilds=" + std::to_string(rebuilds) + "\n" + num + eyes +
                   "enabled=" + std::to_string(cfg.enabled ? 1 : 0) + "\nformat=" + std::to_string(format) +
                   "\nacquired=" + std::to_string(acquired) + "\nreleased=" + std::to_string(released) +
                   "\nlast_index=" + std::to_string(last_index) + "\nsmooth_runs=" + std::to_string(smooth_runs) +
                   "\nstrength=" + std::to_string(cfg.smooth_strength) + "\nov_text=" + std::to_string(ov.text ? 1 : 0) +
                   "\nov_zones=" + std::to_string(ov.zones ? 1 : 0) + "\nov_line1=" + ov.line1 + "\nov_line2=" + ov.line2 +
                   "\nouter=" + std::to_string(cfg.outer) + "\nsmooth=" + std::to_string(cfg.smooth ? 1 : 0) + "\n");
    }
    std::string status() const override { return "gpu=none (mock)"; }
};
} // namespace

std::unique_ptr<Backend> create_backend(const XrSessionCreateInfo*, std::string* why_not) {
    const char* mock = std::getenv("OVALFFR_MOCK_BACKEND");
    if (mock && *mock == '1') return std::unique_ptr<Backend>(new MockBackend());
    if (why_not) *why_not = "no GPU backend for this platform yet (Windows D3D12 only so far; Vulkan is planned, see docs/LINUX_VULKAN_PLAN.md)";
    return nullptr;
}

} // namespace ovalffr
#endif
