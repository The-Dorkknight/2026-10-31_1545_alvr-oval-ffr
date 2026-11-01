// Graphics backend interface for the OvalFFR layer.
// A backend knows how to turn a shading-rate mask into real GPU state for one
// graphics API. The layer core (layer.cpp) is API-neutral.
#pragma once
#include <openxr/openxr.h>
#include <cstdint>
#include <memory>
#include <string>
#include "ovalffr_core.h"

namespace ovalffr {

struct Backend {
    virtual ~Backend() {}
    virtual const char* name() const = 0;
    // A colour swapchain was created / its images enumerated. `images` is the
    // runtime's array for the graphics API this backend handles.
    virtual void onSwapchainImages(XrSwapchain sc, uint32_t width, uint32_t height,
                                   uint32_t arraySize, const XrSwapchainImageBaseHeader* images,
                                   uint32_t count) = 0;
    virtual void onSwapchainDestroyed(XrSwapchain sc) = 0;
    // The app told the compositor which swapchain feeds which eye (from xrEndFrame).
    virtual void onSwapchainEye(XrSwapchain sc, uint32_t eye) = 0;
    // Optical-axis centre for an eye, 0..1 with y down (from runtime FOV).
    virtual void onEyeCentre(uint32_t eye, float cx, float cy) = 0;
    // Config changed (or first load). Rebuild masks.
    virtual void onConfig(const Config& cfg) = 0;
    virtual std::string status() const = 0; // one line for the status file / the UI
    // LUID of the graphics adapter the game's device is on, 0 if not known.
    virtual uint64_t adapterLuid() const { return 0; }

    // ---- Smooth periphery (optional; the defaults do nothing) ----------------
    // Called just before onSwapchainImages with the format the app asked for (a DXGI_FORMAT on
    // D3D12) and its sample count.
    virtual void onSwapchainFormat(XrSwapchain, int64_t /*format*/, uint32_t /*samples*/) {}
    // The app acquired image `index` of a swapchain.
    virtual void onAcquire(XrSwapchain, uint32_t /*index*/) {}
    // The app is about to hand the acquired image back. Its drawing is already queued, so this is
    // where the smoothing pass is queued behind it (before the runtime reads the image).
    virtual void onRelease(XrSwapchain) {}
    // One word plus detail for the status file: "off", "running", "unavailable: why", ...
    virtual std::string smoothStatus() const { return "unavailable: this graphics backend has no smoothing pass"; }

    // ---- In-headset overlay and VRS check (optional) -------------------------
    struct Overlay {
        bool text = false;    // draw the fps box
        bool zones = false;   // tint the low-res zones (and mark where VRS did not happen)
        std::string line1, line2;
        bool operator!=(const Overlay& o) const { return text != o.text || zones != o.zones || line1 != o.line1 || line2 != o.line2; }
    };
    virtual void onOverlay(const Overlay&) {}
    // VRS check since the last call: fraction (0..1) of sampled low-res blocks that were exact copies
    // in the image the runtime received. -1 = no data (the check only runs while the image is copied,
    // i.e. with Smooth low-res edge or the zone highlight on).
    virtual double takeVrsSeen() { return -1.0; }
};

// Returns a backend if `createInfo->next` holds a graphics binding it supports,
// otherwise nullptr (the layer then stays a pure pass-through).
std::unique_ptr<Backend> create_backend(const XrSessionCreateInfo* createInfo,
                                        std::string* why_not);

void log_line(const char* fmt, ...);

} // namespace ovalffr
