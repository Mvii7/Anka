// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bitset>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_front_same.h"

using namespace VideoCommon::Nx2FrontSame;

TEST_CASE("Front same filter table", "[video_core][nx2_front_same]") {
    std::mt19937 rng{7};
    for (int round = 0; round < 8; ++round) {
        std::bitset<0x10000> mask;
        for (std::size_t method = 0; method < mask.size(); ++method) {
            mask[method] = rng() % 3 == 0;
        }
        // Round 0: nothing executable; the MME DMA reads stay out all the same.
        if (round == 0) {
            mask.reset();
        }
        const Table table = BuildTable(mask);
        REQUIRE(table.size() == NumRegs);
        for (u32 method = 0; method < NumRegs; ++method) {
            const bool expected = !mask[method] && method != DmaRead && method != DmaReadFifoed;
            REQUIRE((table[method] != 0) == expected);
            REQUIRE(table[method] <= 1);
        }
        REQUIRE(table[DmaRead] == 0);
        REQUIRE(table[DmaReadFifoed] == 0);
        // The bits past the registers (the macro range) change nothing.
        std::bitset<0x10000> high = mask;
        for (std::size_t method = NumRegs; method < high.size(); ++method) {
            high[method] = !high[method];
        }
        REQUIRE(BuildTable(high) == table);
    }
    for (const u8 entry : ZeroTable) {
        REQUIRE(entry == 0);
    }
}

TEST_CASE("Front same filter arming", "[video_core][nx2_front_same]") {
    const Table table = BuildTable(std::bitset<NumRegs>{});
    const u8* const t = table.data();
    // Every input on its own; the first reason in order wins.
    REQUIRE(Why(true, Track, false, false, false, 0) == Disarm::None);
    REQUIRE(Why(false, Track, false, false, false, ~0U) == Disarm::Skip);
    REQUIRE(Why(true, Replay, false, false, false, ~0U) == Disarm::ShadowReplay);
    REQUIRE(Why(true, Track, true, false, false, ~0U) == Disarm::Check);
    REQUIRE(Why(true, Track, false, true, false, ~0U) == Disarm::Diag);
    REQUIRE(Why(true, Track, false, false, true, 0) == Disarm::Gap);
    REQUIRE(Why(true, Track, false, false, true, GapMethods - 1) == Disarm::Gap);
    REQUIRE(Why(true, Track, false, false, true, GapMethods) == Disarm::None);
    REQUIRE(Why(true, Track, false, false, true, ~0U) == Disarm::None);
    // The gap counts only with the profile (it stays at 0 without).
    REQUIRE(Why(true, Track, false, false, false, 0) == Disarm::None);
    REQUIRE(Why(false, Replay, true, true, true, 0) == Disarm::Skip);
    REQUIRE(Why(true, Replay, true, true, true, 0) == Disarm::ShadowReplay);
    REQUIRE(Why(true, Track, true, true, true, 0) == Disarm::Check);
    REQUIRE(Why(true, Track, false, true, true, 0) == Disarm::Diag);
    for (const u32 control : {Track, TrackWithFilter, Passthrough, 7U, 0xffffffffU}) {
        REQUIRE(Why(true, control, false, false, false, 0) == Disarm::None);
        REQUIRE(Arm(t, true, control, false, false, false, 0) == t);
    }
    REQUIRE(Arm(t, false, Track, false, false, false, 0) == ZeroTable.data());
    REQUIRE(Arm(t, true, Replay, false, false, false, 0) == ZeroTable.data());
    REQUIRE(Arm(t, true, Track, true, false, false, 0) == ZeroTable.data());
    REQUIRE(Arm(t, true, Track, false, true, false, 0) == ZeroTable.data());
    REQUIRE(Arm(t, true, Track, false, false, true, 3) == ZeroTable.data());
    REQUIRE(Arm(t, true, Track, false, false, true, GapMethods) == t);
    // Exhaustively against the rule.
    for (u32 bits = 0; bits < 64; ++bits) {
        const bool skip = (bits & 1) != 0;
        const u32 control = (bits >> 1) & 3;
        const bool check = (bits & 8) != 0;
        const bool diag = (bits & 16) != 0;
        const bool profile = (bits & 32) != 0;
        for (const u32 gap : {0U, 15U, 16U, ~0U}) {
            const bool armed =
                skip && control != Replay && !check && !diag && !(profile && gap < 16);
            REQUIRE((Arm(t, skip, control, check, diag, profile, gap) == t) == armed);
            REQUIRE((Why(skip, control, check, diag, profile, gap) == Disarm::None) == armed);
        }
    }
    REQUIRE(disarm_names.size() == static_cast<std::size_t>(Disarm::Count));
}

