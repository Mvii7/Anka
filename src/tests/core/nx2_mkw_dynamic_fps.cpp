// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_mkw_dynamic_fps.h"

namespace {
using namespace Loader::Nx2MkwDynamicFps;

constexpr u32 Filler = 0x12345678;
constexpr u32 TextSize = 0x4000;
constexpr size_t Mode = 0x100;
constexpr size_t Wait = 0x300;
constexpr u32 WaitSkip = 0x20; // the b.lo's imm19, in words

struct Text {
    std::vector<u32> words = std::vector<u32>(TextSize / 4, Filler);

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }

    void PutMode(size_t at) {
        words[at - 2] = 0x6B01013F; // cmp w9, w1
        words[at - 1] = 0x54000041; // b.ne
        words[at] = ModeLoad;
        words[at + 1] = 0x35000048; // cbnz w8
        words[at + 4] = Mov60;
    }

    void PutFrameTime(size_t at, u32 reg = 9) {
        words[at] = FrameCpuTime | reg;
    }

    void PutWait(size_t at, u32 skip = WaitSkip) {
        words[at] = WaitLoadInterval;
        words[at + 1] = WaitCompare;
        words[at + 2] = 0x54000003 | (skip << 5); // b.lo
        words[at + 4] = Mov60;
    }

    void PutAll() {
        PutMode(Mode);
        PutFrameTime(Mode + 10);
        PutWait(Wait);
    }
};
} // Anonymous namespace

TEST_CASE("Nx2MkwDynamicFps: the three sites are rewritten", "[core]") {
    Text text;
    text.PutAll();
    const auto result = Apply(text.Bytes(), TextSize);
    REQUIRE(result.applied);
    REQUIRE(result.mode == Mode);
    REQUIRE(result.frame_time == Mode + 10);
    REQUIRE(result.vblank_wait == Wait + 2);
    REQUIRE(text.words[Mode] == 0x52800008);                   // mov w8, #0
    REQUIRE(text.words[Mode + 10] == (0xF9407660U | 9));       // ldr x9, [x19, #0xe8]
    REQUIRE(text.words[Wait + 2] == (0x14000000U | WaitSkip)); // b, same target
    REQUIRE(text.words[Wait + 1] == WaitCompare);
}

TEST_CASE("Nx2MkwDynamicFps: a backward b.lo keeps its target", "[core]") {
    Text text;
    text.PutAll();
    text.PutWait(Wait, 0x7FFF0); // imm19 -16 words
    REQUIRE(Apply(text.Bytes(), TextSize).applied);
    REQUIRE(text.words[Wait + 2] == (0x14000000U | 0x3FFFFF0U));
}

TEST_CASE("Nx2MkwDynamicFps: nothing changes unless each site matches once", "[core]") {
    const auto untouched = [](auto&& build, size_t mode, size_t frame, size_t wait) {
        Text text;
        text.PutAll();
        build(text);
        const auto before = text.words;
        const auto result = Apply(text.Bytes(), TextSize);
        REQUIRE(!result.applied);
        REQUIRE(result.mode_matches == mode);
        REQUIRE(result.frame_time_matches == frame);
        REQUIRE(result.vblank_wait_matches == wait);
        REQUIRE(text.words == before);
    };
    untouched([](Text& t) { t.words[Mode + 4] = Filler; }, 0, 0, 1);
    untouched([](Text& t) { t.PutMode(0x800); t.PutFrameTime(0x80a); }, 2, 0, 1);
    untouched([](Text& t) { t.words[Mode + 10] = Filler; }, 1, 0, 1);
    untouched([](Text& t) { t.PutFrameTime(Mode + 12, 10); }, 1, 2, 1);
    untouched([](Text& t) { t.words[Wait + 4] = Filler; }, 1, 1, 0);
    untouched([](Text& t) { t.PutWait(0x900); }, 1, 1, 2);
    // A b.ne instead of the b.lo.
    untouched([](Text& t) { t.words[Wait + 2] = 0x54000001; }, 1, 1, 0);
}

TEST_CASE("Nx2MkwDynamicFps: the entry applies to Mario Kart World's main module with the switch",
          "[core]") {
    const auto untouched = [](const char* value, u64 program_id, std::string_view module) {
        Text text;
        text.PutAll();
        const auto before = text.words;
        REQUIRE(!ApplyIfEnabled(value, program_id, module, text.Bytes(), TextSize).has_value());
        REQUIRE(text.words == before);
    };
    untouched(nullptr, ProgramId, "main");
    untouched("", ProgramId, "main");
    untouched("0", ProgramId, "main");
    untouched("1", 0x040074A01BF12000ULL, "main"); // another title
    untouched("1", ProgramId, "sdk");
    Text text;
    text.PutAll();
    const auto result = ApplyIfEnabled("1", ProgramId, "main", text.Bytes(), TextSize);
    REQUIRE(result.has_value());
    REQUIRE(result->applied);
}

TEST_CASE("Nx2MkwDynamicFps: a text segment larger than the span is refused", "[core]") {
    Text text;
    text.PutAll();
    REQUIRE(!Apply(text.Bytes().first(TextSize / 2), TextSize).applied);
}
