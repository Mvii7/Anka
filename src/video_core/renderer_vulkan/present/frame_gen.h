// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <optional>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"
#include "video_core/renderer_vulkan/present/lsfg_chain.h"
#include "video_core/renderer_vulkan/present/lsfg_shaders.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

class Device;
class Scheduler;
struct Frame;

class FrameGen {
public:
    explicit FrameGen(MemoryAllocator& memory_allocator, Scheduler& scheduler,
                      void* native_window = nullptr);
    ~FrameGen();

    void Process(const Device& device, Frame* frame, VkFormat format, VkExtent2D guest_extent);

    /// Retains resources while invalidating pacing and history across a presentation gap.
    void Pause();

    [[nodiscard]] size_t WantedGenerations(size_t capacity);

    [[nodiscard]] size_t GeneratedFrameCount() const;

    void GenerateInto(const Device& device, Frame* destination, size_t generation);

private:
    void Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale);
    void DumpDebugImages(u64 count);
    void UpdateTiming(bool use_fsr3);
    [[nodiscard]] f32 RefreshPeriod();

    // EDEN_NX2_FG_TIMED / EDEN_NX2_FG_DIAG (present/nx2_fg_timing.h), all on the Composite thread
    struct Timing {
        bool timed{};
        bool diag{};
        bool applies{};     // this burst takes the timed weights
        f32 interval{};     // B of this burst (DisplayGap), seconds
        f32 period{};       // V of this burst, seconds (0: unknown)
        std::optional<f32> override_hz;
        f32 display_hz{};
        std::optional<std::chrono::steady_clock::time_point> display_read;
        u64 composites{};      // Process calls
        u64 last_composite{};  // the one UpdateTiming saw last
        size_t previous_count{}; // and its generated frames
        // EDEN_NX2_FG_DIAG: the frames since the last line
        struct Stats {
            std::array<u32, 8> bursts{}; // by generated frames, the last bin 7 or more
            u32 frames{};
            u32 timed_bursts{};
            u32 interval_count{};
            f32 interval_sum{};
            f32 interval_min{};
            f32 interval_max{};
            size_t sample_count{};
            std::array<f32, 8> sample_blend{};
        } stats;
    };

    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    void* native_window{};
    Timing timing{};

    std::optional<LsfgShaders> shaders;
    std::optional<LsfgChain> chain;
    std::optional<Fsr3FrameGen> fsr3_chain;
    FrameGenPacer pacer;
    FrameGenPlan plan{};
    VkExtent2D peak_guest_extent{};
    VkExtent2D built_extent{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    f32 built_flow_scale{};
    u64 frame_count{};
    u64 last_count{};
    u64 generated_total{};
    size_t last_generations{};
    u32 warm_streak{};
    bool generated{};
    bool unavailable{};
    bool dumped{};
};

} // namespace Vulkan
