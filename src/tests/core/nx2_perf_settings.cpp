// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/nx2_perf_settings.h"

namespace {

using namespace Core::Nx2PerfSettings;

constexpr u64 Bananza = 0x040074A01BF12000ULL;
constexpr u64 OtherGame = 0x0100000000010000ULL;

// start-balanced.bat's lines (dist/dozer), DK_DYN and OSD at their defaults (1).
const std::vector<Var> BalancedBananza{
    {"EDEN_NX2_BACK_STAGES", "1"},
    {"EDEN_NX2_BACK_CUTS", "a"},
    {"EDEN_NX2_ACQUIRE_PEEK", "1"},
    {"EDEN_NX2_JIT_SAME", "1"},
    {"EDEN_NX2_FRONT_MACRO_LEAN", "1"},
    {"EDEN_NX2_FG_TIMED", "1"},
    {"EDEN_NX2_OSD", "1"},
    {"EDEN_NX2_DK_DYN", "1"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};

const std::set<std::string_view> BananzaOnly{
    "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD",
    "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",
};

// An ini group as the config sees it: the on / off keys and "preset".
struct Ini {
    std::map<std::string, bool> values;
    std::optional<int> preset;

    Switches Read() const {
        return Load(
            [this](std::string_view key, bool def) {
                const auto it = values.find(std::string{key});
                return it == values.end() ? def : it->second;
            },
            [this](std::string_view, int def) { return preset.value_or(def); });
    }
    void Write(const Switches& switches) {
        Save(
            switches,
            [this](std::string_view key, bool value, bool) { values[std::string{key}] = value; },
            [this](std::string_view key, int value, int) {
                REQUIRE(key == "preset");
                preset = value;
            });
    }
};

bool NoneOutside(std::string_view) {
    return false;
}

Switches Custom(Switches switches) {
    switches.preset = Preset::Custom;
    return switches;
}

} // Anonymous namespace

TEST_CASE("Nx2PerfSettings: the first start is the performance setup", "[core]") {
    const Ini empty;
    REQUIRE(empty.Read() == Performance());
    REQUIRE(empty.Read() == FromPreset(Preset::Performance, Balanced()));
    REQUIRE(empty.Read().preset == Preset::Performance);
    REQUIRE(Wanted(Balanced(), true) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: every setting reads back what was written", "[core]") {
    for (const Key& key : Keys) {
        Switches switches = Balanced();
        switches.*key.field = false;
        switches = AfterBoxChange(switches);
        REQUIRE(switches.preset == Preset::Custom);
        Ini ini;
        ini.Write(switches);
        REQUIRE(ini.values.size() == Keys.size());
        REQUIRE(ini.Read() == switches);
        REQUIRE(ini.values.at(std::string{key.name}) == false);
    }
    for (const Preset preset :
         {Preset::Balanced, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        Ini ini;
        ini.Write(FromPreset(preset, Balanced()));
        REQUIRE(ini.Read() == FromPreset(preset, Balanced()));
    }
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' boxes", "[core]") {
    for (const Preset preset : {Preset::Balanced, Preset::Performance, Preset::Reactive}) {
        const Switches switches = FromPreset(preset, AllOff());
        REQUIRE(switches.preset == preset);
        for (const Key& key : Keys) {
            const bool on = key.name != "read_traps" || preset == Preset::Balanced;
            REQUIRE(switches.*key.field == on);
        }
    }
    REQUIRE(FromPreset(Preset::Balanced, AllOff()).read_traps);
    REQUIRE(!FromPreset(Preset::Performance, AllOff()).read_traps);
    REQUIRE(!FromPreset(Preset::Reactive, AllOff()).read_traps);
    const Switches safe = FromPreset(Preset::Safe, Balanced());
    REQUIRE(safe == AllOff());
    for (const Key& key : Keys) {
        REQUIRE(!(safe.*key.field));
    }
    // Custom keeps the boxes as they are.
    Switches mine = Balanced();
    mine.osd = false;
    REQUIRE(FromPreset(Preset::Custom, mine) == Custom(mine));
    // Reactive Flushing: Balanced and Performance off, Experimental (Reactive) on, Safe and Custom
    // leave it.
    REQUIRE(ReactiveFlushing(Preset::Balanced) == false);
    REQUIRE(ReactiveFlushing(Preset::Performance) == false);
    REQUIRE(ReactiveFlushing(Preset::Reactive) == true);
    REQUIRE(!ReactiveFlushing(Preset::Safe).has_value());
    REQUIRE(!ReactiveFlushing(Preset::Custom).has_value());
    // Barrier feedback loops: on with Reactive (it needs them), the others leave it.
    REQUIRE(BarrierFeedbackLoops(Preset::Reactive) == true);
    REQUIRE(!BarrierFeedbackLoops(Preset::Balanced).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Safe).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Custom).has_value());
}

TEST_CASE("Nx2PerfSettings: a box changed by hand makes it Custom", "[core]") {
    for (const Preset preset :
         {Preset::Balanced, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Key& key : Keys) {
            Switches switches = FromPreset(preset, Balanced());
            REQUIRE(AfterBoxChange(switches).preset == preset);
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == Preset::Custom);
            // Changed back: still the preset's boxes, so the preset stays (a later check).
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == preset);
        }
    }
    // Custom stays Custom whatever the boxes.
    REQUIRE(AfterBoxChange(Custom(Balanced())).preset == Preset::Custom);
    // A file edited by hand: boxes not the stored preset's read as Custom; a bad preset too.
    Ini ini;
    ini.Write(Balanced());
    ini.values["osd"] = false;
    REQUIRE(ini.Read().preset == Preset::Custom);
    Ini bad;
    bad.preset = 9;
    REQUIRE(bad.Read().preset == Preset::Custom);
    // A file from before read_traps: the key is missing, the preset stays, and Performance / Experimental
    // / Safe keep read traps off.
    for (const Preset preset :
         {Preset::Balanced, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        Ini old;
        old.Write(FromPreset(preset, Balanced()));
        old.values.erase("read_traps");
        REQUIRE(old.Read() == FromPreset(preset, Balanced()));
    }
    Ini stored_performance;
    stored_performance.preset = static_cast<int>(Preset::Performance);
    REQUIRE(stored_performance.Read() == FromPreset(Preset::Performance, Balanced()));
    REQUIRE(IndexOf(Preset::Performance) == 0);
    REQUIRE(IndexOf(Preset::Balanced) == 1);
    REQUIRE(PresetOrder[static_cast<std::size_t>(IndexOf(Preset::Reactive))] == Preset::Reactive);
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' variables", "[core]") {
    // Performance: Balanced's, and read traps off (start-performance.bat).
    auto performance = BalancedBananza;
    performance.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    REQUIRE(Wanted(FromPreset(Preset::Performance, Balanced()), true) == performance);
    // Experimental: Performance's and RX_RT_FIT=3 (start-reactive-experimental.bat).
    auto reactive = performance;
    reactive.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    REQUIRE(Wanted(FromPreset(Preset::Reactive, Balanced()), true) == reactive);
    // Safe: none of the boxes, read traps off, every per-draw shortcut =0 (start-safe.bat).
    for (const bool bananza : {true, false}) {
        const auto safe = Wanted(AllOff(), bananza);
        REQUIRE(safe.size() == SafeOff.size() + 1);
        REQUIRE(safe.front() == Var{"EDEN_NX2_READ_TRAPS", "0"});
        for (std::size_t i = 0; i < SafeOff.size(); ++i) {
            REQUIRE(safe[i + 1] == Var{SafeOff[i], "0"});
        }
    }
    // Custom with everything off sets read traps off only; Custom with the boxes on: only the
    // boxes' (read traps on sets nothing for that switch).
    REQUIRE(Wanted(Custom(AllOff()), true) == std::vector<Var>{{"EDEN_NX2_READ_TRAPS", "0"}});
    REQUIRE(Wanted(Custom(Balanced()), true) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: keys are distinct and cover every field", "[core]") {
    std::set<std::string_view> names;
    for (const Key& key : Keys) {
        names.insert(key.name);
        Switches one = AllOff();
        one.*key.field = true;
        REQUIRE(one != AllOff());
    }
    REQUIRE(names.size() == Keys.size());
    REQUIRE(names.count("preset") == 0);
    for (const Key& key : Keys) {
        REQUIRE(!(AllOff().*key.field));
        REQUIRE(Balanced().*key.field);
    }
}

TEST_CASE("Nx2PerfSettings: Bananza by the patch's own program id", "[core]") {
    REQUIRE(IsBananza(Loader::Nx2DkDynamicFps::ProgramId));
    REQUIRE(IsBananza(Bananza));
    REQUIRE(!IsBananza(OtherGame));
    REQUIRE(!IsBananza(0));
    REQUIRE(!IsBananza(Bananza + 0x800)); // its update's id: the patch checks the base id only
}

TEST_CASE("Nx2PerfSettings: the Bananza settings never reach another game", "[core]") {
    for (const Preset preset :
         {Preset::Balanced, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Var& var : Wanted(FromPreset(preset, Balanced()), false)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
        }
    }
    const auto other = Wanted(Balanced(), IsBananza(OtherGame));
    REQUIRE(other.size() == BalancedBananza.size() - 4);
    // Bananza's own switches off: the others as for any game.
    Switches no_dk = Balanced();
    no_dk.dk_dyn = false;
    no_dk.dk_pacing = false;
    no_dk = AfterBoxChange(no_dk);
    REQUIRE(Wanted(no_dk, true) == other);
    // One of the two alone.
    Switches dyn_only = no_dk;
    dyn_only.dk_dyn = true;
    const auto dyn = Wanted(dyn_only, true);
    REQUIRE(dyn.size() == other.size() + 1);
    REQUIRE(dyn.back() == Var{"EDEN_NX2_DK_DYN", "1"});
    Switches pacing_only = no_dk;
    pacing_only.dk_pacing = true;
    const auto pacing = Wanted(pacing_only, true);
    REQUIRE(pacing.size() == other.size() + 3);
    REQUIRE(pacing.back() == Var{"EDEN_NX2_PACE_SPIN_US", "1500"});
}

TEST_CASE("Nx2PerfSettings: a variable set outside the program wins", "[core]") {
    REQUIRE(Plan(Balanced(), true, NoneOutside) == BalancedBananza);

    // A launcher (or the user) set two of them: those are left out, whatever their value.
    const std::set<std::string_view> outside{"EDEN_NX2_JIT_SAME", "EDEN_NX2_MODE_PACE"};
    const auto is_outside = [&](std::string_view name) { return outside.count(name) != 0; };
    const auto plan = Plan(Balanced(), true, is_outside);
    REQUIRE(plan.size() == BalancedBananza.size() - 2);
    for (const Var& var : plan) {
        REQUIRE(outside.count(var.name) == 0);
    }

    // Set outside and turned off here: still theirs (the plan never clears a variable).
    Switches off = Balanced();
    off.jit_same = false;
    REQUIRE(Plan(AfterBoxChange(off), true, is_outside) == plan);

    // Safe's shortcuts set outside: theirs too.
    const auto light = [](std::string_view name) { return name == "EDEN_NX2_LIGHT_DRAW"; };
    REQUIRE(Plan(AllOff(), true, light).size() == SafeOff.size() - 1);

    // Everything set outside: nothing to set.
    REQUIRE(Plan(Balanced(), true, [](std::string_view) { return true; }).empty());
    REQUIRE(Plan(AllOff(), true, [](std::string_view) { return true; }).empty());
}

TEST_CASE("Nx2PerfSettings: every name is one a preset can set", "[core]") {
    std::set<std::string_view> wanted;
    for (const Preset preset :
         {Preset::Balanced, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Var& var : Wanted(FromPreset(preset, Balanced()), true)) {
            wanted.insert(var.name);
        }
    }
    std::set<std::string_view> names;
    ForEachName([&](std::string_view name) { names.insert(name); });
    REQUIRE(names.size() == Names.size() + SafeOff.size()); // no name twice
    REQUIRE(wanted == names);
}

TEST_CASE("Nx2PerfSettings: a boot that needs other variables differs from the session's", "[core]") {
    // What the session compares (yuzu/nx2perf_session.cpp): the plan of the first boot.
    const auto first_bananza = Plan(Balanced(), IsBananza(Bananza), NoneOutside);
    REQUIRE(Plan(Balanced(), IsBananza(Bananza), NoneOutside) == first_bananza);
    REQUIRE(Plan(Balanced(), IsBananza(OtherGame), NoneOutside) != first_bananza);
    Switches changed = Balanced();
    changed.osd = false;
    REQUIRE(Plan(AfterBoxChange(changed), true, NoneOutside) != first_bananza);
    // Bananza's own switches set outside: another game needs nothing else, no restart.
    const auto outside = [](std::string_view name) { return BananzaOnly.count(name) != 0; };
    REQUIRE(Plan(Balanced(), true, outside) == Plan(Balanced(), false, outside));
}

TEST_CASE("Nx2PerfSettings: saving settings needs a restart only after a game set them", "[core]") {
    const auto plan_now = [](const Switches& switches) {
        return Plan(switches, true, NoneOutside);
    };
    // No game started in this process yet: never (they apply to the first game).
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(AllOff())));
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(Balanced())));
    // Set for a game: only when the variables would differ.
    const std::optional<std::vector<Var>> applied = plan_now(Balanced());
    REQUIRE(!NeedsRestart(applied, plan_now(Balanced())));
    REQUIRE(!NeedsRestart(applied, plan_now(Custom(Balanced())))); // same boxes, same variables
    REQUIRE(NeedsRestart(applied, plan_now(AllOff())));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Performance, Balanced()))));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Reactive, Balanced()))));
    Switches no_osd = Balanced();
    no_osd.osd = false;
    REQUIRE(NeedsRestart(applied, plan_now(AfterBoxChange(no_osd))));
    // A change that only touches variables set outside: nothing of ours differs, no restart.
    const auto osd_outside = [](std::string_view name) { return name == "EDEN_NX2_OSD"; };
    const std::optional<std::vector<Var>> applied_outside =
        Plan(Balanced(), true, osd_outside);
    REQUIRE(!NeedsRestart(applied_outside, Plan(AfterBoxChange(no_osd), true, osd_outside)));
}

