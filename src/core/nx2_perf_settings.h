// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"

// Anka's settings in the Qt program (anka.exe): the EDEN_NX2_* switches of the launchers
// (dist/dozer/start-*.bat), as a preset and on / off settings kept in the [Anka] group of
// qt-config.ini, the Recommended preset until changed. The program sets the variables in its own
// environment before a game boots (yuzu/nx2perf_session.h); anka-cli and the launchers don't read
// these settings.
//
// Every switch is read once per process, most of them into a function's static the first time
// a game reaches it (EDEN_NX2_BACK_STAGES, ACQUIRE_PEEK, PRESENT_INTERVAL_WORD, MODE_PACE,
// PACE_SPIN_US, DK_DYN, OSD); JIT_SAME, FRONT_MACRO_LEAN and FG_TIMED are read again at each boot.
// So the variables are set once, at the first boot, and a later boot that would need others (a
// changed setting, or Bananza after another game and back) restarts the program for that game.
namespace Core::Nx2PerfSettings {

/// The launchers' three setups, and Custom: a box changed by hand.
enum class Preset : u8 {
    Recommended = 0, ///< start-recommended.bat: all on, Reactive Flushing off
    Reactive = 1,    ///< start-reactive-experimental.bat: all on, Reactive Flushing on, RX_RT_FIT=3
    Safe = 2,        ///< start-safe.bat: all off, and the shortcuts Anka keeps on set to 0
    Custom = 3,      ///< the boxes as set, nothing else
};

struct Switches {
    bool split_gpu_thread = true;  ///< EDEN_NX2_BACK_STAGES=1 + EDEN_NX2_BACK_CUTS=a
    bool acquire_peek = true;      ///< EDEN_NX2_ACQUIRE_PEEK=1
    bool jit_same = true;          ///< EDEN_NX2_JIT_SAME=1
    bool front_macro_lean = true;  ///< EDEN_NX2_FRONT_MACRO_LEAN=1
    bool fg_timed = true;          ///< EDEN_NX2_FG_TIMED=1
    bool osd = true;               ///< EDEN_NX2_OSD=1
    bool dk_dyn = true;            ///< EDEN_NX2_DK_DYN=1, Bananza only
    bool dk_pacing = true;         ///< PRESENT_INTERVAL_WORD + MODE_PACE + PACE_SPIN_US, Bananza only
    Preset preset = Preset::Recommended;

    bool operator==(const Switches&) const = default;
};

/// What the recommended launcher sets (and the first start's settings).
constexpr Switches Recommended() {
    return Switches{};
}

/// start-safe.bat's setup: none of the switches, and the per-draw shortcuts off.
constexpr Switches AllOff() {
    return Switches{false, false, false, false, false, false, false, false, Preset::Safe};
}

/// The keys of the [Anka] group, one per setting (the preset has its own, "preset").
struct Key {
    std::string_view name;
    bool Switches::*field;
};
inline constexpr std::array<Key, 8> Keys{{
    {"split_gpu_thread", &Switches::split_gpu_thread},
    {"acquire_peek", &Switches::acquire_peek},
    {"jit_same", &Switches::jit_same},
    {"front_macro_lean", &Switches::front_macro_lean},
    {"fg_timed", &Switches::fg_timed},
    {"osd", &Switches::osd},
    {"bananza_dynamic_fps", &Switches::dk_dyn},
    {"bananza_frame_pacing", &Switches::dk_pacing},
}};

/// The boxes of a preset (Custom: `current` as it is), with the preset set.
constexpr Switches FromPreset(Preset preset, const Switches& current) {
    Switches switches = preset == Preset::Safe ? AllOff() : Recommended();
    if (preset == Preset::Custom) {
        switches = current;
    }
    switches.preset = preset;
    return switches;
}

/// Whether the boxes are the preset's (Custom always is).
constexpr bool MatchesPreset(const Switches& switches) {
    const Switches preset = FromPreset(switches.preset, switches);
    for (const Key& key : Keys) {
        if (switches.*key.field != preset.*key.field) {
            return false;
        }
    }
    return true;
}

/// After a box was changed by hand: Custom, unless the boxes are still the preset's.
constexpr Switches AfterBoxChange(Switches switches) {
    if (!MatchesPreset(switches)) {
        switches.preset = Preset::Custom;
    }
    return switches;
}

/// The Anka tab's frame generation choice. It writes the Graphics tab's settings (frame_gen,
/// frame_gen_type, frame_gen_multiplier, frame_gen_target_rate); they are not EDEN_NX2_* switches.
enum class FrameGen : u8 {
    Off = 0,     ///< frame generation off
    Fixed2x = 1, ///< FSR3, one generated frame per real frame
    Dynamic = 2, ///< FSR3 aiming at DYNAMIC_TARGET_RATE (the pacer caps it at the refresh rate)
    Other = 3,   ///< the Graphics tab holds another setup: shown, never written
};

/// Dynamic's target frame rate (frame_gen_target_rate).
constexpr u32 DYNAMIC_TARGET_RATE = 120;

/// The Graphics tab's frame generation settings, as far as the choice goes.
struct FrameGenSettings {
    bool on = false;
    bool fsr3 = true; ///< frame_gen_type is FSR3
    u32 multiplier = 2;
    u32 target_rate = 0;

