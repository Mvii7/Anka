// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <ranges>
#include <span>

#include "common/logging.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

namespace VideoCommon {

// Mario Kart World's water shader samples a ground-height map this lowering never binds. For this
// one program, an empty pair (both heights zero) keeps the wave at zero; real shallows and dry
// ground still use the game's own amplitudes. From Lichii (zortos293).
inline bool ApplyMkwZeroDepthFallback(std::span<const Shader::Ampere::Instruction> source,
                                      Shader::Ampere::LoweredProgram& program) {
    if (!program || source.size_bytes() != 0x3b0 ||
        Shader::Ampere::NativeProgramKey(source) != 0x5a4f3726590efb17ULL ||
        program.source_instruction_offsets.size() != source.size()) {
        return false;
    }
    constexpr size_t next_source = 0x2d0 / sizeof(Shader::Ampere::Instruction);
    const size_t insert = program.source_instruction_offsets[next_source];
    if (insert > program.instructions.size() ||
        (next_source != 0 && program.source_instruction_offsets[next_source - 1] > insert)) {
        return false;
    }
    // Inserting moves every later instruction. A branch, call, or SSY/PBK/PCNT (Maxwell opcodes
    // 0xe20-0xe2b) would keep a target that no longer matches.
    if (std::ranges::any_of(program.instructions, [](u64 instruction) {
            const u64 opcode = instruction >> 52;
            return opcode >= 0xe20 && opcode <= 0xe2b;
        })) {
        LOG_WARNING(Shader, "Mario Kart World wave fallback skipped: the lowered program branches");
        return false;
    }
    constexpr u64 always = u64{7} << 16;
    constexpr std::array<u64, 3> fallback{
        0x5c40000000000000ULL | always | 250 | (u64{8} << 8) | (u64{9} << 20) | (u64{1} << 41) |
            (u64{7} << 48),
        0x5b50000000000000ULL | always | 250 | (u64{250} << 8) | (u64{255} << 20) |
            (u64{7} << 39) | (u64{1} << 44) | (u64{2} << 49),
        0x5c40000000000000ULL | always | 4 | (u64{4} << 8) | (u64{250} << 20) | (u64{1} << 41) |
            (u64{7} << 48),
    };
    program.instructions.insert(program.instructions.begin() + static_cast<std::ptrdiff_t>(insert),
                                fallback.begin(), fallback.end());
    for (size_t index = next_source; index < program.source_instruction_offsets.size(); ++index) {
        program.source_instruction_offsets[index] += static_cast<u32>(fallback.size());
    }
    return true;
}

} // namespace VideoCommon
