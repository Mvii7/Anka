// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/nx2.h"
#include "video_core/nx2_busy_clock.h"
#include "video_core/nx2_mode_pace21_logic.h"
#include "video_core/nx2_mode_pace_logic.h"

namespace Core {
extern std::atomic<u64> g_nx2_present_count; // core/core.h: vi2 229 presents so far
extern std::atomic<u32> g_nx2_session;        // core/core.h: emulation sessions so far
}

namespace VideoCommon::Nx2ModePace {

// EDEN_NX2_MODE_PACE=1 (multicore, off by default; needs EDEN_NX2_PRESENT_INTERVAL_WORD for the
// game's mode): steer Donkey Kong Bananza's 60 / 30 / 20 fps mode by holding its vi2 229 presents
// for a few frames (late frames, which is what it switches on), the GPU timer left at
// fast_gpu_time's /256. Decisions in nx2_mode_pace_logic.h; this part times the presents, holds
// them (Hold, after the vblank pacing) and logs "nx2 mode pace:" lines. Off with
// EDEN_NX2_PRESENT_INTERVAL > 1, EDEN_NX2_GPU_BUSY_CLOCK (both make the game read GPU time and drop
// its resolution) and EDEN_NX2_VBLANK_GRID (its grid would not move with a hold: the next frame
// would come early); it turns EDEN_NX2_MODE_GOV off (nx2_mode_gov.h). Holds only at speed limit
// 100%. EDEN_NX2_MODE_PACE=2: v2.1 (V21 below), which holds only inside 30's window and releases
// presents early to take 20 up, by the emulator's own frame capacity.
// The controller's clock is the sum of the present intervals, each at most 1 s: across a pause or
// a long load its rests run slower than the wall clock.
//
// EDEN_NX2_PACE_SPIN_US=<0-3000> (default 0, with or without the controller): every vblank pacing
// sleep (and the hold) wakes that long before its target and yields until it; 0 sleeps as before.
// EDEN_NX2_PACE_GUEST_SLEEP (below): those sleeps in the guest kernel.

inline bool On(const char* text) {
    return text != nullptr && text[0] != '0';
}

inline bool Enabled() {
    static const bool env_enabled = [] {
        if (!On(std::getenv("EDEN_NX2_MODE_PACE"))) {
            return false;
        }
        if (!Settings::values.use_multi_core.GetValue()) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE needs multicore: off");
            return false;
        }
        if (std::getenv("EDEN_NX2_PRESENT_INTERVAL_WORD") == nullptr) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE needs EDEN_NX2_PRESENT_INTERVAL_WORD: off");
            return false;
        }
        if (const char* text = std::getenv("EDEN_NX2_PRESENT_INTERVAL");
            text != nullptr && std::strtol(text, nullptr, 10) > 1) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE with EDEN_NX2_PRESENT_INTERVAL > 1 (the game's "
                                "mode would read as at least that): off");
            return false;
        }
        if (On(std::getenv("EDEN_NX2_GPU_BUSY_CLOCK"))) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE with EDEN_NX2_GPU_BUSY_CLOCK (the game would "
                                "read GPU time and drop its resolution): off");
            return false;
        }
        // nx2_gsv.h's VblankGridMode reads it the same way (0-2).
        if (const char* text = std::getenv("EDEN_NX2_VBLANK_GRID");
            text != nullptr && std::strtol(text, nullptr, 10) > 0) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE with EDEN_NX2_VBLANK_GRID (the grid does not "
                                "move with a hold, the next frame would come early): off");
            return false;
        }
        if (const char* text = std::getenv("EDEN_NX2_GPU_TICK_DIV")) {
            const double div = std::strtod(text, nullptr);
            if (div >= 1.0 && div < 64.0) {
                LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE with EDEN_NX2_GPU_TICK_DIV={}: the game "
                                    "reads GPU time and may drop its resolution",
                            div);
            }
        } else if (Settings::values.gpu_clock.GetValue() == Settings::GpuClock::Normal) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE with fast_gpu_time off (/1): the game reads "
                                "GPU time and may drop its resolution");
        }
        return true;
    }();
    return env_enabled && Tegra::Nx2IsBananza();
}

/// EDEN_NX2_PACE_SPIN_US: 0-3000, default 0.
inline u32 SpinUs() {
    static const u32 spin = [] {
        const char* text = std::getenv("EDEN_NX2_PACE_SPIN_US");
        if (text == nullptr) {
            return 0U;
        }
        const long value = std::strtol(text, nullptr, 10);
        if (value < 0 || value > 3000) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_PACE_SPIN_US={} out of 0-3000: 0", text);
            return 0U;
        }
        if (value > 0) {
            LOG_INFO(HW_GPU, "vblank pacing wakes {} us early and yields to its target "
                             "(EDEN_NX2_PACE_SPIN_US)",
                     value);
        }
        return static_cast<u32>(value);
    }();
    return spin;
}

