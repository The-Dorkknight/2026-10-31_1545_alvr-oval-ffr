// OvalFFR - OpenXR API layer (implicit). Loader negotiation, call interception,
// hot-reloaded config, status reporting. The GPU work lives in a Backend (backend_*.cpp).
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "backend.h"
#include "ovalffr_core.h"
#include "ovalffr_gpu.h"
#include "ovalffr_dyn.h"

#ifdef _WIN32
#define OVALFFR_EXPORT extern "C" __declspec(dllexport)
#include <windows.h>
#include <process.h>
#else
#define OVALFFR_EXPORT extern "C" __attribute__((visibility("default")))
#include <unistd.h>
#endif

#define LAYER_NAME "XR_APILAYER_OVALFFR_oval_ffr"

namespace ovalffr {

static long process_id() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

static std::string sep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

static std::mutex g_log_mutex;
void log_line(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::string dir = data_dir();
    if (dir.empty()) return;
    std::string path = dir + sep() + "layer.log";
    // keep the log small: restart it once it passes ~256 KB
    struct stat st;
    const char* mode = (stat(path.c_str(), &st) == 0 && st.st_size > 256 * 1024) ? "w" : "a";
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) return;
    std::time_t t = std::time(nullptr);
    std::tm tmv = {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(f, "[%s] [pid %ld] ", ts, process_id());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// Same layout as XrGraphicsRequirementsD3D12KHR / ...D3D11KHR (type, next, LUID, feature level).
// Declared here so this file needs no Direct3D headers.
struct GfxRequirements {
    XrStructureType type;
    void* next;
    uint32_t luidLow;
    int32_t luidHigh;
    uint32_t minFeatureLevel;
};
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(GfxRequirements) == 32, "must match XrGraphicsRequirementsD3D12KHR on 64-bit");
#endif
typedef XrResult(XRAPI_PTR* PFN_GetGfxReq)(XrInstance, XrSystemId, GfxRequirements*);

struct Next {
    PFN_xrGetInstanceProcAddr getProc = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_GetGfxReq GetD3D12Req = nullptr;
    PFN_GetGfxReq GetD3D11Req = nullptr;
};

struct SwapInfo { uint32_t w = 0, h = 0, arr = 1, samples = 1; int64_t format = 0; bool colour = false; };

// OVALFFR_TEST_FAST_AUTO=1 (tests only) makes Auto foveation react within a handful of frames.
static DynParams dyn_params() {
    DynParams p;
    const char* f = std::getenv("OVALFFR_TEST_FAST_AUTO");
    if (f && *f == '1') {
        p.warmup_frames = 6; p.window = 8; p.cooldown_windows = 1; p.relax_windows = 6;
        p.lock_windows = 8; p.max_lock_windows = 64; p.rejudge_windows = 4; p.ceiling_windows = 40;
    }
    return p;
}

// Live fps statistics and the A/B test. OVALFFR_TEST_FAST_AB=1 (tests only) shrinks the timings.
struct Timings {
    double stats_s = 1.0, ab_phase_s = 10.0, ab_warmup_s = 3.0, flash_s = 3.0, result_s = 30.0;
    double log_block_s = 120.0;     // fps log: one averaged row per setting every 2 minutes...
    double log_min_s = 20.0;        // ...or when the setting changes, if it ran at least this long
};
static Timings timings() {
    Timings t;
    const char* f = std::getenv("OVALFFR_TEST_FAST_AB");
    if (f && *f == '1') {
        t.stats_s = 0.04; t.ab_phase_s = 0.30; t.ab_warmup_s = 0.08; t.flash_s = 0.5; t.result_s = 5.0;
        t.log_block_s = 1.50; t.log_min_s = 0.10;
    }
    return t;
}

using Clock = std::chrono::steady_clock;

struct AbTest {                 // FFR off / on / off / on, the first seconds of each phase ignored
    bool running = false;
    int phase = 0;              // even = FFR off, odd = FFR on
    Clock::time_point t0{};
    double frames[2] = {0, 0}, time[2] = {0, 0}, wait[2] = {0, 0};
    std::string result, short_result;
    Clock::time_point result_at{};
    bool have_result = false;
};

struct State {
    std::mutex m;
    Next next;
    XrInstance instance = XR_NULL_HANDLE;
    std::string app_name;
    std::unique_ptr<Backend> backend;
    std::string why_not;                 // reason there is no backend
    std::map<XrSwapchain, SwapInfo> swaps;
    Config cfg;
    std::string cfg_text;                // last file content seen (compared by content, see poll_config_locked)
    bool cfg_have = false;
    std::chrono::steady_clock::time_point last_poll{};
    float fov_cx[2] = {-1.0f, -1.0f}, fov_cy[2] = {-1.0f, -1.0f};   // -1 = "not applied yet"
    bool suppressed = false;
    std::string suppress_reason;
    uint64_t runtime_luid = 0;
    std::string gpu_lines;
    bool ended = false;
    DynamicFoveation dyn{dyn_params()};   // Auto foveation
    std::chrono::steady_clock::time_point last_wait{};
    bool have_wait = false;
    std::string last_smooth;              // smooth_state last written to the status file
    // live statistics (always measured while a backend is active)
    Timings tm = timings();
    Clock::time_point stat_t0{};
    int stat_frames = 0, stat_missed = 0;
    double stat_wait = 0, period = 0;
    double fps = -1, hz = 0, missed_pct = 0, spare_ms = 0, vrs_seen = -1, saving = 0;
    // overlay, keys and commands
    bool zones = false;                   // zone highlight (not saved; Ctrl+Alt+Z)
    bool focused = true;                  // only the focused VR app reacts to keys and commands
    std::string flash;                    // short message after a key press
    Clock::time_point flash_until{};
    Backend::Overlay sent_overlay;
    AbTest ab;
    // fps log: the current block of seconds with unchanged settings
    struct Block {
        std::string sig;                  // the settings this block measures
        Config cfg;
        std::time_t start = 0;
        double frames = 0, time = 0, missed = 0, wait = 0, vrs_sum = 0, worst = -1, scale_sum = 0;
        int vrs_n = 0, ticks = 0;
    } block;
};

static State g;

static std::string config_path() { return data_dir() + sep() + "config.ini"; }

static Config effective_config_locked() {
    Config c = g.cfg;
    if (g.ab.running) {
        c.enabled = (g.ab.phase % 2) == 1;   // A/B test: your sliders, FFR off and on in turn, no Auto
    } else {
        c.h *= g.dyn.scale();     // Auto foveation (1.0 unless it is on and frames are being missed)
        c.v *= g.dyn.scale();
    }
    if (g.suppressed) c.enabled = false;
    return c;
}

// Hand the effective config to the backend and refresh the "saves about N %" figure.
static void push_config_locked() {
    Config e = effective_config_locked();
    int tw, th;
    g.saving = e.enabled ? estimated_saving(make_mask(e, 2000, 2000, 16, 0.5f, 0.5f, 2, &tw, &th)) : 0.0;
    if (g.backend) g.backend->onConfig(e);
}

static void write_status_locked() {
    std::time_t t = std::time(nullptr);
    std::tm tmv = {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    std::string s = std::string("time=") + ts + "\napp=" + g.app_name + "\nversion=" OVALFFR_VERSION "\npid=" +
                    std::to_string(process_id()) + "\n";
    if (g.backend && !g.suppressed) {
        s += "state=active\nbackend=" + std::string(g.backend->name()) + "\n" + g.backend->status() + "\n";
    } else if (g.backend && g.suppressed) {
        s += "state=passthrough\nreason=" + g.suppress_reason + "\n";
    } else {
        s += "state=passthrough\nreason=" + (g.why_not.empty() ? std::string("no session yet") : g.why_not) + "\n";
    }
    s += g.gpu_lines;
    {
        char ab[96];
        std::snprintf(ab, sizeof ab, "auto=%d\nauto_scale=%.3f\nauto_state=%s\n", g.cfg.auto_fov ? 1 : 0,
                      (double)g.dyn.scale(), g.dyn.state());
        s += ab;
    }
    {
        char lb[400];
        Config e = effective_config_locked();
        std::snprintf(lb, sizeof lb, "fps=%.1f\nhz=%.1f\nmissed_pct=%.1f\nspare_ms=%.2f\nvrs_seen=%.1f\nffr_now=%d\nsaving=%.3f\n"
                      "overlay=%d\nzones=%d\nab=%s\n",
                      g.fps, g.hz, g.missed_pct, g.spare_ms, g.vrs_seen, e.enabled ? 1 : 0, g.saving,
                      g.cfg.overlay ? 1 : 0, g.zones ? 1 : 0,
                      g.ab.running ? (g.ab.phase % 2 ? "running-on" : "running-off") : "idle");
        s += lb;
        if (g.ab.have_result) s += "ab_result=" + g.ab.result + "\n";
    }
    s += std::string("smooth=") + (g.cfg.smooth ? "1" : "0") + "\n";
    if (g.backend) s += "smooth_state=" + (g.cfg.smooth ? g.backend->smoothStatus() : std::string("off")) + "\n";
    if (g.ended) s += "ended=1\n";
    // one file per process, so an overlay app cannot overwrite the game's status
    write_file(data_dir() + sep() + "status-" + std::to_string(process_id()) + ".txt", s);
}

static void process_commands_locked();

// Poll the config file at most every 400 ms. Caller holds g.m.
//
// The file is compared BY CONTENT, not by modification time: two slider saves inside the same
// second share an mtime on coarse filesystems, and the second one used to be missed.
static void poll_config_locked(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g.last_poll < std::chrono::milliseconds(400)) return;
    g.last_poll = now;
    process_commands_locked();   // commands.txt from the panel / mini window (A/B test, zones, ...)
    std::string text;
    bool have = read_file(config_path(), &text);
    if (!force && have == g.cfg_have && text == g.cfg_text) return;
    g.cfg_have = have;
    g.cfg_text = text;
    g.cfg = have ? parse_config(text) : Config();
    g.dyn.configure(g.cfg.auto_fov, g.cfg.auto_floor);
    push_config_locked();
    log_line("config %s: enabled=%d h=%.2f v=%.2f shape=%.1f band=%.2f outer=%d auto=%d floor=%.2f smooth=%d strength=%.2f",
             have ? "loaded" : "missing (defaults)", g.cfg.enabled, g.cfg.h, g.cfg.v, g.cfg.shape, g.cfg.band,
             g.cfg.outer, g.cfg.auto_fov ? 1 : 0, g.cfg.auto_floor,
             g.cfg.smooth ? 1 : 0, g.cfg.smooth_strength);
    write_status_locked();   // the panel shows the 'auto' line from here
}

static void suppress_locked(const char* reason) {
    if (g.suppressed) return;
    g.suppressed = true;
    g.suppress_reason = reason;
    push_config_locked();
    write_status_locked();
    log_line("foveation switched off for this session: %s", reason);
}


// ---- live statistics, fps log, overlay text -------------------------------------------------

static std::string csv_clean(const std::string& in) {
    std::string o = in;
    for (char& c : o) if (c == ',' || c == '\n' || c == '\r' || c == '"') c = ' ';
    return o;
}

// fps_log.csv: one row per setting, averaged over 2 minutes (or over the shorter time the setting
// was used, if at least 20 s). Loading screens and the A/B test's own phases are not mixed in.
static std::string block_sig(const Config& c) {
    char b[160];
    std::snprintf(b, sizeof b, "%d|%.3f|%.3f|%.2f|%.3f|%d|%d|%.2f|%d", c.enabled ? 1 : 0, c.h, c.v, c.shape, c.band, c.outer,
                  c.smooth ? 1 : 0, c.smooth_strength, c.auto_fov ? 1 : 0);
    return b;
}

static void write_log_row_locked(const char* note, std::time_t start, double secs, const Config& c, double fps,
                                 double worst, double missed_pct, double spare_ms, double vrs, double auto_scale) {
    std::string dir = data_dir();
    if (dir.empty()) return;
    std::string path = dir + sep() + "fps_log.csv";
    struct stat st;
    bool exists = stat(path.c_str(), &st) == 0;
    if (exists && st.st_size > 1024 * 1024) {           // keep it small: one old copy
        std::string old = dir + sep() + "fps_log.old.csv";
        std::remove(old.c_str());
        std::rename(path.c_str(), old.c_str());
        exists = false;
    }
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    if (!exists)
        std::fputs("start,seconds,app,ffr,h,v,shape,ring,edge,smooth,smooth_strength,auto,auto_scale,avg_fps,worst_fps,hz,"
                   "missed_pct,spare_ms,vrs_seen,note\n", f);
    std::tm tmv = {};
#ifdef _WIN32
    localtime_s(&tmv, &start);
#else
    localtime_r(&start, &tmv);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    char vb[16] = "", wb[16] = "", mb[16] = "";   // blank = not measured
    if (vrs >= 0) std::snprintf(vb, sizeof vb, "%.1f", vrs);
    if (worst >= 0) std::snprintf(wb, sizeof wb, "%.1f", worst);
    if (missed_pct >= 0) std::snprintf(mb, sizeof mb, "%.1f", missed_pct);
    std::fprintf(f, "%s,%.0f,%s,%d,%.3f,%.3f,%.1f,%.3f,%dx%d,%d,%.2f,%d,%.3f,%.1f,%s,%.1f,%s,%.2f,%s,%s\n", ts, secs,
                 csv_clean(g.app_name).c_str(), c.enabled ? 1 : 0, c.h, c.v, c.shape, c.band, 1 << c.outer, 1 << c.outer,
                 c.smooth ? 1 : 0, c.smooth_strength, c.auto_fov ? 1 : 0, auto_scale, fps, wb, g.hz, mb, spare_ms,
                 vb, csv_clean(note).c_str());
    std::fclose(f);
}

// Write the current block if it is long enough, then forget it.
static void flush_block_locked() {
    State::Block& b = g.block;
    if (b.ticks > 0 && b.time >= g.tm.log_min_s && b.frames > 0) {
        write_log_row_locked(b.time >= g.tm.log_block_s - 1e-6 ? "" : "setting changed before 2 minutes", b.start, b.time,
                             b.cfg, b.frames / b.time, b.worst, 100.0 * b.missed / b.frames, 1000.0 * b.wait / b.frames,
                             b.vrs_n ? b.vrs_sum / b.vrs_n : -1.0, b.scale_sum / b.ticks);
    }
    g.block = State::Block();
}

// One stats period (about a second) goes into the current block.
static void add_to_block_locked(double elapsed) {
    if (g.ab.running) { g.block = State::Block(); return; }   // the A/B test writes its own rows
    Config c = g.cfg;
    if (g.suppressed) c.enabled = false;
    std::string sig = block_sig(c);
    if (sig != g.block.sig) {
        flush_block_locked();
        g.block.sig = sig;
        g.block.cfg = c;
        g.block.start = std::time(nullptr);
    }
    State::Block& b = g.block;
    b.frames += g.stat_frames; b.time += elapsed; b.missed += g.stat_missed; b.wait += g.stat_wait;
    if (g.vrs_seen >= 0) { b.vrs_sum += g.vrs_seen; b.vrs_n += 1; }
    if (b.worst < 0 || g.fps < b.worst) b.worst = g.fps;
    b.scale_sum += g.dyn.scale();
    b.ticks += 1;
    if (b.time >= g.tm.log_block_s) {   // 2 minutes with this setting: write it and start the next block
        std::string keep = b.sig;
        Config kc = b.cfg;
        flush_block_locked();
        g.block.sig = keep; g.block.cfg = kc; g.block.start = std::time(nullptr);
    }
}

static void flash_locked(const std::string& msg) {
    g.flash = msg;
    g.flash_until = Clock::now() + std::chrono::milliseconds((int)(g.tm.flash_s * 1000));
}

// Called once per stats period with the frames counted since stat_t0.
static void finish_stats_locked(double elapsed) {
    g.fps = g.stat_frames / elapsed;
    g.hz = g.period > 0 ? 1.0 / g.period : 0.0;
    g.missed_pct = 100.0 * g.stat_missed / g.stat_frames;
    g.spare_ms = 1000.0 * g.stat_wait / g.stat_frames;
    double v = g.backend ? g.backend->takeVrsSeen() : -1.0;
    g.vrs_seen = v >= 0 ? 100.0 * v : -1.0;
    if (g.ab.running && std::chrono::duration<double>(Clock::now() - g.ab.t0).count() >= g.tm.ab_warmup_s) {
        int i = g.ab.phase % 2;
        g.ab.frames[i] += g.stat_frames; g.ab.time[i] += elapsed; g.ab.wait[i] += g.stat_wait;
    }
    add_to_block_locked(elapsed);
    write_status_locked();
}

static void ab_finish_locked() {
    AbTest& a = g.ab;
    a.running = false;
    if (a.time[0] <= 0 || a.time[1] <= 0 || a.frames[0] <= 0 || a.frames[1] <= 0) {
        a.result = "A/B test: not enough frames were measured";
        a.short_result = "A/B: NOT ENOUGH FRAMES";
    } else {
        double off = a.frames[0] / a.time[0], on = a.frames[1] / a.time[1];
        double soff = 1000.0 * a.wait[0] / a.frames[0], son = 1000.0 * a.wait[1] / a.frames[1];
        double pct = 100.0 * (on / off - 1.0);
        char b[256], sb[64];
        bool capped = g.hz > 0 && off >= 0.97 * g.hz && on >= 0.97 * g.hz;
        std::snprintf(b, sizeof b, "FFR off %.1f fps, on %.1f fps (%+.1f%%); spare time %.1f -> %.1f ms%s", off, on, pct,
                      soff, son, capped ? " - both already at the headset's full rate, so fps cannot rise; "
                                          "raise the render resolution to see a difference" : "");
        if (capped) std::snprintf(sb, sizeof sb, "A/B: BOTH MAX FPS SPARE %.1f>%.1fMS", soff, son);
        else std::snprintf(sb, sizeof sb, "A/B: OFF %.0f ON %.0f FPS %+.0f%%", off, on, pct);
        a.result = b;
        a.short_result = sb;
        Config c = g.cfg;
        c.enabled = false;
        write_log_row_locked("A/B test: FFR off phases", std::time(nullptr), a.time[0], c, off, -1, -1, soff, -1, 1.0);
        c.enabled = true;
        write_log_row_locked(("A/B test: FFR on phases; " + a.result).c_str(), std::time(nullptr), a.time[1], c, on, -1, -1,
                             son, -1, 1.0);
    }
    a.have_result = true;
    a.result_at = Clock::now();
    push_config_locked();
    write_status_locked();
    log_line("A/B test finished: %s", a.result.c_str());
}

static void ab_start_locked() {
    g.ab = AbTest();
    g.ab.running = true;
    g.ab.t0 = Clock::now();
    push_config_locked();
    write_status_locked();
    log_line("A/B test started (FFR off/on/off/on, %.0f s each)", g.tm.ab_phase_s);
}

static void ab_tick_locked(Clock::time_point now) {
    if (!g.ab.running) return;
    if (std::chrono::duration<double>(now - g.ab.t0).count() < g.tm.ab_phase_s) return;
    g.ab.phase += 1;
    g.ab.t0 = now;
    if (g.ab.phase >= 4) { ab_finish_locked(); return; }
    push_config_locked();
    write_status_locked();
}

// Save a changed setting so the panel and the next session see it too.
static void save_config_locked(const Config& c) {
    g.cfg = clamp(c);
    std::string text = to_text(g.cfg);
    write_file(config_path(), text);
    g.cfg_text = text;
    g.cfg_have = true;
    g.dyn.configure(g.cfg.auto_fov, g.cfg.auto_floor);
    push_config_locked();
    write_status_locked();
}

// Keys and commands.txt end up here. Returns false for an unknown command.
static bool apply_command_locked(const std::string& cmd) {
    Config c = g.cfg;
    char b[64];
    if (cmd == "overlay") { c.overlay = !c.overlay; save_config_locked(c); flash_locked(c.overlay ? "FPS BOX ON" : "FPS BOX OFF"); }
    else if (cmd == "zones") { g.zones = !g.zones; flash_locked(g.zones ? "ZONES SHOWN" : "ZONES HIDDEN"); write_status_locked(); }
    else if (cmd == "ffr") { c.enabled = !c.enabled; save_config_locked(c); flash_locked(c.enabled ? "FFR ON" : "FFR OFF"); }
    else if (cmd == "smooth") { c.smooth = !c.smooth; save_config_locked(c); flash_locked(c.smooth ? "SMOOTH EDGE ON" : "SMOOTH EDGE OFF"); }
    else if (cmd == "auto") { c.auto_fov = !c.auto_fov; save_config_locked(c); flash_locked(c.auto_fov ? "AUTO ON" : "AUTO OFF"); }
    else if (cmd == "edge") { c.outer = c.outer == 2 ? 1 : 2; save_config_locked(c); flash_locked(c.outer == 2 ? "EDGE 4X4" : "EDGE 2X2"); }
    else if (cmd == "bigger" || cmd == "smaller") {
        float d = cmd == "bigger" ? 0.05f : -0.05f;
        c.h = std::round((c.h + d) * 20.0f) / 20.0f;
        c.v = std::round((c.v + d) * 20.0f) / 20.0f;
        save_config_locked(c);
        std::snprintf(b, sizeof b, "SHARP AREA %.0f%% X %.0f%%", g.cfg.h * 100.0f, g.cfg.v * 100.0f);
        flash_locked(b);
    }
    else if (cmd == "abtest") {
        if (g.ab.running) { g.ab.running = false; push_config_locked(); write_status_locked(); flash_locked("A/B TEST STOPPED"); }
        else ab_start_locked();
    }
    else return false;
    log_line("command: %s", cmd.c_str());
    return true;
}

// commands.txt: lines "<unix time> <command>" written by the panel or the mini window. Only the
// focused VR app takes them, so a second program with the layer loaded cannot undo a toggle.
static void process_commands_locked() {
    if (!g.backend || !g.focused) return;
    std::string path = data_dir() + sep() + "commands.txt", text;
    if (!read_file(path, &text) || text.empty()) return;
    std::remove(path.c_str());
    long long now = (long long)std::time(nullptr);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string ln = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        size_t sp = ln.find(' ');
        if (sp == std::string::npos) continue;
        long long when = std::atoll(ln.substr(0, sp).c_str());
        if (when < now - 15 || when > now + 60) continue;   // stale (e.g. written while no game ran)
        if (!apply_command_locked(ln.substr(sp + 1))) log_line("unknown command ignored: %s", ln.c_str());
    }
}

