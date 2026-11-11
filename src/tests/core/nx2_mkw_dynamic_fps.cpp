// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_mkw_dynamic_fps.h"

namespace {
using namespace Loader::Nx2MkwDynamicFps;

std::vector<u32> Program() {
    std::vector<u32> words(16, 0xD503201F);
    words[2] = ModeCompare;
    words[3] = ModeBranch;
    words[4] = ModeLoad;
    words[5] = ModeTest;
    words[6] = ModeSixty;
    words[7] = FrameTimeLoad | 3;
    return words;
}

std::span<u8> Bytes(std::vector<u32>& words) {
    return {reinterpret_cast<u8*>(words.data()), words.size() * sizeof(u32)};
}

} // namespace

TEST_CASE("Mario Kart World dynamic fps patches one mode site", "[core]") {
    auto words = Program();
    const auto applied = Apply(Bytes(words));
    REQUIRE(applied);
    REQUIRE(applied->applied);
    REQUIRE(applied->mode == 4);
    REQUIRE(applied->frame_time == 7);
    REQUIRE(words[4] == ModeDynamic);
    REQUIRE(words[7] == (WholeFrameTime | 3));
    REQUIRE_FALSE(ApplyIfEnabled(nullptr, ProgramId, "main", Bytes(words)));
    REQUIRE_FALSE(ApplyIfEnabled("1", ProgramId, "rtld", Bytes(words)));
    REQUIRE_FALSE(ApplyIfEnabled("1", 1, "main", Bytes(words)));
}

TEST_CASE("Mario Kart World dynamic fps leaves an ambiguous module", "[core]") {
    std::vector<u32> words(24, 0xD503201F);
    words[2] = ModeCompare;
    words[3] = ModeBranch;
    words[4] = ModeLoad;
    words[5] = ModeTest;
    words[6] = ModeSixty;
    words[10] = ModeCompare;
    words[11] = ModeBranch;
    words[12] = ModeLoad;
    words[13] = ModeTest;
    words[14] = ModeSixty;
    const auto applied = Apply(Bytes(words));
    REQUIRE(applied);
    REQUIRE_FALSE(applied->applied);
    REQUIRE(words[4] == ModeLoad);
    REQUIRE(words[12] == ModeLoad);
}
