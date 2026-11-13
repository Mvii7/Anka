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
    BloomScale,        ///< BloomBufferBaseScale, %
    CubeMapInterval,   ///< CubeMapShotInterval, frames
    RadialBlurScale,   ///< RadialBlurRenderScale, %
    RadialBlurSamples, ///< RadialBlurSamplingNum
    VoxelLodDist,      ///< terrain Lod1Dist and Lod2Dist, % of the game's
    CameraFar,         ///< the camera's far distance, % (EDEN_NX2_FAR_SCALE of the Selevet fork)
    ColorDecalMax,     ///< ColorDecalMaxNum
    MeshSimplify,      ///< terrain MeshSimplifyType: 1 Normal, 2 High, 3 SuperHigh, 4 NoSimplify, 5 Low
    Count,
};

struct Knob {
    std::string_view key; ///< in the [Bananza] group of qt-config.ini
    std::string_view env;
    int min;
    int max;
    int game; ///< the game's own value, shown only (the % of the game's ones: 100)
};


inline constexpr std::array<Knob, Count> Knobs{{
    {"res_best", "EDEN_NX2_DKQ_RES_BEST", 25, 100, 73},
    {"res_worst", "EDEN_NX2_DKQ_RES_WORST", 25, 100, 50},
    {"bloom_scale", "EDEN_NX2_DKQ_BLOOM_SCALE", 10, 100, 25},
    {"cubemap_interval", "EDEN_NX2_DKQ_CUBEMAP_INTERVAL", 1, 240, 8},
    {"radial_blur_scale", "EDEN_NX2_DKQ_RADIAL_BLUR_SCALE", 10, 100, 50},
    {"radial_blur_samples", "EDEN_NX2_DKQ_RADIAL_BLUR_SAMPLES", 1, 32, 20},
    {"voxel_lod_dist", "EDEN_NX2_DKQ_VOXEL_LOD_DIST", 10, 400, 100},
    {"camera_far", "EDEN_NX2_DKQ_CAMERA_FAR", 10, 100, 100},
    {"color_decal_max", "EDEN_NX2_DKQ_COLOR_DECAL_MAX", 1, 660, 660},
    {"mesh_simplify", "EDEN_NX2_DKQ_MESH_SIMPLIFY", 1, 5, 1},
}};

/// One number per knob; 0 leaves the game's own.
using Values = std::array<int, Count>;

/// The presets of the Bananza tab. Default leaves every setting to the game; Custom is whatever
/// else the rows hold.
enum class Preset : u8 {
    Default,
    Balanced,
    Performance,
    Custom,
};

/// A preset's values (0: the game's). Custom has none of its own: nothing set.
constexpr Values PresetValues(Preset preset) {
    Values values{};
    if (preset == Preset::Balanced) {
        values[ResBest] = 65;
        values[ResWorst] = 45;
        values[BloomScale] = 20;
        values[CubeMapInterval] = 12;
        values[RadialBlurScale] = 40;
        values[RadialBlurSamples] = 12;
        values[VoxelLodDist] = 75;
        values[CameraFar] = 80;
        values[ColorDecalMax] = 400;
    } else if (preset == Preset::Performance) {
        values[ResBest] = 50;
        values[ResWorst] = 35;
        values[BloomScale] = 12;
        values[CubeMapInterval] = 24;
        values[RadialBlurScale] = 25;
        values[RadialBlurSamples] = 8;
        values[VoxelLodDist] = 50;
        values[MeshSimplify] = 5;
        values[CameraFar] = 50;
        values[ColorDecalMax] = 200;
    }
    return values;
}

/// The preset the values are (Custom when none).
constexpr Preset PresetOf(const Values& values) {
    for (const Preset preset : {Preset::Default, Preset::Balanced, Preset::Performance}) {
        if (values == PresetValues(preset)) {
            return preset;
        }
    }
    return Preset::Custom;
}

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
/// the terrain LOD routines start at word 20.
constexpr size_t CaveOffset = 20;
constexpr size_t CaveRoutineWords = 5;
constexpr size_t MeshRoutine = 2 * CaveRoutineWords; ///< after the terrain LOD routines
constexpr size_t MeshRoutineWords = 4;
constexpr size_t FarRoutine = MeshRoutine + MeshRoutineWords; ///< three, after the mesh one
constexpr size_t FarRoutineWords = 5;
constexpr size_t CaveWords = FarRoutine + 3 * FarRoutineWords;

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
constexpr u32 Ret = 0xD65F03C0U;