// EDEN_NX2_PACE_GUEST_SLEEP=1 (multicore, off by default): the vblank pacing and the hold sleep
// the vi2 service thread in the guest kernel (nx2_gsv.h passes a GuestSleeper), all but the last
// EDEN_NX2_PACE_GUEST_TAIL_US=<0-5000> (default 1000; at least the spin) on the host as before.
// That thread is AM's on emulated core 3: a host sleep held the core for the whole wait, the
// game's threads there ran only between presents.
inline bool GuestSleepOn() {
    static const bool on = [] {
        if (!On(std::getenv("EDEN_NX2_PACE_GUEST_SLEEP"))) {
            return false;
        }
        if (!Settings::values.use_multi_core.GetValue()) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_PACE_GUEST_SLEEP needs multicore: off");
            return false;
        }
        return true;
    }();
    return on;
}

/// EDEN_NX2_PACE_GUEST_TAIL_US: 0-5000, default 1000.
inline u32 GuestTailUs() {
    static const u32 tail = [] {
        const char* text = std::getenv("EDEN_NX2_PACE_GUEST_TAIL_US");
        if (text == nullptr) {
            return 1000U;
        }
        const long value = std::strtol(text, nullptr, 10);
        if (value < 0 || value > 5000) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_PACE_GUEST_TAIL_US={} out of 0-5000: 1000", text);
            return 1000U;
        }
        return static_cast<u32>(value);
    }();
    return tail;
}

/// Nanoseconds the pacing sleeps yielded (EDEN_NX2_PACE_SPIN_US), for the 10 s line.
inline std::atomic<u64> g_spun_ns{};

/// EDEN_NX2_PACE_GUEST_SLEEP's guest kernel sleeps, for vi2's 600 present line (nx2_gsv.h).
struct GuestSleepStats {
    std::atomic<u64> sleeps{};
    std::atomic<u64> slept_ns{};   ///< asked for
    std::atomic<u64> late_ns{};    ///< woke after the asked time, summed
    std::atomic<u64> late_max_ns{};
};
inline GuestSleepStats g_guest_sleep{};

/// `guest`: EDEN_NX2_PACE_GUEST_SLEEP's sleeper, null to sleep on the host alone.
inline void SleepUntil(std::chrono::steady_clock::time_point target,
                       const GuestSleeper* guest = nullptr) {
    const u32 spin = SpinUs();
    const std::chrono::nanoseconds spun =
        SleepUntil(target, spin, guest, guest != nullptr ? GuestTailUs() : 0U);
    if (spin != 0) {
        g_spun_ns.fetch_add(static_cast<u64>(spun.count()), std::memory_order_relaxed);
    }
}

inline void SleepFor(std::chrono::nanoseconds duration, const GuestSleeper* guest = nullptr) {
    if (SpinUs() == 0 && guest == nullptr) {
        std::this_thread::sleep_for(duration);
        return;
    }
    SleepUntil(std::chrono::steady_clock::now() + duration, guest);
}

