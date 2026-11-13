// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_quality.h"

namespace {
using namespace Loader::Nx2DkQuality;

constexpr u32 Filler = 0x12345678;
// A text segment of 0x2f04 bytes in 0x3000: the free space starts at word 0xbc4, the terrain
// LOD routines at 0xbc4 + CaveOffset.
constexpr u32 TextSize = 0x2f04;
constexpr u32 AlignedSize = 0x3000;
constexpr size_t Cave = 0xbc4 + CaveOffset;
constexpr size_t MinNear = 0x100, MaxFar = 0x120, MaxNear = 0x140, MinFar = 0x160;
constexpr size_t GpuGetter = 0x200, FrameGetter = 0x210, BlurScaleGetter = 0x220;
constexpr size_t BlurSamplesGetter = 0x230, CubeMapGetter = 0x240;
constexpr size_t BloomFirst = 0x300, BloomSecond = 0x310, BloomThird = 0x320;
constexpr size_t DecalGetter = 0x260, GgxGetter = 0x270, CubeOccGetter = 0x280;
constexpr size_t DepthGetter = 0x290, RadiusGetter = 0x2a0, PolygonGetter = 0x2b0;
constexpr size_t MeshStore = 0x500;
constexpr size_t FarA = 0x600, FarB = 0x620, FarC = 0x640;
constexpr size_t Voxel = 0x400;

struct Text {
    std::vector<u32> words;

    Text() : words(AlignedSize / 4, 0) {
        for (size_t i = 0; i < TextSize / 4; ++i) {
            words[i] = Filler;
        }
        words[MinNear] = LdrX(8, 10, MinRateParam);
        words[MinNear + 1] = LdrS(2, 8, ValueOffset);
        words[MinFar] = LdrX(10, 10, MinRateParam);
        words[MinFar + 9] = LdrS(2, 10, ValueOffset);
        words[MaxNear] = LdrX(8, 10, MaxRateParam);
        words[MaxNear + 1] = LdrS(3, 8, ValueOffset);
        words[MaxFar] = LdrX(10, 10, MaxRateParam);
        words[MaxFar + 11] = LdrS(5, 10, ValueOffset);
        PutGetter(GpuGetter, GpuTargetParam, true);
        PutGetter(FrameGetter, FrameMinScaleParam, true);
        PutGetter(BlurScaleGetter, RadialBlurScaleParam, true);
        PutGetter(DepthGetter, OcclusionDepthParam, true);
        PutGetter(RadiusGetter, OcclusionRadiusParam, true);
        PutGetter(PolygonGetter, OcclusionPolygonParam, false);
        for (const auto& [at, offset] : {std::pair{GgxGetter, GgxFilterParam},
                                          std::pair{CubeOccGetter, CubeMapOcclusionParam}}) {
            words[at] = LdrX(8, 0, offset);
            words[at + 1] = 0x39419100; // ldrb w0, [x8, #0x64]
            words[at + 2] = Ret;
        }
        PutGetter(BlurSamplesGetter, RadialBlurSamplesParam, false);
        PutGetter(CubeMapGetter, CubeMapIntervalParam, false);
        PutGetter(DecalGetter, ColorDecalMaxParam, false);
        const u32 first[]{0x1E204100, 0x1E204121, 0x910023E2, 0x1E204142};
        const u32 second[]{0x1E204100, 0x910023E2, 0x1E204121, 0x1E204142, 0xAA1303E0};
        const u32 third[]{0xAA1503E0, 0x1E222821, 0x2D418D02};
        std::copy(std::begin(first), std::end(first), &words[FarA]);
        std::copy(std::begin(second), std::end(second), &words[FarB]);
        std::copy(std::begin(third), std::end(third), &words[FarC]);
        words[MeshStore] = 0xB90097E8;     // str w8, [sp, #0x94]
        words[MeshStore + 1] = 0xF9403260; // ldr x0, [x19, #0x60]
        words[BloomFirst] = LdrX(8, 8, BloomScaleParam);
        words[BloomFirst + 1] = LdrS(1, 8, ValueOffset);
        for (const size_t at : {BloomSecond, BloomThird}) {
            words[at] = LdrX(11, 26, BloomScaleParam);
            words[at + 1] = LdrS(0, 11, ValueOffset);
        }
        const u32 loads[]{LdrS(8, 0, 0), LdrS(9, 0, 0), LdrS(10, 0, 0), LdrS(0, 0, 0)};
        for (size_t i = 0; i < 4; ++i) {
            words[Voxel + i * 3] = 0x94000080; // bl
            words[Voxel + i * 3 + 1] = loads[i];
            if (i < 3) {
                words[Voxel + i * 3 + 2] = 0xAA1803E0; // mov x0, x24
            }
        }
    }