// A graphics parameter is an object with its value at +0x64, held by GraphicsParam at a fixed
// offset; the game reads one with `ldr xA, [xB, #offset]; ldr sC, [xA, #0x64]`.
constexpr u32 ValueOffset = 0x64;
constexpr u32 MinRateParam = 0x5c8;      // NdsrUpscaleMinRate
constexpr u32 MaxRateParam = 0x5d0;      // NdsrUpscaleMaxRate
constexpr u32 BloomScaleParam = 0x570;   // BloomBufferBaseScale
constexpr u32 CubeMapIntervalParam = 0x220; // CubeMapShotInterval
constexpr u32 RadialBlurScaleParam = 0x508; // RadialBlurRenderScale
constexpr u32 RadialBlurSamplesParam = 0x500; // RadialBlurSamplingNum
constexpr u32 ColorDecalMaxParam = 0x280;     // ColorDecalMaxNum

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
    const size_t cave = (text_size + 15) / 16 * 4 + CaveOffset;
    // Whether `words` words of the free space from word `first` on are unused.
    const auto CaveFree = [&](size_t first, size_t words) {
        bool free = (cave + CaveWords) * sizeof(u32) <= aligned_size;
        for (size_t i = first; free && i < first + words; ++i) {
            free = word(cave + i) == 0;
        }
        return free;
    };
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
    const auto int_getter = [&](Id id, u32 offset) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrW(0, 8, ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], Movz(0, static_cast<u32>(set(id))));
            put(at[0] + 1, Ret);
            result[id] = State::Applied;
        }
    };
    float_getter(RadialBlurScale, RadialBlurScaleParam, percent(set(RadialBlurScale)));
    int_getter(RadialBlurSamples, RadialBlurSamplesParam);
    int_getter(CubeMapInterval, CubeMapIntervalParam);
    int_getter(ColorDecalMax, ColorDecalMaxParam);

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
    // The two distance loads become a call to a routine in the free space past the text segment
    // that loads and scales (x16, s16 and x30 are free right after a call):
    //   ldr sN, [x0]; movz w16, #scale, lsl #16; fmov s16, w16; fmul sN, sN, s16; ret
    if (const int dist = set(VoxelLodDist); dist != 0) {
        constexpr u32 MovX0X24 = 0xAA1803E0U;
        std::array<size_t, 1> at{};
        const bool cave_free = CaveFree(0, MeshRoutine);
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
            // The distances are the 1st and 3rd loads; the values (2nd, 4th) are not scaled.
            const std::array<Load, 2> loads{{{1, 8, dist}, {7, 10, dist}}};
            for (size_t i = 0; i < loads.size(); ++i) {
                const Load& load = loads[i];
                const size_t from = at[0] + load.distance;
                const size_t routine = cave + i * CaveRoutineWords;
                put(routine, LdrS(load.reg, 0, 0));
                put(routine + 1, MovzHigh(16, FloatHigh(percent(load.scale))));
                put(routine + 2, FmovFromW(16, 16));
                put(routine + 3, 0x1E300800U | (load.reg << 5) | load.reg); // fmul sN, sN, s16
                put(routine + 4, Ret);
                put(from, 0x94000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU)); // bl
            }
            result[VoxelLodDist] = State::Applied;
        }
    }

    // The terrain's mesh simplification type (VoxelMeshLodParam's MeshSimplifyType: Normal 0,
    // High 1, SuperHigh 2, NoSimplify 3, Low 4) is stored in the loader and read after the one
    // place all of its paths meet:
    //   str w8, [sp, #0x94]; ldr x0, [x19, #0x60]
    // The load becomes a jump to a routine that stores the chosen type first (w8 is loaded
    // again right after):
    //   movz w8, #type; str w8, [sp, #0x94]; ldr x0, [x19, #0x60]; b back
    if (const int type = set(MeshSimplify); type != 0) {
        constexpr u32 StrW8Sp94 = 0xB90097E8U;
        constexpr u32 LdrX0X19 = 0xF9403260U;
        std::array<size_t, 1> at{};
        if (CaveFree(MeshRoutine, MeshRoutineWords) && find({{0, StrW8Sp94}, {1, LdrX0X19}}, at)) {
            const size_t routine = cave + MeshRoutine;
            const size_t from = at[0] + 1;
            put(routine, Movz(8, static_cast<u32>(type - 1)));
            put(routine + 1, StrW8Sp94);
            put(routine + 2, LdrX0X19);
            put(routine + 3,
                0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU));
            put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            result[MeshSimplify] = State::Applied;
        }
    }

    // The camera's far distance (the Selevet fork's EDEN_NX2_FAR_SCALE, from its pull request 15):
    // three places load a fresh far into s1 before the camera setter. Each instruction becomes a
    // jump to a routine that runs it, then multiplies s1 by the scale; s2 is written by the next
    // instruction that uses it at all three, so the routine borrows it:
    //   <the instruction>; ldr s2, <scale>; fmul s1, s1, s2; b back; <scale>
    // Beyond the far the game does not draw: a low scale drops the scenery in the distance.
    if (const int far_percent = set(CameraFar); far_percent != 0) {
        constexpr u32 FmovS0S8 = 0x1E204100U, FmovS1S9 = 0x1E204121U, AddX2Sp8 = 0x910023E2U;
        constexpr u32 FmovS2S10 = 0x1E204142U, FaddS1S1S2 = 0x1E222821U;
        constexpr u32 LdrS2Literal = 0x1C000062U; // ldr s2, #12: the word after the branch
        constexpr u32 FmulS1S1S2 = 0x1E220821U;
        std::array<size_t, 1> first{}, second{}, third{};
        if (CaveFree(FarRoutine, 3 * FarRoutineWords) &&
            find({{0, FmovS0S8}, {1, FmovS1S9}, {2, AddX2Sp8}, {3, FmovS2S10}}, first) &&
            find({{0, FmovS0S8}, {1, AddX2Sp8}, {2, FmovS1S9}, {3, FmovS2S10}, {4, 0xAA1303E0U}},
                 second) &&
            find({{0, 0xAA1503E0U}, {1, FaddS1S1S2}, {2, 0x2D418D02U}}, third)) {
            const std::array<size_t, 3> sites{first[0] + 1, second[0] + 2, third[0] + 1};
            const float scale = percent(far_percent);
            for (size_t i = 0; i < sites.size(); ++i) {
                const size_t routine = cave + FarRoutine + i * FarRoutineWords;
                put(routine, word(sites[i]));
                put(routine + 1, LdrS2Literal);
                put(routine + 2, FmulS1S1S2);
                put(routine + 3, 0x14000000U |
                                     (static_cast<u32>((sites[i] + 1) - (routine + 3)) &
                                      0x3FFFFFFU));
                put(routine + 4, std::bit_cast<u32>(scale));
                put(sites[i],
                    0x14000000U | (static_cast<u32>(routine - sites[i]) & 0x3FFFFFFU));
            }
            result[CameraFar] = State::Applied;
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