#ifdef _WIN32
// Ctrl+Alt+key, read with GetAsyncKeyState once per frame (works while the game window is in the
// background, which is normal with a headset on). Only on a key going down.
static void poll_keys_locked() {
    static const struct { int vk; const char* cmd; } keys[] = {
        {'O', "overlay"}, {'Z', "zones"}, {'F', "ffr"}, {'S', "smooth"}, {'A', "auto"},
        {'E', "edge"}, {VK_OEM_4, "smaller"}, {VK_OEM_6, "bigger"}, {'T', "abtest"}};
    static bool down[sizeof keys / sizeof keys[0]] = {};
    bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        bool d = mods && (GetAsyncKeyState(keys[i].vk) & 0x8000);
        if (d && !down[i]) apply_command_locked(keys[i].cmd);
        down[i] = d;
    }
}
#endif

// Text and zone highlight for the headset. Sent to the backend only when something changed.
static void update_overlay_locked(Clock::time_point now) {
    if (!g.backend) return;
    Config e = effective_config_locked();
    Backend::Overlay o;
    bool flashing = now < g.flash_until;
    bool result_new = g.ab.have_result && now - g.ab.result_at < std::chrono::milliseconds((int)(g.tm.result_s * 1000));
    o.zones = g.zones && e.enabled;
    o.text = g.cfg.overlay || g.zones || g.ab.running || flashing || result_new;
    if (!o.text && !o.zones) {
        if (o != g.sent_overlay) { g.sent_overlay = o; g.backend->onOverlay(o); }
        return;
    }
    char b[64];
    if (g.fps >= 0) std::snprintf(b, sizeof b, "%.0f FPS / %.0f HZ  FFR %s", g.fps, g.hz, e.enabled ? "ON" : "OFF");
    else std::snprintf(b, sizeof b, "-- FPS  FFR %s", e.enabled ? "ON" : "OFF");
    o.line1 = b;
    if (flashing) o.line2 = g.flash;
    else if (g.ab.running) {
        double left = g.tm.ab_phase_s - std::chrono::duration<double>(now - g.ab.t0).count();
        std::snprintf(b, sizeof b, "A/B TEST %d/4: FFR %s %.0fS", g.ab.phase + 1, g.ab.phase % 2 ? "ON" : "OFF", left < 0 ? 0.0 : left);
        o.line2 = b;
    }
    else if (result_new) o.line2 = g.ab.short_result;
    else if (g.zones) o.line2 = "YELLOW 2X2 RED 4X4 PURPLE=FULL";
    else if (g.vrs_seen >= 0) { std::snprintf(b, sizeof b, "SAVES %.0f%%  VRS SEEN %.0f%%", g.saving * 100.0, g.vrs_seen); o.line2 = b; }
    else { std::snprintf(b, sizeof b, "SAVES %.0f%%  CTRL+ALT+Z ZONES", g.saving * 100.0); o.line2 = b; }
    if (o != g.sent_overlay) { g.sent_overlay = o; g.backend->onOverlay(o); }
}

