// Windows only: list graphics adapters with DXGI. Kept in its own file because it must NOT see
// the vendored DirectX-Headers directory (see CMakeLists.txt): it uses the system DXGI headers.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>

#include "ovalffr_gpu.h"

namespace ovalffr {

namespace {

std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

uint64_t luid64(const LUID& l) { return ((uint64_t)(uint32_t)l.HighPart << 32) | (uint64_t)l.LowPart; }

GpuAdapter describe(IDXGIAdapter1* a) {
    GpuAdapter g;
    DXGI_ADAPTER_DESC1 d = {};
    if (SUCCEEDED(a->GetDesc1(&d))) {
        g.luid = luid64(d.AdapterLuid);
        g.vendor = d.VendorId;
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        g.vram = (uint64_t)d.DedicatedVideoMemory;
        g.name = utf8(d.Description);
    }
    return g;
}

} // namespace

GpuInfo dxgi_enumerate() {
    GpuInfo info;
    typedef HRESULT(WINAPI * PFN_CreateFactory1)(REFIID, void**);
    HMODULE dll = LoadLibraryW(L"dxgi.dll");
    if (!dll) return info;
    PFN_CreateFactory1 create = (PFN_CreateFactory1)GetProcAddress(dll, "CreateDXGIFactory1");
    IDXGIFactory1* f1 = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&f1)) || !f1) return info;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        info.adapters.push_back(describe(a));
        a->Release();
    }
    // Windows' own idea of the "high performance" adapter (needs Windows 10 1803+)
    IDXGIFactory6* f6 = nullptr;
    if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6)) && f6) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                               (void**)&a) == DXGI_ERROR_NOT_FOUND) break;
            GpuAdapter g = describe(a);
            a->Release();
            if (g.software) continue;
            for (size_t k = 0; k < info.adapters.size(); ++k)
                if (info.adapters[k].luid == g.luid) { info.best = (int)k; break; }
            if (info.best >= 0) break;
        }
        f6->Release();
    }
    f1->Release();
    return info;
}

} // namespace ovalffr
#endif
