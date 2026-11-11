// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "common/common_types.h"

// EDEN_NX2_MKW_DYN=1: Mario Kart World's dynamic frame rate, from Eglantine/Lichii (the dynamic
// mode only). The main module is patched as it loads. vi2 then presents every vblank, or the
// game still sleeps in two-vblank steps (Core::g_nx2_mkw_dyn_applied).
namespace Loader::Nx2MkwDynamicFps {

constexpr u64 ProgramId = 0x0400C3F00006E000ULL;

constexpr bool Enabled(const char* value) {
    return value != nullptr && value[0] == '1';
}

constexpr u32 ModeLoad = 0xB9421268;      // ldr w8, [x19, #0x210]
constexpr u32 ModeCompare = 0x6B01013F;   // cmp w9, w1
constexpr u32 ModeBranch = 0x54000001;    // b.ne, any imm19
constexpr u32 ModeBranchMask = 0xFF00001F;
constexpr u32 ModeTest = 0x35000008;      // cbnz w8, any imm19
constexpr u32 ModeTestMask = 0xFF00001F;
constexpr u32 ModeSixty = 0x52A84E08;     // mov w8, #60.0
constexpr u32 ModeDynamic = 0x52800008;   // mov w8, #0
constexpr u32 FrameTimeLoad = 0xF9409660; // ldr xN, [x19, #0x128]
constexpr u32 FrameTimeMask = 0xFFFFFFE0;
constexpr u32 WholeFrameTime = 0xF9407660; // ldr xN, [x19, #0xe8]

struct Result {
    bool applied{};
    size_t mode{};
    size_t frame_time{};
};

inline std::optional<Result> Apply(std::span<u8> text) {
    Result result;
    if (text.size() < 16 || text.size() % sizeof(u32) != 0) {
        return result;
    }
    const size_t count = text.size() / sizeof(u32);
    auto* const words = reinterpret_cast<u32*>(text.data());
    size_t mode = 0;
    int mode_matches = 0;
    for (size_t i = 2; i + 8 < count; ++i) {
        if (words[i] != ModeLoad || words[i - 2] != ModeCompare ||
            (words[i - 1] & ModeBranchMask) != ModeBranch ||
            (words[i + 1] & ModeTestMask) != ModeTest) {
            continue;
        }
        bool sixty = false;
        for (size_t j = i + 2; j < i + 8; ++j) {
            sixty = sixty || words[j] == ModeSixty;
        }
        if (!sixty) {
            continue;
        }
        mode = i;
        ++mode_matches;
    }
    if (mode_matches != 1) {
        return result;
    }
    size_t frame_time = 0;
    int frame_matches = 0;
    const size_t end = mode + 1 + 32 < count ? mode + 1 + 32 : count;
    for (size_t j = mode + 1; j < end; ++j) {
        if ((words[j] & FrameTimeMask) == FrameTimeLoad) {
            frame_time = j;
            ++frame_matches;
        }
    }
    if (frame_matches != 1) {
        return result;
    }
    const u32 frame_word = words[frame_time];
    words[mode] = ModeDynamic;
    words[frame_time] = WholeFrameTime | (frame_word & 0x1F);
    result.applied = true;
    result.mode = mode;
    result.frame_time = frame_time;
    return result;
}

/// Nullopt unless the switch is on and this is Mario Kart World's main module.
inline std::optional<Result> ApplyIfEnabled(const char* switch_value, u64 program_id,
                                            std::string_view module_name, std::span<u8> text) {
    if (!Enabled(switch_value) || module_name != "main" || program_id != ProgramId) {
        return std::nullopt;
    }
    return Apply(text);
}

} // namespace Loader::Nx2MkwDynamicFps