// ---- intercepted calls -------------------------------------------------------

static XrResult XRAPI_CALL l_xrDestroyInstance(XrInstance instance) {
    PFN_xrDestroyInstance fn;
    {
        std::lock_guard<std::mutex> lk(g.m);
        fn = g.next.DestroyInstance;
        try { if (g.backend) flush_block_locked(); } catch (...) {}
        g.backend.reset();
        g.swaps.clear();
        g.ended = true;
        try { write_status_locked(); } catch (...) {}
    }
    log_line("instance destroyed");
    return fn ? fn(instance) : XR_ERROR_HANDLE_INVALID;
}

static XrResult XRAPI_CALL l_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* ci, XrSession* session) {
    XrResult r = g.next.CreateSession(instance, ci, session);
    if (XR_FAILED(r)) return r;
    std::lock_guard<std::mutex> lk(g.m);
    try {
        g.suppressed = false;
        g.suppress_reason.clear();
        g.ended = false;
        g.dyn.reset();
        g.have_wait = false;
        g.last_smooth.clear();
        g.zones = false;
        g.focused = true;
        g.ab = AbTest();
        g.fps = -1; g.vrs_seen = -1; g.stat_frames = 0;
        g.sent_overlay = Backend::Overlay();
        g.block = State::Block();
        g.fov_cx[0] = g.fov_cx[1] = g.fov_cy[0] = g.fov_cy[1] = -1.0f;   // a new backend needs the centres again
        std::string why;
        g.backend = create_backend(ci, &why);
        g.why_not = why;
        poll_config_locked(true);
        uint64_t game_luid = g.backend ? g.backend->adapterLuid() : 0;
        GpuInfo info = gpu_enumerate();
        GpuAssessment a = gpu_assess(info, game_luid, g.runtime_luid);
        g.gpu_lines = gpu_status_lines(info, a, game_luid, g.runtime_luid);
        push_config_locked();
        write_status_locked();
        if (g.backend) log_line("session created: backend %s", g.backend->name());
        else log_line("session created: pass-through (%s)", why.c_str());
        log_line("gpu: %s | game=%s runtime=%s best=%s", gpu_verdict_name(a.verdict), a.game.c_str(),
                 a.runtime.c_str(), a.best.c_str());
    } catch (...) {
        g.backend.reset();
        log_line("session setup failed; staying out of the way");
    }
    return r;
}