TEST_CASE("Front same filter context", "[video_core][nx2_front_same]") {
    std::vector<u8> table(0x2000, 1); // Past the registers: entries a check would take
    std::vector<u32> regs(0x2000, 5);
    std::vector<u32> shadow(0x2000, 5);
    for (u32 method = 0; method < NumRegs; ++method) {
        table[method] = method % 3 != 0 ? 1 : 0;
    }
    REQUIRE(CmpTarget(Track, regs.data(), shadow.data()) == shadow.data());
    REQUIRE(CmpTarget(TrackWithFilter, regs.data(), shadow.data()) == shadow.data());
    REQUIRE(CmpTarget(Passthrough, regs.data(), shadow.data()) == regs.data());
    REQUIRE(CmpTarget(Replay, regs.data(), shadow.data()) == regs.data());
    REQUIRE(CmpTarget(9, regs.data(), shadow.data()) == regs.data());

    Filter filter;
    REQUIRE(filter.ctx.table == ZeroTable.data());
    filter.ctx.hits = 9;
    filter.Begin(table.data(), true, Track, false, false, false, ~0U, regs.data(), shadow.data());
    REQUIRE(filter.why == Disarm::None);
    REQUIRE(filter.ctx.table == table.data());
    REQUIRE(filter.ctx.regs == regs.data());
    REQUIRE(filter.ctx.cmp2 == shadow.data());
    REQUIRE(filter.ctx.hits == 0);
    // Hit: in the table, register and shadow RAM hold the value.
    REQUIRE(Hit(filter.ctx, 1, 5));
    REQUIRE(!Hit(filter.ctx, 0, 5));
    REQUIRE(!Hit(filter.ctx, 1, 6));
    shadow[1] = 6;
    REQUIRE(!Hit(filter.ctx, 1, 5));
    REQUIRE(!Hit(filter.ctx, 1, 6));
    regs[2] = 6;
    REQUIRE(!Hit(filter.ctx, 2, 5));
    REQUIRE(!Hit(filter.ctx, 2, 6));
    // Past the registers: never (the padding would say yes).
    for (const u32 method : {NumRegs, NumRegs + 1, 0xfffU, 0x1000U, 0x1fffU}) {
        REQUIRE(!Hit(filter.ctx, method, 5));
        REQUIRE(!InScope(filter.ctx, method));
    }
    REQUIRE(InScope(filter.ctx, 1));
    REQUIRE(!InScope(filter.ctx, 3));
    REQUIRE(InScope(filter.ctx, NumRegs - 1) == (table[NumRegs - 1] != 0));
    // Passthrough: the register alone.
    filter.Control(Passthrough, regs.data(), shadow.data());
    REQUIRE(filter.ctx.cmp2 == regs.data());
    REQUIRE(filter.ctx.table == table.data());
    REQUIRE(Hit(filter.ctx, 1, 5));
    // Replay inside the call: nothing; Track again: armed again.
    filter.Control(Replay, regs.data(), shadow.data());
    REQUIRE(filter.ctx.table == ZeroTable.data());
    REQUIRE(!Hit(filter.ctx, 1, 5));
    filter.Control(TrackWithFilter, regs.data(), shadow.data());
    REQUIRE(filter.ctx.table == table.data());
    REQUIRE(filter.ctx.cmp2 == shadow.data());
    // A call that starts in Replay: armed once the macro leaves it.
    filter.Begin(table.data(), true, Replay, false, false, false, ~0U, regs.data(), shadow.data());
    REQUIRE(filter.why == Disarm::ShadowReplay);
    REQUIRE(filter.ctx.table == ZeroTable.data());
    filter.Control(Track, regs.data(), shadow.data());
    REQUIRE(filter.ctx.table == table.data());
    // A call disarmed otherwise stays so whatever the mode.
    for (const auto& [skip, check, diag, profile, gap, why] :
         std::vector<std::tuple<bool, bool, bool, bool, u32, Disarm>>{
             {false, false, false, false, ~0U, Disarm::Skip},
             {true, true, false, false, ~0U, Disarm::Check},
             {true, false, true, false, ~0U, Disarm::Diag},
             {true, false, false, true, 4, Disarm::Gap}}) {
        filter.Begin(table.data(), skip, Track, check, diag, profile, gap, regs.data(),
                     shadow.data());
        REQUIRE(filter.why == why);
        REQUIRE(filter.ctx.table == ZeroTable.data());
        for (const u32 control : {Track, TrackWithFilter, Passthrough, Replay, 7U}) {
            filter.Control(control, regs.data(), shadow.data());
            REQUIRE(filter.ctx.table == ZeroTable.data());
        }
    }
}