    void PutGetter(size_t at, u32 offset, bool is_float) {
        words[at] = LdrX(8, 0, offset);
        words[at + 1] = is_float ? LdrS(0, 8, ValueOffset) : LdrW(0, 8, ValueOffset);
        words[at + 2] = Ret;
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }
};

Values Only(Id id, int value) {
    Values values{};
    values[id] = value;
    return values;
}
} // Anonymous namespace

TEST_CASE("Nx2DkQuality: nothing set, nothing touched", "[core]") {
    Text text;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Values{});
    for (const State state : result) {
        REQUIRE(state == State::Unset);
    }
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the resolution range", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[ResBest] = 50;  // rate 2.0
    values[ResWorst] = 25; // rate 4.0
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(result[ResWorst] == State::Applied);
    expected[MinNear] = 0x52A80008;     // movz w8, #0x4000, lsl #16 (2.0)
    expected[MinNear + 1] = 0x1E270102; // fmov s2, w8
    expected[MinFar] = 0x52A8000A;      // movz w10, #0x4000, lsl #16
    expected[MinFar + 9] = 0x1E270142;  // fmov s2, w10
    expected[MaxNear] = 0x52A81008;     // movz w8, #0x4080, lsl #16 (4.0)
    expected[MaxNear + 1] = 0x1E270103; // fmov s3, w8
    expected[MaxFar] = 0x52A8100A;      // movz w10, #0x4080, lsl #16
    expected[MaxFar + 11] = 0x1E270145; // fmov s5, w10
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: a best resolution below the game's worst moves the worst too", "[core]") {
    Text text;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 40));
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(result[ResWorst] == State::Unset);
    REQUIRE(text.words[MinNear] == 0x52A80408); // 2.5
    REQUIRE(text.words[MaxNear] == 0x52A80408); // 2.5 as well: the game's 2.0 would be below
}

TEST_CASE("Nx2DkQuality: the game's own worst resolution is not moved by a best of 50", "[core]") {
    Text text;
    Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 50));
    REQUIRE(text.words[MaxNear] == LdrX(8, 10, MaxRateParam));
}

TEST_CASE("Nx2DkQuality: a best resolution within the game's range leaves the worst", "[core]") {
    Text text;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 60));
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(text.words[MaxNear] == LdrX(8, 10, MaxRateParam));
    REQUIRE(text.words[MaxFar] == LdrX(10, 10, MaxRateParam));
}

