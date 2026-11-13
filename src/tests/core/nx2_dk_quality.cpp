// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_quality.h"

namespace {
using namespace Loader::Nx2DkQuality;

constexpr u32 Filler = 0x12345678;
// A text segment of 0x2f04 bytes in 0x3000: the free space starts at word 0xbc4, the terrain
// LOD routines at 0xbc4 + 32.
constexpr u32 TextSize = 0x2f04;
constexpr u32 AlignedSize = 0x3000;
constexpr size_t Cave = 0xbc4 + CaveOffset;
constexpr size_t MinNear = 0x100, MaxFar = 0x120, MaxNear = 0x140, MinFar = 0x160;
constexpr size_t GpuGetter = 0x200, FrameGetter = 0x210, BlurScaleGetter = 0x220;
constexpr size_t BlurSamplesGetter = 0x230, CubeMapGetter = 0x240;
constexpr size_t BloomFirst = 0x300, BloomSecond = 0x310, BloomThird = 0x320;
constexpr size_t DecalGetter = 0x260, GgxGetter = 0x270, CubeOccGetter = 0x280;
constexpr size_t DepthGetter = 0x290, RadiusGetter = 0x2a0, PolygonGetter = 0x2b0;
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
        PutGetter(BlurSamplesGetter, RadialBlurSamplesParam, false);
        PutGetter(CubeMapGetter, CubeMapIntervalParam, false);
        PutGetter(DecalGetter, ColorDecalMaxParam, false);
        PutGetter(DepthGetter, OcclusionDepthParam, true);
        PutGetter(RadiusGetter, OcclusionRadiusParam, true);
        PutGetter(PolygonGetter, OcclusionPolygonParam, false);
        for (const auto& [at, offset] : {std::pair{GgxGetter, GgxFilterParam},
                                          std::pair{CubeOccGetter, CubeMapOcclusionParam}}) {
            words[at] = LdrX(8, 0, offset);
            words[at + 1] = 0x39419100; // ldrb w0, [x8, #0x64]
            words[at + 2] = Ret;
        }
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

TEST_CASE("Nx2DkQuality: the decal limit, the toggles and the occlusion thresholds", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[ColorDecalMax] = 32;
    values[GgxFilterOff] = 1;
    values[CubeMapOcclusionOff] = 1;
    values[OcclusionDepth] = 200;   // 100.0
    values[OcclusionRadius] = 50;   // 25.0
    values[OcclusionPolygon] = 150; // 1500
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    for (const Id id : {ColorDecalMax, GgxFilterOff, CubeMapOcclusionOff, OcclusionDepth,
                        OcclusionRadius, OcclusionPolygon}) {
        REQUIRE(result[id] == State::Applied);
    }
    expected[DecalGetter] = 0x52800400;      // movz w0, #32
    expected[DecalGetter + 1] = Ret;
    for (const size_t at : {GgxGetter, CubeOccGetter}) {
        expected[at] = 0x52800000;           // movz w0, #0
        expected[at + 1] = Ret;
    }
    expected[DepthGetter] = 0x52A85908;      // movz w8, #0x42c8, lsl #16 (100.0)
    expected[DepthGetter + 1] = 0x1E270100;
    expected[RadiusGetter] = 0x52A83908;     // movz w8, #0x41c8, lsl #16 (25.0)
    expected[RadiusGetter + 1] = 0x1E270100;
    expected[PolygonGetter] = 0x5280BB80;    // movz w0, #1500
    expected[PolygonGetter + 1] = Ret;
    REQUIRE(text.words == expected);
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

TEST_CASE("Nx2DkQuality: the terrain LOD distance scales the two distances", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(VoxelLodDist, 50));
    REQUIRE(result[VoxelLodDist] == State::Applied);
    REQUIRE(result[VoxelLodValue] == State::Unset);
    const auto bl = [](size_t from, size_t to) {
        return 0x94000000U | (static_cast<u32>(to - from) & 0x3FFFFFFU);
    };
    expected[Voxel + 1] = bl(Voxel + 1, Cave);
    expected[Voxel + 7] = bl(Voxel + 7, Cave + 10);
    expected[Cave] = 0xBD400008;      // ldr s8, [x0]
    expected[Cave + 1] = 0x52A7E010;  // movz w16, #0x3f00, lsl #16 (0.5)
    expected[Cave + 2] = 0x1E270210;  // fmov s16, w16
    expected[Cave + 3] = 0x1E300908;  // fmul s8, s8, s16
    expected[Cave + 4] = Ret;
    expected[Cave + 10] = 0xBD40000A; // ldr s10, [x0]
    expected[Cave + 11] = 0x52A7E010;
    expected[Cave + 12] = 0x1E270210;
    expected[Cave + 13] = 0x1E30094A; // fmul s10, s10, s16
    expected[Cave + 14] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the terrain LOD needs its free space", "[core]") {
    Text text;
    text.words[Cave + 7] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(VoxelLodValue, 200));
    REQUIRE(result[VoxelLodValue] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: a setting found twice is left alone", "[core]") {
    Text text;
    text.PutGetter(0x500, GpuTargetParam, true);
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(GpuTarget, 75));
    REQUIRE(result[GpuTarget] == State::Missed);
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
