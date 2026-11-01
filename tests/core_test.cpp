// Unit tests for the platform-independent core. Plain asserts, no framework.
#include "active_owner.h"
#include "ovalffr_core.h"
#include "ovalffr_dyn.h"
#include "ovalffr_gpu.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace ovalffr;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
    } while (0)

static uint8_t at(const std::vector<uint8_t>& m, int tw, int x, int y) { return m[(size_t)y * tw + x]; }

int main() {
    // config round trip + clamping
    Config c;
    Config r = parse_config(to_text(c));
    CHECK(r.enabled && std::fabs(r.h - 0.70f) < 1e-3 && r.outer == 2);
    Config w = parse_config("h=5\nv=-1\nshape=99\nouter=9\nband=-3\nenabled=0\n# comment\n");
    CHECK(w.h == 1.0f && w.v == 0.20f && w.shape == 6.0f && w.outer == 2 && w.band == 0.0f && !w.enabled);

    // mask geometry: centre sharp, corner coarsest
    int tw = 0, th = 0;
    auto m = make_mask(c, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(tw == 100 && th == 100 && m.size() == 10000);
    CHECK(at(m, tw, 50, 50) == RATE_1X1);
    CHECK(at(m, tw, 0, 0) == RATE_4X4);
    CHECK(at(m, tw, 99, 99) == RATE_4X4);
    CHECK(at(m, tw, 50, 0) != RATE_1X1); // top-centre is outside a 70% oval

    // disabled -> everything full rate, no saving
    Config off = c; off.enabled = false;
    auto mo = make_mask(off, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(estimated_saving(mo) == 0.0);

    // GPU that only supports 2x2 must never get a 4x4 code
    auto m2 = make_mask(c, 1600, 1600, 16, 0.5f, 0.5f, 1, &tw, &th);
    bool any4 = false;
    for (uint8_t v : m2) any4 |= (v == RATE_4X4);
    CHECK(!any4);
    CHECK(at(m2, tw, 0, 0) == RATE_2X2);

    // savings behave: smaller oval saves more; default is comfortably >= 10 %
    double s_default = estimated_saving(m);
    Config small = c; small.h = 0.5f; small.v = 0.5f;
    Config big = c; big.h = 0.95f; big.v = 0.95f;
    double s_small = estimated_saving(make_mask(small, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th));
    double s_big = estimated_saving(make_mask(big, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th));
    std::printf("saving: default=%.3f small=%.3f big=%.3f\n", s_default, s_small, s_big);
    CHECK(s_small > s_default && s_default > s_big);
    CHECK(s_default >= 0.10);

    // slot (high exponent) keeps more of the frame sharp than an oval of the same size
    Config slot = c; slot.shape = 4.0f;
    double s_slot = estimated_saving(make_mask(slot, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th));
    CHECK(s_slot < s_default);

    // non-square, non-multiple sizes round up to whole tiles
    auto m3 = make_mask(c, 1000, 600, 32, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(tw == 32 && th == 19 && m3.size() == (size_t)tw * th);

    // horizontal and vertical sliders are independent
    Config wide = c; wide.h = 0.95f; wide.v = 0.30f;
    auto mw = make_mask(wide, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(at(mw, tw, 50, 50) == RATE_1X1);
    CHECK(at(mw, tw, 85, 50) == RATE_1X1);   // far left/right of centre is sharp
    CHECK(at(mw, tw, 50, 15) != RATE_1X1);   // same distance up is not

    // optical axis from FOV
    float cx, cy;
    centre_from_fov(-0.8f, 0.8f, 0.8f, -0.8f, &cx, &cy);
    CHECK(std::fabs(cx - 0.5f) < 1e-4 && std::fabs(cy - 0.5f) < 1e-4);
    centre_from_fov(-0.9f, 0.7f, 0.8f, -0.8f, &cx, &cy);   // more field to the left
    CHECK(cx > 0.5f);
    centre_from_fov(0, 0, 0, 0, &cx, &cy);                  // degenerate -> safe fallback
    CHECK(cx == 0.5f && cy == 0.5f);

    // ---- v0.2.0 edge cases ---------------------------------------------------------------
    // NaN / inf in a config must fall back to the defaults, not turn the whole view low-res
    Config nanc = parse_config("h=nan\nv=inf\nshape=-inf\nband=nan\n");
    CHECK(std::fabs(nanc.h - 0.70f) < 1e-6 && std::fabs(nanc.v - 0.70f) < 1e-6 && nanc.shape == 2.0f && nanc.band == 0.5f);
    auto mn = make_mask(nanc, 1600, 1600, 16, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(estimated_saving(mn) < 0.6);
    // NaN centre falls back to the middle
    auto mc = make_mask(c, 1600, 1600, 16, std::nanf(""), std::nanf(""), 2, &tw, &th);
    CHECK(at(mc, tw, 50, 50) == RATE_1X1);
    // tile size 0 is treated as 8 (the Python mirror must agree: tests/test_mask_parity.py)
    make_mask(c, 160, 160, 0, 0.5f, 0.5f, 2, &tw, &th);
    CHECK(tw == 20 && th == 20);

    // ActiveOwner: a second backend replaces the first; the first one's destructor must not clear it
    {
        int a = 0, b = 0;
        ActiveOwner<int> owner;
        owner.set(&a);
        owner.set(&b);       // new session created before the old one is destroyed
        owner.release(&a);   // old session destroyed
        CHECK(owner.get() == &b);
        owner.release(&b);
        CHECK(owner.get() == nullptr);
    }

    // ---- dual-GPU assessment -------------------------------------------------------------
    {
        GpuInfo gi;
        GpuAdapter intel; intel.luid = 0x100; intel.vendor = 0x8086; intel.vram = 128ull << 20; intel.name = "Intel UHD";
        GpuAdapter nv; nv.luid = 0x200; nv.vendor = 0x10de; nv.vram = 6ull << 30; nv.name = "NVIDIA RTX";
        GpuAdapter warp; warp.luid = 0x300; warp.software = true; warp.name = "Microsoft Basic Render Driver";
        gi.adapters = {intel, nv, warp};
        gi.best = 1;
        CHECK(gpu_assess(gi, 0x200, 0x200).verdict == GpuVerdict::Ok);
        CHECK(gpu_assess(gi, 0x100, 0x200).verdict == GpuVerdict::GameOnWeakGpu);
        CHECK(gpu_assess(gi, 0x100, 0x100).verdict == GpuVerdict::RuntimeOnWeakGpu);
        CHECK(gpu_assess(gi, 0x200, 0x100).verdict == GpuVerdict::RuntimeOnWeakGpu);   // runtime decides, so it wins
        CHECK(gpu_assess(gi, 0, 0x100).verdict == GpuVerdict::RuntimeOnWeakGpu);
        CHECK(gpu_assess(gi, 0x100, 0).verdict == GpuVerdict::GameOnWeakGpu);
        CHECK(gpu_assess(gi, 0, 0).verdict == GpuVerdict::Unknown);
        CHECK(gpu_assess(gi, 0x999, 0x888).verdict == GpuVerdict::Unknown);             // not in the list: stay quiet
        CHECK(gpu_assess(gi, 0x300, 0x200).verdict == GpuVerdict::GameOnWeakGpu);       // game on the software adapter
        CHECK(gpu_assess(gi, 0x200, 0x200).hint.find("NVIDIA RTX") != std::string::npos);
        GpuAssessment bad = gpu_assess(gi, 0x100, 0x100);
        CHECK(bad.hint.find("Intel UHD") != std::string::npos && bad.hint.find("NVIDIA RTX") != std::string::npos);
        // the "best" hint from Windows is ignored if it points at a software adapter
        GpuInfo gi2 = gi; gi2.best = 2;
        CHECK(gpu_assess(gi2, 0x200, 0x200).verdict == GpuVerdict::Ok);
        GpuInfo gi3 = gi; gi3.best = -1;                                                  // no hint: biggest VRAM wins
        CHECK(gpu_assess(gi3, 0x100, 0x100).verdict == GpuVerdict::RuntimeOnWeakGpu);
        // one real card (plus software) -> nothing to choose
        GpuInfo one; one.adapters = {nv, warp}; one.best = 0;
        CHECK(gpu_assess(one, 0x200, 0x200).verdict == GpuVerdict::SingleGpu);
        CHECK(gpu_assess(GpuInfo(), 0x1, 0x2).verdict == GpuVerdict::Unknown);
        std::string lines = gpu_status_lines(gi, bad, 0x100, 0x100);
        CHECK(lines.find("gpu_verdict=runtime_on_weak\n") != std::string::npos);
        CHECK(lines.find("gpu_runtime_luid=0x100\n") != std::string::npos);
        CHECK(lines.find("Intel UHD | NVIDIA RTX | Microsoft Basic Render Driver (software)") != std::string::npos);
    }

    // ---- Auto foveation controller (simulated frame timings) -----------------------------
    {
        // config keys round-trip and clamp
        Config a = parse_config("auto=1\nauto_floor=0.1\n");
        CHECK(a.auto_fov && a.auto_floor == 0.40f);
        CHECK(parse_config("auto_floor=nan\n").auto_floor == 0.60f);
        CHECK(!parse_config(to_text(Config())).auto_fov);
        CHECK(to_text(Config()).find("auto=0\nauto_floor=0.600\n") != std::string::npos);   // Python writes the same text
        Config on; on.auto_fov = true; on.auto_floor = 0.55f;
        Config back = parse_config(to_text(on));
        CHECK(back.auto_fov && std::fabs(back.auto_floor - 0.55f) < 1e-3);

        const double period = 1.0 / 90.0;
        // run the controller closed-loop: frame_time(scale) -> interval -> controller
        auto run = [&](DynamicFoveation& d, long frames, std::function<double(float)> frame_time, bool quantised) {
            for (long i = 0; i < frames; ++i) {
                double ft = frame_time(d.scale());
                double iv = quantised ? std::ceil(ft / period - 1e-9) * period : std::fmax(ft, period);
                d.update(iv, period);
            }
        };
        auto gpu_bound = [&](double load) {      // load = frame time at full size, in periods
            return [=](float s) { return load * period * (0.55 + 0.45 * s * s); };
        };

        // off: never moves
        { DynamicFoveation d; d.configure(false, 0.6f); run(d, 3000, gpu_bound(2.0), false);
          CHECK(d.scale() == 1.0f && std::string(d.state()) == "off"); }
        // healthy game: never moves
        { DynamicFoveation d; d.configure(true, 0.6f); run(d, 5000, gpu_bound(0.8), false);
          CHECK(d.scale() == 1.0f && d.changes() == 0); }
        // GPU-bound, smooth intervals: settles near on-time, not at the floor
        // (it deliberately tolerates up to 10% over the refresh period, so it stops at "close enough")
        { DynamicFoveation d; d.configure(true, 0.5f); run(d, 20000, gpu_bound(1.25), false);
          CHECK(d.scale() < 0.90f && d.scale() > 0.60f);
          CHECK(gpu_bound(1.25)(d.scale()) <= period * 1.10);
          CHECK(d.changes() <= 12);                               // no hunting
          std::printf("auto smooth: scale=%.2f changes=%d state=%s\n", d.scale(), d.changes(), d.state()); }
        // GPU-bound, runtime halves the rate on every miss (interval jumps 1x -> 2x): must still find the answer
        { DynamicFoveation d; d.configure(true, 0.5f); run(d, 20000, gpu_bound(1.25), true);
          CHECK(d.scale() < 0.80f && d.scale() > 0.60f && d.changes() <= 12);
          std::printf("auto quantised: scale=%.2f changes=%d state=%s\n", d.scale(), d.changes(), d.state()); }
        // CPU-bound: shrinking changes nothing -> back to 1.0 and locked, retries get slower
        { DynamicFoveation d; d.configure(true, 0.6f);
          run(d, 8000, [&](float) { return 2.0 * period; }, true);
          CHECK(d.scale() == 1.0f || std::string(d.state()) == "locked" || d.changes() > 0);
          long frames_low = 0, total = 40000; float mn = 1.0f;
          DynamicFoveation e; e.configure(true, 0.6f);
          for (long i = 0; i < total; ++i) { e.update(2.0 * period, period); if (e.scale() < 0.999f) ++frames_low; mn = std::fmin(mn, e.scale()); }
          std::printf("auto cpu-bound: spent %.1f%% of %ld frames below full size (min scale %.2f), changes=%d\n",
                      100.0 * frames_low / total, total, mn, e.changes());
          CHECK(frames_low < total / 6);                          // the penalty for a CPU-bound game stays small
          CHECK(mn >= 0.6f - 1e-6f); }
        // CPU-bound with heavy, spiky timing noise (a busy PC): a wrong "it helped" verdict must not stick
        { DynamicFoveation d; d.configure(true, 0.5f);
          unsigned long long r = 88172645463325252ULL; long low = 0, total = 60000;
          for (long i = 0; i < total; ++i) {
              r ^= r << 13; r ^= r >> 7; r ^= r << 17;
              double u = (double)(r >> 11) / 9007199254740992.0;
              double ratio = 2.0 + 1.2 * u + (u > 0.97 ? 4.0 * u : 0.0);          // 2.0..3.2 plus rare spikes
              d.update(ratio * period, period);
              if (d.scale() < 0.999f) ++low;
          }
          std::printf("auto noisy cpu-bound: %.1f%% of frames below full size, changes=%d\n", 100.0 * low / total, d.changes());
          CHECK(low < total / 4); }
        // hitches (loading, alt-tab) are ignored
        { DynamicFoveation d; d.configure(true, 0.6f);
          for (int i = 0; i < 2000; ++i) d.update(i % 50 == 0 ? 3.0 : period, period);   // 3 s every 50th frame
          CHECK(d.scale() == 1.0f); }
        // garbage input never moves it or crashes
        { DynamicFoveation d; d.configure(true, 0.6f);
          for (int i = 0; i < 1000; ++i) { d.update(std::nanf(""), period); d.update(period, 0.0); d.update(-1.0, period); d.update(1e300, 1e-300); }
          CHECK(d.scale() == 1.0f); }
        // load goes away: relaxes back to full size eventually
        { DynamicFoveation d; d.configure(true, 0.5f);
          run(d, 6000, gpu_bound(1.25), false);
          float low = d.scale(); CHECK(low < 0.85f);
          run(d, 80000, gpu_bound(0.5), false);
          CHECK(d.scale() == 1.0f); }
        // load that comes and goes every ~10 s: bounded number of changes (no wild hunting)
        { DynamicFoveation d; d.configure(true, 0.5f);
          for (int cycle = 0; cycle < 20; ++cycle) { run(d, 900, gpu_bound(1.25), false); run(d, 900, gpu_bound(0.6), false); }
          std::printf("auto on/off load: changes=%d final=%.2f\n", d.changes(), d.scale());
          CHECK(d.changes() < 150); }
        // floor raised by the user while below it
        { DynamicFoveation d; d.configure(true, 0.4f); run(d, 20000, gpu_bound(1.6), false);
          float before = d.scale(); d.configure(true, 0.9f);
          CHECK(before <= 0.6f && d.scale() >= 0.9f); }
    }

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("core_test: all checks passed\n");
    return 0;
}