TEST_CASE("Nx2DkQuality: the getters", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[GpuTarget] = 75;
    values[FrameMinScale] = 50;
    values[RadialBlurScale] = 25;
    values[RadialBlurSamples] = 4;
    values[CubeMapInterval] = 30;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    for (const Id id : {GpuTarget, FrameMinScale, RadialBlurScale, RadialBlurSamples,
                        CubeMapInterval}) {
        REQUIRE(result[id] == State::Applied);
    }
    expected[GpuGetter] = 0x52A852C8;         // movz w8, #0x4296, lsl #16 (75.0)
    expected[GpuGetter + 1] = 0x1E270100;     // fmov s0, w8
    expected[FrameGetter] = 0x52A7E008;       // movz w8, #0x3f00, lsl #16 (0.5)
    expected[FrameGetter + 1] = 0x1E270100;
    expected[BlurScaleGetter] = 0x52A7D008;   // movz w8, #0x3e80, lsl #16 (0.25)
    expected[BlurScaleGetter + 1] = 0x1E270100;
    expected[BlurSamplesGetter] = 0x52800080; // movz w0, #4
    expected[BlurSamplesGetter + 1] = Ret;
    expected[CubeMapGetter] = 0x528003C0;     // movz w0, #30
    expected[CubeMapGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the toggles and the occlusion thresholds", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[GgxFilterOff] = 1;
    values[CubeMapOcclusionOff] = 1;
    values[OcclusionDepth] = 200;   // 100.0
    values[OcclusionRadius] = 50;   // 25.0
    values[OcclusionPolygon] = 150; // 1500
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    for (const Id id : {GgxFilterOff, CubeMapOcclusionOff, OcclusionDepth, OcclusionRadius,
                        OcclusionPolygon}) {
        REQUIRE(result[id] == State::Applied);
    }
    for (const size_t at : {GgxGetter, CubeOccGetter}) {
        expected[at] = 0x52800000; // movz w0, #0
        expected[at + 1] = Ret;
    }
    expected[DepthGetter] = 0x52A85908;  // movz w8, #0x42c8, lsl #16 (100.0)
    expected[DepthGetter + 1] = 0x1E270100;
    expected[RadiusGetter] = 0x52A83908; // movz w8, #0x41c8, lsl #16 (25.0)
    expected[RadiusGetter + 1] = 0x1E270100;
    expected[PolygonGetter] = 0x5280BB80; // movz w0, #1500
    expected[PolygonGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the decal limit", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ColorDecalMax, 32));
    REQUIRE(result[ColorDecalMax] == State::Applied);
    expected[DecalGetter] = 0x52800400; // movz w0, #32
    expected[DecalGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the mesh simplification type", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(MeshSimplify, 5));
    REQUIRE(result[MeshSimplify] == State::Applied);
    const size_t routine = Cave + MeshRoutine;
    expected[MeshStore + 1] = 0x14000000U | static_cast<u32>(routine - (MeshStore + 1));
    expected[routine] = 0x52800088;     // movz w8, #4 (Low)
    expected[routine + 1] = 0xB90097E8; // str w8, [sp, #0x94]
    expected[routine + 2] = 0xF9403260; // ldr x0, [x19, #0x60]
    expected[routine + 3] =
        0x14000000U | (static_cast<u32>((MeshStore + 2) - (routine + 3)) & 0x3FFFFFFU);
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the terrain LOD and the mesh simplification together", "[core]") {
    Text text;
    Values values{};
    values[VoxelLodDist] = 50;
    values[MeshSimplify] = 5;
    values[CameraFar] = 50;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[VoxelLodDist] == State::Applied);
    REQUIRE(result[MeshSimplify] == State::Applied);
    REQUIRE(result[CameraFar] == State::Applied);
}

TEST_CASE("Nx2DkQuality: the camera far, three places", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(CameraFar, 10));
    REQUIRE(result[CameraFar] == State::Applied);
    const std::array<size_t, 3> sites{FarA + 1, FarB + 2, FarC + 1};
    const std::array<u32, 3> originals{0x1E204121, 0x1E204121, 0x1E222821};
    for (size_t i = 0; i < 3; ++i) {
        const size_t routine = Cave + FarRoutine + i * FarRoutineWords;
        expected[sites[i]] = 0x14000000U | static_cast<u32>(routine - sites[i]);
        expected[routine] = originals[i];
        expected[routine + 1] = 0x1C000062; // ldr s2, #12
        expected[routine + 2] = 0x1E220821; // fmul s1, s1, s2
        expected[routine + 3] =
            0x14000000U | (static_cast<u32>((sites[i] + 1) - (routine + 3)) & 0x3FFFFFFU);
        expected[routine + 4] = 0x3DCCCCCD; // 0.1f
    }
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the camera far needs all three places", "[core]") {
    Text text;
    text.words[FarC + 1] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(CameraFar, 10));
    REQUIRE(result[CameraFar] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the bloom scale, all three places", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(BloomScale, 25));
    REQUIRE(result[BloomScale] == State::Applied);
    expected[BloomFirst] = 0x52A7D008;     // movz w8, #0x3e80, lsl #16
    expected[BloomFirst + 1] = 0x1E270101; // fmov s1, w8
    for (const size_t at : {BloomSecond, BloomThird}) {
        expected[at] = 0x52A7D00B;     // movz w11, #0x3e80, lsl #16
        expected[at + 1] = 0x1E270160; // fmov s0, w11
    }
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the bloom scale needs all three places", "[core]") {
    Text text;
    text.words[BloomThird] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(BloomScale, 25));
    REQUIRE(result[BloomScale] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the terrain LOD distance and value", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[VoxelLodDist] = 50;
    values[VoxelLodValue] = 200;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[VoxelLodDist] == State::Applied);
    REQUIRE(result[VoxelLodValue] == State::Applied);
    const auto bl = [](size_t from, size_t to) {
        return 0x94000000U | (static_cast<u32>(to - from) & 0x3FFFFFFU);
    };
    // The loads of Lod1Dist, Lod1Value, Lod2Dist and Lod2Value, one routine each
    const std::array<u32, 4> loads{0xBD400008, 0xBD400009, 0xBD40000A, 0xBD400000};
    const std::array<u32, 4> movz{0x52A7E010, 0x52A80010, 0x52A7E010, 0x52A80010}; // 0.5, 2.0
    const std::array<u32, 4> regs{8, 9, 10, 0};
    for (size_t i = 0; i < 4; ++i) {
        const size_t routine = Cave + i * CaveRoutineWords;
        expected[Voxel + 1 + i * 3] = bl(Voxel + 1 + i * 3, routine);
        expected[routine] = loads[i];
        expected[routine + 1] = movz[i];
        expected[routine + 2] = 0x1E270210;                        // fmov s16, w16
        expected[routine + 3] = 0x1E300800U | (regs[i] << 5) | regs[i]; // fmul sN, sN, s16
        expected[routine + 4] = Ret;
    }
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the terrain LOD distance alone leaves the values", "[core]") {
    Text text;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(VoxelLodDist, 50));
    REQUIRE(result[VoxelLodDist] == State::Applied);
    REQUIRE(result[VoxelLodValue] == State::Unset);
    REQUIRE(text.words[Voxel + 4] == 0xBD400009); // the Lod1Value load is not replaced
}