static XrResult XRAPI_CALL l_xrDestroySession(XrSession session) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        try { flush_block_locked(); } catch (...) {}
        g.backend.reset();
        g.swaps.clear();
    }
    return g.next.DestroySession(session);
}

static XrResult XRAPI_CALL l_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* sc) {
    XrResult r = g.next.CreateSwapchain(session, ci, sc);
    if (XR_SUCCEEDED(r)) {
        std::lock_guard<std::mutex> lk(g.m);
        SwapInfo si;
        si.w = ci->width; si.h = ci->height; si.arr = ci->arraySize ? ci->arraySize : 1;
        si.colour = (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        si.format = ci->format;
        si.samples = ci->sampleCount ? ci->sampleCount : 1;
        g.swaps[*sc] = si;
    }
    return r;
}

static XrResult XRAPI_CALL l_xrDestroySwapchain(XrSwapchain sc) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (g.backend) g.backend->onSwapchainDestroyed(sc);
        g.swaps.erase(sc);
    }
    return g.next.DestroySwapchain(sc);
}

static XrResult XRAPI_CALL l_xrEnumerateSwapchainImages(XrSwapchain sc, uint32_t capIn, uint32_t* capOut,
                                                        XrSwapchainImageBaseHeader* images) {
    XrResult r = g.next.EnumerateSwapchainImages(sc, capIn, capOut, images);
    if (XR_SUCCEEDED(r) && images && capOut && *capOut > 0) {
        std::lock_guard<std::mutex> lk(g.m);
        try {
            auto it = g.swaps.find(sc);
            if (g.backend && it != g.swaps.end() && it->second.colour) {
                g.backend->onSwapchainFormat(sc, it->second.format, it->second.samples);
                g.backend->onSwapchainImages(sc, it->second.w, it->second.h, it->second.arr, images, *capOut);
            }
        } catch (...) { log_line("onSwapchainImages failed"); }
    }
    return r;
}