    constexpr bool operator==(const FrameGenSettings&) const = default;
};

/// The choice the settings amount to.
constexpr FrameGen FrameGenFrom(const FrameGenSettings& settings) {
    if (!settings.on) {
        return FrameGen::Off;
    }
    if (settings.fsr3 && settings.target_rate == 0 && settings.multiplier == 2) {
        return FrameGen::Fixed2x;
    }
    if (settings.fsr3 && settings.target_rate == DYNAMIC_TARGET_RATE) {
        return FrameGen::Dynamic;
    }
    return FrameGen::Other;
}

/// The settings a choice writes; what it leaves alone comes from `current` (Off only turns frame
/// generation off, Dynamic keeps the multiplier: the target rate decides, Other changes nothing).
constexpr FrameGenSettings FrameGenApply(FrameGen choice, FrameGenSettings current) {
    switch (choice) {
    case FrameGen::Off:
        current.on = false;
        break;
    case FrameGen::Fixed2x:
        current = {.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 0};
        break;
    case FrameGen::Dynamic:
        current.on = true;
        current.fsr3 = true;
        current.target_rate = DYNAMIC_TARGET_RATE;
        break;
    case FrameGen::Other:
        break;
    }
    return current;
}

/// The frame generation a preset asks for: Safe turns it off, the others leave it.
constexpr std::optional<FrameGen> FrameGenFor(Preset preset) {
    if (preset == Preset::Safe) {
        return FrameGen::Off;
    }
    return std::nullopt;
}

/// The barrier feedback loops setting (Graphics > Advanced) a preset asks for: on for Reactive,
/// which needs it (start-reactive-experimental.bat sets both); none for the others.
constexpr std::optional<bool> BarrierFeedbackLoops(Preset preset) {
    if (preset == Preset::Reactive) {
        return true;
    }
    return std::nullopt;
}

/// The Reactive Flushing setting (Graphics > Advanced) a preset asks for; none for Safe and Custom
/// (the launchers' safe one leaves the .ini as it is).
constexpr std::optional<bool> ReactiveFlushing(Preset preset) {
    switch (preset) {
    case Preset::Recommended:
        return false;
    case Preset::Reactive:
        return true;
    default:
        return std::nullopt;
    }
}

/// Reads every setting through `read(key, default)` and the preset through
/// `read_preset(key, default)`; missing keys give the first start's (Recommended). Boxes that are
/// not their preset's (the file edited by hand) make it Custom.
template <typename Read, typename ReadPreset>
Switches Load(Read&& read, ReadPreset&& read_preset) {
    Switches switches{};
    for (const Key& key : Keys) {
        switches.*key.field = read(key.name, Recommended().*key.field);
    }
    const int preset = read_preset("preset", static_cast<int>(Preset::Recommended));
    switches.preset = preset >= 0 && preset <= static_cast<int>(Preset::Custom)
                          ? static_cast<Preset>(preset)
                          : Preset::Custom;
    return AfterBoxChange(switches);
}

/// Writes every setting through `write(key, value, default)` and the preset through
/// `write_preset(key, value, default)`.
template <typename Write, typename WritePreset>
void Save(const Switches& switches, Write&& write, WritePreset&& write_preset) {
    for (const Key& key : Keys) {
        write(key.name, switches.*key.field, Recommended().*key.field);
    }
    write_preset("preset", static_cast<int>(switches.preset),
                 static_cast<int>(Preset::Recommended));
}

/// The settings for Donkey Kong Bananza only take its title: the program id EDEN_NX2_DK_DYN's
/// patch checks (Loader::Nx2DkDynamicFps::ApplyIfEnabled).
constexpr bool IsBananza(u64 program_id) {
    return program_id == Loader::Nx2DkDynamicFps::ProgramId;
}

struct Var {
    std::string_view name;
    std::string_view value;