TEST_CASE("Nx2PerfSettings: the frame generation choice and the Graphics settings", "[core]") {
    const FrameGenSettings off{};
    // Each choice gives the settings that read back as that choice.
    for (const FrameGen choice : {FrameGen::Off, FrameGen::Fixed2x, FrameGen::Dynamic}) {
        REQUIRE(FrameGenFrom(FrameGenApply(choice, off)) == choice);
    }
    REQUIRE(FrameGenApply(FrameGen::Fixed2x, off) ==
            FrameGenSettings{.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 0});
    REQUIRE(FrameGenApply(FrameGen::Dynamic, off).target_rate == DYNAMIC_TARGET_RATE);
    REQUIRE(DYNAMIC_TARGET_RATE == 120);
    // Off only turns it off; Other changes nothing.
    const FrameGenSettings lsfg3{.on = true, .fsr3 = false, .multiplier = 3, .target_rate = 0};
    REQUIRE(FrameGenFrom(lsfg3) == FrameGen::Other);
    REQUIRE(FrameGenApply(FrameGen::Off, lsfg3) ==
            FrameGenSettings{.on = false, .fsr3 = false, .multiplier = 3, .target_rate = 0});
    REQUIRE(FrameGenApply(FrameGen::Other, lsfg3) == lsfg3);
    // Dynamic switches the type to FSR3 and keeps the multiplier (the target rate decides).
    REQUIRE(FrameGenApply(FrameGen::Dynamic, lsfg3) ==
            FrameGenSettings{.on = true, .fsr3 = true, .multiplier = 3, .target_rate = 120});
    // FSR3 with another multiplier or target rate is the Graphics tab's own setup.
    REQUIRE(FrameGenFrom({.on = true, .fsr3 = true, .multiplier = 3, .target_rate = 0}) ==
            FrameGen::Other);
    REQUIRE(FrameGenFrom({.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 60}) ==
            FrameGen::Other);
    REQUIRE(FrameGenFrom({.on = false, .fsr3 = true, .multiplier = 2, .target_rate = 120}) ==
            FrameGen::Off);
    // Presets leave it, Safe turns it off.
    REQUIRE(FrameGenFor(Preset::Safe) == FrameGen::Off);
    REQUIRE(!FrameGenFor(Preset::Balanced).has_value());
    REQUIRE(!FrameGenFor(Preset::Performance).has_value());
    REQUIRE(!FrameGenFor(Preset::Reactive).has_value());
    REQUIRE(!FrameGenFor(Preset::Custom).has_value());
}
