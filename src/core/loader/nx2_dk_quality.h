// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"
#include "core/nx2_perf_settings.h"

// EDEN_NX2_DKQ_*=<n> (all unset by default): Donkey Kong Bananza's quality settings, the Bananza
// tab of the menu program. Each one replaces a value the game reads from its graphics parameters
// (GraphicsParam, VoxelMeshLodParam) by a constant, in the main module as it loads. A setting is
// only put in when its instructions are found exactly where expected; unset, nothing is touched.
// Found in the game's 1.0.0 main module (build id e25c90c1c136...); experimental, see
// NX2_NOTES.md.
namespace Loader::Nx2DkQuality {

enum Id : size_t {
    ResBest,           ///< highest render resolution, % of the output (NdsrUpscaleMinRate)
    ResWorst,          ///< lowest render resolution, % of the output (NdsrUpscaleMaxRate)
    GpuTarget,         ///< GPU load the resolution control aims for, % (MaintainGpuPercentage)
    FrameMinScale,     ///< FrameBufferMinScale, %
    BloomScale,        ///< BloomBufferBaseScale, %
    CubeMapInterval,   ///< CubeMapShotInterval, frames
    RadialBlurScale,   ///< RadialBlurRenderScale, %
    RadialBlurSamples, ///< RadialBlurSamplingNum
    VoxelLodDist,      ///< terrain Lod1Dist and Lod2Dist, % of the game's
    VoxelLodValue,     ///< terrain Lod1Value and Lod2Value, % of the game's
    ColorDecalMax,     ///< ColorDecalMaxNum
    GgxFilterOff,      ///< IsUseGGXFilter forced off (reflection cube map filtering)
    CubeMapOcclusionOff, ///< IsUseCubeMapOcclusionCheck forced off
    OcclusionDepth,    ///< OcclusionQueryThresholdDepth, % of the game's
    OcclusionRadius,   ///< OcclusionQueryThresholdRadius, % of the game's
    OcclusionPolygon,  ///< OcclusionQueryThresholdPolygon, % of the game's
    Count,
};

struct Knob {
    std::string_view key; ///< in the [Bananza] group of qt-config.ini
    std::string_view env;
    int min;
    int max;
    int game; ///< the game's own value, shown only (the % of the game's ones: 100)
};

/// A row with nothing to choose but on: its variable is 1 and it forces something off.
constexpr bool IsToggle(const Knob& knob) {
    return knob.min == knob.max;
}

inline constexpr std::array<Knob, Count> Knobs{{
    {"res_best", "EDEN_NX2_DKQ_RES_BEST", 25, 100, 73},
    {"res_worst", "EDEN_NX2_DKQ_RES_WORST", 25, 100, 50},
    {"gpu_target", "EDEN_NX2_DKQ_GPU_TARGET", 40, 100, 90},
    {"frame_min_scale", "EDEN_NX2_DKQ_FRAME_MIN_SCALE", 25, 100, 100},
    {"bloom_scale", "EDEN_NX2_DKQ_BLOOM_SCALE", 10, 100, 25},
    {"cubemap_interval", "EDEN_NX2_DKQ_CUBEMAP_INTERVAL", 1, 240, 8},
    {"radial_blur_scale", "EDEN_NX2_DKQ_RADIAL_BLUR_SCALE", 10, 100, 50},
    {"radial_blur_samples", "EDEN_NX2_DKQ_RADIAL_BLUR_SAMPLES", 1, 32, 20},
    {"voxel_lod_dist", "EDEN_NX2_DKQ_VOXEL_LOD_DIST", 10, 400, 100},
    {"voxel_lod_value", "EDEN_NX2_DKQ_VOXEL_LOD_VALUE", 10, 400, 100},
    {"color_decal_max", "EDEN_NX2_DKQ_COLOR_DECAL_MAX", 1, 660, 660},
    {"ggx_filter_off", "EDEN_NX2_DKQ_GGX_FILTER_OFF", 1, 1, 0},
    {"cubemap_occlusion_off", "EDEN_NX2_DKQ_CUBEMAP_OCCLUSION_OFF", 1, 1, 0},
    {"occlusion_depth", "EDEN_NX2_DKQ_OCCLUSION_DEPTH", 10, 400, 100},
    {"occlusion_radius", "EDEN_NX2_DKQ_OCCLUSION_RADIUS", 10, 400, 100},
    {"occlusion_polygon", "EDEN_NX2_DKQ_OCCLUSION_POLYGON", 10, 400, 100},
}};

/// One number per knob; 0 leaves the game's own.
using Values = std::array<int, Count>;

/// A stored or given number: 0 unless it is within the knob's range.
constexpr int Sanitize(int value, const Knob& knob) {
    return value >= knob.min && value <= knob.max ? value : 0;
}

/// A variable's value: a whole number within the knob's range, nothing else around it. 0 when
/// unset, -1 when refused.
constexpr int Parse(const char* text, const Knob& knob) {
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    int value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9' || i >= 3) {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }
    return Sanitize(value, knob) != 0 ? value : -1;
}

/// "0" to "999", for the variables' values.
inline constexpr auto NumberTexts = [] {
    std::array<std::array<char, 4>, 1000> texts{};
    for (size_t i = 0; i < texts.size(); ++i) {
        size_t digits = i >= 100 ? 3 : i >= 10 ? 2 : 1;
        size_t rest = i;
        while (digits-- > 0) {
            texts[i][digits] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        }
    }
    return texts;
}();

/// The variables the settings ask for, Bananza only.
inline std::vector<Core::Nx2PerfSettings::Var> Wanted(const Values& values, bool bananza) {
    std::vector<Core::Nx2PerfSettings::Var> vars;
    for (size_t i = 0; bananza && i < Count; ++i) {
        if (const int value = Sanitize(values[i], Knobs[i]); value != 0) {
            vars.push_back({Knobs[i].env, NumberTexts[static_cast<size_t>(value)].data()});
        }
    }
    return vars;
}

/// Reads every knob through `read(key, default)`.
template <typename Read>
Values Load(Read&& read) {
    Values values{};
    for (size_t i = 0; i < Count; ++i) {
        values[i] = Sanitize(read(Knobs[i].key, 0), Knobs[i]);
    }
    return values;
}

/// Writes every knob through `write(key, value, default)`.
template <typename Write>
void Save(const Values& values, Write&& write) {
    for (size_t i = 0; i < Count; ++i) {
        write(Knobs[i].key, values[i], 0);
    }
}

enum class State : u8 {
    Unset,   ///< not asked for
    Applied, ///< put in
    Missed,  ///< asked for, but the instructions are not where expected: left alone
};

using Result = std::array<State, Count>;

/// The game's own upscale rates (output size / render size), from its ProjectGraphicsParam:
/// 2880x1620 from 2095x1179 at best and 1440x810 at worst.
constexpr float GameMinRate = 1.375f;
constexpr float GameMaxRate = 2.0f;

/// Words of free space past the end of the text segment: the dynamic FPS patch has the first 19,
/// the terrain LOD routines start at word 32.
constexpr size_t CaveOffset = 32;
constexpr size_t CaveRoutineWords = 5;
constexpr size_t CaveWords = 4 * CaveRoutineWords;

/// The upper half of a float, rounded: the constants are put in with one `movz wN, #x, lsl #16`.
constexpr u32 FloatHigh(float value) {
    return (std::bit_cast<u32>(value) + 0x8000U) >> 16;
}
constexpr u32 MovzHigh(u32 reg, u32 high) {
    return 0x52A00000U | (high << 5) | reg; // movz wN, #high, lsl #16
}
constexpr u32 Movz(u32 reg, u32 value) {
    return 0x52800000U | (value << 5) | reg; // movz wN, #value
}
constexpr u32 FmovFromW(u32 s_reg, u32 w_reg) {
    return 0x1E270000U | (w_reg << 5) | s_reg; // fmov sN, wM
}
constexpr u32 LdrX(u32 rt, u32 rn, u32 offset) {
    return 0xF9400000U | ((offset / 8) << 10) | (rn << 5) | rt; // ldr xT, [xN, #offset]
}
constexpr u32 LdrS(u32 rt, u32 rn, u32 offset) {
    return 0xBD400000U | ((offset / 4) << 10) | (rn << 5) | rt; // ldr sT, [xN, #offset]
}
constexpr u32 LdrW(u32 rt, u32 rn, u32 offset) {
    return 0xB9400000U | ((offset / 4) << 10) | (rn << 5) | rt; // ldr wT, [xN, #offset]
}
constexpr u32 LdrBW0X8(u32 offset) {
    return 0x39400100U | (offset << 10); // ldrb w0, [x8, #offset]
}
constexpr u32 Ret = 0xD65F03C0U;

// A graphics parameter is an object with its value at +0x64, held by GraphicsParam at a fixed
// offset; the game reads one with `ldr xA, [xB, #offset]; ldr sC, [xA, #0x64]`.
constexpr u32 ValueOffset = 0x64;
constexpr u32 MinRateParam = 0x5c8;      // NdsrUpscaleMinRate
constexpr u32 MaxRateParam = 0x5d0;      // NdsrUpscaleMaxRate
constexpr u32 GpuTargetParam = 0x538;    // MaintainGpuPercentage
constexpr u32 FrameMinScaleParam = 0x540; // FrameBufferMinScale
constexpr u32 BloomScaleParam = 0x570;   // BloomBufferBaseScale
constexpr u32 CubeMapIntervalParam = 0x220; // CubeMapShotInterval
constexpr u32 RadialBlurScaleParam = 0x508; // RadialBlurRenderScale
constexpr u32 RadialBlurSamplesParam = 0x500; // RadialBlurSamplingNum
constexpr u32 ColorDecalMaxParam = 0x280;     // ColorDecalMaxNum
constexpr u32 GgxFilterParam = 0x210;         // IsUseGGXFilter
constexpr u32 CubeMapOcclusionParam = 0x218;  // IsUseCubeMapOcclusionCheck
constexpr u32 OcclusionDepthParam = 0x520;    // OcclusionQueryThresholdDepth
constexpr u32 OcclusionRadiusParam = 0x528;   // OcclusionQueryThresholdRadius
constexpr u32 OcclusionPolygonParam = 0x530;  // OcclusionQueryThresholdPolygon

// The game's values of the "% of the game's" rows, from its ProjectGraphicsParam (Default).
constexpr float GameOcclusionDepth = 50.0f;
constexpr float GameOcclusionRadius = 50.0f;
constexpr int GameOcclusionPolygon = 1000;

// `text` starts at the text segment and holds at least `aligned_size` bytes, the segment's size
// rounded up to whole pages; `text_size` is its size in the NSO header.
inline Result Apply(std::span<u8> text, u32 text_size, u32 aligned_size, const Values& values) {
    Result result{};
    bool any = false;
    for (size_t i = 0; i < Count; ++i) {
        if (Sanitize(values[i], Knobs[i]) != 0) {
            result[i] = State::Missed;
            any = true;
        }
    }
    if (!any || text.size() < aligned_size || aligned_size < text_size) {
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
    using Pattern = std::initializer_list<std::pair<size_t, u32>>; // word distance, word
    constexpr size_t Reach = 12;
    // The places a pattern is at, when there are exactly `out.size()` of them.
    const auto find = [&](Pattern pattern, std::span<size_t> out) {
        size_t found = 0;
        for (size_t i = 0; i + Reach < count; ++i) {
            bool same = true;
            for (const auto& [distance, value] : pattern) {
                if (word(i + distance) != value) {
                    same = false;
                    break;
                }
            }
            if (same) {
                if (found < out.size()) {
                    out[found] = i;
                }
                ++found;
            }
        }
        return found == out.size();
    };
    const auto set = [&](Id id) { return Sanitize(values[id], Knobs[id]); };
    const auto percent = [](int value) { return static_cast<float>(value) / 100.0f; };

    // The resolution control steps the upscale rate between NdsrUpscaleMinRate and
    // NdsrUpscaleMaxRate; it reads each in two places:
    //   ldr x8, [x10, #0x5c8]; ldr s2, [x8, #0x64]            (x8 is loaded again next)
    //   ldr x10, [x10, #0x5c8]; 8 words; ldr s2, [x10, #0x64] (x10 unused between, set next)
    //   ldr x8, [x10, #0x5d0]; ldr s3, [x8, #0x64]
    //   ldr x10, [x10, #0x5d0]; 10 words; ldr s5, [x10, #0x64]
    if (const int best = set(ResBest), worst = set(ResWorst); best != 0 || worst != 0) {
        float min_rate = best != 0 ? 100.0f / static_cast<float>(best) : GameMinRate;
        float max_rate = worst != 0 ? 100.0f / static_cast<float>(worst) : GameMaxRate;
        // Best below worst: the one that was set decides (best, when both were).
        if (min_rate > max_rate) {
            if (best != 0) {
                max_rate = min_rate;
            } else {
                min_rate = max_rate;
            }
        }
        std::array<size_t, 1> min_near{}, min_far{}, max_near{}, max_far{};
        if (find({{0, LdrX(8, 10, MinRateParam)}, {1, LdrS(2, 8, ValueOffset)}}, min_near) &&
            find({{0, LdrX(10, 10, MinRateParam)}, {9, LdrS(2, 10, ValueOffset)}}, min_far) &&
            find({{0, LdrX(8, 10, MaxRateParam)}, {1, LdrS(3, 8, ValueOffset)}}, max_near) &&
            find({{0, LdrX(10, 10, MaxRateParam)}, {11, LdrS(5, 10, ValueOffset)}}, max_far)) {
            if (min_rate != GameMinRate) {
                const u32 high = FloatHigh(min_rate);
                put(min_near[0], MovzHigh(8, high));
                put(min_near[0] + 1, FmovFromW(2, 8));
                put(min_far[0], MovzHigh(10, high));
                put(min_far[0] + 9, FmovFromW(2, 10));
            }
            if (max_rate != GameMaxRate) {
                const u32 high = FloatHigh(max_rate);
                put(max_near[0], MovzHigh(8, high));
                put(max_near[0] + 1, FmovFromW(3, 8));
                put(max_far[0], MovzHigh(10, high));
                put(max_far[0] + 11, FmovFromW(5, 10));
            }
            if (best != 0) {
                result[ResBest] = State::Applied;
            }
            if (worst != 0) {
                result[ResWorst] = State::Applied;
            }
        }
    }

    // Getters: `ldr x8, [x0, #offset]; ldr s0 / w0, [x8, #0x64]; ret`.
    const auto float_getter = [&](Id id, u32 offset, float value) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrS(0, 8, ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], MovzHigh(8, FloatHigh(value)));
            put(at[0] + 1, FmovFromW(0, 8));
            result[id] = State::Applied;
        }
    };
    const auto int_getter = [&](Id id, u32 offset, int value = 0) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrW(0, 8, ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], Movz(0, static_cast<u32>(value != 0 ? value : set(id))));
            put(at[0] + 1, Ret);
            result[id] = State::Applied;
        }
    };
    // A boolean getter: `ldr x8, [x0, #offset]; ldrb w0, [x8, #0x64]; ret`, forced to false.
    const auto false_getter = [&](Id id, u32 offset) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrBW0X8(ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], Movz(0, 0));
            put(at[0] + 1, Ret);
            result[id] = State::Applied;
        }
    };
    false_getter(GgxFilterOff, GgxFilterParam);
    false_getter(CubeMapOcclusionOff, CubeMapOcclusionParam);
    float_getter(OcclusionDepth, OcclusionDepthParam,
                 GameOcclusionDepth * percent(set(OcclusionDepth)));
    float_getter(OcclusionRadius, OcclusionRadiusParam,
                 GameOcclusionRadius * percent(set(OcclusionRadius)));
    float_getter(GpuTarget, GpuTargetParam, static_cast<float>(set(GpuTarget)));
    float_getter(FrameMinScale, FrameMinScaleParam, percent(set(FrameMinScale)));
    float_getter(RadialBlurScale, RadialBlurScaleParam, percent(set(RadialBlurScale)));
    int_getter(RadialBlurSamples, RadialBlurSamplesParam);
    int_getter(CubeMapInterval, CubeMapIntervalParam);
    int_getter(ColorDecalMax, ColorDecalMaxParam);
    int_getter(OcclusionPolygon, OcclusionPolygonParam,
               GameOcclusionPolygon * set(OcclusionPolygon) / 100);

    // BloomBufferBaseScale is read in three places; the register it comes through is loaded
    // again right after each:
    //   ldr x8, [x8, #0x570]; ldr s1, [x8, #0x64]
    //   ldr x11, [x26, #0x570]; ldr s0, [x11, #0x64]   (twice)
    if (set(BloomScale) != 0) {
        std::array<size_t, 1> first{};
        std::array<size_t, 2> others{};
        if (find({{0, LdrX(8, 8, BloomScaleParam)}, {1, LdrS(1, 8, ValueOffset)}}, first) &&
            find({{0, LdrX(11, 26, BloomScaleParam)}, {1, LdrS(0, 11, ValueOffset)}}, others)) {
            const u32 high = FloatHigh(percent(set(BloomScale)));
            put(first[0], MovzHigh(8, high));
            put(first[0] + 1, FmovFromW(1, 8));
            for (const size_t at : others) {
                put(at, MovzHigh(11, high));
                put(at + 1, FmovFromW(0, 11));
            }
            result[BloomScale] = State::Applied;
        }
    }

    // The terrain (voxel field) takes its two LOD steps from VoxelMeshLodParam, one place:
    //   bl Lod1Dist;  ldr s8, [x0];  mov x0, x24
    //   bl Lod1Value; ldr s9, [x0];  mov x0, x24
    //   bl Lod2Dist;  ldr s10, [x0]; mov x0, x24
    //   bl Lod2Value; ldr s0, [x0]
    // Each load becomes a call to a routine in the free space past the text segment that loads
    // and scales (x16, s16 and x30 are free right after a call):
    //   ldr sN, [x0]; movz w16, #scale, lsl #16; fmov s16, w16; fmul sN, sN, s16; ret
    if (const int dist = set(VoxelLodDist), value = set(VoxelLodValue); dist != 0 || value != 0) {
        constexpr u32 MovX0X24 = 0xAA1803E0U;
        std::array<size_t, 1> at{};
        const size_t cave = (text_size + 15) / 16 * 4 + CaveOffset;
        bool cave_free = (cave + CaveWords) * sizeof(u32) <= aligned_size;
        for (size_t i = 0; cave_free && i < CaveWords; ++i) {
            cave_free = word(cave + i) == 0;
        }
        if (cave_free && find({{1, LdrS(8, 0, 0)},
                               {2, MovX0X24},
                               {4, LdrS(9, 0, 0)},
                               {5, MovX0X24},
                               {7, LdrS(10, 0, 0)},
                               {8, MovX0X24},
                               {10, LdrS(0, 0, 0)}},
                              at)) {
            struct Load {
                size_t distance;
                u32 reg;
                int scale;
            };
            const std::array<Load, 4> loads{
                {{1, 8, dist}, {4, 9, value}, {7, 10, dist}, {10, 0, value}}};
            for (size_t i = 0; i < loads.size(); ++i) {
                const Load& load = loads[i];
                if (load.scale == 0) {
                    continue;
                }
                const size_t from = at[0] + load.distance;
                const size_t routine = cave + i * CaveRoutineWords;
                put(routine, LdrS(load.reg, 0, 0));
                put(routine + 1, MovzHigh(16, FloatHigh(percent(load.scale))));
                put(routine + 2, FmovFromW(16, 16));
                put(routine + 3, 0x1E300800U | (load.reg << 5) | load.reg); // fmul sN, sN, s16
                put(routine + 4, Ret);
                put(from, 0x94000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU)); // bl
            }
            if (dist != 0) {
                result[VoxelLodDist] = State::Applied;
            }
            if (value != 0) {
                result[VoxelLodValue] = State::Applied;
            }
        }
    }
    return result;
}

/// The knobs' values from the environment through `get(name)`; a refused one is left at 0 and
/// reported through `refused(knob, text)`.
template <typename Get, typename Refused>
Values FromEnvironment(Get&& get, Refused&& refused) {
    Values values{};
    for (size_t i = 0; i < Count; ++i) {
        const char* const text = get(Knobs[i].env);
        const int value = Parse(text, Knobs[i]);
        if (value < 0) {
            refused(Knobs[i], text);
        } else {
            values[i] = value;
        }
    }
    return values;
}

/// Whether a module is the one the settings are for: Bananza's main.
constexpr bool IsTarget(u64 program_id, std::string_view module_name) {
    return module_name == "main" && program_id == Nx2DkDynamicFps::ProgramId;
}

} // namespace Loader::Nx2DkQuality