    bool operator==(const Var&) const = default;
};

/// The per-draw shortcuts this fork keeps on by default; the Safe preset turns them off (=0), as
/// start-safe.bat does.
inline constexpr std::array<std::string_view, 61> SafeOff{
    "EDEN_NX2_LIGHT_DRAW",       "EDEN_NX2_LIGHT_UNIFORM",     "EDEN_NX2_LIGHT_UNIFORM_VALUES",
    "EDEN_NX2_LIGHT_STORAGE",    "EDEN_NX2_UNIFORM_SYNC_SKIP", "EDEN_NX2_STORAGE_KEEP",
    "EDEN_NX2_UBO_REUSE",        "EDEN_NX2_UBO_DIRECT",        "EDEN_NX2_UBO_STAGE",
    "EDEN_NX2_UBO_BIND_SKIP",    "EDEN_NX2_SSBO_FAST",         "EDEN_NX2_SSBO_MEMO",
    "EDEN_NX2_SYNC_SETS",        "EDEN_NX2_SYNC_SKIP",         "EDEN_NX2_SYNC_EXACT",
    "EDEN_NX2_SYNC_MEMO",        "EDEN_NX2_VERTEX_SKIP",       "EDEN_NX2_VERTEX_FAST",
    "EDEN_NX2_VERTEX_CLEAN",     "EDEN_NX2_LIGHT_SYNC_SKIP",   "EDEN_NX2_INDEX_SKIP",
    "EDEN_NX2_INLINE_FIX",       "EDEN_NX2_TOUCH_SKIP",        "EDEN_NX2_TEX_TOUCH_SKIP",
    "EDEN_NX2_ALIAS_LAZY",       "EDEN_NX2_USAGE_SKIP",        "EDEN_NX2_USAGE_RESET_SKIP",
    "EDEN_NX2_GPU_WRITTEN_FILTER", "EDEN_NX2_MARK_WORDS",      "EDEN_NX2_INVAL_HULL",
    "EDEN_NX2_INVAL_SKIP",       "EDEN_NX2_SMALL_WRITE",       "EDEN_NX2_RANGE_SKIP",
    "EDEN_NX2_SLOT_SKIP",        "EDEN_NX2_QUERY_BOUNDS",      "EDEN_NX2_SHADER_BOUNDS",
    "EDEN_NX2_DESC_REUSE",       "EDEN_NX2_DESC_HULL",         "EDEN_NX2_DESC_SECONDARY",
    "EDEN_NX2_DESC_XLAT",        "EDEN_NX2_KEY_CMP",           "EDEN_NX2_NOTIFY_LAZY",
    "EDEN_NX2_RESET_BATCH",      "EDEN_NX2_RESET_LAST",        "EDEN_NX2_RESET_RUNS",
    "EDEN_NX2_RESET_SORT",       "EDEN_NX2_WRITE_BATCH",       "EDEN_NX2_BATCH_DIRECT",
    "EDEN_NX2_CB_RUNS",          "EDEN_NX2_TICK_SKIP",         "EDEN_NX2_HLE_INDIRECT",
    "EDEN_NX2_INDIRECT_BATCH",   "EDEN_NX2_LAYOUT_MEMO",       "EDEN_NX2_SAME_SKIP",
    "EDEN_NX2_HSETP2_REG",       "EDEN_NX2_DIRTY_THUNK_SKIP",  "EDEN_NX2_FAST_PRIORITY_SCAN",
    "EDEN_NX2_PBFLUSH_WINDOW_KIB", "EDEN_NX2_UPSCALE_SCAN",    "EDEN_NX2_UPSCALE_LEARN",
    "EDEN_NX2_UPSCALE_HEADER",
};

/// The variables a setting or preset can set, besides SafeOff, in the order they are set.
inline constexpr std::array<std::string_view, 12> Names{
    "EDEN_NX2_BACK_STAGES",   "EDEN_NX2_BACK_CUTS",
    "EDEN_NX2_ACQUIRE_PEEK",  "EDEN_NX2_JIT_SAME",
    "EDEN_NX2_FRONT_MACRO_LEAN", "EDEN_NX2_FG_TIMED",
    "EDEN_NX2_OSD",           "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",  "EDEN_NX2_RX_RT_FIT",
};

/// Calls `f(name)` for every variable the settings can set (Names, then SafeOff).
template <typename F>
void ForEachName(F&& f) {
    for (const std::string_view name : Names) {
        f(name);
    }
    for (const std::string_view name : SafeOff) {
        f(name);
    }
}

/// The variables the settings ask for when the game is (or is not) Bananza, the same values as
/// the launcher of the preset. The Bananza ones never for another game.
inline std::vector<Var> Wanted(const Switches& switches, bool bananza) {
    std::vector<Var> vars;
    if (switches.split_gpu_thread) {
        vars.push_back({"EDEN_NX2_BACK_STAGES", "1"});
        vars.push_back({"EDEN_NX2_BACK_CUTS", "a"});
    }
    if (switches.acquire_peek) {
        vars.push_back({"EDEN_NX2_ACQUIRE_PEEK", "1"});
    }
    if (switches.jit_same) {
        vars.push_back({"EDEN_NX2_JIT_SAME", "1"});
    }
    if (switches.front_macro_lean) {
        vars.push_back({"EDEN_NX2_FRONT_MACRO_LEAN", "1"});
    }
    if (switches.fg_timed) {
        vars.push_back({"EDEN_NX2_FG_TIMED", "1"});
    }
    if (switches.osd) {
        vars.push_back({"EDEN_NX2_OSD", "1"});
    }
    if (bananza && switches.dk_dyn) {
        vars.push_back({"EDEN_NX2_DK_DYN", "1"});
    }
    if (bananza && switches.dk_pacing) {
        vars.push_back({"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"});
        vars.push_back({"EDEN_NX2_MODE_PACE", "2"});
        vars.push_back({"EDEN_NX2_PACE_SPIN_US", "1500"});
    }
    if (switches.preset == Preset::Reactive) {
        vars.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    }
    if (switches.preset == Preset::Safe) {
        for (const std::string_view name : SafeOff) {
            vars.push_back({name, "0"});
        }
    }
    return vars;
}

/// What the program sets: the wanted variables but those already in its environment when it
/// started (a launcher, the user, or nx2_switches.txt set them: theirs wins, set or not by us).
/// `set_outside(name)` tells whether a name was there.
template <typename SetOutside>
std::vector<Var> Plan(const Switches& switches, bool bananza, SetOutside&& set_outside) {
    std::vector<Var> plan;
    for (const Var& var : Wanted(switches, bananza)) {
        if (!set_outside(var.name)) {
            plan.push_back(var);
        }
    }
    return plan;
}

/// Whether settings just saved need a restart: only when this process already set its variables
/// for a game (`applied`, the plan it set) and a game of the same kind would now get others.
/// Before the first game nothing was set: the settings simply apply to it.
inline bool NeedsRestart(const std::optional<std::vector<Var>>& applied,
                         const std::vector<Var>& plan_now) {
    return applied.has_value() && *applied != plan_now;
}

} // namespace Core::Nx2PerfSettings