static XrResult XRAPI_CALL l_xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* vs,
                                           uint32_t capIn, uint32_t* capOut, XrView* views) {
    XrResult r = g.next.LocateViews(session, info, vs, capIn, capOut, views);
    if (XR_SUCCEEDED(r) && views && capOut && *capOut > 0) {
        std::lock_guard<std::mutex> lk(g.m);
        try {
            if (g.backend) {
                for (uint32_t i = 0; i < *capOut && i < 2; ++i) {
                    float cx = 0.5f, cy = 0.5f;
                    if (g.cfg.centre_on_lens)
                        centre_from_fov(views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp,
                                        views[i].fov.angleDown, &cx, &cy);
                    // Ignore sub-0.4% wobble: some runtimes report a slightly different FOV every frame,
                    // and every change would rebuild masks (a GPU upload) inside this call.
                    if (std::fabs(cx - g.fov_cx[i]) > 0.004f || std::fabs(cy - g.fov_cy[i]) > 0.004f) {
                        g.fov_cx[i] = cx; g.fov_cy[i] = cy;
                        g.backend->onEyeCentre(i, cx, cy);
                    }
                }
                poll_config_locked(false);
            }
        } catch (...) { log_line("xrLocateViews handling failed"); }
    }
    return r;
}

// Smooth periphery: remember which image the app is drawing into...
static XrResult XRAPI_CALL l_xrAcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo* info,
                                                     uint32_t* index) {
    XrResult r = g.next.AcquireSwapchainImage(sc, info, index);
    if (XR_SUCCEEDED(r) && index) {
        std::lock_guard<std::mutex> lk(g.m);
        try { if (g.backend) g.backend->onAcquire(sc, *index); } catch (...) { log_line("onAcquire failed"); }
    }
    return r;
}

