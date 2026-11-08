// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <utility>

#include "common/common_types.h"

// EDEN_NX2_JIT_SAME (Maxwell3D's split front, FrontRunMacro): the front's macro JIT drops the
// state writes FrontWrite would not send without calling back into C++. A write is dropped when
// its method is not executable (nor an MME DMA read, which FrontRunMacro runs after FrontWrite)
// and the front's register and, with the shadow RAM tracked, its shadow RAM copy hold the value.
// =1 drops them in the JIT, =2 checks the JIT's choice against FrontWrite and sends everything,
// =3 drops them in the emit callback (C++). This header keeps the logic and the layout the JIT
// reads (macro_turing_jit.h), apart from the emulator so it can be tested.
namespace VideoCommon::Nx2FrontSame {

inline constexpr u32 NumRegs = 0xE00; ///< Maxwell3D::Regs::NUM_REGS (the macro range starts there)
inline constexpr u32 DmaRead = 0x157;
inline constexpr u32 DmaReadFifoed = 0x158;
/// The only method whose emit appends to the macro's FIFO (Maxwell3D::FrontDmaRead):
/// EDEN_NX2_FRONT_MACRO_LEAN's front JIT reloads the FIFO after it alone.
inline constexpr u32 FifoAppend = DmaReadFifoed;

/// Maxwell3D::Regs::ShadowRamControl. Any other value behaves as Passthrough (FrontWrite).
inline constexpr u32 Track = 0;
inline constexpr u32 TrackWithFilter = 1;
inline constexpr u32 Passthrough = 2;
inline constexpr u32 Replay = 3;

/// What the JIT's emits read (through rbp, at these offsets).
struct Ctx {
    const u8* table; ///< Per method: whether its writes may be dropped (BuildTable), else ZeroTable
    const u32* regs; ///< The front's registers
    const u32* cmp2; ///< What must hold the value too: the shadow RAM when tracked, else regs
    u64 hits;        ///< Writes dropped (=2: that would have been)
};
inline constexpr std::size_t CtxTable = offsetof(Ctx, table);
inline constexpr std::size_t CtxRegs = offsetof(Ctx, regs);
inline constexpr std::size_t CtxCmp2 = offsetof(Ctx, cmp2);
inline constexpr std::size_t CtxHits = offsetof(Ctx, hits);

using Table = std::array<u8, NumRegs>;
inline constexpr Table ZeroTable{};

/// The methods whose same value writes FrontWrite returns from early: not executable, not an
/// MME DMA read.
template <std::size_t N>
[[nodiscard]] Table BuildTable(const std::bitset<N>& execution_mask) {
    static_assert(N >= NumRegs);
    Table table{};
    for (u32 method = 0; method < NumRegs; ++method) {
        table[method] =
            !execution_mask[method] && method != DmaRead && method != DmaReadFifoed ? 1 : 0;
    }
    return table;
}

/// Why a macro call runs without the filter (the profile line counts them).
enum class Disarm : u8 {
    None,
    Skip,   ///< FrontWrite sends same value writes too (EDEN_NX2_SPLIT_SKIP=0, the setting)
    ShadowReplay, ///< Shadow RAM replay: the register gets its shadow copy, not the value
    Check,  ///< The call's writes are compared with NVN's indirect draw (FrontState::checked_writes)
    Diag,   ///< Front diag's register table counts every write (EDEN_NX2_FRONT_DIAG_REGS=0: off)
    Gap,    ///< The profile notes the methods after an indirect draw (FrontState::NoteGapMethod;
            ///< not with EDEN_NX2_PROFILE_LEAN=1, where it notes the methods sent)
    Count,
};

inline constexpr std::array<const char*, static_cast<std::size_t>(Disarm::Count)> disarm_names{
    "armed", "skip", "replay", "check", "diag", "gap",
};

/// The methods noted after each indirect draw (FrontState::NoteGapMethod).
inline constexpr u32 GapMethods = 16;

[[nodiscard]] constexpr Disarm Why(bool skip_same, u32 control, bool has_check, bool diag_regs_on,
                                   bool profile_on, u32 gap_methods) noexcept {
    if (!skip_same) {
        return Disarm::Skip;
    }
    if (control == Replay) {
        return Disarm::ShadowReplay;
    }
    if (has_check) {
        return Disarm::Check;
    }
    if (diag_regs_on) {
        return Disarm::Diag;
    }
    if (profile_on && gap_methods < GapMethods) {
        return Disarm::Gap;
    }
    return Disarm::None;
}

/// The table a call may drop writes with: `table`, or ZeroTable (no write) when disarmed.
[[nodiscard]] constexpr const u8* Arm(const u8* table, bool skip_same, u32 control, bool has_check,
                                      bool diag_regs_on, bool profile_on,
                                      u32 gap_methods) noexcept {
    return Why(skip_same, control, has_check, diag_regs_on, profile_on, gap_methods) ==
                   Disarm::None
               ? table
               : ZeroTable.data();
}

/// The copy that must hold the value besides the register: FrontWrite compares the shadow RAM
/// (and keeps it) when tracked; Passthrough, and any unknown mode, only the register.
[[nodiscard]] constexpr const u32* CmpTarget(u32 control, const u32* regs,
                                             const u32* shadow) noexcept {
    return control == Track || control == TrackWithFilter ? shadow : regs;
}

/// Whether the filter drops a write of `value` to `method` (the JIT's code does the same).
[[nodiscard]] constexpr bool Hit(const Ctx& ctx, u32 method, u32 value) noexcept {
    return method < NumRegs && ctx.table[method] != 0 && ctx.regs[method] == value &&
           ctx.cmp2[method] == value;
}

/// Whether the filter looks at the method's writes at all.
[[nodiscard]] constexpr bool InScope(const Ctx& ctx, u32 method) noexcept {
    return method < NumRegs && ctx.table[method] != 0;
}

/// =2: mismatches logged, the first 16 and then each power of two.
[[nodiscard]] constexpr bool ShouldLog(u64 count) noexcept {
    return count <= 16 || std::has_single_bit(count);
}

/// EDEN_NX2_JIT_SAME: 0 (or unset) off, 1, 2, 3.
[[nodiscard]] constexpr u32 ParseMode(const char* value) noexcept {
    if (value == nullptr) {
        return 0;
    }
    switch (value[0]) {
    case '1':
        return 1;
    case '2':
        return 2;
    case '3':
        return 3;
    default:
        return 0;
    }
}

/// A front's filter (Maxwell3D::FrontState): each macro call's context.
struct Filter {
    Ctx ctx{ZeroTable.data(), nullptr, nullptr, 0};
    /// The call's table for any shadow RAM mode but Replay (a mode written inside the macro
    /// takes it up again: Control).
    const u8* armed{ZeroTable.data()};
    u32 mode{};
    Disarm why{Disarm::None};
    /// =2: FrontWrite sent the write (set there), mismatches and misses so far and as the call
    /// began.
    bool sent{};
    u64 mismatches{};
    u64 missed{};
    u64 begin_mismatches{};
    u64 begin_missed{};