TEST_CASE("Nx2DkQuality: the terrain LOD needs its free space", "[core]") {
    Text text;
    text.words[Cave + 7] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(VoxelLodDist, 50));
    REQUIRE(result[VoxelLodDist] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: a setting found twice is left alone", "[core]") {
    Text text;
    text.PutGetter(0x500, RadialBlurScaleParam, true);
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(RadialBlurScale, 25));
    REQUIRE(result[RadialBlurScale] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: values and variables", "[core]") {
    REQUIRE(Parse(nullptr, Knobs[ResBest]) == 0);
    REQUIRE(Parse("", Knobs[ResBest]) == 0);
    REQUIRE(Parse("60", Knobs[ResBest]) == 60);
    REQUIRE(Parse("24", Knobs[ResBest]) == -1);
    REQUIRE(Parse("101", Knobs[ResBest]) == -1);
    REQUIRE(Parse("60%", Knobs[ResBest]) == -1);
    REQUIRE(Parse("400", Knobs[VoxelLodDist]) == 400);
    REQUIRE(Sanitize(9, Knobs[VoxelLodDist]) == 0);

    Values values{};
    values[ResBest] = 60;
    values[VoxelLodDist] = 400;
    values[CubeMapInterval] = 999; // out of range: left out
    REQUIRE(Wanted(values, false).empty());
    const auto vars = Wanted(values, true);
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0].name == "EDEN_NX2_DKQ_RES_BEST");
    REQUIRE(vars[0].value == "60");
    REQUIRE(vars[1].name == "EDEN_NX2_DKQ_VOXEL_LOD_DIST");
    REQUIRE(vars[1].value == "400");
}

TEST_CASE("Nx2DkQuality: the presets", "[core]") {
    REQUIRE(PresetOf(Values{}) == Preset::Default);
    for (const Preset preset : {Preset::Balanced, Preset::Performance}) {
        const Values values = PresetValues(preset);
        REQUIRE(PresetOf(values) == preset);
        REQUIRE(values != Values{});
        for (size_t i = 0; i < Count; ++i) {
            REQUIRE(values[i] == Sanitize(values[i], Knobs[i]));
        }
        // The best resolution is not below the worst one
        REQUIRE(values[ResBest] >= values[ResWorst]);
    }
    Values changed = PresetValues(Preset::Balanced);
    changed[BloomScale] = 11;
    REQUIRE(PresetOf(changed) == Preset::Custom);
    REQUIRE(PresetOf(Only(ResBest, 60)) == Preset::Custom);
}