// ...and queue the pass behind the app's drawing BEFORE the runtime gets the image back.
static XrResult XRAPI_CALL l_xrReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo* info) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        try {
            if (g.backend && !g.suppressed) {
                g.backend->onRelease(sc);
                if (g.cfg.smooth) {
                    std::string st = g.backend->smoothStatus();
                    if (st != g.last_smooth) {   // only write the status file when the state really changes
                        g.last_smooth = st;
                        write_status_locked();
                        log_line("smooth periphery: %s", st.c_str());
                    }
                }
            }
        } catch (...) { log_line("onRelease failed"); }
    }
    return g.next.ReleaseSwapchainImage(sc, info);
}

// The time between two xrWaitFrame returns is the game's real frame interval (Auto foveation and
// the fps figures); the time spent inside xrWaitFrame is the "spare" time (rough headroom).
static XrResult XRAPI_CALL l_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    auto t_in = Clock::now();
    XrResult r = g.next.WaitFrame ? g.next.WaitFrame(session, info, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    auto now = Clock::now();
    if (XR_FAILED(r) || !state) return r;
    try {
        std::lock_guard<std::mutex> lk(g.m);
        if (g.backend && !g.suppressed && state->shouldRender) {
            double period = (double)state->predictedDisplayPeriod * 1e-9;
            if (g.have_wait) {
                double interval = std::chrono::duration<double>(now - g.last_wait).count();
                if (g.cfg.auto_fov && !g.ab.running && g.dyn.update(interval, period)) {
                    push_config_locked();
                    write_status_locked();
                    log_line("auto foveation: sharp area now %.0f%% of your sliders (%s)", g.dyn.scale() * 100.0,
                             g.dyn.state());
                }
                g.stat_frames += 1;
                if (period > 0 && interval > 1.35 * period) g.stat_missed += 1;
                g.stat_wait += std::chrono::duration<double>(now - t_in).count();
            } else {
                g.stat_t0 = now; g.stat_frames = 0; g.stat_missed = 0; g.stat_wait = 0;
            }
            g.last_wait = now;
            g.have_wait = true;
            g.period = period;
            double el = std::chrono::duration<double>(now - g.stat_t0).count();
            if (el >= g.tm.stats_s && g.stat_frames > 0) {
                finish_stats_locked(el);
                g.stat_t0 = now; g.stat_frames = 0; g.stat_missed = 0; g.stat_wait = 0;
            }
            ab_tick_locked(now);
            if (g.focused) {
#ifdef _WIN32
                poll_keys_locked();
#endif
                poll_config_locked(false);   // also picks up commands.txt
            }
            update_overlay_locked(now);
        } else {
            g.have_wait = false;   // idle: intervals would mean nothing
        }
    } catch (...) { log_line("xrWaitFrame handling failed"); }
    return r;
}

