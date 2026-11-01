// Auto foveation controller: decides how much to shrink the sharp area from frame timing alone.
// Pure logic (no OpenXR, no GPU, no clock): it is fed (frame interval, display period) once per
// frame, so it can be tested with simulated timings.
//
// Idea (same family as Meta's dynamic fixed foveation, whose docs also say to avoid frequent level
// changes because the jumps are noticeable): the user's sliders are the sharpest it will ever be.
// While the game keeps missing display refreshes, tighten in small steps down to a floor. When
// frames are clean for a long time, relax slowly. Two safeguards:
//   * oscillation: if a relax is quickly followed by another tighten, wait twice as long next time
//   * no gain: if the sharp area has been shrunk all the way to the floor and the frame interval is
//     no shorter than before, the GPU is not the bottleneck (CPU-bound, streaming, ...), so go back
//     to full quality and stop trying; each repeat of this waits twice as long
//     (judged only at the floor, because a runtime that halves the frame rate on every miss shows no
//     improvement at all until the GPU finally keeps up)
#pragma once

#include <vector>

namespace ovalffr {

struct DynParams {
    int    warmup_frames = 120;     // ignored after start (shader compiles, loading)
    int    window = 30;             // frames per decision
    double late_ratio = 1.35;       // interval / period above this counts as a missed refresh
    double hitch_ratio = 8.0;       // above this = pause or loading hitch: frame ignored
    double tighten_late_frac = 0.25;// fraction of late frames in a window that triggers a tighten
    double tighten_mean_ratio = 1.10; // ...or the typical (median) interval is this much over the period (steady 10% slow)
    double clean_mean_ratio = 1.04;   // a window is "clean" only if the typical interval is below this and < 5% frames are late
    int    ceiling_windows = 1500;    // after a failed relax, do not relax above that scale for this long
    double step = 0.05;             // scale change per tighten (relax uses the same step)
    int    cooldown_windows = 3;    // windows to wait after any change before judging again
    int    relax_windows = 20;      // clean windows before relaxing (doubles after an oscillation)
    int    max_relax_windows = 1280;
    double nogain_gain = 0.05;      // required fractional drop of the mean interval
    int    rejudge_windows = 20;    // while stuck at the floor, ask "did it help?" again this often (noise can fool one verdict)
    int    judge_windows = 3;       // windows averaged at the floor before deciding "did it help?"
    int    lock_windows = 200;      // how long to stop trying after "no gain" (doubles each time)
    int    max_lock_windows = 6400;
};

class DynamicFoveation {
public:
    explicit DynamicFoveation(const DynParams& p = DynParams()) : p_(p), relax_cur_(p.relax_windows), lock_cur_(p.lock_windows) {}

    // enabled=false pins the scale at 1.0. floor is clamped to 0.40..1.00.
    void configure(bool enabled, float floor_scale);
    void reset();                                   // new session: forget everything
    // Feed one frame. Returns true if scale() changed.
    bool update(double interval_s, double period_s);

    float scale() const { return scale_; }          // 1.0 = the user's sizes
    const char* state() const;                      // off | warmup | steady | tightening | at_floor | relaxing | locked
    int changes() const { return changes_; }

private:
    bool evaluate(double typical_ratio, double late_frac);   // typical = median of the window (robust to spikes)
    DynParams p_;
    bool enabled_ = false;
    float floor_ = 0.60f, scale_ = 1.0f;
    long frames_ = 0;
    int n_ = 0, late_ = 0;
    std::vector<double> buf_;       // this window's interval/period ratios
    int cooldown_ = 0, clean_ = 0, locked_ = 0, relax_cur_;
    int streak_ = 0;                // tightenings in the current sequence
    int lock_cur_;
    int since_judge_ = 0;
    int judge_n_ = 0;               // windows collected at the floor for the no-gain judgement
    double judge_sum_ = 0;
    float ceiling_ = 1.0f;
    int ceiling_age_ = 0;
    bool floor_judged_ = false;
    double ref_mean_ = 0;
    int since_relax_ = 1000000;     // windows since the last relax
    int changes_ = 0;
    bool last_tightened_ = false, at_floor_ = false;
};

} // namespace ovalffr
