#include "ovalffr_gpu.h"

#include <cstdio>

#ifdef _WIN32
namespace ovalffr { GpuInfo dxgi_enumerate(); } // gpu_dxgi.cpp
#endif

namespace ovalffr {

namespace {

int index_of(const GpuInfo& info, uint64_t luid) {
    if (!luid) return -1;
    for (size_t i = 0; i < info.adapters.size(); ++i)
        if (info.adapters[i].luid == luid) return (int)i;
    return -1;
}

std::string name_of(const GpuInfo& info, int idx) {
    return idx >= 0 ? info.adapters[(size_t)idx].name : std::string("unknown");
}

// Pick the "gaming" adapter: Windows' own high-performance pick if we have it, otherwise the
// real (non-software) adapter with the most dedicated video memory.
int pick_best(const GpuInfo& info) {
    if (info.best >= 0 && info.best < (int)info.adapters.size() && !info.adapters[(size_t)info.best].software)
        return info.best;
    int best = -1;
    for (size_t i = 0; i < info.adapters.size(); ++i) {
        if (info.adapters[i].software) continue;
        if (best < 0 || info.adapters[i].vram > info.adapters[(size_t)best].vram) best = (int)i;
    }
    return best;
}

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

} // namespace

GpuInfo gpu_enumerate() {
#ifdef _WIN32
    return dxgi_enumerate();
#else
    return GpuInfo();
#endif
}

const char* gpu_verdict_name(GpuVerdict v) {
    switch (v) {
        case GpuVerdict::SingleGpu: return "single";
        case GpuVerdict::Ok: return "ok";
        case GpuVerdict::GameOnWeakGpu: return "game_on_weak";
        case GpuVerdict::RuntimeOnWeakGpu: return "runtime_on_weak";
        default: return "unknown";
    }
}

GpuAssessment gpu_assess(const GpuInfo& info, uint64_t game_luid, uint64_t runtime_luid) {
    GpuAssessment a;
    int real = 0;
    for (const GpuAdapter& g : info.adapters) real += g.software ? 0 : 1;
    int best = pick_best(info);
    // A LUID that is not in the list (virtual or remote adapter) is treated as "not known"
    // rather than as a problem: better to stay quiet than to raise a false alarm.
    int gi = index_of(info, game_luid), ri = index_of(info, runtime_luid);
    a.best = name_of(info, best);
    a.game = name_of(info, gi);
    a.runtime = name_of(info, ri);

    if (real == 0) { a.hint = "Could not list graphics cards."; return a; }
    if (real == 1) {
        a.verdict = GpuVerdict::SingleGpu;
        a.hint = "Only one graphics card: nothing to choose.";
        return a;
    }
    if (ri >= 0 && ri != best) {
        a.verdict = GpuVerdict::RuntimeOnWeakGpu;
        a.hint = "The VR runtime (SteamVR/ALVR) is rendering on " + a.runtime + " but " + a.best +
                 " is the gaming card. Use \"Use gaming GPU for SteamVR\" in the panel, then restart SteamVR.";
        return a;
    }
    if (gi >= 0 && gi != best) {
        a.verdict = GpuVerdict::GameOnWeakGpu;
        a.hint = "This game is running on " + a.game + ", not on " + a.best +
                 ". Set the game to High performance (the panel's \"Add program\" can do it) and restart it.";
        return a;
    }
    if (gi >= 0 || ri >= 0) {
        a.verdict = GpuVerdict::Ok;
        a.hint = "Game and VR runtime are on " + a.best + ".";
        return a;
    }
    a.hint = "Could not tell which card the game or the runtime is using.";
    return a;
}

std::string gpu_status_lines(const GpuInfo& info, const GpuAssessment& a, uint64_t game_luid, uint64_t runtime_luid) {
    std::string out;
    std::string list;
    for (const GpuAdapter& g : info.adapters) {
        if (!list.empty()) list += " | ";
        list += g.name + (g.software ? " (software)" : "");
    }
    out += "gpu_adapters=" + list + "\n";
    out += "gpu_best=" + a.best + "\n";
    out += "gpu_game=" + a.game + "\n";
    out += "gpu_runtime=" + a.runtime + "\n";
    out += "gpu_game_luid=" + hex(game_luid) + "\n";
    out += "gpu_runtime_luid=" + hex(runtime_luid) + "\n";
    out += std::string("gpu_verdict=") + gpu_verdict_name(a.verdict) + "\n";
    out += "gpu_hint=" + a.hint + "\n";
    return out;
}

} // namespace ovalffr