// Only the VR app that has input focus reacts to keys and commands.
static XrResult XRAPI_CALL l_xrPollEvent(XrInstance instance, XrEventDataBuffer* ev) {
    XrResult r = g.next.PollEvent(instance, ev);
    if (r == XR_SUCCESS && ev && ev->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto* e = (const XrEventDataSessionStateChanged*)ev;
        std::lock_guard<std::mutex> lk(g.m);
        g.focused = e->state == XR_SESSION_STATE_FOCUSED;
    }
    return r;
}

static XrResult XRAPI_CALL l_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    if (info && info->layers) {
        std::lock_guard<std::mutex> lk(g.m);
        try {
            if (g.backend) {
                for (uint32_t i = 0; i < info->layerCount; ++i) {
                    const XrCompositionLayerBaseHeader* l = info->layers[i];
                    if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                    auto* p = (const XrCompositionLayerProjection*)l;
                    if (p->viewCount > 2) {   // quad views: the app already renders a foveated inset
                        suppress_locked("app uses quad views (more than 2 views), so it already foveates");
                        break;
                    }
                    if (p->viewCount == 2 && p->views[0].subImage.swapchain == p->views[1].subImage.swapchain) {
                        g.backend->onSwapchainEye(p->views[0].subImage.swapchain, 2);   // one array swapchain, both eyes
                    } else {
                        for (uint32_t v = 0; v < p->viewCount; ++v)
                            g.backend->onSwapchainEye(p->views[v].subImage.swapchain, v);
                    }
                }
            }
        } catch (...) { log_line("xrEndFrame handling failed"); }
    }
    return g.next.EndFrame(session, info);
}

static XrResult XRAPI_CALL l_xrGetD3D12GraphicsRequirementsKHR(XrInstance instance, XrSystemId sys, GfxRequirements* req) {
    XrResult r = g.next.GetD3D12Req(instance, sys, req);
    if (XR_SUCCEEDED(r) && req) {
        std::lock_guard<std::mutex> lk(g.m);
        g.runtime_luid = ((uint64_t)(uint32_t)req->luidHigh << 32) | req->luidLow;   // the adapter the runtime wants
    }
    return r;
}