    /// A macro call begins in shadow RAM mode `control`, `table` the front's (BuildTable).
    void Begin(const u8* table, bool skip_same, u32 control, bool has_check, bool diag_regs_on,
               bool profile_on, u32 gap_methods, const u32* regs, const u32* shadow) {
        why = Why(skip_same, control, has_check, diag_regs_on, profile_on, gap_methods);
        armed = Arm(table, skip_same, Track, has_check, diag_regs_on, profile_on, gap_methods);
        Control(control, regs, shadow);
        ctx.hits = 0;
        begin_mismatches = mismatches;
        begin_missed = missed;
    }

    /// The shadow RAM mode is `control` now (at the call's start or written by the macro).
    void Control(u32 control, const u32* regs, const u32* shadow) {
        ctx.table = control == Replay ? ZeroTable.data() : armed;
        ctx.regs = regs;
        ctx.cmp2 = CmpTarget(control, regs, shadow);
    }

    /// After the call: the writes dropped, which FrontWrite would have counted (=2 sent them).
    [[nodiscard]] u64 Dropped() const {
        return mode == 2 ? 0 : ctx.hits;
    }

    /// =2: the JIT would have dropped a write FrontWrite sent. Returns whether to log it.
    [[nodiscard]] bool NoteMismatch() {
        return ShouldLog(++mismatches);
    }

    /// =2: the call's mismatches and misses (since Begin).
    [[nodiscard]] u64 CallMismatches() const {
        return mismatches - begin_mismatches;
    }
    [[nodiscard]] u64 CallMissed() const {
        return missed - begin_missed;
    }
};

/// EDEN_NX2_PROFILE_LEAN=1: FrontWrite's profile counts (MethodWrites, SameValueWrites) in plain
/// counters, added to the profile every `Every` writes and at the end of each front macro.
struct LeanCounts {
    static constexpr u64 Every = 4096;
    u64 writes{};
    u64 same{};

    /// A write counted. True when the counts are due.
    [[nodiscard]] bool Write() {
        return ++writes >= Every;
    }

    /// The counts so far (writes, same value), cleared.
    [[nodiscard]] std::pair<u64, u64> Take() {
        const std::pair<u64, u64> counts{writes, same};
        writes = 0;
        same = 0;
        return counts;
    }
};

} // namespace VideoCommon::Nx2FrontSame