/// EDEN_NX2_MODE_PACE_<NAME>: a value out of its range is ignored (warning).
inline Params ReadParams() {
    Params p{};
    const auto number = [](const char* name, double& value, double low, double high) {
        if (const char* text = std::getenv(name)) {
            char* end{};
            const double parsed = std::strtod(text, &end);
            if (end != text && parsed >= low && parsed <= high) {
                value = parsed;
            } else {
                LOG_WARNING(HW_GPU, "{}={} out of {}-{}: {}", name, text, low, high, value);
            }
        }
    };
    const auto count = [](const char* name, u32& value, long low, long high) {
        if (const char* text = std::getenv(name)) {
            char* end{};
            const long parsed = std::strtol(text, &end, 10);
            if (end != text && parsed >= low && parsed <= high) {
                value = static_cast<u32>(parsed);
            } else {
                LOG_WARNING(HW_GPU, "{}={} out of {}-{}: {}", name, text, low, high, value);
            }
        }
    };
    const auto flag = [](const char* name, bool& value) {
        if (const char* text = std::getenv(name)) {
            value = text[0] != '0';
        }
    };
    flag("EDEN_NX2_MODE_PACE_AUTO", p.auto_push);
    flag("EDEN_NX2_MODE_PACE_60", p.allow60);
    number("EDEN_NX2_MODE_PACE_FROM60", p.from60_ms, 10.0, 33.0);
    number("EDEN_NX2_MODE_PACE_SELF60", p.self60_ms, 20.0, 60.0);
    number("EDEN_NX2_MODE_PACE_DOWN_DWELL", p.down_dwell_s, 0.0, 5.0);
    number("EDEN_NX2_MODE_PACE_TO60", p.to60_ms, 5.0, 17.0);
    number("EDEN_NX2_MODE_PACE_UP_DWELL", p.up_dwell_s, 1.0, 120.0);
    number("EDEN_NX2_MODE_PACE_UP_DWELL_MAX", p.up_dwell_max_s, 1.0, 600.0);
    number("EDEN_NX2_MODE_PACE_VERIFY60", p.verify60_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE_STABLE60", p.stable60_s, 10.0, 600.0);
    number("EDEN_NX2_MODE_PACE_TO30", p.to30_ms, 15.0, 33.0);
    number("EDEN_NX2_MODE_PACE_VERIFY30", p.verify30_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE_UP20_DWELL", p.up20_dwell_s, 0.5, 10.0);
    number("EDEN_NX2_MODE_PACE_AFTER_UP20", p.after_up20_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE_HOLD", p.hold_ms, 34.5, 60.0);
    number("EDEN_NX2_MODE_PACE_HOLD_LATE", p.hold_late_ms, 34.5, 80.0);
    number("EDEN_NX2_MODE_PACE_HOLD20", p.hold20_ms, 51.0, 80.0);
    number("EDEN_NX2_MODE_PACE_HOLD20_STEP", p.hold20_step_ms, 0.0, 20.0);
    number("EDEN_NX2_MODE_PACE_HOLD20_MAX", p.hold20_max_ms, 51.0, 90.0);
    count("EDEN_NX2_MODE_PACE_HOLD20_EVERY", p.hold20_step_frames, 1, 100);
    if (const char* text = std::getenv("EDEN_NX2_MODE_PACE_UP_SINGLE")) {
        const double parsed = std::strtod(text, nullptr);
        if (parsed == 0.0 || (parsed >= 50.0 && parsed <= 95.0)) {
            p.up_single_ms = parsed;
        } else {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE_UP_SINGLE={} not 0 or 50-95: 0", text);
        }
    }
    count("EDEN_NX2_MODE_PACE_DOWN_FRAMES", p.down_frames, 3, 300);
    count("EDEN_NX2_MODE_PACE_UP_FRAMES", p.up_frames, 10, 300);
    count("EDEN_NX2_MODE_PACE_UP20_FRAMES", p.up20_frames, 5, 200);
    number("EDEN_NX2_MODE_PACE_DOWN_REST", p.down_rest_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE_DOWN_REST_MAX", p.down_rest_max_s, 0.0, 600.0);
    count("EDEN_NX2_MODE_PACE_DOWN_TRIES", p.down_tries, 1, 20);
    number("EDEN_NX2_MODE_PACE_UP_REST", p.up_rest_s, 0.0, 300.0);
    number("EDEN_NX2_MODE_PACE_UP_REST_MAX", p.up_rest_max_s, 0.0, 600.0);
    count("EDEN_NX2_MODE_PACE_UP_TRIES", p.up_tries, 1, 20);
    number("EDEN_NX2_MODE_PACE_UP20_REST", p.up20_rest_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE_UP20_REST_MAX", p.up20_rest_max_s, 0.0, 600.0);
    count("EDEN_NX2_MODE_PACE_UP20_TRIES", p.up20_tries, 1, 20);
    number("EDEN_NX2_MODE_PACE_STALL", p.stall_ms, 50.0, 500.0);
    number("EDEN_NX2_MODE_PACE_STALL_QUIET", p.stall_quiet_s, 0.0, 10.0);
    number("EDEN_NX2_MODE_PACE_MODE_QUIET", p.mode_quiet_s, 0.0, 10.0);
    number("EDEN_NX2_MODE_PACE_MODE_QUIET60", p.mode_quiet60_s, 0.0, 10.0);
    flag("EDEN_NX2_MODE_PACE_TO20", p.to20);
    number("EDEN_NX2_MODE_PACE_TO20_K", p.to20_k_ms, 30.0, 60.0);
    number("EDEN_NX2_MODE_PACE_TO20_MS", p.to20_ms, 100.0, 300.0);
    count("EDEN_NX2_MODE_PACE_TO20_FRAMES", p.to20_frames, 1, 4);
    number("EDEN_NX2_MODE_PACE_TO20_REST", p.to20_rest_s, 5.0, 300.0);
    return p;
}

inline std::optional<Probe> ReadProbe() {
    const char* text = std::getenv("EDEN_NX2_MODE_PACE_PROBE");
    if (text == nullptr) {
        return std::nullopt;
    }
    auto probe = ParseProbe(text);
    if (!probe) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE_PROBE={} is not <down|up|up20|to20|cycle>:<1-3600 "
                            "s>: no probe",
                    text);
    }
    return probe;
}

constexpr double TicksPerMs = static_cast<double>(Common::WallClock::GPUTickFreq) / 1000.0;

/// "26.3", or "-" for NaN (no K yet).
inline std::string Ms(double value) {
    return std::isnan(value) ? std::string{"-"} : fmt::format("{:.1f}", value);
}

struct State {
    State(const Params& params_, std::optional<Probe> probe)
        : params{params_}, controller{params_, probe} {}

    Params params;
    std::mutex mutex;
    Controller controller;
    Meter meter;
    std::chrono::steady_clock::time_point last_paced{}; ///< the hold's base
    std::atomic<bool> holding{};
    double held_now_ms{}; ///< what Hold slept for the present being made
    bool last_held{};
    std::array<double, 6> gaps{};
    u32 gap_next{};
    // The 10 s summary.
    std::chrono::steady_clock::time_point summary_since{};
    std::array<u32, 4> presents{}; ///< 60 / 30 / 20 / other
    u32 pushes{};
    double held_ms{};
    u32 gaps30{};      ///< present gaps at 30, not after a held present
    u32 late_gaps30{}; ///< of those, 34 ms or more
};

inline State& GetState() {
    static State state = [] {
        const Params p = ReadParams();
        const std::optional<Probe> probe = ReadProbe();
        LOG_INFO(HW_GPU,
                 "nx2 mode pace: on (from60 {}, self60 {}, to60 {} over {} s, to30 {} over {} s "
                 "verified over {} s, hold {}/{}/{} ms, frames {}/{}/{}, spin {} us, auto {}, 60 "
                 "{}, to20 {}, probe {}); up20's hold climbs {} ms every {} presents to {}, rests "
                 "{}/{}/{} s doubling (at most {}/{}/{}), {}/{}/{} tries, quiet {} s at 60 and {} "
                 "s else, stall {} ms",
                 p.from60_ms, p.self60_ms, p.to60_ms, p.up_dwell_s, p.to30_ms, p.up20_dwell_s,
                 p.verify30_s, p.hold_ms, p.hold_late_ms, p.hold20_ms, p.down_frames, p.up_frames,
                 p.up20_frames, SpinUs(), p.auto_push ? 1 : 0, p.allow60 ? 1 : 0, p.to20 ? 1 : 0,
                 probe ? fmt::format("{}:{}", probe->cycle ? "cycle" : PushName(probe->kind),
                                     probe->every_s)
                       : std::string{"-"},
                 p.hold20_step_ms, p.hold20_step_frames, p.hold20_max_ms, p.down_rest_s,
                 p.up_rest_s, p.up20_rest_s, p.down_rest_max_s, p.up_rest_max_s, p.up20_rest_max_s,
                 p.down_tries, p.up_tries, p.up20_tries, p.mode_quiet60_s, p.mode_quiet_s,
                 p.stall_ms);
        return State{p, probe};
    }();
    return state;
}

/// Holds only at the 100% speed limit (the thresholds are wall milliseconds).
inline bool SpeedOk() {
    return Settings::values.use_speed_limit.GetValue() && Settings::SpeedLimit() == 100;
}

// EDEN_NX2_MODE_PACE=2: v2.1 (nx2_mode_pace21_logic.h), the same conditions as =1. Its settings are
// EDEN_NX2_MODE_PACE21_<NAME>; =1 and its EDEN_NX2_MODE_PACE_<NAME> stay as they were.
inline bool V21() {
    static const bool v21 =
        Enabled() && std::strtol(std::getenv("EDEN_NX2_MODE_PACE"), nullptr, 10) == 2;
    return v21;
}

/// EDEN_NX2_MODE_PACE21_<NAME>: a value out of its range is ignored (warning).
inline Params21 ReadParams21() {
    Params21 p{};
    const auto number = [](const char* name, double& value, double low, double high) {
        if (const char* text = std::getenv(name)) {
            char* end{};
            const double parsed = std::strtod(text, &end);
            if (end != text && parsed >= low && parsed <= high) {
                value = parsed;
            } else {
                LOG_WARNING(HW_GPU, "{}={} out of {}-{}: {}", name, text, low, high, value);
            }
        }
    };
    const auto count = [](const char* name, u32& value, long low, long high) {
        if (const char* text = std::getenv(name)) {
            char* end{};
            const long parsed = std::strtol(text, &end, 10);
            if (end != text && parsed >= low && parsed <= high) {
                value = static_cast<u32>(parsed);
            } else {
                LOG_WARNING(HW_GPU, "{}={} out of {}-{}: {}", name, text, low, high, value);
            }
        }
    };
    const auto flag = [](const char* name, bool& value) {
        if (const char* text = std::getenv(name)) {
            value = text[0] != '0';
        }
    };
    flag("EDEN_NX2_MODE_PACE21_AUTO", p.auto_push);
    number("EDEN_NX2_MODE_PACE21_CAP60", p.cap60_ms, 10.0, 25.0);
    number("EDEN_NX2_MODE_PACE21_SELF60", p.self60_ms, 20.0, 60.0);
    number("EDEN_NX2_MODE_PACE21_DOWN_DWELL", p.down_dwell_s, 0.0, 5.0);
    number("EDEN_NX2_MODE_PACE21_DOWN_HOLD", p.down_hold_ms, 34.5, 41.5);
    count("EDEN_NX2_MODE_PACE21_DOWN_FRAMES", p.down_frames, 3, 300);
    number("EDEN_NX2_MODE_PACE21_DOWN_REST", p.down_rest_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE21_DOWN_REST_MAX", p.down_rest_max_s, 0.0, 600.0);
    number("EDEN_NX2_MODE_PACE21_CAP20", p.cap20_ms, 25.0, 45.0);
    number("EDEN_NX2_MODE_PACE21_TO20_DWELL", p.to20_dwell_s, 0.5, 60.0);
    number("EDEN_NX2_MODE_PACE21_TO20_HOLD", p.to20_hold_ms, 42.0, 80.0);
    count("EDEN_NX2_MODE_PACE21_TO20_FRAMES", p.to20_frames, 3, 60);
    number("EDEN_NX2_MODE_PACE21_TO20_REST", p.to20_rest_s, 0.0, 300.0);
    number("EDEN_NX2_MODE_PACE21_TO20_REST_MAX", p.to20_rest_max_s, 0.0, 600.0);
    count("EDEN_NX2_MODE_PACE21_FULL30_ESCAPES", p.full30_escapes, 1, 4);
    number("EDEN_NX2_MODE_PACE21_ESCAPE", p.escape_s, 0.0, 300.0);
    number("EDEN_NX2_MODE_PACE21_CAP30", p.cap30_ms, 15.0, 33.0);
    number("EDEN_NX2_MODE_PACE21_REL_DWELL", p.rel_dwell_s, 0.2, 30.0);
    number("EDEN_NX2_MODE_PACE21_REL", p.rel_ms, 17.0, 40.0);
    count("EDEN_NX2_MODE_PACE21_REL_FRAMES", p.rel_frames, 1, 60);
    count("EDEN_NX2_MODE_PACE21_REL_WAIT", p.rel_wait, 1, 60);
    number("EDEN_NX2_MODE_PACE21_REL_REST", p.rel_rest_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE21_REL_REST_MAX", p.rel_rest_max_s, 0.0, 600.0);
    number("EDEN_NX2_MODE_PACE21_VERIFY30", p.verify30_s, 0.0, 60.0);
    flag("EDEN_NX2_MODE_PACE21_UP_REL", p.up_rel);
    number("EDEN_NX2_MODE_PACE21_CAP_UP", p.cap_up_ms, 5.0, 17.0);
    number("EDEN_NX2_MODE_PACE21_UP_DWELL", p.up_dwell_s, 1.0, 120.0);
    number("EDEN_NX2_MODE_PACE21_UP_REL_MS", p.up_rel_ms, 5.0, 17.0);
    number("EDEN_NX2_MODE_PACE21_UP_REST", p.up_rest_s, 0.0, 300.0);
    number("EDEN_NX2_MODE_PACE21_UP_REST_MAX", p.up_rest_max_s, 0.0, 600.0);
    number("EDEN_NX2_MODE_PACE21_VERIFY60", p.verify60_s, 0.0, 60.0);
    number("EDEN_NX2_MODE_PACE21_STALL", p.stall_ms, 50.0, 500.0);
    number("EDEN_NX2_MODE_PACE21_STALL_QUIET", p.stall_quiet_s, 0.0, 10.0);
    number("EDEN_NX2_MODE_PACE21_MODE_QUIET", p.mode_quiet_s, 0.0, 10.0);
    number("EDEN_NX2_MODE_PACE21_MODE_QUIET60", p.mode_quiet60_s, 0.0, 10.0);
    if (p.cap20_ms < p.cap30_ms) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE21_CAP20 {} below CAP30 {}: {}", p.cap20_ms,
                    p.cap30_ms, p.cap30_ms);
        p.cap20_ms = p.cap30_ms;
    }
    return p;
}

inline std::optional<Probe21> ReadProbe21() {
    const char* text = std::getenv("EDEN_NX2_MODE_PACE21_PROBE");
    if (text == nullptr) {
        return std::nullopt;
    }
    auto probe = ParseProbe21(text);
    if (!probe) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_PACE21_PROBE={} is not <down|to20|rel20|rel60|cycle>:"
                            "<1-3600 s>: no probe",
                    text);
    }
    return probe;
}

struct State21 {
    State21(const Params21& params_, std::optional<Probe21> probe_)
        : params{params_}, probe{probe_}, controller{params_, probe_},
          session{Core::g_nx2_session.load(std::memory_order_relaxed)} {}

    /// Another emulation session in this process: the controller, the meter and the summary start
    /// over (the settings stay).
    void Restart() {
        controller = Controller21{params, probe};
        meter = Meter{};
        last_paced = {};
        held_now_ms = 0.0;
        last_held = false;
        release_ms = 0.0;
        gaps = {};
        gap_next = 0;
        summary_since = {};
        presents = {};
        pushes = 0;
        held_ms = 0.0;
        released = 0;
        gaps30 = 0;
        late_gaps30 = 0;
        why = {};
    }

    Params21 params;
    std::optional<Probe21> probe;
    std::mutex mutex;
    Controller21 controller;
    Meter meter;
    std::chrono::steady_clock::time_point last_paced{}; ///< the hold's base
    std::atomic<bool> holding{};
    double held_now_ms{}; ///< what Hold slept for the present being made
    bool last_held{};
    double release_ms{}; ///< the last present's release (0: none)
    std::array<double, 6> gaps{};
    u32 gap_next{};
    // The 10 s summary.
    std::chrono::steady_clock::time_point summary_since{};
    std::array<u32, 4> presents{}; ///< 60 / 30 / 20 / other
    u32 pushes{};
    double held_ms{};
    u32 released{};
    u32 gaps30{};      ///< present gaps at 30, not after a held or released present
    u32 late_gaps30{}; ///< of those, 34 ms or more
    std::array<u32, static_cast<std::size_t>(Why::Count)> why{};
    u32 session; ///< Core::g_nx2_session the state belongs to
};

inline State21& GetState21() {
    static State21 state = [] {
        const Params21 p = ReadParams21();
        const std::optional<Probe21> probe = ReadProbe21();
        LOG_INFO(HW_GPU,
                 "nx2 mode pace v2.1: on (60 below {} ms, 30 up to {} ms over {} s, 20 above {} "
                 "ms over {} s; holds {}/{} ms, release {} ms x{}, self60 {}, rel60 {} below {} "
                 "ms at {} ms, auto {}, probe {}, spin {} us)",
                 p.cap60_ms, p.cap30_ms, p.rel_dwell_s, p.cap20_ms, p.to20_dwell_s,
                 p.down_hold_ms, p.to20_hold_ms, p.rel_ms, p.rel_frames, p.self60_ms,
                 p.up_rel ? 1 : 0, p.cap_up_ms, p.up_rel_ms, p.auto_push ? 1 : 0,
                 probe ? fmt::format("{}:{}", probe->cycle ? "cycle" : Push21Name(probe->kind),
                                     probe->every_s)
                       : std::string{"-"},
                 SpinUs());
        return State21{p, probe};
    }();
    return state;
}

/// Hold for =2 (as Hold below).
inline bool Hold21(u32 mode, const GuestSleeper* guest) {
    if (!SpeedOk()) {
        return false;
    }
    auto& s = GetState21();
    std::chrono::steady_clock::time_point target;
    {
        std::scoped_lock lock{s.mutex};
        const double ms = s.controller.HoldMs(mode);
        if (ms <= 0.0 || s.last_paced == std::chrono::steady_clock::time_point{}) {
            return false;
        }
        target = s.last_paced + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::duration<double, std::milli>(ms));
    }
    if (std::chrono::steady_clock::now() >= target) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    s.holding.store(true, std::memory_order_relaxed);
    SleepUntil(target, guest);
    s.holding.store(false, std::memory_order_relaxed);
    std::scoped_lock lock{s.mutex};
    s.held_now_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

/// Present for =2 (as Present below).
inline void Present21(u32 mode, std::chrono::steady_clock::time_point entered,
                      std::chrono::steady_clock::time_point composited,
                      std::chrono::steady_clock::time_point paced) {
    const u64 busy = Nx2BusyClock::Ticks();
    const u64 present = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    const u32 session = Core::g_nx2_session.load(std::memory_order_relaxed);
    auto& s = GetState21();
    std::scoped_lock lock{s.mutex};
    if (session != s.session) {
        s.session = session;
        s.Restart();
        LOG_INFO(HW_GPU, "nx2 mode pace v2.1: emulation session {}: the controller starts over",
                 session);
    }
    const auto ms = [](std::chrono::steady_clock::time_point at) {
        return std::chrono::duration<double, std::milli>(at.time_since_epoch()).count();
    };
    const std::optional<Measured> m = s.meter.Present(
        mode, ms(entered), ms(composited), ms(paced),
        static_cast<double>(busy) / TicksPerMs);
    const bool after_pushed = s.last_held || s.release_ms > 0.0;
    const double held_ms = s.held_now_ms;
    s.held_now_ms = 0.0;
    s.last_held = held_ms > 0.0;
    s.last_paced = paced;
    s.release_ms = 0.0;
    if (!m) {
        s.summary_since = paced;
        return;
    }
    const double gap_ms = m->gap_ms;
    s.gaps[s.gap_next++ % s.gaps.size()] = gap_ms;
    if (!SpeedOk()) {
        return;
    }
    const Output21 out = s.controller.Step(m->input);
    s.release_ms = out.release_ms;

    ++s.presents[mode >= 1 && mode <= 3 ? mode - 1 : 3];
    ++s.why[static_cast<std::size_t>(out.why)];
    s.released += out.release_ms > 0.0 ? 1 : 0;
    if (mode == 2 && !after_pushed && !out.mode_changed) {
        ++s.gaps30;
        s.late_gaps30 += gap_ms >= 34.0 ? 1 : 0;
    }
    const auto backoff = [&out] {
        return fmt::format(" (rest {:.0f} s, fails {})", out.rest_s, out.fails);
    };
    if (out.ended != Push21::None) {
        const bool failed = out.result == Result::GaveUp || out.result == Result::Other ||
                            out.result == Result::Aborted;
        LOG_INFO(HW_GPU, "nx2 mode pace: end {} {} after {} presents, game mode {}{}",
                 Push21Name(out.ended), ResultName(out.result), out.frames, mode,
                 failed && out.fails != 0 ? backoff() : std::string{});
    }
    if (out.unverified != Push21::None) {
        LOG_INFO(HW_GPU, "nx2 mode pace: {} did not last: game mode {} within {} s{}",
                 Push21Name(out.unverified), mode,
                 out.unverified == Push21::Rel20 ? s.params.verify30_s : s.params.verify60_s,
                 backoff());
    }
    if (out.mode_changed) {
        std::string gaps;
        const u32 n = (std::min<u32>)(s.gap_next, static_cast<u32>(s.gaps.size()));
        for (u32 i = n; i > 0; --i) {
            gaps += fmt::format("{}{:.1f}", gaps.empty() ? "" : " ",
                                s.gaps[(s.gap_next - i) % s.gaps.size()]);
        }
        LOG_INFO(HW_GPU, "nx2 mode pace: game mode {} (was {}) at present {}, K {} ms, {}; gaps {}",
                 mode, out.old_mode, present, Ms(out.change_cap_ms),
                 out.by == Push21::None ? std::string{"by itself"}
                                        : fmt::format("by {}", Push21Name(out.by)),
                 gaps);
    }
    if (out.started != Push21::None) {
        ++s.pushes;
        LOG_INFO(HW_GPU,
                 "nx2 mode pace: start {} in mode {} ({}): K {} ms (work {}, busy {}), {} {:.0f} "
                 "ms{}",
                 Push21Name(out.started), mode, out.by_probe ? "probe" : "auto", Ms(out.cap_ms),
                 Ms(out.work_med_ms), Ms(out.busy_med_ms),
                 out.release_ms > 0.0 ? "release" : "hold",
                 out.release_ms > 0.0 ? out.release_ms : out.hold_ms,
                 out.down_to20 ? " (to 20: the 30 before was full)" : "");
    }
    s.held_ms += held_ms;
    if (paced - s.summary_since >= std::chrono::seconds{10}) {
        // Every reason the 10 s saw, the most frequent first.
        std::array<std::size_t, static_cast<std::size_t>(Why::Count)> order{};
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&s](std::size_t a, std::size_t b) { return s.why[a] > s.why[b]; });
        std::string why;
        for (const std::size_t i : order) {
            if (s.why[i] != 0) {
                why += fmt::format("{}{} x{}", why.empty() ? "" : ", ",
                                   WhyName(static_cast<Why>(i)), s.why[i]);
            }
        }
        LOG_INFO(HW_GPU,
                 "nx2 mode pace 10 s: presents 60/30/20 {}/{}/{}, pushes {}, held {:.0f} ms, "
                 "released {}, gaps of 34 ms or more at 30 {}/{}, K {} ms (busy {}, work {}), "
                 "now {}; why {}, spun {:.1f} ms",
                 s.presents[0], s.presents[1], s.presents[2], s.pushes, s.held_ms, s.released,
                 s.late_gaps30, s.gaps30, Ms(out.cap_ms), Ms(out.busy_med_ms),
                 Ms(out.work_med_ms), WhyName(out.why), why,
                 static_cast<double>(g_spun_ns.exchange(0, std::memory_order_relaxed)) / 1e6);
        s.summary_since = paced;
        s.presents = {};
        s.pushes = 0;
        s.held_ms = 0.0;
        s.released = 0;
        s.gaps30 = 0;
        s.late_gaps30 = 0;
        s.why = {};
    }
}

