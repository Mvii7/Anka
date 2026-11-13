// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"

// EDEN_NX2_DK_DYN on the vi2 side (nx2_gsv.h), once Bananza's dynamic frame rate patch went into
// its main module (core/loader/nx2_dk_dynamic_fps.h, Core::g_nx2_dk_dyn_applied) and Bananza is
// the application. Only then: told VRR works the patched game times its own frames, while the
// unpatched game on its VRR path kept its rate at 60 and ran at ~0.73x (E-2).
namespace Service::Nx2DkDynPace {

constexpr u64 ProgramId = 0x040074A01BF12000ULL;

/// vi2 107's reply with VRR supported: bit 63 of its first u64, where the SDK reads it (E-2's
/// "107: 0x0 0x80000000"). ZendNevs patched the SDK's QueryVrrSupported instead.
constexpr std::array<u32, 2> VrrReply{0x0, 0x80000000};

constexpr bool ReportVrr(bool applied, u64 application) {
    return applied && application == ProgramId;
}

/// Presents paced to every vblank (60 Hz at most) instead of the 2 or 3 the game asks for, and
/// EDEN_NX2_MODE_PACE set aside: holding a frame for its interval stretched the whole frame time
/// the patched game measures and kept it at 20-30 fps (ZendNevs's pace_one). EDEN_NX2_PACE_ONE=0
/// keeps the interval. EDEN_NX2_MODE_GOV (off with EDEN_NX2_MODE_PACE) is left as it is: it also
/// drives the GPU timer.
constexpr bool PaceOne(bool applied, u64 application, const char* pace_one) {
    return ReportVrr(applied, application) && !(pace_one != nullptr && pace_one[0] == '0');
}

/// EDEN_NX2_MKW_DYN on the vi2 side: Mario Kart World's patch (core/loader/nx2_mkw_dynamic_fps.h,
/// Core::g_nx2_mkw_dyn_applied) went in and it is the application. Presents are paced to every
/// vblank like Bananza's; VRR is not reported to it. EDEN_NX2_PACE_ONE=0 keeps the interval.
constexpr u64 MkwProgramId = 0x0400C3F00006E000ULL;

constexpr bool PaceOneMkw(bool applied, u64 application, const char* pace_one) {
    return applied && application == MkwProgramId && !(pace_one != nullptr && pace_one[0] == '0');
}

/// The patched clock steps by the frame's time up to 2.5 vblanks (24 fps): a guess at the game's
/// speed from the presents' intervals, for the 600 present stats line. A guess only: the step is
/// the game's own, from its frame time.
struct StepEstimate {
    static constexpr long long CapUs = 41'667;

    long long frame_us{};
    long long stepped_us{};
    u32 capped{};

    constexpr void Add(long long us) {
        frame_us += us;
        stepped_us += (std::min)(us, CapUs);
        capped += us > CapUs ? 1 : 0;
    }

    constexpr double Speed() const {
        return frame_us > 0 ? static_cast<double>(stepped_us) / static_cast<double>(frame_us) : 1.0;
    }
};

} // namespace Service::Nx2DkDynPace
