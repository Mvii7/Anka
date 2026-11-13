// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_dk_dyn_pace.h"

namespace {
using namespace Service::Nx2DkDynPace;
constexpr u64 Mkw = 0x0400C3F00006E000ULL;
} // Anonymous namespace

TEST_CASE("Nx2DkDynPace: only once the patch went in, only for Bananza", "[core]") {
    REQUIRE(ReportVrr(true, ProgramId));
    REQUIRE(!ReportVrr(false, ProgramId));
    REQUIRE(!ReportVrr(true, Mkw));
    REQUIRE(!ReportVrr(true, 0));
    REQUIRE(PaceOne(true, ProgramId, nullptr));
    REQUIRE(PaceOne(true, ProgramId, "1"));
    REQUIRE(PaceOne(true, ProgramId, ""));
    REQUIRE(!PaceOne(true, ProgramId, "0"));
    REQUIRE(!PaceOne(false, ProgramId, nullptr));
    REQUIRE(!PaceOne(false, ProgramId, "1"));
    REQUIRE(!PaceOne(true, Mkw, nullptr));
    // Bit 63 of the first u64, the second word's top bit (E-2).
    REQUIRE(VrrReply[0] == 0);
    REQUIRE(VrrReply[1] == 0x80000000U);
    REQUIRE(((u64{VrrReply[1]} << 32 | VrrReply[0]) >> 63) == 1);
}

TEST_CASE("Nx2DkDynPace: Mario Kart World paces every vblank once patched, without VRR", "[core]") {
    REQUIRE(MkwProgramId == Mkw);
    REQUIRE(PaceOneMkw(true, Mkw, nullptr));
    REQUIRE(PaceOneMkw(true, Mkw, "1"));
    REQUIRE(!PaceOneMkw(true, Mkw, "0"));
    REQUIRE(!PaceOneMkw(false, Mkw, nullptr));
    REQUIRE(!PaceOneMkw(true, ProgramId, nullptr));
    REQUIRE(!PaceOneMkw(true, 0, nullptr));
}

TEST_CASE("Nx2DkDynPace: the step estimate", "[core]") {
    StepEstimate none;
    REQUIRE(none.Speed() == 1.0);
    StepEstimate even;
    for (int i = 0; i < 600; ++i) {
        even.Add(i % 2 == 0 ? 20'000 : 33'000); // 20 and 33 ms: under the cap
    }
    REQUIRE(even.capped == 0);
    REQUIRE(even.Speed() == 1.0);
    StepEstimate slow;
    slow.Add(16'667);
    slow.Add(83'333); // 2.5 vblanks of it count
    REQUIRE(slow.capped == 1);
    REQUIRE(slow.frame_us == 100'000);
    REQUIRE(slow.stepped_us == 16'667 + 41'667);
    REQUIRE(slow.Speed() > 0.5833);
    REQUIRE(slow.Speed() < 0.5834);
    StepEstimate edge;
    edge.Add(41'667);
    REQUIRE(edge.capped == 0);
    REQUIRE(edge.Speed() == 1.0);
}
