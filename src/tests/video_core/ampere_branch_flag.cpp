// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

// The conditional branch of a Donkey Kong Bananza fragment effect shader (@!P1 BRA, 80 instructions
// ahead in the real program; here 3), with the offset field's low nibble as the game encodes it.
Instruction Branch(u32 offset_field) {
    return {{0x00009947, offset_field, 0x03800000, 0x000fea00}};
}

std::array<Instruction, 6> Program(u32 offset_field) {
    return {{
        {{0x00007802, 0, 0x00000f00, 0}}, // MOV
        Branch(offset_field),
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x0000794d, 0, 0x03800000, 0}}, // EXIT
    }};
}

TEST_CASE("Ampere BRA with offset flag 1 is a plain branch", "[video_core][ampere]") {
    // Offset 0x30 (three instructions on from the next one) with flag 1: 0x31, as in the game.
    const auto flagged = Program(0x31);
    const auto plain = Program(0x30);
    const auto lowered_flagged = LowerToMaxwell(flagged);
    const auto lowered_plain = LowerToMaxwell(plain);
    REQUIRE(static_cast<bool>(lowered_flagged));
    REQUIRE(static_cast<bool>(lowered_plain));
    REQUIRE(lowered_flagged.instructions == lowered_plain.instructions);
}

TEST_CASE("Ampere BRA capture takes offset flag 1 as a plain branch", "[video_core][ampere]") {
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(Branch(0x31), 1) == ProgramCaptureState::Result::Continue);
}

TEST_CASE("Ampere BRA with other offset flags is still rejected", "[video_core][ampere]") {
    for (const u32 flag : {2U, 3U, 4U, 8U, 15U}) {
        const auto program = Program(0x30 | flag);
        REQUIRE(!static_cast<bool>(LowerToMaxwell(program)));
        ProgramCaptureState capture;
        REQUIRE(capture.Observe(Branch(0x30 | flag), 1) ==
                ProgramCaptureState::Result::InvalidTarget);
    }
}

TEST_CASE("Ampere BRA without flags is unchanged", "[video_core][ampere]") {
    const auto program = Program(0x30);
    REQUIRE(static_cast<bool>(LowerToMaxwell(program)));
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(Branch(0x30), 1) == ProgramCaptureState::Result::Continue);
}
} // namespace