TEST_CASE("Front same filter counts", "[video_core][nx2_front_same]") {
    Filter filter;
    filter.ctx.hits = 12;
    for (const u32 mode : {1U, 3U}) {
        filter.mode = mode;
        REQUIRE(filter.Dropped() == 12);
    }
    filter.mode = 2;
    REQUIRE(filter.Dropped() == 0);
    // Mismatches: the first 16 logged, then the powers of two.
    std::vector<u64> logged;
    for (u64 i = 1; i <= 300; ++i) {
        if (filter.NoteMismatch()) {
            logged.push_back(i);
        }
    }
    REQUIRE(filter.mismatches == 300);
    REQUIRE(logged == std::vector<u64>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32,
                                       64, 128, 256});
    // The call's own mismatches and misses, from Begin on (the profile line adds them up).
    filter.missed = 7;
    std::array<u32, NumRegs> regs{};
    std::array<u32, NumRegs> shadow{};
    const Table table{};
    filter.Begin(table.data(), true, Track, false, false, false, ~0U, regs.data(), shadow.data());
    REQUIRE(filter.CallMismatches() == 0);
    REQUIRE(filter.CallMissed() == 0);
    (void)filter.NoteMismatch();
    (void)filter.NoteMismatch();
    ++filter.missed;
    REQUIRE(filter.CallMismatches() == 2);
    REQUIRE(filter.CallMissed() == 1);
    REQUIRE(filter.mismatches == 302);
    filter.Begin(table.data(), true, Track, false, false, false, ~0U, regs.data(), shadow.data());
    REQUIRE(filter.CallMismatches() == 0);
    REQUIRE(filter.CallMissed() == 0);
    REQUIRE(filter.missed == 8);

    REQUIRE(ShouldLog(1));
    REQUIRE(!ShouldLog(17));
    REQUIRE(ShouldLog(1U << 20));
    REQUIRE(!ShouldLog((1U << 20) + 1));

    REQUIRE(ParseMode(nullptr) == 0);
    REQUIRE(ParseMode("") == 0);
    REQUIRE(ParseMode("0") == 0);
    REQUIRE(ParseMode("1") == 1);
    REQUIRE(ParseMode("2") == 2);
    REQUIRE(ParseMode("3") == 3);
    REQUIRE(ParseMode("4") == 0);
    REQUIRE(ParseMode("x") == 0);

    // Lean counts: due every 4096 writes; the totals as they came.
    LeanCounts lean;
    u64 writes = 0;
    u64 same = 0;
    u64 flushes = 0;
    for (u64 i = 1; i <= 3 * LeanCounts::Every + 100; ++i) {
        const bool due = lean.Write();
        if (i % 3 == 0) {
            ++lean.same;
        }
        REQUIRE(due == (i % LeanCounts::Every == 0));
        if (due) {
            const auto [w, s] = lean.Take();
            REQUIRE(w == LeanCounts::Every);
            writes += w;
            same += s;
            ++flushes;
            REQUIRE(lean.writes == 0);
            REQUIRE(lean.same == 0);
        }
    }
    const auto [w, s] = lean.Take();
    writes += w;
    same += s;
    REQUIRE(flushes == 3);
    REQUIRE(writes == 3 * LeanCounts::Every + 100);
    REQUIRE(same == (3 * LeanCounts::Every + 100) / 3);
    REQUIRE(lean.Take() == std::pair<u64, u64>{0, 0});
}
