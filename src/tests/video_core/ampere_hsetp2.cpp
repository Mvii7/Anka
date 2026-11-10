// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string_view>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

// The first entry of the Maxwell decoder's table that matches, as Shader::Maxwell::Decode picks it.
std::string_view MaxwellOpcode(u64 word) {
    const auto matches = [word](std::string_view encoding) {
        u64 mask = 0, value = 0, bit = u64{1} << 63;
        for (const char c : encoding) {
            if (c == '0' || c == '1') {
                mask |= bit;
                value |= c == '1' ? bit : 0;
            }
            if (c != ' ') bit >>= 1;
        }
        return (word & mask) == value;
    };
#define INST(name, cute, encode) \
    if (matches(encode)) return #name;
#include "shader_recompiler/frontend/maxwell/maxwell.inc"
#undef INST
    return {};
}

// Fields as TranslatorVisitor::HSETP2_reg reads them (half_floating_point_set_predicate.cpp).
struct Hsetp2 {
    u32 guard, pred_b, pred_a, ftz, reg_a, reg_b, swizzle_b, abs_b, neg_b, compare, pred, neg_pred,
        neg_a, abs_a, bop, swizzle_a, h_and;
    bool operator==(const Hsetp2&) const = default;
};

Hsetp2 Fields(u64 word) {
    const auto f = [word](u32 offset, u32 count) {
        return static_cast<u32>((word >> offset) & ((u64{1} << count) - 1));
    };
    return {f(16, 4), f(0, 3),  f(3, 3),  f(6, 1),  f(8, 8),  f(20, 8),
            f(28, 2), f(30, 1), f(31, 1), f(35, 4), f(39, 3), f(42, 1),
            f(43, 1), f(44, 1), f(45, 2), f(47, 2), f(49, 1)};
}

u64 LowerOne(const Instruction& insn) {
    const std::array<Instruction, 2> code{insn, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 2);
    return lowered.instructions[0];
}

TEST_CASE("Ampere HSETP2 with a register B lowers to Maxwell HSETP2_R", "[video_core][ampere]") {
    // HSETP2.EQ.AND Pp, PT, Ra.H0_H0, Rb.H1_H1 (or .H0_H0), Pp from title 4's compute programs.
    const std::array<std::pair<Instruction, u64>, 5> cases{{
        {{{0x23007234, 0x30000023, 0x00702800, 0x000fe400}}, 0x5d21001032372307ULL},
        {{{0x28007234, 0x30000028, 0x00702800, 0x000fe400}}, 0x5d21001032872807ULL},
        {{{0x29007234, 0x2000000c, 0x00702800, 0x000fe400}}, 0x5d21001020c72907ULL},
        {{{0x2c007234, 0x3000002c, 0x01742800, 0x000fe400}}, 0x5d21011032c72c17ULL},
        {{{0x34007234, 0x30000034, 0x00702800, 0x000fe400}}, 0x5d21001033473407ULL},
    }};
    for (const auto& [insn, expected] : cases) {
        const u64 word = LowerOne(insn);
        REQUIRE(word == expected);
        REQUIRE(MaxwellOpcode(word) == "HSETP2_reg");
    }
    // P2 variant: HSETP2.EQ.AND P2, PT, R44.H0_H0, R44.H1_H1, P2.
    REQUIRE(Fields(0x5d21011032c72c17ULL) ==
            Hsetp2{7, 7, 2, 0, 44, 44, 3, 0, 0, 2, 2, 0, 0, 0, 0, 2, 0});
}

TEST_CASE("Ampere HSETP2 constant buffer form loads B then compares", "[video_core][ampere]") {
    // HSETP2 with c[bank][offset] in B: the load, then HSETP2_reg from the scratch register.
    Instruction insn{{0x634u | (4u << 24), (0x100u << 6) | (1u << 28), 0, 0}};
    const std::array<Instruction, 2> code{insn, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 3);
    REQUIRE(MaxwellOpcode(lowered.instructions[0]) == "MOV_cbuf");
    REQUIRE(MaxwellOpcode(lowered.instructions[1]) == "HSETP2_reg");
    const Hsetp2 fields = Fields(lowered.instructions[1]);
    REQUIRE(fields.reg_a == 4);
    REQUIRE(fields.reg_b == 250);
    REQUIRE(fields.swizzle_b == 1);
    Instruction misaligned{{0x634u, 2u << 6, 0, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array<Instruction, 2>{misaligned, EXIT})));
}

TEST_CASE("Ampere HSETP2 register form keeps every modifier", "[video_core][ampere]") {
    // @!P4 HSETP2.GTU.H_AND.FTZ.OR P3, P5, -|R10|.H1_H1, -|R11|.H0_H0, !P2
    const Instruction insn{{0x0a00c234, 0xe000000b, 0x0557cfa0, 0}};
    const u64 word = LowerOne(insn);
    REQUIRE(MaxwellOpcode(word) == "HSETP2_reg");
    REQUIRE(Fields(word) == Hsetp2{0xc, 5, 3, 1, 10, 11, 2, 1, 1, 12, 2, 1, 1, 1, 1, 3, 1});

    // The immediate form (0x434) maps the shared fields the same way; B is its scratch R250.
    Instruction immediate = insn;
    immediate.words[0] = (insn.words[0] & ~0xfffU) | 0x434;
    immediate.words[1] = 0x3c003c00;
    const std::array<Instruction, 2> code{immediate, EXIT};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 3);
    auto expected = Fields(word);
    expected.reg_b = 250;
    expected.swizzle_b = 0;
    expected.abs_b = 0;
    expected.neg_b = 0;
    REQUIRE(Fields(lowered.instructions[1]) == expected);
}

TEST_CASE("Ampere HSETP2 register form rejects unknown bits", "[video_core][ampere]") {
    const Instruction valid{{0x34007234, 0x30000034, 0x00702800, 0x000fe400}};
    // Bit 40, bit 64, bit 91, combine 3 (bits 69-70).
    for (const auto& [word, bits] : std::array<std::pair<u32, u32>, 4>{{
             {1, 1U << 8}, {2, 1U << 0}, {2, 1U << 27}, {2, 3U << 5}}}) {
        Instruction insn = valid;
        insn.words[word] |= bits;
        const std::array<Instruction, 2> code{insn, EXIT};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(!static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native half comparison mode");
        REQUIRE(lowered.error_offset == 0);
    }
}
} // namespace