/// vi2 229 after Present: with =2, how long after this present the next one comes at the latest
/// (a release; 0: none, the pacing as it is).
inline std::chrono::nanoseconds Release() {
    if (!V21() || !SpeedOk()) {
        return {};
    }
    auto& s = GetState21();
    std::scoped_lock lock{s.mutex};
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double, std::milli>(s.release_ms));
}

/// vi2 229, after the vblank pacing: hold the present the controller asks for, unless the game
/// asked for another mode than the push's (`mode`: PresentInterval's vblanks). True if it slept.
/// `guest`: as SleepUntil's.
inline bool Hold(u32 mode, const GuestSleeper* guest = nullptr) {
    if (V21()) {
        return Hold21(mode, guest);
    }
    if (!SpeedOk()) {
        return false;
    }
    auto& s = GetState();
    std::chrono::steady_clock::time_point target;
    {
        std::scoped_lock lock{s.mutex};
        const double ms = s.controller.HoldMs(mode);
        if (ms <= 0.0 || s.last_paced == std::chrono::steady_clock::time_point{}) {
            return false;
        }
        target = s.last_paced + std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::duration<double, std::milli>(ms));
    }
    if (std::chrono::steady_clock::now() >= target) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    s.holding.store(true, std::memory_order_relaxed);
    SleepUntil(target, guest);
    s.holding.store(false, std::memory_order_relaxed);
    std::scoped_lock lock{s.mutex};
    s.held_now_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

