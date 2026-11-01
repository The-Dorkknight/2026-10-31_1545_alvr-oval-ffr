#include "ovalffr_dyn.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ovalffr {

void DynamicFoveation::configure(bool enabled, float floor_scale) {
    if (!std::isfinite(floor_scale)) floor_scale = 0.60f;
    floor_ = std::min(1.0f, std::max(0.40f, floor_scale));
    if (enabled == enabled_) {
        if (scale_ < floor_) scale_ = floor_;     // the user raised the floor while we were below it
        return;
    }
    enabled_ = enabled;
    reset();
}

void DynamicFoveation::reset() {
    scale_ = 1.0f;
    frames_ = 0; n_ = late_ = 0; buf_.clear();
    cooldown_ = clean_ = locked_ = 0;
    relax_cur_ = p_.relax_windows;
    lock_cur_ = p_.lock_windows;
    ceiling_ = 1.0f; ceiling_age_ = 0;
    floor_judged_ = false;
    judge_n_ = 0; judge_sum_ = 0; since_judge_ = 0;
    streak_ = 0; ref_mean_ = 0;
    since_relax_ = 1000000;
    last_tightened_ = at_floor_ = false;
}

const char* DynamicFoveation::state() const {
    if (!enabled_) return "off";
    if (locked_ > 0) return "locked";
    if (frames_ <= p_.warmup_frames) return "warmup";
    if (at_floor_) return "at_floor";
    if (scale_ >= 0.9999f) return "steady";
    return last_tightened_ ? "tightening" : "relaxing";
}

bool DynamicFoveation::update(double interval, double period) {
    if (!enabled_) return false;
    if (!std::isfinite(interval) || !std::isfinite(period) || interval <= 0 || period <= 0) return false;
    double ratio = interval / period;
    if (ratio > p_.hitch_ratio) return false;           // pause / loading: not a signal
    ++frames_;
    if (frames_ <= p_.warmup_frames) return false;
    buf_.push_back(ratio);
    if (ratio > p_.late_ratio) ++late_;
    if (++n_ < p_.window) return false;
    // Median, not mean: one scheduler spike or hitch in a window must not decide anything.
    std::nth_element(buf_.begin(), buf_.begin() + buf_.size() / 2, buf_.end());
    double typical = buf_[buf_.size() / 2], late_frac = (double)late_ / n_;
    n_ = late_ = 0; buf_.clear();
    return evaluate(typical, late_frac);
}

bool DynamicFoveation::evaluate(double mean, double late_frac) {   // 'mean' below is the window median
    ++since_relax_;
    if (ceiling_ < 1.0f && ++ceiling_age_ >= p_.ceiling_windows) { ceiling_ = 1.0f; ceiling_age_ = 0; }
    if (locked_ > 0) { --locked_; return false; }
    if (cooldown_ > 0) { --cooldown_; return false; }

    if (late_frac >= p_.tighten_late_frac || mean >= p_.tighten_mean_ratio) {
        clean_ = 0;
        if (scale_ <= floor_ + 1e-6f) {
            at_floor_ = true;
            if (floor_judged_ && ++since_judge_ >= p_.rejudge_windows) { floor_judged_ = false; since_judge_ = 0; }
            if (!floor_judged_ && streak_ > 0) {
                judge_sum_ += mean;
                if (++judge_n_ < p_.judge_windows) return false;    // average several windows: one jittery window proves nothing
                floor_judged_ = true;
                double avg = judge_sum_ / judge_n_;
                judge_n_ = 0; judge_sum_ = 0;
                if (avg > ref_mean_ * (1.0 - p_.nogain_gain)) {    // all the way down and nothing got faster
                    scale_ = 1.0f;
                    locked_ = lock_cur_;
                    lock_cur_ = std::min(p_.max_lock_windows, lock_cur_ * 2);
                    streak_ = 0; at_floor_ = false; last_tightened_ = false; floor_judged_ = false;
                    ++changes_;
                    return true;
                }
            }
            return false;
        }
        // a tighten soon after a relax = we are oscillating: be slower to relax next time
        bool failed_relax = since_relax_ <= 5;
        if (failed_relax) relax_cur_ = std::min(p_.max_relax_windows, relax_cur_ * 2);
        if (streak_ == 0) { ref_mean_ = mean; floor_judged_ = false; judge_n_ = 0; judge_sum_ = 0; }
        scale_ = std::max(floor_, scale_ - (float)p_.step);
        if (failed_relax) { ceiling_ = scale_; ceiling_age_ = 0; }   // the scale we just left was too sharp
        ++streak_; ++changes_;
        cooldown_ = p_.cooldown_windows;
        last_tightened_ = true; at_floor_ = false;
        return true;
    }

    streak_ = 0;
    at_floor_ = false;
    if (late_frac < 0.05 && mean < p_.clean_mean_ratio) ++clean_; else clean_ = 0;
    if (scale_ < 0.9999f && clean_ >= relax_cur_ && scale_ + (float)p_.step <= ceiling_ + 1e-6f) {
        scale_ = std::min(1.0f, scale_ + (float)p_.step);
        clean_ = 0; since_relax_ = 0; ++changes_;
        cooldown_ = p_.cooldown_windows;
        last_tightened_ = false;
        return true;
    }
    return false;
}

} // namespace ovalffr
