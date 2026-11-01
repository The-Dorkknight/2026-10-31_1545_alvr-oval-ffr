// Dual-GPU (laptop) detection. The decision logic is portable and unit-tested; only the
// adapter enumeration (gpu_enumerate) is Windows-specific (DXGI).
//
// Why this exists: in OpenXR the *runtime* names the graphics adapter (LUID) the app must
// use. So on a laptop with an integrated and a gaming GPU, it is the runtime's process
// (SteamVR / ALVR) that decides, and the app just follows. The layer therefore reports
// which adapter the runtime asked for and which the game's device is really on, and the
// control panel can set the right programs to "High performance" in Windows.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ovalffr {

struct GpuAdapter {
    uint64_t luid = 0;        // (HighPart << 32) | LowPart
    unsigned vendor = 0;      // PCI vendor id: 0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel
    bool software = false;    // WARP / Microsoft Basic Render Driver
    uint64_t vram = 0;        // dedicated video memory in bytes
    std::string name;
};

struct GpuInfo {
    std::vector<GpuAdapter> adapters;
    int best = -1;            // index of Windows' "high performance" adapter, -1 if unknown
};

enum class GpuVerdict { Unknown, SingleGpu, Ok, GameOnWeakGpu, RuntimeOnWeakGpu };

struct GpuAssessment {
    GpuVerdict verdict = GpuVerdict::Unknown;
    std::string best, game, runtime, hint;
};

GpuInfo gpu_enumerate();   // Windows: DXGI. Elsewhere: empty.
GpuAssessment gpu_assess(const GpuInfo& info, uint64_t game_luid, uint64_t runtime_luid);
const char* gpu_verdict_name(GpuVerdict v);
// key=value lines for status-<pid>.txt (always ends with a newline)
std::string gpu_status_lines(const GpuInfo& info, const GpuAssessment& a, uint64_t game_luid, uint64_t runtime_luid);

} // namespace ovalffr