/// Whether a present is being held now (any thread).
inline bool Holding() {
    if (V21()) {
        return GetState21().holding.load(std::memory_order_relaxed);
    }
    return GetState().holding.load(std::memory_order_relaxed);
}

/// vi2 229 after the pacing and the hold: `mode` is PresentInterval's vblanks, `entered` when the
/// request came in, `composited` and `paced` the times before and after the pacing and the hold.
inline void Present(u32 mode, std::chrono::steady_clock::time_point entered,
                    std::chrono::steady_clock::time_point composited,
                    std::chrono::steady_clock::time_point paced) {
    if (V21()) {
        Present21(mode, entered, composited, paced);
        return;
    }
    const u64 busy = Nx2BusyClock::Ticks();
    const u64 present = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto ms = [](std::chrono::steady_clock::time_point at) {
        return std::chrono::duration<double, std::milli>(at.time_since_epoch()).count();
    };
    const std::optional<Measured> m = s.meter.Present(
        mode, ms(entered), ms(composited), ms(paced), static_cast<double>(busy) / TicksPerMs);
    const bool after_hold = s.last_held;
    const double held_ms = s.held_now_ms;
    s.held_now_ms = 0.0;
    s.last_held = held_ms > 0.0;
    s.last_paced = paced;
    if (!m) {
        s.summary_since = paced;
        return;
    }
    const double gap_ms = m->gap_ms;
    s.gaps[s.gap_next++ % s.gaps.size()] = gap_ms;
    if (!SpeedOk()) {
        return;
    }
    const Output out = s.controller.Step(m->input);

    ++s.presents[mode >= 1 && mode <= 3 ? mode - 1 : 3];
    if (mode == 2 && !after_hold && !out.mode_changed) {
        ++s.gaps30;
        s.late_gaps30 += gap_ms >= 34.0 ? 1 : 0;
    }
    // The rest, tries, aborts and block of a failed push (probe pushes have none).
    const auto backoff = [&out] {
        return fmt::format(" (rest {:.0f} s, tries {}, aborted {}{})", out.rest_s, out.tries,
                           out.aborts,
                           out.blocked ? ", blocked until the game changes its mode itself" : "");
    };
    if (out.ended != Push::None) {
        const bool failed = out.result == Result::GaveUp || out.result == Result::Other ||
                            out.result == Result::Aborted;
        std::string detail;
        if (failed && out.ended == Push::To20) {
            detail = fmt::format(" (rest {:.0f} s)", out.rest_s);
        } else if (failed && (out.tries != 0 || out.aborts != 0)) {
            detail = backoff();
        }
        LOG_INFO(HW_GPU, "nx2 mode pace: end {} {} after {} presents, game mode {}{}",
                 PushName(out.ended), ResultName(out.result), out.frames, mode, detail);
    }
    if (out.unverified != Push::None) {
        LOG_INFO(HW_GPU, "nx2 mode pace: {} did not last: game mode {} by itself within {} s{}",
                 PushName(out.unverified), mode, s.params.verify30_s, backoff());
    }
    if (out.mode_changed) {
        std::string gaps;
        const u32 n = (std::min<u32>)(s.gap_next, static_cast<u32>(s.gaps.size()));
        for (u32 i = n; i > 0; --i) {
            gaps += fmt::format("{}{:.1f}", gaps.empty() ? "" : " ",
                                s.gaps[(s.gap_next - i) % s.gaps.size()]);
        }
        LOG_INFO(HW_GPU, "nx2 mode pace: game mode {} (was {}) at present {}, K {} ms, {}; gaps {}",
                 mode, out.old_mode, present, Ms(out.change_k_ms),
                 out.by == Push::None ? std::string{"by itself"}
                                      : fmt::format("by {}", PushName(out.by)),
                 gaps);
    }
    if (out.started != Push::None) {
        ++s.pushes;
        LOG_INFO(
            HW_GPU,
            "nx2 mode pace: start {} in mode {} ({}): K {} ms (work {}, busy {}), hold {:.0f} ms",
            PushName(out.started), mode, out.by_probe ? "probe" : "auto", Ms(out.k_ms),
            Ms(out.work_med_ms), Ms(out.busy_med_ms), out.min_period_ms);
    }
    s.held_ms += held_ms;
    if (paced - s.summary_since >= std::chrono::seconds{10}) {
        LOG_INFO(HW_GPU,
                 "nx2 mode pace 10 s: presents 60/30/20 {}/{}/{}, pushes {}, held {:.0f} ms, gaps "
                 "of 34 ms or more at 30 {}/{}, K {} ms, spun {:.1f} ms",
                 s.presents[0], s.presents[1], s.presents[2], s.pushes, s.held_ms, s.late_gaps30,
                 s.gaps30, Ms(out.k_ms),
                 static_cast<double>(g_spun_ns.exchange(0, std::memory_order_relaxed)) / 1e6);
        s.summary_since = paced;
        s.presents = {};
        s.pushes = 0;
        s.held_ms = 0.0;
        s.gaps30 = 0;
        s.late_gaps30 = 0;
    }
}

} // namespace VideoCommon::Nx2ModePace
