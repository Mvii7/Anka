// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "common/common_types.h"

// EDEN_NX2_MKW_DYN=1 (off by default): Mario Kart World's dynamic frame rate patch, after the
// Lichii fork's (zortos293/Lichii#1332, with SerkiGohan's vblank wait fix). It rewrites the game's
// main module as it loads; unset or any other value leaves the module untouched. Applied, it
// sets Core::g_nx2_mkw_dyn_applied, which nx2_gsv.h reads: presents are paced to every vblank.
//
// The game picks a 1- or 2-vblank frame mode in its main loop and keeps a time account (frame
// cost - interval * 16.67 ms), advancing 1 or 2 steps a frame. It stays in real time while the
// cost it reads is the real frame time, so three words change:
//   mode:       ldr w8, [x19, #0x210]  ->  mov w8, #0 (the game chooses the mode itself)
//   frame time: ldr xN, [x19, #0x128]  ->  ldr xN, [x19, #0xe8] (the whole frame, not the CPU's)
//   vblank wait: b.lo -> b: the vsync function waits until 0.99 * interval / 60 s after the
//     frame start, which made every 2-step frame read as 33 ms and kept the game at 30 fps.
// Sites are found by their instruction sequences, not by address; nothing is changed unless each
// matches exactly once.
namespace Loader::Nx2MkwDynamicFps {

constexpr u64 ProgramId = 0x0400C3F00006E000ULL;

/// EDEN_NX2_MKW_DYN's value: only "1..." turns the patch on.
constexpr bool Enabled(const char* value) {
    return value != nullptr && value[0] == '1';
}

constexpr u32 ModeLoad = 0xB9421268;         // ldr w8, [x19, #0x210]
constexpr u32 ModeSet = 0x52800008;          // mov w8, #0
constexpr u32 FrameCpuTime = 0xF9409660;     // ldr xN, [x19, #0x128], Rt in bits 0-4
constexpr u32 FrameWholeTime = 0xF9407660;   // ldr xN, [x19, #0xe8]
constexpr u32 Mov60 = 0x52A84E08;            // mov w8, #60.0f
constexpr u32 WaitLoadInterval = 0xB9408A68; // ldr w8, [x19, #0x88]
constexpr u32 WaitCompare = 0x7100091F;      // cmp w8, #2

struct Result {
    bool applied{};
    size_t mode{};
    size_t frame_time{};
    size_t vblank_wait{};
    size_t mode_matches{};
    size_t frame_time_matches{};
    size_t vblank_wait_matches{};
};

/// `text` starts at the text segment; `text_size` is its size in the NSO header.
inline Result Apply(std::span<u8> text, u32 text_size) {
    Result result{};
    if (text.size() < text_size) {
        return result;
    }
    u8* const code = text.data();
    const size_t count = text_size / sizeof(u32);
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [code](size_t index, u32 value) {
        std::memcpy(code + index * sizeof(u32), &value, sizeof(value));
    };
    for (size_t i = 2; i + 8 < count; ++i) {
        if (word(i) != ModeLoad || word(i - 2) != 0x6B01013F || // cmp w9, w1
            (word(i - 1) & 0xFF00001F) != 0x54000001 ||         // b.ne
            (word(i + 1) & 0xFF00001F) != 0x35000008) {         // cbnz w8
            continue;
        }
        for (size_t j = i + 2; j < i + 8; ++j) {
            if (word(j) == Mov60) {
                result.mode = i;
                ++result.mode_matches;
                break;
            }
        }
    }
    if (result.mode_matches == 1) {
        const size_t end = (std::min)(result.mode + 32, count);
        for (size_t i = result.mode + 1; i < end; ++i) {
            if ((word(i) & 0xFFFFFFE0) == FrameCpuTime) {
                result.frame_time = i;
                ++result.frame_time_matches;
            }
        }
    }
    for (size_t i = 0; i + 4 < count; ++i) {
        if (word(i) == WaitLoadInterval && word(i + 1) == WaitCompare &&
            (word(i + 2) & 0xFF00001F) == 0x54000003 && word(i + 4) == Mov60) { // b.lo
            result.vblank_wait = i + 2;
            ++result.vblank_wait_matches;
        }
    }
    if (result.mode_matches != 1 || result.frame_time_matches != 1 ||
        result.vblank_wait_matches != 1) {
        return result;
    }
    put(result.mode, ModeSet);
    put(result.frame_time, FrameWholeTime | (word(result.frame_time) & 0x1F));
    // b.lo's imm19 (bits 5-23, in words) is the unconditional branch's imm26.
    const s32 offset = static_cast<s32>(word(result.vblank_wait) << 8) >> 13;
    put(result.vblank_wait, 0x14000000U | (static_cast<u32>(offset) & 0x03FFFFFFU));
    result.applied = true;
    return result;
}

/// The loader's entry: nullopt (the module untouched) unless EDEN_NX2_MKW_DYN is on and this is
/// Mario Kart World's main module.
inline std::optional<Result> ApplyIfEnabled(const char* switch_value, u64 program_id,
                                            std::string_view module_name, std::span<u8> text,
                                            u32 text_size) {
    if (!Enabled(switch_value) || module_name != "main" || program_id != ProgramId) {
        return std::nullopt;
    }
    return Apply(text, text_size);
}

} // namespace Loader::Nx2MkwDynamicFps
