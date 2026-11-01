// Exercises the real layer shared library against a fake OpenXR runtime: loader
// negotiation, instance chaining, hooks, config hot-reload, FOV centring and the edge
// cases found in the v0.2.0 review (see docs/EDGE_CASES.md).
// Uses the mock backend (OVALFFR_MOCK_BACKEND=1), so no GPU or headset is needed.
#include <dlfcn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include "ovalffr_core.h"

static int failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL line %d: %s (%s)\n", __LINE__, #cond, msg); ++failures; } \
    } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string val(const std::string& text, const std::string& key) {
    auto i = text.find("\n" + key + "=");
    if (i == std::string::npos) { if (text.compare(0, key.size() + 1, key + "=") != 0) return ""; i = (size_t)-1; }
    size_t s = i + key.size() + 2;
    auto e = text.find('\n', s);
    return text.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
static double num(const std::string& text, const std::string& key) { return std::atof(val(text, key).c_str()); }
static void pause_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::string g_dir;
static std::string status_text() { return slurp(g_dir + "/status-" + std::to_string((long)getpid()) + ".txt"); }
static std::string mock_text() { return slurp(g_dir + "/mock_state.txt"); }
static void write_cfg(const ovalffr::Config& c, long pin_time = 0) {
    std::string p = g_dir + "/config.ini";
    ovalffr::write_file(p, ovalffr::to_text(c));
    if (pin_time) { struct utimbuf t; t.actime = t.modtime = pin_time; utime(p.c_str(), &t); }
}

// ---- fake runtime -----------------------------------------------------------
static XrInstance kInst = (XrInstance)(uintptr_t)0x1001;
static XrSession kSess = (XrSession)(uintptr_t)0x2002;
static XrSwapchain kSc0 = (XrSwapchain)(uintptr_t)0x3003, kSc1 = (XrSwapchain)(uintptr_t)0x3004;
static int g_endframes = 0;
static int g_wait_ms = 1;   // how long the fake runtime's xrWaitFrame takes
// The fake display period is 10 ms: healthy frames (1 ms) keep a huge margin against scheduler jitter,
// slow frames (20 ms) are clearly late.
static float g_jitter = 0.0f;

struct GfxReq { XrStructureType type; void* next; uint32_t low; int32_t high; uint32_t fl; };

static XrResult XRAPI_CALL f_DestroyInstance(XrInstance) { return XR_SUCCESS; }
static XrResult XRAPI_CALL f_CreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* s) { *s = kSess; return XR_SUCCESS; }
static XrResult XRAPI_CALL f_DestroySession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL f_CreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* s) {
    static int n = 0; *s = (n++ % 2) ? kSc1 : kSc0; return XR_SUCCESS; }
static XrResult XRAPI_CALL f_DestroySwapchain(XrSwapchain) { return XR_SUCCESS; }
static XrResult XRAPI_CALL f_EnumImages(XrSwapchain, uint32_t, uint32_t* out, XrSwapchainImageBaseHeader*) { *out = 3; return XR_SUCCESS; }
static XrResult XRAPI_CALL f_LocateViews(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t cap, uint32_t* out, XrView* v) {
    *out = 2;
    if (v && cap >= 2) {
        v[0].fov = {-0.95f + g_jitter, 0.65f, 0.80f, -0.80f};   // left eye sees more to the left
        v[1].fov = {-0.65f, 0.95f + g_jitter, 0.80f, -0.80f};
    }
    return XR_SUCCESS; }
static XrResult XRAPI_CALL f_EndFrame(XrSession, const XrFrameEndInfo*) { ++g_endframes; return XR_SUCCESS; }
static XrResult XRAPI_CALL f_WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* st) {
    std::this_thread::sleep_for(std::chrono::milliseconds(g_wait_ms));
    st->shouldRender = XR_TRUE; st->predictedDisplayPeriod = 10000000; st->predictedDisplayTime = 0; return XR_SUCCESS; }