static XrResult XRAPI_CALL l_xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId sys, GfxRequirements* req) {
    XrResult r = g.next.GetD3D11Req(instance, sys, req);
    if (XR_SUCCEEDED(r) && req) {
        std::lock_guard<std::mutex> lk(g.m);
        g.runtime_luid = ((uint64_t)(uint32_t)req->luidHigh << 32) | req->luidLow;
    }
    return r;
}

static XrResult XRAPI_CALL l_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    if (!g.next.getProc) return XR_ERROR_HANDLE_INVALID;
    XrResult r = g.next.getProc(instance, name, fn);
    if (XR_FAILED(r) || !fn || !*fn) return r;
#define HOOK(NAME, SLOT)                                              \
    if (!std::strcmp(name, "xr" #NAME)) {                             \
        g.next.SLOT = (PFN_xr##NAME)*fn;                              \
        *fn = (PFN_xrVoidFunction)l_xr##NAME;                         \
        return XR_SUCCESS;                                            \
    }
    HOOK(DestroyInstance, DestroyInstance)
    HOOK(CreateSession, CreateSession)
    HOOK(DestroySession, DestroySession)
    HOOK(CreateSwapchain, CreateSwapchain)
    HOOK(DestroySwapchain, DestroySwapchain)
    HOOK(EnumerateSwapchainImages, EnumerateSwapchainImages)
    HOOK(LocateViews, LocateViews)
    HOOK(EndFrame, EndFrame)
    HOOK(WaitFrame, WaitFrame)
    HOOK(AcquireSwapchainImage, AcquireSwapchainImage)
    HOOK(ReleaseSwapchainImage, ReleaseSwapchainImage)
    HOOK(PollEvent, PollEvent)
#undef HOOK
    if (!std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR")) {
        g.next.GetD3D12Req = (PFN_GetGfxReq)*fn;
        *fn = (PFN_xrVoidFunction)l_xrGetD3D12GraphicsRequirementsKHR;
        return XR_SUCCESS;
    }
    if (!std::strcmp(name, "xrGetD3D11GraphicsRequirementsKHR")) {
        g.next.GetD3D11Req = (PFN_GetGfxReq)*fn;
        *fn = (PFN_xrVoidFunction)l_xrGetD3D11GraphicsRequirementsKHR;
        return XR_SUCCESS;
    }
    return r;
}

static XrResult XRAPI_CALL l_xrCreateApiLayerInstance(const XrInstanceCreateInfo* ci,
                                                      const XrApiLayerCreateInfo* layerInfo,
                                                      XrInstance* instance) {
    if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerNextInfo* nextInfo = layerInfo->nextInfo;
    XrApiLayerCreateInfo down = *layerInfo;
    down.nextInfo = nextInfo->next;
    XrResult r = nextInfo->nextCreateApiLayerInstance(ci, &down, instance);
    if (XR_FAILED(r)) return r;
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.next.getProc = nextInfo->nextGetInstanceProcAddr;
        g.instance = *instance;
        g.app_name = ci && ci->applicationInfo.applicationName[0] ? ci->applicationInfo.applicationName : "unknown";
        g.runtime_luid = 0;
        g.gpu_lines.clear();
        // Pre-resolve everything we call ourselves so a hooked name is never queried twice.
        PFN_xrVoidFunction f = nullptr;
#define RESOLVE(NAME, SLOT)                                                       \
    if (XR_SUCCEEDED(g.next.getProc(*instance, "xr" #NAME, &f)) && f) g.next.SLOT = (PFN_xr##NAME)f;
        RESOLVE(DestroyInstance, DestroyInstance)
        RESOLVE(CreateSession, CreateSession)
        RESOLVE(DestroySession, DestroySession)
        RESOLVE(CreateSwapchain, CreateSwapchain)
        RESOLVE(DestroySwapchain, DestroySwapchain)
        RESOLVE(EnumerateSwapchainImages, EnumerateSwapchainImages)
        RESOLVE(LocateViews, LocateViews)
        RESOLVE(EndFrame, EndFrame)
        RESOLVE(WaitFrame, WaitFrame)
        RESOLVE(AcquireSwapchainImage, AcquireSwapchainImage)
        RESOLVE(ReleaseSwapchainImage, ReleaseSwapchainImage)
        RESOLVE(PollEvent, PollEvent)
#undef RESOLVE
        f = nullptr;
        if (XR_SUCCEEDED(g.next.getProc(*instance, "xrGetD3D12GraphicsRequirementsKHR", &f)) && f) g.next.GetD3D12Req = (PFN_GetGfxReq)f;
        f = nullptr;
        if (XR_SUCCEEDED(g.next.getProc(*instance, "xrGetD3D11GraphicsRequirementsKHR", &f)) && f) g.next.GetD3D11Req = (PFN_GetGfxReq)f;
    }
    log_line("layer attached to app '%s' (OvalFFR " OVALFFR_VERSION ")", g.app_name.c_str());
    return XR_SUCCESS;
}

} // namespace ovalffr

OVALFFR_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* layerName, XrNegotiateApiLayerRequest* req) {
    using namespace ovalffr;
    if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    if (layerName && std::strcmp(layerName, LAYER_NAME) != 0) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = l_xrGetInstanceProcAddr;
    req->createApiLayerInstance = l_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
