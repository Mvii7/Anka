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
constexpr u64 MarioKartWorld = 0x0400C3F00006E000ULL;

// start-recommended.bat's lines (dist/dozer), DK_DYN and OSD at their defaults (1).
const std::vector<Var> RecommendedBananza{
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

const std::vector<Var> BananzaVars{
    {"EDEN_NX2_DK_DYN", "1"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};

const std::vector<Var> MkwVars{
    {"EDEN_NX2_MKW_DYN", "1"},
};

// The game-specific variables set for a game (before the preset's own).
std::vector<Var> GameVars(Game game) {
    switch (game) {
    case Game::Bananza:
        return BananzaVars;
    case Game::MarioKartWorld:
        return MkwVars;
    case Game::Other:
        break;
    }
    return {};
}

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

TEST_CASE("Nx2PerfSettings: the first start is the recommended setup", "[core]") {
    const Ini empty;
    REQUIRE(empty.Read() == Recommended());
    REQUIRE(empty.Read().preset == Preset::Recommended);
    REQUIRE(Wanted(Recommended(), Game::Bananza) == RecommendedBananza);
}

TEST_CASE("Nx2PerfSettings: every setting reads back what was written", "[core]") {
    for (const Key& key : Keys) {
        Switches switches = Recommended();
        switches.*key.field = false;
        switches = AfterBoxChange(switches);
        REQUIRE(switches.preset == Preset::Custom);
        Ini ini;
        ini.Write(switches);
        REQUIRE(ini.values.size() == Keys.size() + GameKeys.size());
        REQUIRE(ini.Read() == switches);
        REQUIRE(ini.values.at(std::string{key.name}) == false);
    }
    // The game-specific settings are stored like the others, but are no part of any preset.
    for (const Key& key : GameKeys) {
        for (const Preset preset :
             {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
            Switches switches = FromPreset(preset, Recommended());
            switches.*key.field = false;
            REQUIRE(AfterBoxChange(switches).preset == preset);
            REQUIRE(FromPreset(preset, switches).*key.field == false);
            REQUIRE(FromPreset(Preset::Custom, switches).*key.field == false);
            Ini ini;
            ini.Write(switches);
            REQUIRE(ini.values.at(std::string{key.name}) == false);
            REQUIRE(ini.Read() == switches);
        }
        // Missing from the file (before the setting existed): on.
        Ini old;
        old.Write(Recommended());
        old.values.erase(std::string{key.name});
        REQUIRE(old.Read() == Recommended());
    }
    for (const Preset preset :
         {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
        Ini ini;
        ini.Write(FromPreset(preset, Recommended()));
        REQUIRE(ini.Read() == FromPreset(preset, Recommended()));
    }
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' boxes", "[core]") {
    for (const Preset preset : {Preset::Recommended, Preset::Fast, Preset::Reactive}) {
        const Switches switches = FromPreset(preset, AllOff());
        REQUIRE(switches.preset == preset);
        for (const Key& key : Keys) {
            const bool on = key.name != "read_traps" || preset == Preset::Recommended;
            REQUIRE(switches.*key.field == on);
        }
    }
    REQUIRE(FromPreset(Preset::Recommended, AllOff()).read_traps);
    REQUIRE(!FromPreset(Preset::Fast, AllOff()).read_traps);
    REQUIRE(!FromPreset(Preset::Reactive, AllOff()).read_traps);
    const Switches safe = FromPreset(Preset::Safe, Recommended());
    REQUIRE(safe == AllOff());
    for (const Key& key : Keys) {
        REQUIRE(!(safe.*key.field));
    }
    // Custom keeps the boxes as they are.
    Switches mine = Recommended();
    mine.osd = false;
    REQUIRE(FromPreset(Preset::Custom, mine) == Custom(mine));
    // Reactive Flushing: Recommended and Fast off, Experimental (Reactive) on, Safe and Custom
    // leave it.
    REQUIRE(ReactiveFlushing(Preset::Recommended) == false);
    REQUIRE(ReactiveFlushing(Preset::Fast) == false);
    REQUIRE(ReactiveFlushing(Preset::Reactive) == true);
    REQUIRE(!ReactiveFlushing(Preset::Safe).has_value());
    REQUIRE(!ReactiveFlushing(Preset::Custom).has_value());
    // Barrier feedback loops: on with Reactive (it needs them), the others leave it.
    REQUIRE(BarrierFeedbackLoops(Preset::Reactive) == true);
    REQUIRE(!BarrierFeedbackLoops(Preset::Recommended).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Safe).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Custom).has_value());
}

TEST_CASE("Nx2PerfSettings: a box changed by hand makes it Custom", "[core]") {
    for (const Preset preset :
         {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
        for (const Key& key : Keys) {
            Switches switches = FromPreset(preset, Recommended());
            REQUIRE(AfterBoxChange(switches).preset == preset);
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == Preset::Custom);
            // Changed back: still the preset's boxes, so the preset stays (a later check).
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == preset);
        }
    }
    // Custom stays Custom whatever the boxes.
    REQUIRE(AfterBoxChange(Custom(Recommended())).preset == Preset::Custom);
    // A file edited by hand: boxes not the stored preset's read as Custom; a bad preset too.
    Ini ini;
    ini.Write(Recommended());
    ini.values["osd"] = false;
    REQUIRE(ini.Read().preset == Preset::Custom);
    Ini bad;
    bad.preset = 9;
    REQUIRE(bad.Read().preset == Preset::Custom);
    // A file from before read_traps: the key is missing, the preset stays, and Fast / Experimental
    // / Safe keep read traps off.
    for (const Preset preset :
         {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
        Ini old;
        old.Write(FromPreset(preset, Recommended()));
        old.values.erase("read_traps");
        REQUIRE(old.Read() == FromPreset(preset, Recommended()));
    }
    Ini stored_fast;
    stored_fast.preset = static_cast<int>(Preset::Fast);
    REQUIRE(stored_fast.Read() == FromPreset(Preset::Fast, Recommended()));
    REQUIRE(IndexOf(Preset::Fast) == 1);
    REQUIRE(PresetOrder[static_cast<std::size_t>(IndexOf(Preset::Reactive))] == Preset::Reactive);
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' variables", "[core]") {
    // Fast: Recommended's, and read traps off (start-fast.bat).
    auto fast = RecommendedBananza;
    fast.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    REQUIRE(Wanted(FromPreset(Preset::Fast, Recommended()), Game::Bananza) == fast);
    // Experimental: Fast's and RX_RT_FIT=3 (start-reactive-experimental.bat).
    auto reactive = fast;
    reactive.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    REQUIRE(Wanted(FromPreset(Preset::Reactive, Recommended()), Game::Bananza) == reactive);
    // Safe: none of the boxes, read traps off, every per-draw shortcut =0 (start-safe.bat).
    // The game-specific settings are not Safe's to change: a game's own variables stay.
    for (const Game game : {Game::Bananza, Game::MarioKartWorld, Game::Other}) {
        const auto safe = Wanted(AllOff(), game);
        const auto own = GameVars(game);
        REQUIRE(safe.size() == own.size() + SafeOff.size() + 1);
        REQUIRE(std::vector<Var>(safe.begin(), safe.begin() + own.size()) == own);
        REQUIRE(safe[own.size()] == Var{"EDEN_NX2_READ_TRAPS", "0"});
        for (std::size_t i = 0; i < SafeOff.size(); ++i) {
            REQUIRE(safe[own.size() + 1 + i] == Var{SafeOff[i], "0"});
        }
    }
    // Custom with everything off sets read traps off only; Custom with the boxes on: only the
    // boxes' (read traps on sets nothing for that switch).
    auto only_traps = BananzaVars;
    only_traps.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    REQUIRE(Wanted(Custom(AllOff()), Game::Bananza) == only_traps);
    REQUIRE(Wanted(Custom(Recommended()), Game::Bananza) == RecommendedBananza);
}

TEST_CASE("Nx2PerfSettings: keys are distinct and cover every field", "[core]") {
    std::set<std::string_view> names;
    for (const Key& key : Keys) {
        names.insert(key.name);
        Switches one = AllOff();
        one.*key.field = true;
        REQUIRE(one != AllOff());
    }
    for (const Key& key : GameKeys) {
        names.insert(key.name);
        Switches one = Recommended();
        one.*key.field = false;
        REQUIRE(one != Recommended());
    }
    REQUIRE(names.size() == Keys.size() + GameKeys.size());
    REQUIRE(names.count("preset") == 0);
    for (const Key& key : Keys) {
        REQUIRE(!(AllOff().*key.field));
        REQUIRE(Recommended().*key.field);
    }
}

TEST_CASE("Nx2PerfSettings: a game by the patch's own program id", "[core]") {
    REQUIRE(GameOf(Loader::Nx2DkDynamicFps::ProgramId) == Game::Bananza);
    REQUIRE(GameOf(Bananza) == Game::Bananza);
    REQUIRE(GameOf(Loader::Nx2MkwDynamicFps::ProgramId) == Game::MarioKartWorld);
    REQUIRE(GameOf(MarioKartWorld) == Game::MarioKartWorld);
    REQUIRE(GameOf(OtherGame) == Game::Other);
    REQUIRE(GameOf(0) == Game::Other);
    // Their updates' ids: the patches check the base ids only.
    REQUIRE(GameOf(Bananza + 0x800) == Game::Other);
    REQUIRE(GameOf(MarioKartWorld + 0x800) == Game::Other);
}

TEST_CASE("Nx2PerfSettings: a game's settings never reach another game", "[core]") {
    for (const Preset preset :
         {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
        for (const Var& var : Wanted(FromPreset(preset, Recommended()), Game::Other)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
            REQUIRE(var.name != "EDEN_NX2_MKW_DYN");
        }
        for (const Var& var : Wanted(FromPreset(preset, Recommended()), Game::MarioKartWorld)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
        }
        for (const Var& var : Wanted(FromPreset(preset, Recommended()), Game::Bananza)) {
            REQUIRE(var.name != "EDEN_NX2_MKW_DYN");
        }
    }
    const auto other = Wanted(Recommended(), GameOf(OtherGame));
    REQUIRE(other.size() == RecommendedBananza.size() - BananzaVars.size());
    // Bananza's dynamic frame rate off: its frame pacing stays, always.
    Switches no_dk = Recommended();
    no_dk.dk_dyn = false;
    const auto pacing = Wanted(no_dk, Game::Bananza);
    REQUIRE(pacing.size() == other.size() + 3);
    REQUIRE(pacing.back() == Var{"EDEN_NX2_PACE_SPIN_US", "1500"});
    // Mario Kart World's switch is its own: Bananza's box does not touch it.
    REQUIRE(Wanted(no_dk, Game::MarioKartWorld) == Wanted(Recommended(), Game::MarioKartWorld));
    auto mkw = other;
    mkw.push_back({"EDEN_NX2_MKW_DYN", "1"});
    REQUIRE(Wanted(Recommended(), Game::MarioKartWorld) == mkw);
    Switches no_mkw = Recommended();
    no_mkw.mkw_dyn = false;
    REQUIRE(Wanted(no_mkw, Game::MarioKartWorld) == other);
    REQUIRE(Wanted(no_mkw, Game::Bananza) == RecommendedBananza);
}

TEST_CASE("Nx2PerfSettings: a variable set outside the program wins", "[core]") {
    REQUIRE(Plan(Recommended(), Game::Bananza, NoneOutside) == RecommendedBananza);

    // A launcher (or the user) set two of them: those are left out, whatever their value.
    const std::set<std::string_view> outside{"EDEN_NX2_JIT_SAME", "EDEN_NX2_MODE_PACE"};
    const auto is_outside = [&](std::string_view name) { return outside.count(name) != 0; };
    const auto plan = Plan(Recommended(), Game::Bananza, is_outside);
    REQUIRE(plan.size() == RecommendedBananza.size() - 2);
    for (const Var& var : plan) {
        REQUIRE(outside.count(var.name) == 0);
    }

    // Set outside and turned off here: still theirs (the plan never clears a variable).
    Switches off = Recommended();
    off.jit_same = false;
    REQUIRE(Plan(AfterBoxChange(off), Game::Bananza, is_outside) == plan);

    // Safe's shortcuts set outside: theirs too.
    const auto light = [](std::string_view name) { return name == "EDEN_NX2_LIGHT_DRAW"; };
    REQUIRE(Plan(AllOff(), Game::Bananza, light).size() == SafeOff.size() + BananzaVars.size());

    // Everything set outside: nothing to set.
    REQUIRE(Plan(Recommended(), Game::Bananza, [](std::string_view) { return true; }).empty());
    REQUIRE(Plan(AllOff(), Game::Bananza, [](std::string_view) { return true; }).empty());
}

TEST_CASE("Nx2PerfSettings: every name is one a preset can set", "[core]") {
    std::set<std::string_view> wanted;
    for (const Preset preset :
         {Preset::Recommended, Preset::Fast, Preset::Reactive, Preset::Safe}) {
        for (const Game game : {Game::Bananza, Game::MarioKartWorld}) {
            for (const Var& var : Wanted(FromPreset(preset, Recommended()), game)) {
                wanted.insert(var.name);
            }
        }
    }
    std::set<std::string_view> names;
    ForEachName([&](std::string_view name) { names.insert(name); });
    REQUIRE(names.size() == Names.size() + SafeOff.size()); // no name twice
    REQUIRE(wanted == names);
}

TEST_CASE("Nx2PerfSettings: a boot that needs other variables differs from the session's", "[core]") {
    // What the session compares (yuzu/nx2perf_session.cpp): the plan of the first boot.
    const auto first_bananza = Plan(Recommended(), GameOf(Bananza), NoneOutside);
    REQUIRE(Plan(Recommended(), GameOf(Bananza), NoneOutside) == first_bananza);
    REQUIRE(Plan(Recommended(), GameOf(OtherGame), NoneOutside) != first_bananza);
    Switches changed = Recommended();
    changed.osd = false;
    REQUIRE(Plan(AfterBoxChange(changed), Game::Bananza, NoneOutside) != first_bananza);
    // Bananza's own switches set outside: another game needs nothing else, no restart.
    const auto outside = [](std::string_view name) { return BananzaOnly.count(name) != 0; };
    REQUIRE(Plan(Recommended(), Game::Bananza, outside) == Plan(Recommended(), Game::Other, outside));
    // Another game's own settings differ from Bananza's, so moving between them restarts.
    REQUIRE(Plan(Recommended(), Game::MarioKartWorld, NoneOutside) != first_bananza);
    REQUIRE(Plan(Recommended(), Game::MarioKartWorld, NoneOutside) !=
            Plan(Recommended(), Game::Other, NoneOutside));
}

TEST_CASE("Nx2PerfSettings: saving settings needs a restart only after a game set them", "[core]") {
    const auto plan_now = [](const Switches& switches) {
        return Plan(switches, Game::Bananza, NoneOutside);
    };
    // No game started in this process yet: never (they apply to the first game).
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(AllOff())));
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(Recommended())));
    // Set for a game: only when the variables would differ.
    const std::optional<std::vector<Var>> applied = plan_now(Recommended());
    REQUIRE(!NeedsRestart(applied, plan_now(Recommended())));
    REQUIRE(!NeedsRestart(applied, plan_now(Custom(Recommended())))); // same boxes, same variables
    REQUIRE(NeedsRestart(applied, plan_now(AllOff())));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Fast, Recommended()))));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Reactive, Recommended()))));
    Switches no_osd = Recommended();
    no_osd.osd = false;
    REQUIRE(NeedsRestart(applied, plan_now(AfterBoxChange(no_osd))));
    // A change that only touches variables set outside: nothing of ours differs, no restart.
    const auto osd_outside = [](std::string_view name) { return name == "EDEN_NX2_OSD"; };
    const std::optional<std::vector<Var>> applied_outside =
        Plan(Recommended(), Game::Bananza, osd_outside);
    REQUIRE(!NeedsRestart(applied_outside, Plan(AfterBoxChange(no_osd), Game::Bananza, osd_outside)));
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
    REQUIRE(!FrameGenFor(Preset::Recommended).has_value());
    REQUIRE(!FrameGenFor(Preset::Fast).has_value());
    REQUIRE(!FrameGenFor(Preset::Reactive).has_value());
    REQUIRE(!FrameGenFor(Preset::Custom).has_value());
}