static int g_acquires = 0, g_releases = 0;
static std::vector<XrSessionState> g_events;   // session-state events the fake runtime will report
static XrResult XRAPI_CALL f_PollEvent(XrInstance, XrEventDataBuffer* b) {
    if (g_events.empty()) return XR_EVENT_UNAVAILABLE;
    auto* e = (XrEventDataSessionStateChanged*)b;
    *e = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    e->session = kSess; e->state = g_events.front();
    g_events.erase(g_events.begin());
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL f_Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* i) { *i = (uint32_t)(g_acquires++ % 3); return XR_SUCCESS; }
static XrResult XRAPI_CALL f_Release(XrSwapchain, const XrSwapchainImageReleaseInfo*) { ++g_releases; return XR_SUCCESS; }
static XrResult XRAPI_CALL f_GetD3D12Req(XrInstance, XrSystemId, GfxReq* r) { r->low = 0x1234; r->high = 0; r->fl = 0xc000; return XR_SUCCESS; }

static XrResult XRAPI_CALL f_GetProc(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
#define M(N, F) if (!std::strcmp(name, N)) { *fn = (PFN_xrVoidFunction)F; return XR_SUCCESS; }
    M("xrDestroyInstance", f_DestroyInstance) M("xrCreateSession", f_CreateSession)
    M("xrDestroySession", f_DestroySession) M("xrCreateSwapchain", f_CreateSwapchain)
    M("xrDestroySwapchain", f_DestroySwapchain) M("xrEnumerateSwapchainImages", f_EnumImages)
    M("xrLocateViews", f_LocateViews) M("xrEndFrame", f_EndFrame) M("xrWaitFrame", f_WaitFrame)
    M("xrGetD3D12GraphicsRequirementsKHR", f_GetD3D12Req)
    M("xrAcquireSwapchainImage", f_Acquire) M("xrReleaseSwapchainImage", f_Release)
    M("xrPollEvent", f_PollEvent)
#undef M
    *fn = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XrResult XRAPI_CALL f_CreateApiLayerInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance* i) {
    *i = kInst; return XR_SUCCESS; }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: layer_test <layer.so> <tmpdir>\n"); return 2; }
    g_dir = argv[2];
    setenv("OVALFFR_DIR", g_dir.c_str(), 1);
    setenv("OVALFFR_MOCK_BACKEND", "1", 1);
    setenv("OVALFFR_TEST_FAST_AUTO", "1", 1);   // Auto foveation reacts in a few frames instead of seconds
    setenv("OVALFFR_TEST_FAST_AB", "1", 1);     // stats every 40 ms, A/B phases 0.3 s, log blocks 0.4 s

    void* lib = dlopen(argv[1], RTLD_NOW);
    CHECK(lib, dlerror());
    if (!lib) return 1;
    auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)dlsym(lib, "xrNegotiateLoaderApiLayerInterface");
    CHECK(negotiate, "negotiate symbol exported");
    if (!negotiate) return 1;

    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO};
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION; li.structSize = sizeof li;
    li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0); li.maxApiVersion = XR_CURRENT_API_VERSION;
    XrNegotiateApiLayerRequest rq = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST};
    rq.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION; rq.structSize = sizeof rq;

    CHECK(negotiate(&li, "wrong_name", &rq) != XR_SUCCESS, "rejects another layer's name");
    XrResult r = negotiate(&li, "XR_APILAYER_OVALFFR_oval_ffr", &rq);
    CHECK(r == XR_SUCCESS && rq.getInstanceProcAddr && rq.createApiLayerInstance, "negotiation succeeds");

    XrApiLayerNextInfo ni = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO};
    ni.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION; ni.structSize = sizeof ni;
    std::strcpy(ni.layerName, "XR_APILAYER_OVALFFR_oval_ffr");
    ni.nextGetInstanceProcAddr = f_GetProc; ni.nextCreateApiLayerInstance = f_CreateApiLayerInstance;
    XrApiLayerCreateInfo lci = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO};
    lci.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION; lci.structSize = sizeof lci; lci.nextInfo = &ni;
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "FakeVRGame");

    XrInstance inst = XR_NULL_HANDLE;
    r = rq.createApiLayerInstance(&ici, &lci, &inst);
    CHECK(r == XR_SUCCESS && inst == kInst, "instance creation chains through");

    auto getp = [&](const char* n) { PFN_xrVoidFunction f = nullptr; rq.getInstanceProcAddr(inst, n, &f); return f; };
    auto createSession = (PFN_xrCreateSession)getp("xrCreateSession");
    auto destroySession = (PFN_xrDestroySession)getp("xrDestroySession");
    auto createSwap = (PFN_xrCreateSwapchain)getp("xrCreateSwapchain");
    auto enumImgs = (PFN_xrEnumerateSwapchainImages)getp("xrEnumerateSwapchainImages");
    auto locate = (PFN_xrLocateViews)getp("xrLocateViews");
    auto endFrame = (PFN_xrEndFrame)getp("xrEndFrame");
    auto waitFrame = (PFN_xrWaitFrame)getp("xrWaitFrame");
    CHECK(waitFrame != nullptr, "xrWaitFrame is hooked");
    CHECK(createSession && createSwap && enumImgs && locate && endFrame, "hooked functions are returned");
    CHECK((void*)createSession != (void*)f_CreateSession, "xrCreateSession is really intercepted");
    CHECK(getp("xrGetSystem") == nullptr, "unknown functions pass through to the runtime's answer");

    // the runtime's adapter requirement is passed through unchanged and remembered
    typedef XrResult (XRAPI_PTR *PFN_req)(XrInstance, XrSystemId, GfxReq*);
    auto getReq = (PFN_req)getp("xrGetD3D12GraphicsRequirementsKHR");
    CHECK(getReq && (void*)getReq != (void*)f_GetD3D12Req, "xrGetD3D12GraphicsRequirementsKHR is intercepted");
    GfxReq req = {};
    if (getReq) { CHECK(getReq(inst, 1, &req) == XR_SUCCESS && req.low == 0x1234 && req.fl == 0xc000, "requirement passes through unchanged"); }

    // ---- session 1 -----------------------------------------------------------
    write_cfg(ovalffr::Config());
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    XrSession sess = XR_NULL_HANDLE;
    CHECK(createSession(inst, &sci, &sess) == XR_SUCCESS && sess == kSess, "xrCreateSession forwarded");
    std::string st = status_text();
    CHECK(val(st, "state") == "active" && val(st, "backend") == "mock", "status file says active/mock");
    CHECK(val(st, "app") == "FakeVRGame", "status file records the app name");
    CHECK(val(st, "gpu_runtime_luid") == "0x1234", "runtime adapter LUID is reported");
    CHECK(!val(st, "gpu_verdict").empty(), "a GPU verdict line is always present");

    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.width = 1600; sc.height = 1600; sc.arraySize = 1; sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = 29;   // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    XrSwapchain s0 = XR_NULL_HANDLE, s1 = XR_NULL_HANDLE;
    createSwap(sess, &sc, &s0); createSwap(sess, &sc, &s1);
    uint32_t n = 0;
    enumImgs(s0, 0, &n, nullptr);                       // size query: must not count as images
    XrSwapchainImageBaseHeader dummy[3] = {};
    enumImgs(s0, 3, &n, dummy);

    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t cnt = 0;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
    CHECK(locate(sess, &vli, &vs, 2, &cnt, views) == XR_SUCCESS && cnt == 2, "xrLocateViews forwarded");
    std::string ms = mock_text();
    CHECK(num(ms, "cx0") > 0.5 && num(ms, "cx1") < 0.5, "oval is centred on each eye's optical axis from the FOV");
    CHECK((int)num(ms, "images") == 3, "images counted once (size query ignored)");
    CHECK(num(ms, "saving") >= 0.10, "default config saves >= 10% of shading");

    // ---- eye association -----------------------------------------------------
    XrCompositionLayerProjectionView pv[4];
    for (auto& p : pv) p = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {(XrCompositionLayerBaseHeader*)&proj};
    XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO}; fe.layerCount = 1; fe.layers = layers;
    pv[0].subImage.swapchain = s0; pv[1].subImage.swapchain = s1;
    proj.viewCount = 2; proj.views = pv;
    CHECK(endFrame(sess, &fe) == XR_SUCCESS && g_endframes == 1, "xrEndFrame forwarded exactly once");
    ms = mock_text();
    CHECK(val(ms, "eye_3003") == "0" && val(ms, "eye_3004") == "1", "separate swapchains map to left/right eye");

    // EDGE: one array swapchain carries both eyes -> must use the shared (average) centre, not 'last eye wins'
    pv[0].subImage.swapchain = s0; pv[1].subImage.swapchain = s0; pv[1].subImage.imageArrayIndex = 1;
    endFrame(sess, &fe);
    ms = mock_text();
    CHECK(val(ms, "eye_3003") == "2", "EDGE array swapchain: both eyes share one mask (eye=2)");
    pv[1].subImage.imageArrayIndex = 0;

    // ---- config hot reload ---------------------------------------------------
    ovalffr::Config a; a.h = 0.70f; a.v = 0.70f;
    write_cfg(a, 1700000000);
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    CHECK(std::fabs(num(mock_text(), "h") - 0.70) < 1e-3, "config A loaded");
    ovalffr::Config b; b.h = 0.50f; b.v = 0.40f;
    write_cfg(b, 1700000000);                          // SAME mtime as A (two saves inside one second)
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    ms = mock_text();
    CHECK(std::fabs(num(ms, "h") - 0.50) < 1e-3 && std::fabs(num(ms, "v") - 0.40) < 1e-3,
          "EDGE two saves in the same second: the second one is not missed");

    ovalffr::write_file(g_dir + "/config.ini", "h=nan\nv=inf\nshape=-inf\nband=nan\n");
    { struct utimbuf t; t.actime = t.modtime = 1700000100; utime((g_dir + "/config.ini").c_str(), &t); }
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    ms = mock_text();
    CHECK(std::fabs(num(ms, "h") - 0.70) < 1e-3 && std::fabs(num(ms, "v") - 0.70) < 1e-3,
          "EDGE NaN/inf in config falls back to defaults instead of low-res everywhere");
    CHECK(num(ms, "saving") < 0.9, "EDGE NaN config does not turn the whole view into the lowest rate");

    remove((g_dir + "/config.ini").c_str());
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    CHECK(std::fabs(num(mock_text(), "h") - 0.70) < 1e-3, "deleted config file -> defaults");

    ovalffr::Config off; off.enabled = false;
    write_cfg(off, 1700000200);
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    CHECK(val(mock_text(), "enabled") == "0", "config hot-reload: disabled picked up");
    write_cfg(ovalffr::Config(), 1700000300);
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);

    // ---- FOV jitter ----------------------------------------------------------
    std::string before = val(mock_text(), "cx0");
    for (int i = 0; i < 40; ++i) {
        g_jitter = 0.0004f * (float)std::sin(i * 1.7);
        locate(sess, &vli, &vs, 2, &cnt, views);
    }
    g_jitter = 0.0f;
    CHECK(val(mock_text(), "cx0") == before, "EDGE a runtime that jitters its FOV every frame does not rebuild masks every frame");

    // ---- session 2: old session destroyed, new one created -------------------
    CHECK(destroySession(sess) == XR_SUCCESS, "xrDestroySession forwarded");
    remove((g_dir + "/mock_state.txt").c_str());
    CHECK(createSession(inst, &sci, &sess) == XR_SUCCESS, "second session forwarded");
    CHECK(val(status_text(), "state") == "active", "second session is active again");
    createSwap(sess, &sc, &s0); createSwap(sess, &sc, &s1);
    enumImgs(s0, 3, &n, dummy);
    locate(sess, &vli, &vs, 2, &cnt, views);
    ms = mock_text();
    CHECK(num(ms, "cx0") > 0.5, "EDGE the new session's backend gets the lens centres (not skipped by the change filter)");

    // ---- Auto foveation, closed loop against the fake runtime --------------------------
    {
        XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        auto frame = [&](int ms) { g_wait_ms = ms; return waitFrame(sess, &wi, &fs); };
        auto load = [&](const ovalffr::Config& c, long pin) { write_cfg(c, pin); pause_ms(450); locate(sess, &vli, &vs, 2, &cnt, views); };

        ovalffr::Config au; au.h = 0.70f; au.v = 0.70f; au.auto_fov = true; au.auto_floor = 0.50f;
        load(au, 1700000500);
        CHECK(val(status_text(), "auto") == "1", "auto foveation: status says it is on");
        for (int i = 0; i < 20; ++i) frame(1);
        CHECK(std::fabs(num(mock_text(), "h") - 0.70) < 1e-3, "auto: a healthy game keeps the full sharp area");

        // GPU-bound game: slow while the sharp area is big, fast once the layer has shrunk it
        for (int i = 0; i < 400; ++i) {
            frame(num(mock_text(), "h") > 0.45 ? 20 : 1);
            if (num(mock_text(), "h") <= 0.45 && i > 30) break;
        }
        for (int i = 0; i < 150; ++i) frame(num(mock_text(), "h") > 0.45 ? 20 : 1);
        double h_after = num(mock_text(), "h");
        CHECK(h_after <= 0.46 && h_after >= 0.35 - 1e-6, "auto: GPU-bound game gets a smaller sharp area (within the floor)");
        CHECK(num(status_text(), "auto_scale") < 0.7 && num(status_text(), "auto_scale") >= 0.5 - 1e-6, "auto: status shows the scale");
        CHECK(slurp(g_dir + "/layer.log").find("auto foveation: sharp area now") != std::string::npos, "auto: change is logged");

        // CPU-bound game: shrinking never helps -> must give the quality back and stop trying
        load(ovalffr::Config(), 1700000600);             // auto off: resets
        CHECK(std::fabs(num(mock_text(), "h") - 0.70) < 1e-3 && val(status_text(), "auto") == "0", "auto off restores full size");
        load(au, 1700000700);                            // auto on again: fresh controller
        bool locked = false;
        for (int i = 0; i < 600 && !locked; ++i) { frame(20); locked = val(status_text(), "auto_state") == "locked"; }
        CHECK(locked, "auto: a CPU-bound game ends up 'locked' (no benefit found)");
        CHECK(std::fabs(num(mock_text(), "h") - 0.70) < 1e-3, "auto: full quality is restored when shrinking did not help");

        load(ovalffr::Config(), 1700000800);   // leave auto off for the rest of the test
        g_wait_ms = 1;
    }

    // ---- Smooth periphery: acquire/release plumbing ----------------------------------------
    {
        auto acquire = (PFN_xrAcquireSwapchainImage)getp("xrAcquireSwapchainImage");
        auto release = (PFN_xrReleaseSwapchainImage)getp("xrReleaseSwapchainImage");
        CHECK(acquire && (void*)acquire != (void*)f_Acquire, "xrAcquireSwapchainImage is intercepted");
        CHECK(release && (void*)release != (void*)f_Release, "xrReleaseSwapchainImage is intercepted");
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        auto load = [&](const ovalffr::Config& c, long pin) { write_cfg(c, pin); pause_ms(450); locate(sess, &vli, &vs, 2, &cnt, views); };
        auto cycle = [&](int frames) {
            for (int i = 0; i < frames; ++i) { uint32_t idx = 99; acquire(s0, &ai, &idx); release(s0, &ri); }
        };

        CHECK(val(status_text(), "smooth") == "0" && val(status_text(), "smooth_state") == "off", "smooth: off by default");
        int rel0 = g_releases;
        cycle(5);
        std::string m = mock_text();
        CHECK(g_releases == rel0 + 5, "smooth: every release still reaches the runtime");
        CHECK(num(m, "smooth_runs") == 0, "smooth: no pass is queued while the option is off");
        CHECK(num(m, "format") == 29, "smooth: the swapchain format reaches the backend");
        CHECK(num(m, "acquired") >= 5 && num(m, "released") >= 5, "smooth: acquire and release reach the backend");

        ovalffr::Config sm; sm.smooth = true; sm.smooth_strength = 0.40f;
        load(sm, 1700000900);
        CHECK(val(status_text(), "smooth") == "1", "smooth: status says it is on");
        cycle(4);
        m = mock_text();
        CHECK(num(m, "smooth_runs") == 4, "smooth: one pass per released image");
        CHECK(std::fabs(num(m, "strength") - 0.40) < 1e-3, "smooth: strength reaches the backend");
        CHECK(val(status_text(), "smooth_state") == "running", "smooth: status shows it running");
        CHECK(slurp(g_dir + "/layer.log").find("smooth periphery: running") != std::string::npos, "smooth: state change is logged");

        ovalffr::Config smOff = sm; smOff.enabled = false;   // foveation off: nothing to smooth
        load(smOff, 1700001000);
        cycle(3);
        CHECK(num(mock_text(), "smooth_runs") == 4, "smooth: no pass while foveation itself is off");

        load(ovalffr::Config(), 1700001100);
        cycle(2);
        CHECK(num(mock_text(), "smooth_runs") == 4 && val(status_text(), "smooth_state") == "off", "smooth: switching it off stops the pass");
    }

    // ---- live fps, fps log, overlay, commands, focus, A/B test ---------------------------------
    {
        XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        auto frames = [&](int count, int ms) { g_wait_ms = ms; for (int i = 0; i < count; ++i) waitFrame(sess, &wi, &fs); };
        auto run_for = [&](int total_ms) {   // frames for a while (covers several 40 ms stats ticks)
            auto t = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t < std::chrono::milliseconds(total_ms)) frames(1, 3);
        };
        auto load = [&](const ovalffr::Config& c, long pin) { write_cfg(c, pin); pause_ms(450); locate(sess, &vli, &vs, 2, &cnt, views); };
        auto command = [&](const char* c) {
            ovalffr::write_file(g_dir + "/commands.txt", std::to_string((long long)time(nullptr)) + " " + c + "\n");
            pause_ms(420);
            frames(2, 1);
        };
        auto pollEvent = (PFN_xrPollEvent)getp("xrPollEvent");
        CHECK(pollEvent && (void*)pollEvent != (void*)f_PollEvent, "xrPollEvent is intercepted");
        remove((g_dir + "/fps_log.csv").c_str());

        load(ovalffr::Config(), 1700001200);
        frames(40, 4);
        std::string st2 = status_text();
        double fps = num(st2, "fps");
        std::printf("live fps measured: %.1f\n", fps);
        CHECK(fps > 20 && fps < 400, "live fps is measured");
        CHECK(std::fabs(num(st2, "hz") - 100.0) < 0.5, "refresh rate from the runtime's display period (100 Hz)");
        CHECK(val(mock_text(), "ov_text") == "0", "fps box is off by default");

        // fps box on through a command (same path as the panel's mini window)
        command("overlay");
        run_for(600);                                          // past the 0.5 s flash
        std::string m = mock_text();
        CHECK(val(m, "ov_text") == "1", "overlay command shows the fps box");
        CHECK(val(m, "ov_line1").find("FPS / 100 HZ  FFR ON") != std::string::npos, "line 1 shows fps, Hz and FFR state");
        CHECK(ovalffr::parse_config(slurp(g_dir + "/config.ini")).overlay, "the fps box setting is saved to config.ini");
        CHECK(val(m, "ov_line2").find("SAVES") != std::string::npos, "line 2 shows the estimated saving");

        setenv("OVALFFR_TEST_VRS_SEEN", "0.93", 1);
        run_for(150);
        CHECK(val(mock_text(), "ov_line2").find("VRS SEEN 93%") != std::string::npos, "line 2 shows the VRS check");
        CHECK(std::fabs(num(status_text(), "vrs_seen") - 93.0) < 0.1, "status shows the VRS check");
        unsetenv("OVALFFR_TEST_VRS_SEEN");

        command("zones");
        m = mock_text();
        CHECK(val(m, "ov_zones") == "1" && val(status_text(), "zones") == "1", "zones command turns the highlight on");
        CHECK(val(m, "ov_line2") == "ZONES SHOWN", "a key or command flashes what changed");
        pause_ms(550); frames(3, 1);
        CHECK(val(mock_text(), "ov_line2") == "YELLOW 2X2 RED 4X4 PURPLE=FULL", "zones legend after the flash");
        command("zones");
        CHECK(val(mock_text(), "ov_zones") == "0", "zones off again");

        command("smaller");
        CHECK(std::fabs(num(mock_text(), "h") - 0.65) < 1e-3, "smaller: sharp area -5%");
        CHECK(val(mock_text(), "ov_line2") == "SHARP AREA 65% X 65%", "smaller: flash shows the size");
        command("edge");
        CHECK(num(mock_text(), "outer") == 1, "edge: 4x4 -> 2x2");
        command("smooth");
        CHECK(num(mock_text(), "smooth") == 1, "smooth command");
        command("ffr");
        CHECK(val(mock_text(), "enabled") == "0" && val(mock_text(), "ov_line1").find("FFR OFF") != std::string::npos, "ffr command switches FFR off");
        command("ffr");
        command("bogus");
        CHECK(slurp(g_dir + "/layer.log").find("unknown command ignored") != std::string::npos, "unknown commands are ignored and logged");
        // stale command (written long ago) is ignored
        ovalffr::write_file(g_dir + "/commands.txt", "1000 ffr\n");
        pause_ms(420); frames(2, 1);
        CHECK(val(mock_text(), "enabled") == "1", "a stale command is ignored");

        // not focused: commands stay in the file for the focused app
        g_events = {XR_SESSION_STATE_VISIBLE};
        XrEventDataBuffer eb = {XR_TYPE_EVENT_DATA_BUFFER};
        CHECK(pollEvent(inst, &eb) == XR_SUCCESS, "event passes through");
        ovalffr::write_file(g_dir + "/commands.txt", std::to_string((long long)time(nullptr)) + " ffr\n");
        pause_ms(420); frames(2, 1);
        CHECK(val(mock_text(), "enabled") == "1" && !slurp(g_dir + "/commands.txt").empty(), "unfocused app leaves commands alone");
        g_events = {XR_SESSION_STATE_FOCUSED};
        pollEvent(inst, &eb);
        pause_ms(420); frames(2, 1);
        CHECK(val(mock_text(), "enabled") == "0", "focused again: the waiting command is applied");
        command("ffr");

        // fps log: one averaged row per setting block, not one per second
        load([] { ovalffr::Config c; c.overlay = true; return c; }(), 1700001300);
        remove((g_dir + "/fps_log.csv").c_str());
        run_for(3200);                                         // > 2 blocks of 1.5 s (2 minutes in real use)
        command("smaller");                                    // setting change: partial block written if long enough
        run_for(200);
        command("bigger");
        run_for(120);                                          // next stats tick writes the finished block
        std::string csv = slurp(g_dir + "/fps_log.csv");
        int rows = 0;
        for (char ch : csv) rows += ch == '\n';
        rows -= 1;   // header
        std::printf("fps log after ~4 s of frames and two changes: %d rows\n%s", rows, csv.c_str());
        CHECK(csv.rfind("start,seconds,app,ffr,h,v", 0) == 0, "fps log has its header");
        CHECK(rows >= 3 && rows <= 5, "fps log has a few averaged rows, not one per stats tick");
        CHECK(csv.find("setting changed before 2 minutes") != std::string::npos, "a shortened block is marked");
        CHECK(csv.find(",FakeVRGame,1,0.650,0.650,") != std::string::npos, "the smaller setting got its own row");

        // A/B test: off, on, off, on, then a result; the user's settings come back
        command("abtest");
        bool saw_off = false, saw_on = false;
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1700)) {
            frames(1, 2);
            std::string ab = val(status_text(), "ab");
            saw_off |= ab == "running-off" && val(mock_text(), "enabled") == "0";
            saw_on |= ab == "running-on" && val(mock_text(), "enabled") == "1";
            if (ab == "idle" && saw_on) break;
        }
        frames(3, 1);
        st2 = status_text();
        CHECK(saw_off && saw_on, "A/B test switches FFR off and on");
        CHECK(val(st2, "ab") == "idle" && val(st2, "ab_result").find("FFR off") != std::string::npos, "A/B result in the status");
        CHECK(val(mock_text(), "enabled") == "1", "settings restored after the A/B test");
        CHECK(val(mock_text(), "ov_line2").rfind("A/B:", 0) == 0, "A/B result shown in the headset");
        CHECK(slurp(g_dir + "/fps_log.csv").find("A/B test: FFR on phases") != std::string::npos, "A/B result in the fps log");
        command("abtest");
        command("abtest");                                     // second press stops it
        CHECK(val(status_text(), "ab") == "idle", "pressing the A/B key again stops the test");

        load(ovalffr::Config(), 1700001400);
        g_wait_ms = 1;
    }

    // EDGE: quad views (3+ views) means the app already foveates: stay out of the way
    XrCompositionLayerProjectionView q[4];
    for (auto& p : q) p = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    proj.viewCount = 4; proj.views = q;
    endFrame(sess, &fe);
    CHECK(val(mock_text(), "enabled") == "0", "EDGE quad-views app: foveation switched off");
    st = status_text();
    CHECK(val(st, "state") == "passthrough" && val(st, "reason").find("quad") != std::string::npos, "EDGE quad-views reason is reported");
    write_cfg(ovalffr::Config(), 1700000400);
    pause_ms(450);
    locate(sess, &vli, &vs, 2, &cnt, views);
    CHECK(val(mock_text(), "enabled") == "0", "EDGE quad-views suppression survives a config reload");

    // ---- pass-through when no backend ---------------------------------------
    destroySession(sess);
    unsetenv("OVALFFR_MOCK_BACKEND");
    CHECK(createSession(inst, &sci, &sess) == XR_SUCCESS, "third session forwarded");
    st = status_text();
    CHECK(val(st, "state") == "passthrough" && !val(st, "reason").empty(), "pass-through reported with a reason");

    auto destroyInst = (PFN_xrDestroyInstance)getp("xrDestroyInstance");
    CHECK(destroyInst(inst) == XR_SUCCESS, "xrDestroyInstance forwarded");
    CHECK(val(status_text(), "ended") == "1", "status file is marked ended when the instance is destroyed");
    CHECK(slurp(g_dir + "/layer.log").find("layer attached to app 'FakeVRGame'") != std::string::npos, "layer.log records the attach");

    dlclose(lib);
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("layer_test: all checks passed\n");
    return 0;
}
