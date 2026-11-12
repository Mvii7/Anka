// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <set>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/renderer_vulkan.h"

#include "common/assert.h"
#include "shader_recompiler/exception.h"
#include "common/logging.h"
#include "common/nx2_reactive.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/nx2_reactive_flush.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/native_query_trace.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/renderer_vulkan/nx2_record_list.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;


namespace {
struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z,
        .maxDepth = src.translate_z + src.scale_z,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor{};
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = (std::numeric_limits<s32>::max)();
        scissor.extent.height = (std::numeric_limits<s32>::max)();
    }
    return scissor;
}

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return false;
    default:
        return true;
    }
}

bool IsPrimitiveRestartSupported(const Device& device, VkPrimitiveTopology topology) {
    return ((topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsTopologyListPrimitiveRestartSupported()) ||
            SupportsPrimitiveRestart(topology) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsPatchListPrimitiveRestartSupported()));
}

// EDEN_NX2_LIGHT_DRAW=0: every draw goes through the full setup (PrepareDraw).
bool LightDrawEnabled() {
    // Read at the first call, as a function-local static const is, without that static's
    // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
    // several times per draw).
    static std::atomic<s8> enabled{-1};
    s8 value = enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_LIGHT_DRAW");
        value = static_cast<s8>(env == nullptr || env[0] != '0');
        enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

// EDEN_NX2_LIGHT_UNIFORM_VALUES=0: a light draw with uniforms refuses any constant buffer write
// over a descriptor word (or bind of its slot) instead of reading the word again and comparing.
bool LightUniformValues() {
    // Read at the first call, without a guarded static's check on every call (light draws with
    // uniforms).
    static std::atomic<s8> enabled{-1};
    s8 value = enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_LIGHT_UNIFORM_VALUES");
        value = static_cast<s8>(env == nullptr || env[0] != '0');
        enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

// EDEN_NX2_LIGHT_STORAGE=0: a light draw with uniforms refuses storage buffer descriptors written
// with other values (set up in full) instead of binding those buffers again; =2 sets the draws
// that bound them again up in full after all and compares, as EDEN_NX2_LIGHT_UNIFORM=2 does for
// every light draw with uniforms.
u32 LightStorageMode() {
    // Read at the first call, without a guarded static's check on every call (light draws with
    // uniforms).
    static std::atomic<s8> mode{-1};
    s8 value = mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_LIGHT_STORAGE");
        value = static_cast<s8>(env == nullptr ? 1 : env[0] == '0' ? 0 : env[0] == '2' ? 2 : 1);
        mode.store(value, std::memory_order_relaxed);
    }
    return static_cast<u32>(value);
}

// Light draws bind index buffers as they are: no quad index pass (BindQuadIndexBuffer, quad
// topologies) and no 8-bit index conversion.
bool LightDrawShape(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state,
                    bool is_indexed) {
    return draw_state.topology != Maxwell::PrimitiveTopology::Quads &&
           draw_state.topology != Maxwell::PrimitiveTopology::QuadStrip &&
           (!is_indexed || draw_state.index_buffer.format != Maxwell::IndexFormat::UnsignedByte);
}

// Back stages, cut c: the fence calls B runs in a join (nx2_stage_split.h), with this fence
// manager's async check.
using VideoCommon::Nx2Stages::FenceCall;
[[nodiscard]] constexpr bool FenceJoins(FenceCall call, bool force) {
    return VideoCommon::Nx2Stages::FenceJoinsB(call, force, FenceManagerParams::HAS_ASYNC_CHECK);
}

// Stage diagnostics (EDEN_NX2_GPU_PROFILE, " | stage diagnostics:" in the report): what the
// back's work would make the stages of EDEN_NX2_BACK_STAGES cross for. Used only while profiling;
// per thread, like the counters (the thread that runs the rasterizer).
namespace Diagnostics {
namespace Profile = VideoCommon::Nx2Profile;

u64 CounterValue(Profile::Counter counter) {
    return Profile::t_data.counters[static_cast<size_t>(counter)];
}

/// The scheduler's tick (each submission moves it) and the TickWork requests run, when the last
/// draw left its anchor: what recorded commands before a light draw (TryLightDraw).
thread_local u64 t_anchor_tick{};
thread_local u64 t_anchor_tick_work{};

void NoteAnchor(const Scheduler& scheduler) {
    t_anchor_tick = scheduler.CurrentTick();
    t_anchor_tick_work = CounterValue(Profile::Counter::TickWorkRequests);
}

void NoteLightRecorded(const Scheduler& scheduler) {
    if (CounterValue(Profile::Counter::TickWorkRequests) != t_anchor_tick_work) {
        Profile::Add(Profile::Counter::LightRecordedTickWork, 1);
    } else if (scheduler.CurrentTick() != t_anchor_tick) {
        Profile::Add(Profile::Counter::LightRecordedSubmit, 1);
    } else {
        Profile::Add(Profile::Counter::LightRecordedEvent, 1);
    }
}

/// Whether [address, address + size) of the GPU address space overlaps the texture header or
/// sampler pool the 3D engine names (32 bytes an entry; C reads them directly with cut c).
bool OnTexturePools(const Tegra::Engines::Maxwell3D* maxwell3d, GPUVAddr address, u64 size) {
    if (maxwell3d == nullptr || size == 0) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const auto overlaps = [&](GPUVAddr pool, u32 limit) {
        return pool != 0 && address < pool + (u64{limit} + 1) * 32 && pool < address + size;
    };
    return overlaps(regs.tex_header.Address(), regs.tex_header.limit) ||
           overlaps(regs.tex_sampler.Address(), regs.tex_sampler.limit);
}

/// The same for device addresses (the pools' starts translated, the pools taken as contiguous).
bool OnTexturePools(const Tegra::Engines::Maxwell3D* maxwell3d, Tegra::MemoryManager* memory,
                    DAddr address, u64 size) {
    if (maxwell3d == nullptr || memory == nullptr || size == 0) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const auto overlaps = [&](GPUVAddr pool, u32 limit) {
        const std::optional<DAddr> start = pool != 0 ? memory->GpuToCpuAddress(pool) : std::nullopt;
        return start && address < *start + (u64{limit} + 1) * 32 && *start < address + size;
    };
    return overlaps(regs.tex_header.Address(), regs.tex_header.limit) ||
           overlaps(regs.tex_sampler.Address(), regs.tex_sampler.limit);
}

/// Inline writes whose caches hear of them at the next FlushCaching (EDEN_NX2_INLINE_DEFER):
/// InnerInvalidation tells their ranges from constant buffer data, which reaches it the same way.
thread_local std::vector<std::pair<DAddr, u64>> t_deferred_inline;

void NoteDeferredInline(DAddr address, u64 size) {
    if (t_deferred_inline.size() < 4096) {
        t_deferred_inline.emplace_back(address, size);
    }
}

/// An inline write (AccelerateInlineToMemory, or one run of AccelerateInlineRuns).
void NoteInlineWrite(const Tegra::Engines::Maxwell3D* maxwell3d, GPUVAddr address, u64 size) {
    Profile::Add(Profile::Counter::InlineWrites, 1);
    if (OnTexturePools(maxwell3d, address, size)) {
        Profile::Add(Profile::Counter::InlineOnPools, 1);
    }
}

/// The 3D engine of the channel bound last, for the DMA engine's writes (AccelerateDMA).
thread_local const Tegra::Engines::Maxwell3D* t_bound_maxwell3d{};

/// A DMA copy or fill writing guest memory at [address, address + size).
void NoteDmaWrite(GPUVAddr address, u64 size) {
    Profile::Add(Profile::Counter::DmaWrites, 1);
    if (OnTexturePools(t_bound_maxwell3d, address, size)) {
        Profile::Add(Profile::Counter::DmaOnPools, 1);
    }
}
} // namespace Diagnostics
} // Anonymous namespace

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE,
                             device.IsExtDescriptorBufferSupported()),
      compute_pass_descriptor_queue(device, UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE),
      descriptor_buffer_ring(device, memory_allocator),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool, texture_cache),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     descriptor_buffer_ring, render_pass_cache, buffer_cache, texture_cache,
                     gpu.ShaderNotify()),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler),
      wfi_event(device.GetLogical().CreateEvent()) {
    scheduler.SetQueryCache(query_cache);
    // Back stages, cut c (T7): the buffer cache and the pipeline lookup are B's (its replica),
    // the rest C's (Role::Record, the default).
    const VideoCommon::Nx2Stages::Config& stages = VideoCommon::Nx2Stages::Get();
    split_draws = stages.mode != 0 && stages.mode != 3 && stages.cut_c;
    if (split_draws) {
        buffer_cache.SetStageRole(VideoCommon::Nx2Stages::Role::Memory);
        pipeline_cache.SetStageRole(VideoCommon::Nx2Stages::Role::Memory);
        VideoCommon::Nx2Stages::SetStageHooks(&BeginStagesHook, &EndStagesHook, this);
        VideoCommon::Nx2Stages::SetCbWriteCheck(&CbWriteCheckHook, this);
    }
}

RasterizerVulkan::~RasterizerVulkan() {
    if (split_draws) {
        VideoCommon::Nx2Stages::SetStageHooks(nullptr, nullptr, nullptr);
        VideoCommon::Nx2Stages::SetCbWriteCheck(nullptr, nullptr);
    }
    scheduler.WaitWorker();
    scheduler.Finish();
}

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, bool indirect, Func&& draw_func) {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    bool submitted = false;
    SCOPE_EXIT {
        auto& native = maxwell3d->GetNativeMacroEngine();
        if (native.native_turing && !submitted) {
            native.native_execution_failed = true;
            LOG_DEBUG(HW_GPU, "Native graphics draw failed; refusing submission completion");
        }
    };
    namespace Profile = VideoCommon::Nx2Profile;
    bool invalidated;
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
        invalidated = gpu_memory->FlushCaching();
    }
    // EDEN_NX2_LIGHT_DRAW: what the next draw compares with, taken before this one binds
    // anything; it becomes the anchor however this draw ends (armed only if it was recorded and
    // a light draw may follow it).
    LightAnchor anchor{
        .maxwell3d = maxwell3d,
        .gpu_memory = gpu_memory,
        .epoch = light_epoch.load(std::memory_order_seq_cst),
        .cached_writes = gpu_memory->CachedWrites(),
        .same_writes = maxwell3d->light.same_writes,
        .cb_writes = maxwell3d->light.cb_writes,
        .image_hits = texture_cache.WriteHits(),
        .shader_hits = pipeline_cache.InvalidationHits(),
        .topology = maxwell3d->draw_manager.draw_state.topology,
        .index_format = maxwell3d->draw_manager.draw_state.index_buffer.format,
        .is_indexed = is_indexed,
        .indirect = indirect,
    };
    LightLevel light_follows = LightLevel::None;
    bool set_up = false;
    SCOPE_EXIT {
        if (light_follows == LightLevel::None && !set_up && LightDrawEnabled()) {
            Profile::Add(Profile::Counter::LightRefusedCutShort, 1);
        }
        anchor.record_count = scheduler.RecordCount();
        if (Profile::Enabled()) {
            Diagnostics::NoteAnchor(scheduler);
        }
        anchor.level = light_follows;
        light_anchor = anchor;
        light_verify.active = false;
        auto& light = maxwell3d->light;
        light.changes = 0;
        light.first_blocking = 0;
        light.first_blocking_uniform = 0;
        light.written_count = 0;
        light.rebound.fill(0);
        // Same-value constant buffer writes matter to a plain light draw only.
        light.armed = light_follows == LightLevel::Full;
    };
    if (LightDrawEnabled() && TryLightDraw(is_indexed, indirect, invalidated, draw_func)) {
        // A light draw leaves the setup it followed: what may follow that may follow it.
        submitted = true;
        anchor.pipeline = light_anchor.pipeline;
        light_follows = light_anchor.level;
        return;
    }

    const auto report_failure = [&](std::string_view reason) {
        thread_local std::set<std::tuple<u64, u64, std::string_view>> seen;
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        if (seen.size() < 64 && seen.emplace(vs, ps, reason).second) {
            LOG_WARNING(HW_GPU, "Draw skipped ({}): vs={:#x} ps={:#x} rt0={:#x} {}x{}", reason, vs, ps,
                        regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height);
        }
    };
    GraphicsPipeline* const pipeline{[&] {
        Profile::Scope scope{Profile::Zone::PipelineLookup};
        return pipeline_cache.CurrentGraphicsPipeline();
    }()};
    if (!pipeline) {
        report_failure("no pipeline");
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // Donkey Kong Bananza's terrain: a GPU program run over points resets each rock/dirt piece's
    // 5-word indirect draw record ({triangles 0, copies 1, 0, 0, 0}) through the pointers in a list
    // (c0[0x110]). Stores through addresses loaded from memory can't be mapped onto a storage
    // buffer, so the recompiler loses them and every record keeps 0 copies -- and a draw of 0
    // copies draws nothing. Do that program's stores here instead. EDEN_NX2_RESETFIX=0 leaves it
    // to the GPU.
    static const bool reset_fix = [] {
        const char* value = std::getenv("EDEN_NX2_RESETFIX");
        return value == nullptr || value[0] != '0';
    }();
    if (reset_fix && !is_indexed && maxwell3d->GetNativeMacroEngine().native_turing) {
        // The program's 11 native instructions.
        static constexpr std::array<u32, 44> NX2_INDIRECT_RESET_PROGRAM{
            0xff007321, 0x0002fcff, 0x00000000, 0x000e2400, 0x00007a11, 0x00004400, 0x078018ff,
            0x001fca00, 0xff017624, 0x00004500, 0x000e06ff, 0x000fcc00, 0x00007381, 0x00000000,
            0x001eab00, 0x000ea200, 0x00037802, 0x00000001, 0x00000f00, 0x000fca00, 0x00007386,
            0x00000403, 0x0010a900, 0x004fe800, 0x00007386, 0x000000ff, 0x0010a900, 0x000fe800,
            0x00007386, 0x000008ff, 0x0010a900, 0x000fe800, 0x00007386, 0x00000cff, 0x0010a900,
            0x000fe800, 0x00007386, 0x000010ff, 0x0010a900, 0x000fe200, 0x0000794d, 0x00000000,
            0x03800000, 0x000fea00,
        };
        const auto& reset_regs = maxwell3d->regs;
        const GPUVAddr program =
            (u64{reset_regs.pipelines[1].reserved[0] & 0xff} << 32) | reset_regs.pipelines[1].reserved[1];
        // Per thread, as the answers kept below (whichever thread sets draws up: the split's back, the
        // stage running the rasterizer); entries are decided from guest memory and never change.
        thread_local std::unordered_map<GPUVAddr, bool> reset_programs;
        // EDEN_NX2_RESET_LAST=0: the map is searched for every non-indexed draw set up in full.
        // Most come with the vertex program of the one before: its answer is kept (an entry is
        // decided when it is made and never changes, and none is removed).
        static const bool reset_last = [] {
            const char* value = std::getenv("EDEN_NX2_RESET_LAST");
            return value == nullptr || value[0] != '0';
        }();
        thread_local GPUVAddr last_program{};
        thread_local bool last_known{};
        thread_local bool last_is_reset{};
        bool is_reset;
        if (reset_last && last_known && program == last_program) {
            is_reset = last_is_reset;
        } else {
            auto [entry, inserted] = reset_programs.try_emplace(program, false);
            if (inserted) {
                std::array<u32, 44> code{};
                const GPUVAddr code_address = program + 128;
                if (gpu_memory->IsFullyMappedRange(code_address, sizeof(code))) {
                    gpu_memory->ReadBlockUnsafe(code_address, code.data(), sizeof(code));
                    entry->second = code == NX2_INDIRECT_RESET_PROGRAM;
                }
                if (entry->second) {
                    LOG_INFO(HW_GPU,
                             "Indirect draw reset program at {:#x}: its stores are done here",
                             program);
                }
            }
            is_reset = entry->second;
            last_program = program;
            last_known = true;
            last_is_reset = is_reset;
        }
        if (is_reset) {
            const auto& reset_draw = maxwell3d->draw_manager.draw_state;
            const auto& cbuf0 = maxwell3d->state.shader_stages[0].const_buffers[0];
            const GPUVAddr list = gpu_memory->Read<u64>(cbuf0.address + 0x110);
            const u32 count = (std::min)(reset_draw.vertex_buffer.count, 1u << 20);
            const GPUVAddr list_begin = list + u64{reset_draw.vertex_buffer.first} * sizeof(u64);
            const size_t list_bytes = size_t{count} * sizeof(u64);
            if (count != 0 && gpu_memory->IsFullyMappedRange(list_begin, list_bytes)) {
                Profile::Add(Profile::Counter::ResetFixLists, 1);
                if (gpu_memory->IsMemoryDirty(list_begin, list_bytes,
                                              VideoCommon::CacheType::BufferCache)) {
                    Profile::Add(Profile::Counter::ResetFixFlushes, 1);
                    gpu_memory->FlushRegion(list_begin, list_bytes,
                                            VideoCommon::CacheType::BufferCache);
                }
                std::vector<u64> pointers(count);
                gpu_memory->ReadBlockUnsafe(list_begin, pointers.data(), list_bytes);
                // Records the list points at one after another within a 4 KiB page are reset
                // with one inline write (the caches see the same bytes, in the same order).
                // EDEN_NX2_RESET_RUNS=0: one write per record.
                static const bool runs_enabled = [] {
                    const char* value = std::getenv("EDEN_NX2_RESET_RUNS");
                    return value == nullptr || value[0] != '0';
                }();
                // EDEN_NX2_RESET_SORT: the records are reset in address order, so records next to
                // each other in memory make runs in whatever order the list names them. Every
                // record gets the same 20 bytes and a pointer is skipped for its own address
                // only, so when no two records share a byte (checked once sorted; otherwise the
                // order of the list is kept) memory ends up the same, and each cache sees an
                // invalidation of the same bytes. =0 keeps the order of the list.
                static const bool sort_enabled = [] {
                    const char* value = std::getenv("EDEN_NX2_RESET_SORT");
                    return value == nullptr || value[0] != '0';
                }();
                static constexpr u32 RecordSize = 5 * sizeof(u32);
                static constexpr u32 MaxRun = 4096 / RecordSize;
                static const auto run_words = [] {
                    std::array<u32, 5 * MaxRun> words{};
                    for (u32 i = 0; i < MaxRun; ++i) {
                        words[i * 5 + 1] = 1;
                    }
                    return words;
                }();
                // The records written (writing them changes no GPU mapping).
                std::vector<u64> records;
                records.reserve(pointers.size());
                for (const u64 pointer : pointers) {
                    if (pointer != 0 && gpu_memory->IsFullyMappedRange(pointer, RecordSize)) {
                        records.push_back(pointer);
                    }
                }
                bool sorted = false;
                // Diagnostic: records sharing bytes (the list's order is kept then, and the
                // inline writes overlap), at another offset or one record named twice.
                bool overlapping = false;
                bool repeated = false;
                const auto find_overlaps = [&](const std::vector<u64>& ascending) {
                    for (size_t i = 1; i < ascending.size(); ++i) {
                        const u64 distance = ascending[i] - ascending[i - 1];
                        repeated |= distance == 0;
                        overlapping |= distance != 0 && distance < RecordSize;
                    }
                };
                const bool in_order = std::is_sorted(records.begin(), records.end());
                if (runs_enabled && sort_enabled && !in_order) {
                    std::vector<u64> ordered(records);
                    std::sort(ordered.begin(), ordered.end());
                    const bool disjoint =
                        std::adjacent_find(ordered.begin(), ordered.end(), [](u64 lower, u64 upper) {
                            return upper - lower < RecordSize;
                        }) == ordered.end();
                    if (disjoint) {
                        records = std::move(ordered);
                        sorted = true;
                    } else {
                        find_overlaps(ordered);
                    }
                } else if (in_order) {
                    find_overlaps(records);
                }
                {
                    thread_local u64 lists{};
                    thread_local u64 overlapping_lists{};
                    thread_local u64 repeating_lists{};
                    ++lists;
                    overlapping_lists += overlapping ? 1 : 0;
                    repeating_lists += repeated ? 1 : 0;
                    const u64 shared = overlapping_lists + repeating_lists;
                    if ((overlapping || repeated) && (shared & (shared - 1)) == 0) {
                        LOG_INFO(HW_GPU,
                                 "Indirect draw reset list {:#x}: records share bytes ({} lists "
                                 "with records overlapping at another offset, {} naming one "
                                 "record twice, of {})",
                                 list_begin, overlapping_lists, repeating_lists, lists);
                    }
                }
                // EDEN_NX2_RESET_BATCH: the runs written together (AccelerateInlineRuns), the
                // uploads into GPU-written buffer data recorded as one copy per buffer instead of
                // a staging buffer, two barriers and a copy each. Not with accurate GPU fences,
                // where finding a buffer may wait for the host GPU. =0 writes each run on its own.
                static const bool batch_enabled = [] {
                    const char* value = std::getenv("EDEN_NX2_RESET_BATCH");
                    return value == nullptr || value[0] != '0';
                }();
                const bool batch = batch_enabled && !Settings::IsGPUFenceBehaviorAccurate();
                std::vector<std::pair<GPUVAddr, u32>> batch_runs;
                if (batch) {
                    batch_runs.reserve(records.size());
                }
                GPUVAddr run_start = 0;
                u32 run_records = 0;
                u32 writes = 0;
                const auto write_run = [&] {
                    if (run_records == 0) {
                        return;
                    }
                    const size_t run_bytes = size_t{run_records} * RecordSize;
                    if (batch) {
                        batch_runs.emplace_back(run_start, static_cast<u32>(run_bytes));
                    } else {
                        AccelerateInlineToMemory(
                            run_start, run_bytes,
                            {reinterpret_cast<const u8*>(run_words.data()), run_bytes});
                    }
                    run_records = 0;
                    ++writes;
                };
                for (const u64 pointer : records) {
                    if (runs_enabled && run_records != 0 && run_records < MaxRun &&
                        pointer == run_start + u64{run_records} * RecordSize &&
                        (run_start >> 12) == ((pointer + RecordSize - 1) >> 12)) {
                        ++run_records;
                        continue;
                    }
                    write_run();
                    run_start = pointer;
                    run_records = 1;
                }
                write_run();
                if (!batch_runs.empty()) {
                    AccelerateInlineRuns(batch_runs, {reinterpret_cast<const u8*>(run_words.data()),
                                                      sizeof(run_words)});
                }
                thread_local u32 reset_logs{};
                if (reset_logs < 5) {
                    ++reset_logs;
                    LOG_INFO(HW_GPU,
                             "Indirect draw reset: list {:#x}, {} records ({} written in {} "
                             "inline writes{})",
                             list_begin, count, records.size(), writes, sorted ? ", sorted" : "");
                }
            }
            submitted = true;
            return;
        }
    }
    if (Profile::Enabled() && LightDrawEnabled()) {
        NoteLightPipeline(*pipeline, is_indexed, indirect, invalidated);
    }
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    const u64 render_pass_begins = scheduler.RenderPassBegins();
    if (Profile::Scope scope{Profile::Zone::Configure}; !pipeline->Configure(is_indexed)) {
        report_failure("configure failed");
        return;
    }

    const u64 records_before_dynamic = scheduler.RecordCount();
    {
        Profile::Scope scope{Profile::Zone::DynamicState};
        UpdateDynamicStates();
    }
    if (light_verify.active) {
        CheckLightVerify(*pipeline, scheduler.RenderPassBegins() - render_pass_begins,
                         scheduler.RecordCount() - records_before_dynamic);
    }

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
        draw_func();
    }
    if (Profile::Enabled()) {
        buffer_cache.CommitVertexStreamNote();
    }
    submitted = true;
    set_up = true;
    anchor.pipeline = pipeline;
    light_follows = LightDrawEnabled() ? LightFollowable(*pipeline) : LightLevel::None;
    thread_local u32 native_capture_count{};
    if (maxwell3d->GetNativeMacroEngine().native_turing &&
        native_capture_count < 16 && [] {
            static const bool capture = std::getenv("EDEN_NATIVE_DRAW_CAPTURE") != nullptr;
            return capture;
        }()) {
        const u32 capture = native_capture_count++;
        const auto prefix = fmt::format("native-draw-{}", capture);
        const auto dump = [&](const std::string& name, GPUVAddr address, size_t size) {
            if (!size || !gpu_memory->IsFullyMappedRange(address, size)) return;
            std::vector<u8> bytes(size);
            gpu_memory->ReadBlockUnsafe(address, bytes.data(), bytes.size());
            std::ofstream file(prefix + name + ".bin", std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            LOG_INFO(HW_GPU, "Native draw capture {}{}: address={:#x}, bytes={}, nonzero={}",
                     capture, name, address, size,
                     std::count_if(bytes.begin(), bytes.end(), [](u8 byte) { return byte != 0; }));
        };
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const size_t size = static_cast<size_t>(rt.array_pitch) * 4;
        if (size && size <= 16 * 1024 * 1024) {
            if (const auto address = gpu_memory->GpuToCpuAddress(rt.Address(), size)) {
                LOG_INFO(HW_GPU, "Native draw {} render target physical address={:#x}", capture, *address);
                texture_cache.DownloadMemory(*address, size);
                dump("-rt0", rt.Address(), size);
            }
        }
        std::ofstream registers(prefix + "-registers.bin", std::ios::binary);
        registers.write(reinterpret_cast<const char*>(&regs), sizeof(regs));
        for (size_t stream = 0; stream < 4; ++stream) {
            if (regs.vertex_streams[stream].IsEnabled())
                dump(fmt::format("-vertex-{}", stream), regs.vertex_streams[stream].Address(), 128);
        }
        for (size_t stage : {0U, 4U}) {
            for (size_t bank : {2U, 3U}) {
                const auto& cb = maxwell3d->state.shader_stages[stage].const_buffers[bank];
                if (cb.enabled) dump(fmt::format("-cbuf-{}-{}", stage, bank), cb.address, std::min<size_t>(cb.size, 128));
            }
        }
    }
}

void RasterizerVulkan::BeginLightDraw(bool indirect, bool shape_ok) {
    light_entry.record_count = scheduler.RecordCount();
    light_entry.shape_ok = shape_ok;
    namespace Profile = VideoCommon::Nx2Profile;
    if (!Profile::Enabled()) {
        return;
    }
    // What changed since the previous draw, the widest change first.
    using LightChange = Tegra::Engines::Maxwell3D::LightChange;
    const LightAnchor& last = light_anchor;
    const u32 changes = maxwell3d->light.changes;
    const bool other_work = last.record_count != light_entry.record_count ||
                            last.epoch != light_epoch.load(std::memory_order_seq_cst) ||
                            last.maxwell3d != maxwell3d || last.gpu_memory != gpu_memory;
    Profile::DrawChange change = Profile::DrawChange::Nothing;
    if (other_work || (changes & LightChange::Command) != 0) {
        change = Profile::DrawChange::OtherWork;
    } else if ((changes & LightChange::State) != 0) {
        change = Profile::DrawChange::State;
    } else if ((changes & LightChange::Textures) != 0) {
        change = Profile::DrawChange::Textures;
    } else if ((changes & (LightChange::UniformData | LightChange::UniformBind)) != 0) {
        change = Profile::DrawChange::Uniforms;
    } else if ((changes & LightChange::IndexBuffer) != 0) {
        change = Profile::DrawChange::IndexBuffer;
    } else if ((changes & LightChange::VertexStreams) != 0) {
        change = Profile::DrawChange::VertexStreams;
    }
    const u32 base = static_cast<u32>(indirect ? Profile::Counter::DrawChangeIndirect
                                               : Profile::Counter::DrawChangeDirect);
    Profile::Add(static_cast<Profile::Counter>(base + static_cast<u32>(change)), 1);
    // The register whose write first kept this draw off the light path, armed or not.
    const u32 first_blocking = maxwell3d->light.first_blocking;
    if ((changes & ~LightChange::Light) != 0 && first_blocking != 0) {
        Profile::Note(Profile::Histogram::LightBlocking, s64{first_blocking} - 1);
    }
    // And the first outside what a light draw with uniforms sets up again.
    const u32 first_blocking_uniform = maxwell3d->light.first_blocking_uniform;
    if ((changes & ~LightChange::Uniform) != 0 && first_blocking_uniform != 0) {
        Profile::Note(Profile::Histogram::LightUniformBlocking, s64{first_blocking_uniform} - 1);
    }
}

bool RasterizerVulkan::LightFeedbackOff() const {
    return maxwell3d->regs.transform_feedback_enabled == 0;
}

RasterizerVulkan::LightLevel RasterizerVulkan::LightFollowable(const GraphicsPipeline& pipeline) {
    // A light draw repeats none of this draw's setup: it must have left nothing a full setup
    // would do again for the same registers and memory.
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Counter refused;
    if (!light_entry.shape_ok) {
        refused = Profile::Counter::LightRefusedShape;
    } else if (!LightFeedbackOff()) {
        refused = Profile::Counter::LightRefusedFeedback;
    } else if (texture_cache.RenderTargetsNeedRepeat() || pipeline.ViewsNeedRepeat()) {
        // Also sampled or stored images (an alias written after them is copied in first).
        refused = Profile::Counter::LightRefusedTargets;
    } else if (pipeline.WritesMemory()) {
        // Each draw marks what it writes (buffer ranges, storage images): a light draw with
        // uniforms does that as the full setup does (GraphicsPipeline::ConfigureUniforms).
        Profile::Add(Profile::Counter::LightRefusedWrites, 1);
        return LightUniformMode() != 0 ? LightLevel::Uniform : LightLevel::None;
    } else {
        return LightLevel::Full;
    }
    Profile::Add(refused, 1);
    return LightLevel::None;
}

bool RasterizerVulkan::LightUniformOk(GraphicsPipeline& pipeline, bool invalidated,
                                      bool& rebind_storage) {
    // The full setup reads, besides what TryLightDraw checked: the shaders (their memory), the
    // texture header and sampler pools, texture handles and storage buffer descriptors in
    // constant buffers, the images it samples, stores to and renders to, and buffers through the
    // buffer cache. Since the anchor began, memory changed only by constant buffer data
    // (Maxwell3D::ProcessCBMultiData, every cached write); FlushCaching handed those ranges to
    // every cache as it does before any draw. The buffer cache's work (uniform buffers streamed
    // or uploaded, storage and texture buffers synchronized, GPU-written ranges) the uniform way
    // repeats as it is; the rest must not have seen the writes.
    namespace Profile = VideoCommon::Nx2Profile;
    const LightAnchor& last = light_anchor;
    const auto& light = maxwell3d->light;
    const u64 cached_writes = gpu_memory->CachedWrites() - last.cached_writes;
    const u32 ranges = light.written_count;
    if (cached_writes != light.cb_writes - last.cb_writes || ranges > light.written.size() ||
        (invalidated && cached_writes == 0)) {
        // Other writes (inline, DMA, query reports) or too many ranges to check.
        Profile::Add(Profile::Counter::LightUniformMemory, 1);
        return false;
    }
    const std::span<const std::pair<GPUVAddr, u32>> written{light.written.data(), ranges};
    const u64 sparse_low = Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed);
    const u64 sparse_high = Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed);
    for (const auto& [address, size] : written) {
        // A sparse region's first write gives it memory (Nx2BackSparseWrite): mappings change.
        if (address + size > sparse_low && address < sparse_high) {
            Profile::Add(Profile::Counter::LightUniformMemory, 1);
            return false;
        }
    }
    // An image found in a written range is marked CPU-modified (uploaded at its next use); a
    // shader found there is removed and its pipelines looked up again. Checked before the
    // anchor's pipeline is used.
    if (texture_cache.WriteHits() != last.image_hits ||
        pipeline_cache.InvalidationHits() != last.shader_hits) {
        Profile::Add(Profile::Counter::LightUniformCaches, 1);
        return false;
    }
    // The texture handles and storage buffer descriptors the pipeline reads from constant
    // buffers: those written over, or in a slot bound elsewhere (a bind that changed its
    // address, size or enable), must hold what the full setup read
    // (GraphicsPipeline::CheckDescriptorWords). EDEN_NX2_LIGHT_STORAGE: the storage buffer
    // descriptors may hold other values; ConfigureUniforms then binds those buffers again as the
    // full setup binds them (from the words at the bound constant buffers' addresses now, so a
    // rebound slot is no different). With the profile on the scan goes past storage words that
    // changed either way, and the refusals count a handle that changed first.
    const u32 storage_mode = LightStorageMode();
    const GraphicsPipeline::LightWords words = pipeline.CheckDescriptorWords(
        written, light.rebound, LightUniformValues(), storage_mode != 0 || Profile::Enabled());
    const bool storage_changed = (words.changed & GraphicsPipeline::StorageWords) != 0;
    if (storage_changed && Profile::Enabled()) {
        Profile::Add(Profile::Counter::LightStorageChanged, 1);
        Profile::Add(Profile::Counter::LightStorageBuffers, words.storage_buffers);
        Profile::Add(Profile::Counter::LightStorageAddress, words.storage_address ? 1 : 0);
        Profile::Add(Profile::Counter::LightStorageWritten, words.storage_written ? 1 : 0);
        Profile::Add(Profile::Counter::LightStorageWithHandles,
                     (words.changed & GraphicsPipeline::HandleWords) != 0 ? 1 : 0);
        Profile::Note(Profile::Histogram::LightStorageFirst, s64{words.storage_first});
    }
    const u32 refusing =
        storage_mode != 0 ? words.changed & GraphicsPipeline::HandleWords : words.changed;
    if (refusing != 0) {
        Profile::Add((words.rebound_changed & refusing) != 0 ? Profile::Counter::LightUniformBinds
                     : (refusing & GraphicsPipeline::HandleWords) != 0
                         ? Profile::Counter::LightUniformHandles
                         : Profile::Counter::LightUniformStorage,
                     1);
        return false;
    }
    // The texture header and sampler pool entries those handles index (32 bytes each) must not
    // have been written; other entries the full setup does not read.
    const auto& regs = maxwell3d->regs;
    const auto overlaps = [&](GPUVAddr begin, GPUVAddr end) {
        for (const auto& [address, size] : written) {
            if (address < end && begin < address + size) {
                return true;
            }
        }
        return false;
    };
    const auto entries_written = [&](GPUVAddr pool, u32 limit, std::span<const u32> used) {
        if (!overlaps(pool, pool + (u64{limit} + 1) * 32)) {
            return false;
        }
        for (const u32 index : used) {
            const GPUVAddr entry = pool + u64{index} * 32;
            if (overlaps(entry, entry + 32)) {
                return true;
            }
        }
        return false;
    };
    if (entries_written(regs.tex_header.Address(), regs.tex_header.limit,
                        pipeline.UsedTextureHeaders())) {
        Profile::Add(Profile::Counter::LightUniformTic, 1);
        return false;
    }
    // The sampler pool as TextureCache::SynchronizeDescriptors sets it up: at tex_sampler, with
    // the header pool's limit when samplers go by header index (ViaHeaderBinding; TexturePair
    // then gives the header index as the sampler index). Indices past the limit read nothing
    // (GetSamplerId: the null sampler), so the whole-pool test misses no entry read.
    const u32 tsc_limit = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding
                              ? regs.tex_header.limit
                              : regs.tex_sampler.limit;
    if (entries_written(regs.tex_sampler.Address(), tsc_limit, pipeline.UsedSamplers())) {
        Profile::Add(Profile::Counter::LightUniformTsc, 1);
        return false;
    }
    if ((words.rewritten & GraphicsPipeline::HandleWords) != 0) {
        Profile::Add(Profile::Counter::LightUniformSameHandles, 1);
    }
    if ((words.rewritten & GraphicsPipeline::StorageWords) != 0 && !storage_changed) {
        Profile::Add(Profile::Counter::LightUniformSameStorage, 1);
    }
    rebind_storage = storage_changed;
    return true;
}

void RasterizerVulkan::CheckLightVerify(GraphicsPipeline& pipeline, u64 render_pass_begins,
                                        u64 dynamic_records) {
    // The full setup must bind what the light one did and change nothing else: the same
    // pipeline (not bound again), push constants, render pass and dynamic state, and the same
    // descriptors. A uniform buffer the full setup streamed again may lie elsewhere (the
    // direct-mapped reuse slots of EDEN_NX2_UBO_REUSE hold one size per range: a range bound to
    // two stages with different used sizes, or two ranges in one slot, stream again; with
    // EDEN_NX2_UBO_STAGE the slot's last slice may hold the bytes for another range); it counts
    // as the same when it holds the same bytes.
    namespace Profile = VideoCommon::Nx2Profile;
    Profile::Add(Profile::Counter::LightUniformVerified, 1);
    std::string mismatch;
    if (&pipeline != light_verify.pipeline) {
        mismatch = fmt::format("pipeline {:#x} instead of {:#x}", pipeline.KeyHash(),
                               light_verify.pipeline->KeyHash());
    } else {
        const auto* const full = guest_descriptor_queue.UpdateData();
        const u32 entries = pipeline.DescriptorEntryCount();
        const auto bytes = [&](const DescriptorUpdateEntry& entry, VkDeviceSize& range) {
            if (pipeline.UsesDescriptorBuffer()) {
                range = entry.address.range;
                return staging_pool.MappedBytesAt(entry.address.address, range);
            }
            range = entry.buffer.range;
            return staging_pool.MappedBytes(entry.buffer.buffer, entry.buffer.offset, range);
        };
        u32 entry = 0;
        for (;; ++entry) {
            bool uniform;
            entry = pipeline.FirstPayloadDifference(light_verify.payload.data(), full, entry,
                                                    uniform);
            if (entry == entries || !uniform) {
                break;
            }
            VkDeviceSize light_range;
            VkDeviceSize full_range;
            const std::span<const u8> light_bytes = bytes(light_verify.payload[entry], light_range);
            const std::span<const u8> full_bytes = bytes(full[entry], full_range);
            if (light_range != full_range || light_bytes.empty() ||
                light_bytes.size() != full_bytes.size() ||
                std::memcmp(light_bytes.data(), full_bytes.data(), light_bytes.size()) != 0) {
                break;
            }
            Profile::Add(Profile::Counter::LightUniformSlicesSame, 1);
        }
        if (entry != entries) {
            std::array<u64, 3> light_words{};
            std::array<u64, 3> full_words{};
            std::memcpy(light_words.data(), &light_verify.payload[entry], sizeof(light_words));
            std::memcpy(full_words.data(), &full[entry], sizeof(full_words));
            mismatch = fmt::format("descriptor {} of {}: light {:016x} full {:016x}", entry,
                                   entries, fmt::join(light_words, " "),
                                   fmt::join(full_words, " "));
        }
        // The descriptor words later light draws compare with: as the light way left them (with
        // storage buffer descriptors it bound again, EDEN_NX2_LIGHT_STORAGE) and as the full
        // setup read them now.
        const std::span<const u32> full_values = pipeline.DescriptorValues();
        const std::vector<u32>& light_values = light_verify.values;
        if (light_values.size() == full_values.size()) {
            const auto [light_it, full_it] =
                std::mismatch(light_values.begin(), light_values.end(), full_values.begin());
            if (light_it != light_values.end()) {
                mismatch += fmt::format("{}descriptor word {}: light {:#x} full {:#x}",
                                        mismatch.empty() ? "" : "; ",
                                        light_it - light_values.begin(), *light_it, *full_it);
            }
        }
        if (const u32 changes = pipeline.LastDrawChanges(); changes != 0) {
            mismatch += fmt::format("{}draw changes {:#x} (1 pipeline, 2 rescaling, 4 render area)",
                                    mismatch.empty() ? "" : "; ", changes);
        }
    }
    if (render_pass_begins != 0) {
        mismatch += fmt::format("{}{} render passes begun", mismatch.empty() ? "" : "; ",
                                render_pass_begins);
    }
    if (dynamic_records != 0) {
        mismatch += fmt::format("{}{} dynamic state records", mismatch.empty() ? "" : "; ",
                                dynamic_records);
    }
    if (mismatch.empty()) {
        return;
    }
    Profile::Add(Profile::Counter::LightUniformMismatches, 1);
    thread_local u32 logged{};
    if (logged < 64) {
        ++logged;
        LOG_WARNING(Render_Vulkan, "Light uniform draw differs from its full setup (pipeline {:#x}): {}",
                    light_verify.pipeline->KeyHash(), mismatch);
    }
}

void RasterizerVulkan::NoteLightPipeline(const GraphicsPipeline& pipeline, bool is_indexed,
                                         bool indirect, bool invalidated) {
    // A draw the uniform way would have taken (TryLightDraw's and LightUniformOk's conditions on
    // the previous draw, the work in between, the draw's shape and the cached writes) had fixed
    // function state not changed too: no texture pool, command or other work since. Setting up
    // only its pipeline and dynamic state (the EDEN_NX2_LIGHT_PIPELINE design) needs besides:
    // - the pipeline the full setup found is the previous draw's (the same key: only dynamic
    //   state changed, UpdateDynamicStates) or one with the same programs (a pipeline switch with
    //   the descriptors kept, if their layouts match: not checked here);
    // - no render target register written since the last render target update
    //   (VideoCommon::Dirty::RenderTargets, also surface clip and with it the render area);
    // - the descriptor words the previous pipeline reads unchanged (CheckDescriptorWords).
    // Texture header and sampler pool entries and sparse ranges written are not checked here
    // (LightUniformOk does). Changes nothing; the first reason that applies is counted.
    namespace Profile = VideoCommon::Nx2Profile;
    using LightChange = Tegra::Engines::Maxwell3D::LightChange;
    const LightAnchor& last = light_anchor;
    const auto& light = maxwell3d->light;
    if (LightUniformMode() == 0 || last.level == LightLevel::None || last.pipeline == nullptr ||
        (light.changes & LightChange::State) == 0 ||
        (light.changes & ~(LightChange::Uniform | LightChange::State)) != 0 ||
        last.maxwell3d != maxwell3d || last.gpu_memory != gpu_memory ||
        last.record_count != light_entry.record_count ||
        scheduler.RecordCount() != light_entry.record_count ||
        last.epoch != light_epoch.load(std::memory_order_seq_cst)) {
        return;
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (!light_entry.shape_ok || indirect != last.indirect || is_indexed != last.is_indexed ||
        draw_state.topology != last.topology ||
        (is_indexed && draw_state.index_buffer.format != last.index_format) ||
        !LightFeedbackOff()) {
        return;
    }
    const u64 cached_writes = gpu_memory->CachedWrites() - last.cached_writes;
    if (cached_writes != light.cb_writes - last.cb_writes ||
        light.written_count > light.written.size() || (invalidated && cached_writes == 0) ||
        texture_cache.WriteHits() != last.image_hits ||
        pipeline_cache.InvalidationHits() != last.shader_hits) {
        return;
    }
    Profile::Add(Profile::Counter::LightPipelineCandidates, 1);
    const GraphicsPipeline& previous = *last.pipeline;
    if (&pipeline != &previous && pipeline.Key().unique_hashes != previous.Key().unique_hashes) {
        Profile::Add(Profile::Counter::LightPipelinePrograms, 1);
        return;
    }
    if (maxwell3d->dirty.flags[VideoCommon::Dirty::RenderTargets]) {
        Profile::Add(Profile::Counter::LightPipelineTargets, 1);
        return;
    }
    const std::span<const std::pair<GPUVAddr, u32>> written{light.written.data(),
                                                            light.written_count};
    if (previous.CheckDescriptorWords(written, light.rebound, LightUniformValues(), false)
            .changed != 0) {
        Profile::Add(Profile::Counter::LightPipelineDescriptors, 1);
        return;
    }
    if (light.first_blocking_uniform != 0) {
        Profile::Note(Profile::Histogram::LightPipelineFirst,
                      s64{light.first_blocking_uniform} - 1);
    }
    if (&pipeline != &previous) {
        Profile::Add(Profile::Counter::LightPipelineSwitch, 1);
        return;
    }
    Profile::Add(Profile::Counter::LightPipelineSame, 1);
    if ((light.changes & (LightChange::UniformData | LightChange::UniformBind)) == 0 &&
        cached_writes == light.same_writes - last.same_writes && last.level == LightLevel::Full) {
        Profile::Add(Profile::Counter::LightPipelineSamePlain, 1);
    }
}

template <typename Func>
bool RasterizerVulkan::TryLightDraw(bool is_indexed, bool indirect, bool invalidated,
                                    Func& draw_func) {
    // The last draw (the anchor) was set up in full, or light after one: its pipeline, image
    // and buffer descriptors, push constants, render pass and dynamic state are still what the
    // command buffer holds, as nothing was recorded since (Scheduler::RecordCount; a submission
    // or InvalidateState counts too). A full setup now would produce the same, because what it
    // reads did not change:
    // - registers: only vertex stream addresses/limits/sizes, the index buffer start/limit and
    //   draw parameters changed value (Maxwell3D::LightChange; a state register written with
    //   its value changes nothing). The pipeline key, the shaders, the texture and sampler
    //   pools, the render targets and the dynamic state do not read those (their dirty flags are
    //   the buffer cache's own: VertexBuffer*, IndexBuffer). Topology (the effect of
    //   topology_override), index format and draw kind are the anchor's.
    // - memory: nothing was invalidated or flushed (light_epoch: CPU writes, unmaps, flushes,
    //   fences, frames, channel changes, clears, dispatches, copies), no other engine or method
    //   wrote memory (LightChange::Command) and every cached write since the anchor began was a
    //   constant buffer write of the bytes memory held, with no cache holding newer data there
    //   (Maxwell3D::IsLightSameWrite): its invalidation at FlushCaching above makes a full setup
    //   upload the same bytes. Uniform data, storage buffer addresses and texture handles read
    //   from constant buffers are therefore what the anchor read.
    // - the anchor's pipeline writes no memory (no storage buffer, image or global stores),
    //   transform feedback is off and its render targets need no upload, sparse binding, alias
    //   copy or feedback loop barrier (LightFollowable), so skipping the per-draw bookkeeping
    //   (written ranges, modification ticks: no other image is modified in between) loses
    //   nothing. Queries and conditional rendering need no more than the calls made below,
    //   which the full setup makes per draw too.
    // What a full setup would do differently is then the vertex/index/indirect buffers, done
    // here as it does them. Anything else falls back to the full setup (also from the middle:
    // what was bound so far stays valid and the full setup repeats it).
    //
    // EDEN_NX2_LIGHT_UNIFORM, the uniform way: constant buffer data and binds may change too,
    // and the anchor's pipeline may write memory (LightLevel::Uniform). A full setup would then
    // differ in its buffers only: GraphicsPipeline::ConfigureUniforms sets them up as it does
    // (uniform buffers resolved and streamed or uploaded, storage and texture buffers kept as
    // bound and synchronized, written ranges marked) and pushes each stage's image descriptors as
    // it last did, while the data written lies outside handles, storage buffer descriptors and
    // the texture pools, no constant buffer they are read from moved, and no image or shader saw
    // the writes (LightUniformOk). Pipeline, images, render targets, push constants and dynamic
    // state stay the anchor's, as above. EDEN_NX2_LIGHT_STORAGE: storage buffer descriptors may
    // have changed too; ConfigureUniforms binds those buffers again from them as the full setup
    // does (what else it sets up does not read them: same pipeline, layout, handles and images).
    namespace Profile = VideoCommon::Nx2Profile;
    using LightChange = Tegra::Engines::Maxwell3D::LightChange;
    const LightAnchor& last = light_anchor;
    const auto& light = maxwell3d->light;
    if (last.level == LightLevel::None) {
        Profile::Add(Profile::Counter::LightNotArmed, 1);
        return false;
    }
    const u32 uniform_mode = LightUniformMode();
    const u32 allowed = uniform_mode != 0 ? LightChange::Uniform : LightChange::Light;
    if ((light.changes & ~allowed) != 0) {
        Profile::Add((light.changes & LightChange::Command) != 0 ? Profile::Counter::LightOtherWork
                                                                 : Profile::Counter::LightStateChanged,
                     1);
        return false;
    }
    if (last.maxwell3d != maxwell3d || last.gpu_memory != gpu_memory ||
        last.record_count != light_entry.record_count ||
        scheduler.RecordCount() != light_entry.record_count ||
        last.epoch != light_epoch.load(std::memory_order_seq_cst)) {
        Profile::Add(Profile::Counter::LightOtherWork, 1);
        if (Profile::Enabled() && (last.record_count != light_entry.record_count ||
                                   scheduler.RecordCount() != light_entry.record_count)) {
            Diagnostics::NoteLightRecorded(scheduler);
        }
        return false;
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (!light_entry.shape_ok || indirect != last.indirect || is_indexed != last.is_indexed ||
        draw_state.topology != last.topology ||
        (is_indexed && draw_state.index_buffer.format != last.index_format) ||
        !LightFeedbackOff()) {
        Profile::Add(Profile::Counter::LightShape, 1);
        return false;
    }
    const u64 cached_writes = gpu_memory->CachedWrites() - last.cached_writes;
    const u64 same_writes = light.same_writes - last.same_writes;
    const bool plain = last.level == LightLevel::Full &&
                       (light.changes & ~LightChange::Light) == 0 &&
                       cached_writes == same_writes && !(invalidated && cached_writes == 0);
    if (!plain) {
        if (uniform_mode == 0) {
            Profile::Add(Profile::Counter::LightOtherWork, 1);
            return false;
        }
        GraphicsPipeline& pipeline = *last.pipeline;
        bool rebind_storage = false;
        if (!LightUniformOk(pipeline, invalidated, rebind_storage)) {
            return false;
        }
        Profile::Scope scope{Profile::Zone::LightDraw};
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        if (!pipeline.ConfigureUniforms(is_indexed, rebind_storage)) {
            Profile::Add(Profile::Counter::LightUniformFellBack, 1);
            if (rebind_storage) {
                Profile::Add(Profile::Counter::LightStorageFellBack, 1);
            }
            return false;
        }
        if (uniform_mode == 2 || (rebind_storage && LightStorageMode() == 2)) {
            // Set up in full after all (PrepareDraw), then compared (CheckLightVerify).
            const DescriptorUpdateEntry* const payload = guest_descriptor_queue.UpdateData();
            light_verify.payload.assign(payload, payload + pipeline.DescriptorEntryCount());
            const std::span<const u32> left_values = pipeline.DescriptorValues();
            light_verify.values.assign(left_values.begin(), left_values.end());
            light_verify.pipeline = &pipeline;
            light_verify.active = true;
            return false;
        }
        query_cache.NotifySegment(true);
        HandleTransformFeedback();
        query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                                  maxwell3d->regs.zpass_pixel_count_enable);
        if (texture_cache.Nx2InPassFeedback()) [[unlikely]] {
            // EDEN_NX2_RX_RT_FIT=3: the anchor's feedback loop is this draw's (same pipeline,
            // images and render targets).
            RecordInPassFeedbackBarrier(scheduler, true);
        }
        {
            Profile::Scope draw_scope{Profile::Zone::DrawRecord};
            draw_func();
        }
        Profile::Add(Profile::Counter::LightUniformDraws, 1);
        if (indirect) {
            Profile::Add(Profile::Counter::LightUniformIndirectDraws, 1);
        }
        if (rebind_storage) {
            Profile::Add(Profile::Counter::LightStorageDraws, 1);
        }
        return true;
    }
    Profile::Scope scope{Profile::Zone::LightDraw};
    std::scoped_lock lock{buffer_cache.mutex};
    // What ConfigureImpl does for these buffers (UpdateGraphicsBuffers, BindHostGeometryBuffers).
    if (!buffer_cache.UpdateGeometryBuffers(is_indexed)) {
        Profile::Add(Profile::Counter::LightFellBack, 1);
        return false;
    }
    // EDEN_NX2_LIGHT_SYNC_SKIP: plain here (checked above): the anchor chain synchronized every
    // enabled stream after the last memory change; only streams flagged dirty bind anew (a clean
    // stream whose range may hold CPU-modified or untracked pages since is synchronized anyway).
    buffer_cache.plain_light_draw = VideoCommon::Nx2LightSyncSkip() != 0;
    buffer_cache.BindHostGeometryBuffers(is_indexed);
    buffer_cache.plain_light_draw = false;
    // An upload needs ConfigureImpl's barrier after it and ends the render pass.
    if (buffer_cache.any_buffer_uploaded || !scheduler.IsRenderPassActive()) {
        Profile::Add(Profile::Counter::LightFellBack, 1);
        return false;
    }
    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              maxwell3d->regs.zpass_pixel_count_enable);
    if (texture_cache.Nx2InPassFeedback()) [[unlikely]] {
        // EDEN_NX2_RX_RT_FIT=3: as the uniform way above.
        RecordInPassFeedbackBarrier(scheduler, true);
    }
    {
        Profile::Scope draw_scope{Profile::Zone::DrawRecord};
        draw_func();
    }
    if (Profile::Enabled()) {
        buffer_cache.CommitVertexStreamNote();
    }
    Profile::Add(Profile::Counter::LightDraws, 1);
    if (indirect) {
        Profile::Add(Profile::Counter::LightIndirectDraws, 1);
    }
    return true;
}

namespace {
// EDEN_NX2_WINDOW_TRACE: describe clears and draws into screen-sized colour targets (debug aid).
void TraceWindowTarget(const Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& memory,
                       const char* kind, u32 count) {
    static const bool trace = std::getenv("EDEN_NX2_WINDOW_TRACE") != nullptr;
    thread_local u32 traced{};
    const auto& regs = maxwell3d.regs;
    static const u64 skip = [] {
        const char* value = std::getenv("EDEN_NX2_WINDOW_TRACE_SKIP");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    thread_local u64 seen_events{};
    static const u64 only_ps = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_PS");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    const u64 pixel_shader =
        (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
    if (!trace || regs.rt[0].width != 3840 || traced >= 400 || seen_events++ < skip ||
        (only_ps != 0 && pixel_shader != only_ps)) {
        return;
    }
    ++traced;
    const auto& vp = regs.viewport_transform[0];
    const auto& sc = regs.scissor_test[0];
    LOG_INFO(HW_GPU,
             "nx2 window {} rt0={:#x} count={} clear={:#x} color=({},{},{},{}) mask_common={} "
             "mask0={:#x} rt_count={} vp scale=({},{}) translate=({},{}) scissor en={} x={}..{} "
             "y={}..{} blend0={} vs={:#x} ps={:#x} rasterize={}",
             kind, regs.rt[0].Address(), count, regs.clear_surface.raw, regs.clear_color[0],
             regs.clear_color[1], regs.clear_color[2], regs.clear_color[3], regs.color_mask_common,
             regs.color_mask[0].raw, regs.rt_control.count.Value(), vp.scale_x, vp.scale_y,
             vp.translate_x, vp.translate_y, sc.enable, sc.min_x.Value(), sc.max_x.Value(),
             sc.min_y.Value(), sc.max_y.Value(), regs.blend.enable[0],
             (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1],
             (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1],
             regs.rasterize_enable);
    if (kind[0] != 'd') {
        return;
    }
    // Pixel-stage constant buffer 3: colour constants of the loading-screen passes.
    if (const auto& cb3 = maxwell3d.state.shader_stages[4].const_buffers[3]; cb3.enabled) {
        std::array<f32, 8> values{};
        memory.ReadBlockUnsafe(cb3.address, values.data(), sizeof(values));
        LOG_INFO(HW_GPU, "nx2 window ps c[3]@{:#x} size={}: {}", cb3.address, cb3.size,
                 fmt::join(values, " "));
    }
    thread_local u32 texture_traces{};
    if (texture_traces++ > 200) {
        return;
    }
    // Texture used by the pixel shader's first bound TEX (handle in c[2][0x20]) and its TIC.
    const auto& cb = maxwell3d.state.shader_stages[4].const_buffers[2];
    const u32 handle = cb.enabled ? memory.Read<u32>(cb.address + 0x20) : 0;
    const u32 tic_index = handle & 0xfffff;
    const GPUVAddr tic_address = regs.tex_header.Address() + u64{tic_index} * 32;
    std::array<u32, 8> tic{};
    memory.ReadBlockUnsafe(tic_address, tic.data(), sizeof(tic));
    LOG_INFO(HW_GPU,
             "nx2 window texture: cb2={:#x} en={} handle={:#x} tic pool={:#x} limit={} "
             "sampler pool={:#x} tic[{}]={:08x}",
             cb.address, cb.enabled, handle, regs.tex_header.Address(), regs.tex_header.limit,
             regs.tex_sampler.Address(), tic_index, fmt::join(tic, " "));
    // First bytes of the texture's memory (block-linear, so just a sample).
    const GPUVAddr texture = (GPUVAddr{tic[2] & 0xffff} << 32) | tic[1];
    std::array<u32, 16> texels{};
    memory.ReadBlockUnsafe(texture, texels.data(), sizeof(texels));
    LOG_INFO(HW_GPU, "nx2 window texture {:#x} {}x{}: {:08x}", texture, (tic[4] & 0xffff) + 1,
             (tic[5] & 0xffff) + 1, fmt::join(texels, " "));
    // EDEN_NX2_TEXTURE_DUMP=<file>: save the first 4 MiB of the last traced texture's memory.
    if (const char* path = std::getenv("EDEN_NX2_TEXTURE_DUMP"); path != nullptr) {
        std::vector<u8> bytes(2048u * 10240u);
        memory.ReadBlockUnsafe(texture, bytes.data(), bytes.size());
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }
}
} // Anonymous namespace

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    if (Common::Nx2Reactive::StatsEnabled()) [[unlikely]] {
        Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::Draws, 1);
    }
    if (SplitOnB()) [[unlikely]] {
        DrawB(is_indexed, instance_count);
        return;
    }
    if (split_draws) [[unlikely]] {
        ForceGeometryForJoin();
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    {
        // Light draws are plain: one instance, no instancing loop, no inline indices.
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        BeginLightDraw(false, instance_count == 1 &&
                                  draw_state.draw_mode ==
                                      Tegra::Engines::Maxwell3D::DrawManager::DrawMode::General &&
                                  draw_state.inline_index_draw_indexes.empty() &&
                                  LightDrawShape(draw_state, is_indexed));
    }
    TraceWindowTarget(*maxwell3d, *gpu_memory, is_indexed ? "draw-indexed" : "draw",
                      is_indexed ? maxwell3d->draw_manager.draw_state.index_buffer.count
                                 : maxwell3d->draw_manager.draw_state.vertex_buffer.count);
    // EDEN_NX2_CB24_TRACE: for draws whose vertex shader reads bank 24, show the inline
    // constant buffer upload target (CB_ADDRESS) and its first words.
    if (static const bool trace = std::getenv("EDEN_NX2_CB24_TRACE") != nullptr; trace) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        thread_local u32 logged{};
        if ((vs == 0x11471c9a00 || vs == 0x11471ca600 || vs == 0x11471cd300) && logged++ < 3) {
            std::array<u32, 24> words{};
            gpu_memory->ReadBlockUnsafe(regs.const_buffer.Address(), words.data(), sizeof(words));
            LOG_INFO(HW_GPU, "bank24 draw vs={:#x}: CB_ADDRESS {:#x} size {:#x} offset {:#x}: {:08x}", vs,
                     regs.const_buffer.Address(), regs.const_buffer.size, regs.const_buffer.offset,
                     fmt::join(words, " "));
            // Search every bound vertex-stage buffer for render-target-size values.
            const auto& stage = maxwell3d->state.shader_stages[0];
            for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& buffer = stage.const_buffers[slot];
                if (!buffer.enabled || buffer.size == 0) continue;
                std::vector<u32> data(std::min<u32>(buffer.size, 0x10000) / 4);
                gpu_memory->ReadBlockUnsafe(buffer.address, data.data(), data.size() * 4);
                for (size_t i = 0; i < data.size(); ++i) {
                    const u32 w = data[i];
                    if (w == 0x45200000 || w == 0x44b40000 || w == 0x39cccccd || w == 0x3a360b61 ||
                        w == 0x44a00000 || w == 0x44340000 || w == 2559 || w == 1439) {
                        LOG_INFO(HW_GPU, "  slot {} +{:#x} = {:#x}", slot, i * 4, w);
                    }
                }
            }
        }
    }
    // EDEN_NX2_TRACE_VS=<address>: describe the next few draws using that vertex shader like the
    // after-upscale trace does (render target, blend, fragment textures).
    static const u64 trace_vs = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_VS");
        return value ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    if (trace_vs != 0) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        thread_local u32 traced{};
        if (vs == trace_vs && traced++ % 200 == 0 && traced < 1000) {
            Tegra::Engines::g_nx2_trace_draws_after_upscale = 1;
        }
    }
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        --Tegra::Engines::g_nx2_trace_draws_after_upscale;
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        LOG_INFO(HW_GPU, "after upscale: draw rt0 {:#x} {}x{} fmt {:#x} block {}/{}/{} pitch_linear {} depth {} array_pitch {:#x} ctrl {:#x} vs {:#x} ps {:#x}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.rt[0].tile_mode.block_width.Value(),
                 regs.rt[0].tile_mode.block_height.Value(), regs.rt[0].tile_mode.block_depth.Value(),
                 regs.rt[0].tile_mode.is_pitch_linear.Value(), regs.rt[0].depth.Value(),
                 regs.rt[0].array_pitch, regs.rt_control.count.Value(), vs, ps);
        // Fragment-stage texture handles (bound buffer 2) and the images they name.
        const auto& handles = maxwell3d->state.shader_stages[4].const_buffers[2];
        if (handles.enabled) {
            for (u32 i = 0; i < 24; ++i) {
                const u32 handle = gpu_memory->Read<u32>(handles.address + i * 4);
                if (handle == 0 || handle == 0xffffffff) continue;
                Tegra::Texture::TICEntry tic{};
                gpu_memory->ReadBlockUnsafe(regs.tex_header.Address() + u64{handle & 0xfffff} * sizeof(tic),
                                            &tic, sizeof(tic));
                LOG_INFO(HW_GPU, "  fs handle[{}] = {:#x}: {:#x} {}x{} format {:#x}", i, handle,
                         tic.Address(), tic.Width(), tic.Height(), static_cast<u32>(tic.format.Value()));
            }
        }
        const auto& bt = regs.blend_per_target[0];
        LOG_INFO(HW_GPU, "  blend enable {} per-target {} global op {} src {:#x} dst {:#x} | rt0 op {} src {:#x} dst {:#x} alpha {:#x}/{:#x} mask {:#x}",
                 regs.blend.enable[0], regs.blend_per_target_enabled,
                 static_cast<u32>(regs.blend.color_op), static_cast<u32>(regs.blend.color_source),
                 static_cast<u32>(regs.blend.color_dest), static_cast<u32>(bt.color_op),
                 static_cast<u32>(bt.color_source), static_cast<u32>(bt.color_dest),
                 static_cast<u32>(bt.alpha_source), static_cast<u32>(bt.alpha_dest),
                 regs.color_mask[0].raw);
    }
    // EDEN_NX2_SKIP_VS=<address>[,<address>...]: drop draws using those vertex shaders (debug aid).
    static const std::vector<u64> skip_list = [] {
        std::vector<u64> list;
        if (const char* value = std::getenv("EDEN_NX2_SKIP_VS")) {
            for (char* end = nullptr; *value; value = *end ? end + 1 : end) {
                list.push_back(std::strtoull(value, &end, 16));
                if (end == value) break;
            }
        }
        return list;
    }();
    if (!skip_list.empty()) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        if (std::find(skip_list.begin(), skip_list.end(), vs) != skip_list.end()) return;
    }
    // EDEN_NX2_RT_TRACE: log the first draw to every distinct colour target (debug aid).
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        thread_local std::set<std::tuple<GPUVAddr, u32, u32, u64, u64>> seen;
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        const auto key = std::make_tuple(rt.Address(), rt.width, static_cast<u32>(rt.format), vs, ps);
        if (rt.width >= 640 && seen.size() < 600 && seen.insert(key).second) {
            LOG_INFO(HW_GPU, "nx2 draw target {:#x} {}x{} format={:#x} vs={:#x} ps={:#x} blend={} (pairs so far {})",
                     rt.Address(), rt.width, rt.height, static_cast<u32>(rt.format), vs, ps,
                     regs.blend.enable[0], seen.size());
        }
    }
    PrepareDraw(is_indexed, false, [this, is_indexed, instance_count] {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const u32 num_instances{instance_count};
        const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};

        scheduler.Record([draw_params](vk::CommandBuffer cmdbuf) {
            if (draw_params.is_indexed) {
                cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                   draw_params.first_index, draw_params.base_vertex,
                                   draw_params.base_instance);
            } else {
                cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                            draw_params.base_vertex, draw_params.base_instance);
            }
        });

        // Log draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string params = is_indexed ?
                fmt::format("vertices={}, instances={}, firstIndex={}, baseVertex={}, baseInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.first_index, draw_params.base_vertex, draw_params.base_instance) :
                fmt::format("vertices={}, instances={}, firstVertex={}, firstInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.base_vertex, draw_params.base_instance);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                is_indexed ? "vkCmdDrawIndexed" : "vkCmdDraw", params, VK_SUCCESS);
        }
    });
}

namespace {
// EDEN_NX2_VB_CHECK (test build, =0 turns it off): an indexed indirect draw whose record names a
// vertex past the end of a bound vertex stream. Reads guest memory only and changes nothing.
void CheckIndirectVertexBounds(Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& memory) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_VB_CHECK");
        return value == nullptr || value[0] != '0';
    }();
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    const auto& params = maxwell3d.draw_manager.indirect_state;
    if (!enabled || !maxwell3d.GetNativeMacroEngine().native_turing || !params.is_indexed ||
        params.include_count || params.is_byte_count) {
        return;
    }
    static u64 draws{};
    static u64 violations{};
    static std::array<std::pair<GPUVAddr, u64>, Regs::NumVertexArrays> previous{};
    ++draws;
    const auto& regs = maxwell3d.regs;
    std::array<u32, 5> record{};
    if (!memory.IsFullyMappedRange(params.indirect_start_address, sizeof(record))) {
        return;
    }
    memory.ReadBlockUnsafe(params.indirect_start_address, record.data(), sizeof(record));
    const u32 count = record[0];
    const u32 first = record[2];
    const s32 base_vertex = static_cast<s32>(record[3]);
    const size_t index_size = regs.index_buffer.FormatSizeInBytes();
    const GPUVAddr index_address = regs.index_buffer.StartAddress() + u64{first} * index_size;
    s64 max_index = -1;
    if (count != 0 && count <= (1U << 16) &&
        memory.IsFullyMappedRange(index_address, count * index_size)) {
        std::vector<u8> indices(count * index_size);
        memory.ReadBlockUnsafe(index_address, indices.data(), indices.size());
        for (u32 i = 0; i < count; ++i) {
            u32 value = 0;
            std::memcpy(&value, indices.data() + i * index_size, index_size);
            max_index = (std::max)(max_index, s64{value});
        }
    }
    for (u32 stream = 0; stream < Regs::NumVertexArrays; ++stream) {
        const auto& array = regs.vertex_streams[stream];
        const GPUVAddr address = array.Address();
        const u64 size = regs.NativeVertexStreamSize(stream);
        const auto [previous_address, previous_size] = previous[stream];
        previous[stream] = {address, size};
        const u32 stride = array.stride.Value();
        if (max_index < 0 || array.enable == 0 || stride == 0 || size == 0 ||
            regs.vertex_stream_instances.IsInstancingEnabled(stream)) {
            continue;
        }
        const s64 last_vertex = max_index + base_vertex;
        const s64 held = static_cast<s64>(size / stride);
        if (last_vertex < held) {
            continue;
        }
        ++violations;
        if (violations > 48 && (violations & (violations - 1)) != 0) {
            continue;
        }
        const auto vertex = [&](s64 number) {
            std::array<f32, 3> words{};
            const GPUVAddr at = address + static_cast<u64>(number) * stride;
            if (number < 0 || !memory.IsFullyMappedRange(at, sizeof(words))) {
                return std::string{"unmapped"};
            }
            memory.ReadBlockUnsafe(at, words.data(), sizeof(words));
            return fmt::format("{:.2f},{:.2f},{:.2f}", words[0], words[1], words[2]);
        };
        LOG_WARNING(HW_GPU,
                    "VB_CHECK #{} (draw {}): stream {} at {:#x} size {:#x} stride {} holds {} "
                    "vertices, the record needs vertex {} (count {} first {} base {}, index "
                    "buffer {:#x} {} bytes each); limit register {:#x}; the stream before was "
                    "{:#x} size {:#x}; vertex {} = [{}], vertex {} = [{}], vertex {} = [{}]",
                    violations, draws, stream, address, size, stride, held, last_vertex, count,
                    first, base_vertex, regs.index_buffer.StartAddress(), index_size,
                    regs.vertex_stream_limits[stream].Address(), previous_address, previous_size,
                    held - 1, vertex(held - 1), held, vertex(held), last_vertex,
                    vertex(last_vertex));
    }
    if ((draws & (draws - 1)) == 0 && draws >= 1024) {
        LOG_INFO(HW_GPU, "VB_CHECK: {} indexed indirect draws checked, {} read past a stream",
                 draws, violations);
    }
}
} // Anonymous namespace

void RasterizerVulkan::DrawIndirect() {
    if (Common::Nx2Reactive::StatsEnabled()) [[unlikely]] {
        Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::Draws, 1);
    }
    if (SplitOnB()) [[unlikely]] {
        DrawIndirectB();
        return;
    }
    if (split_draws) [[unlikely]] {
        ForceGeometryForJoin();
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    const auto& params = maxwell3d->draw_manager.indirect_state;
    CheckIndirectVertexBounds(*maxwell3d, *gpu_memory);
    // Light indirect draws take indexed records from memory (no count buffer, no byte count).
    BeginLightDraw(true, params.is_indexed && !params.include_count && !params.is_byte_count &&
                             LightDrawShape(maxwell3d->draw_manager.draw_state, true));
    // The arguments may be what a dispatch or draw just wrote (Donkey Kong Bananza's terrain
    // records, Maxwell3D::BackDrawIndexedIndirect): the indirect read must wait for it.
    if (buffer_cache.TakeIndirectBarrier()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::IndirectBarriers, 1);
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            static constexpr VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, barrier);
        });
    }
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, true, [this, &params] {
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            scheduler.Record([draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([buffer_obj = buffer->Handle(), offset, params](vk::CommandBuffer cmdbuf) {
            if (params.is_indexed) {
                cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                           static_cast<u32>(params.max_draw_counts),
                                           static_cast<u32>(params.stride));
            } else {
                cmdbuf.DrawIndirect(buffer_obj, offset, static_cast<u32>(params.max_draw_counts),
                                    static_cast<u32>(params.stride));
            }
        });

        // Log indirect draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string log_params = fmt::format("drawCount={}, stride={}",
                params.max_draw_counts, params.stride);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                params.is_indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
                log_params, VK_SUCCESS);
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerVulkan::DrawTexture() {
    if (SplitOnB()) [[unlikely]] {
        DrawTextureB();
        return;
    }
    NoteLightWork();

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates();

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager.draw_texture_state;
    const auto& sampler = texture_cache.GetSampler(draw_texture_state.src_sampler, false);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
}

void RasterizerVulkan::Clear(u32 layer_count) {
    if (SplitOnB()) [[unlikely]] {
        ClearB(layer_count);
        return;
    }
    NoteLightWork();
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Clear};
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::Clears, 1);
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        const auto& regs = maxwell3d->regs;
        LOG_INFO(HW_GPU, "after upscale: clear rt0 {:#x} {}x{} fmt {:#x} surface {:#x} colour {} {} {} {}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.clear_surface.raw, regs.clear_color[0],
                 regs.clear_color[1], regs.clear_color[2], regs.clear_color[3]);
    }
    TraceWindowTarget(*maxwell3d, *gpu_memory, "clear", layer_count);
    FlushWork();
    gpu_memory->FlushCaching();
    ClearRecord(layer_count);
}

void RasterizerVulkan::ClearRecord(u32 layer_count) {
    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }

    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    const VkExtent2D render_area = framebuffer->RenderArea();

    constexpr bool ENABLE_DEFERRED_CLEAR = true;
    const bool color_full_channels = regs.clear_surface.R && regs.clear_surface.G &&
                                     regs.clear_surface.B && regs.clear_surface.A;
    const bool stencil_partial = use_stencil && framebuffer->HasAspectStencilBit() &&
                                 regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0;
    const bool ds_used = use_depth || use_stencil;
    const bool ds_deferrable =
        !ds_used || ((!framebuffer->HasAspectDepthBit() || use_depth) &&
                     (!framebuffer->HasAspectStencilBit() || use_stencil) && !stencil_partial);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    VkRect2D default_scissor{};
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = (std::numeric_limits<s32>::max)();
    default_scissor.extent.height = (std::numeric_limits<s32>::max)();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = regs.clear_surface.layer,
        .layerCount = layer_count,
    };
    const auto clamp_rect_to_render_area = [render_area](VkRect2D& rect) -> bool {
        const auto clamp_axis = [](s32& offset, u32& extent, u32 limit) {
            auto clamp_offset = [&offset, limit]() {
                if (limit == 0) {
                    offset = 0;
                    return;
                }
                offset = std::clamp(offset, 0, static_cast<s32>(limit));
            };

            if (extent == 0) {
                clamp_offset();
                return;
            }
            if (offset < 0) {
                const u32 shrink = (std::min)(extent, static_cast<u32>(-offset));
                extent -= shrink;
                offset = 0;
            }
            if (limit == 0) {
                extent = 0;
                offset = 0;
                return;
            }
            if (offset >= s32(limit)) {
                offset = s32(limit);
                extent = 0;
                return;
            }
            const u64 end_coord = u64(offset) + extent;
            if (end_coord > limit) {
                extent = limit - u32(offset);
            }
        };

        clamp_axis(rect.offset.x, rect.extent.width, render_area.width);
        clamp_axis(rect.offset.y, rect.extent.height, render_area.height);
        return rect.extent.width != 0 && rect.extent.height != 0;
    };
    if (!clamp_rect_to_render_area(clear_rect.rect)) {
        return;
    }

    const bool clear_covers_render_area =
        clear_rect.rect.offset.x == 0 && clear_rect.rect.offset.y == 0 &&
        clear_rect.rect.extent.width >= render_area.width &&
        clear_rect.rect.extent.height >= render_area.height;
    const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && (!regs.clear_control.use_scissor || clear_covers_render_area) &&
                                 regs.clear_surface.layer == 0 &&
                                 !scheduler.IsRenderPassActive() &&
                                 (!use_color || color_full_channels) && ds_deferrable;
    if (!can_defer_clear) {
        scheduler.RequestRenderpass(framebuffer);
    }

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    UpdateViewportsState(regs);

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (!is_integer) {
            std::memcpy(clear_value.color.float32, regs.clear_color.data(), regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.uint32[i] = u32(f32(u64(int_size) << 1U) * regs.clear_color[i]);
        } else {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.int32[i] = s32(f32(s64(int_size - 1) << 1) * (regs.clear_color[i] - 0.5f));
        }

        if (color_full_channels) {
            if (can_defer_clear) {
                scheduler.DeferColorClear(framebuffer, color_attachment, clear_value);
            } else {
                scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                });
            }
        } else {
            u8 color_mask = u8(regs.clear_surface.R | regs.clear_surface.G << 1 | regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
            blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, use_depth, regs.clear_depth,
                                     u8(regs.stencil_front_mask), regs.clear_stencil,
                                     regs.stencil_front_func_mask, dst_region);
    } else if (can_defer_clear) {
        VkClearValue ds_value{};
        ds_value.depthStencil.depth = regs.clear_depth;
        ds_value.depthStencil.stencil = regs.clear_stencil;
        scheduler.DeferDepthStencilClear(framebuffer, ds_value);
    } else {
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

void RasterizerVulkan::DispatchCompute() {
    NoteLightWork();
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Compute};
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::ComputeDispatches, 1);
    FlushWork();
    gpu_memory->FlushCaching();

    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};
    if (!pipeline) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute pipeline unavailable");
        return;
    }
    CheckBufferLockOffB("RasterizerVulkan::DispatchCompute");
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    if (!pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache,
                             texture_cache)) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute resources unavailable");
        return;
    }

    const auto& qmd{kepler_compute->launch_description};
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    if (indirect_address) {
        // DispatchIndirect
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        const auto post_op = VideoCommon::ObtainBufferOperation::DiscardWrite;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),
                          indirect_offset = offset](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
                return;
            }
            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
        });
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    if (dim[0] > max_dim[0] || dim[1] > max_dim[1] || dim[2] > max_dim[2]) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
    };
    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, READ_BARRIER); });
    scheduler.Record([pipeline, dim](vk::CommandBuffer cmdbuf) {
        if (!pipeline->IsBound()) {
            return;
        }
        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
    });

    // Log compute dispatch
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = fmt::format("groupCountX={}, groupCountY={}, groupCountZ={}",
            dim[0], dim[1], dim[2]);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            "vkCmdDispatch", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    if (SplitOnB()) [[unlikely]] {
        // The query cache is C's (F3: the next WaitForIdle joins).
        NoteLightWork();
        stage_query_since_wfi = true;
        StageToC([this, type] { ResetCounter(type); });
        return;
    }
    NoteLightWork();
    if (split_draws) [[unlikely]] {
        NoteQueryForWfi();
    }
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
    case VideoCommon::QueryType::StreamingByteCount:
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        query_cache.CounterReset(type);
        return;
    default:
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    if (SplitOnB()) [[unlikely]] {
        // The query cache is C's (F3: the next WaitForIdle joins).
        NoteLightWork();
        stage_query_since_wfi = true;
        if (!VideoCommon::Nx2Stages::QueryGoesToC(type, flags)) {
            // A report a fence carries (T7c review finding 1): the old body in a join, C parked;
            // its fence takes the buffer cache mutex, which a record C runs never may.
            const auto run = [&] { Query(gpu_addr, type, flags, payload, subreport); };
            if (Nx2Record::HeldLocks::Count() != 0 ||
                buffer_cache_runtime.Target().list != nullptr) [[unlikely]] {
                // Not seen: reports come from records of ring 2 (the proxy, the replicas), no
                // packet open, no cache lock. Inside a packet the mutex is held: K3 (a)'s join.
                VideoCommon::Nx2Stages::ReportStageHazard("fence query inside B's packet");
                JoinBLocked(run);
                return;
            }
            JoinB(run);
            return;
        }
        if (const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr)) {
            // CounterReport's mark, made here before the query leaves B: the page summary B asks
            // (Nx2GpuPages::Holds) covers the page from this point of the stream on.
            Tegra::Nx2MarkUntrackedWrite(*cpu_addr, 16);
            if (VideoCommon::Nx2Stages::QueryWritesAtOnce(type)) {
                // Its value (and timestamp) there, before the next packet.
                stage_query_writes.Note(*cpu_addr, *cpu_addr + 16, NextStageSeq(),
                                        VideoCommon::Nx2Stages::GpuPages().RanSeq());
            } else {
                // A counter: written from a fence after it, whenever that passes.
                stage_query_later.Note(*cpu_addr, *cpu_addr + 16, NextStageSeq(), 0);
            }
        }
        StageToC([this, gpu_addr, type, flags, payload, subreport] {
            Query(gpu_addr, type, flags, payload, subreport);
        });
        return;
    }
    NoteLightWork();
    if (split_draws) [[unlikely]] {
        NoteQueryForWfi();
    }
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

std::optional<u32> RasterizerVulkan::PendingSemaphoreRelease(DAddr addr) {
    return query_cache.PendingRelease(addr);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (SplitOnB()) [[unlikely]] {
        FlushRegionB(addr, size, which);
        return;
    }
    NoteLightWork();
    if (addr == 0 || size == 0) {
        return;
    }
    if (split_draws && True(which & VideoCommon::CacheType::BufferCache) &&
        !VideoCommon::Nx2Stages::MayTakeBufferLock()) [[unlikely]] {
        // T7d review (k2-c-flushregion-bufferlock): C in a record (the texture cache's accelerated
        // uploads read guest memory with ReadBlock, which flushes here) never waits for the
        // buffer cache mutex: B may wait for C holding it (JoinFromBLocked, K3). Nor does it
        // download from B's buffer cache, which is ahead of C's point: that could clear what B
        // marked GPU modified for packets C has not run. GPU-written buffer data there is
        // reported (STAGE_CHECK), not flushed.
        if (BufferGpuModifiedNoWait(addr, size).value_or(false)) {
            VideoCommon::Nx2Stages::ReportStageHazard("buffer flush on C in a record (skipped)");
        }
        which = VideoCommon::Nx2Stages::WithoutBufferCache(which);
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        CheckBufferLockOffB("RasterizerVulkan::FlushRegion");
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (SplitOnB()) [[unlikely]] {
        return MustFlushRegionB(addr, size, which);
    }
    if (split_draws && True(which & VideoCommon::CacheType::BufferCache) &&
        !VideoCommon::Nx2Stages::MayTakeBufferLock()) [[unlikely]] {
        // As FlushRegion (T7d review): no wait for the buffer cache mutex; B holding it, so.
        if (BufferGpuModifiedNoWait(addr, size).value_or(true)) {
            return true;
        }
        which = VideoCommon::Nx2Stages::WithoutBufferCache(which);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        CheckBufferLockOffB("RasterizerVulkan::MustFlushRegion");
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

std::optional<bool> RasterizerVulkan::BufferGpuModifiedNoWait(DAddr addr, u64 size) {
    std::unique_lock lock{buffer_cache.mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return std::nullopt;
    }
    return buffer_cache.IsRegionGpuModified(addr, size);
}

bool RasterizerVulkan::HoldsGpuWrittenData(DAddr addr, u64 size) {
    // An invalidation marks whole pages CPU-modified (and uploads them whole): look at the
    // pages the area touches.
    const DAddr begin = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
    const size_t span = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE) - begin;
    if (SplitOnB()) [[unlikely]] {
        return HoldsGpuWrittenDataB(begin, span);
    }
    // A query result cached there: the invalidation marks it so its value is never written
    // (QueryCacheBase::InvalidateRegion), which a skipped write (EDEN_NX2_SAME_SKIP) would not.
    namespace Profile = VideoCommon::Nx2Profile;
    if (query_cache.HoldsQueries(begin, span)) {
        Profile::Add(Profile::Counter::HoldsGpuWrittenQuery, 1);
        return true;
    }
    {
        CheckBufferLockOffB("RasterizerVulkan::HoldsGpuWrittenData");
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(begin, span)) {
            Profile::Add(Profile::Counter::HoldsGpuWrittenBuffer, 1);
            return true;
        }
    }
    std::scoped_lock lock{texture_cache.mutex};
    if (texture_cache.IsRegionGpuModified(begin, span)) {
        Profile::Add(Profile::Counter::HoldsGpuWrittenTexture, 1);
        return true;
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    // EDEN_NX2_REACTIVE_STATS: calls and the two cache mutexes' waits
    const bool stats = Common::Nx2Reactive::StatsEnabled();
    if (stats) [[unlikely]] {
        Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FlushAreaCalls, 1);
    }
    const DAddr page_start = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
    const DAddr page_end = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE);
    // EDEN_NX2_FLUSH_FIRST_READ: newly noted pages the GPU wrote are flushed once (nx2_reactive_flush.h)
    bool flush_buffer = false;
    {
        // Guest CPU reads of buffer memory decide which GPU-written buffer data is copied back.
        CheckBufferLockOffB("RasterizerVulkan::GetFlushArea");
        const s64 lock_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
        std::scoped_lock lock{buffer_cache.mutex};
        if (stats) [[unlikely]] {
            Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::BufferLockUs,
                                     Common::Nx2Reactive::MicrosecondsSince(lock_ns));
        }
        const bool newly_noted = buffer_cache.NoteCpuRead(page_start, page_end - page_start);
        if (VideoCommon::Nx2Reactive::FlushFirstRead()) [[unlikely]] {
            flush_buffer = VideoCommon::Nx2Reactive::FirstReadFlushes(
                true, newly_noted,
                newly_noted && buffer_cache.IsRegionGpuModified(page_start, page_end - page_start));
            if (flush_buffer && stats) {
                Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FirstReadFlushes, 1);
            }
        } else if (buffer_cache.ReadTrapsActive()) {
            // A trapped read (or a slow-path read) of GPU-written buffer data: flush it now.
            flush_buffer = buffer_cache.IsRegionGpuModified(page_start, page_end - page_start);
        }
    }
    {
        const s64 lock_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
        std::scoped_lock lock{texture_cache.mutex};
        if (stats) [[unlikely]] {
            Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::TextureLockUs,
                                     Common::Nx2Reactive::MicrosecondsSince(lock_ns));
        }
        const auto area = texture_cache.GetFlushArea(addr, size);
        return VideoCommon::Nx2Reactive::CombineFlushArea(area, page_start, page_end, flush_buffer);
    }
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (SplitOnB()) [[unlikely]] {
        InvalidateRegionB(addr, size, which);
        return;
    }
    NoteLightWork();
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        CheckBufferLockOffB("RasterizerVulkan::InvalidateRegion");
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    if (SplitOnB()) [[unlikely]] {
        InnerInvalidationB(sequences);
        return;
    }
    namespace Profile = VideoCommon::Nx2Profile;
    // Stage diagnostics: the ranges are the cached writes since the last FlushCaching, merged:
    // constant buffer data, and inline writes left for here (Diagnostics::t_deferred_inline).
    const bool diagnose = Profile::Enabled();
    const auto note_range = [&](DAddr addr, std::size_t size, bool on_image) {
        const bool inline_write =
            std::ranges::any_of(Diagnostics::t_deferred_inline, [&](const auto& write) {
                return addr < write.first + write.second && write.first < addr + size;
            });
        if (inline_write) {
            // Their pools were looked at as they were written (Diagnostics::NoteInlineWrite).
            if (on_image) {
                Profile::Add(Profile::Counter::InlineOnImages, 1);
            }
            return;
        }
        if (on_image) {
            Profile::Add(Profile::Counter::CbDataOnImages, 1);
        }
        if (Diagnostics::OnTexturePools(maxwell3d, gpu_memory, addr, size)) {
            Profile::Add(Profile::Counter::CbDataOnPools, 1);
        }
    };
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            const u64 image_hits = diagnose ? texture_cache.WriteHits() : 0;
            texture_cache.WriteMemory(addr, size);
            if (diagnose) {
                note_range(addr, size, texture_cache.WriteHits() != image_hits);
            }
        }
    }
    if (diagnose) {
        Diagnostics::t_deferred_inline.clear();
    }
    {
        CheckBufferLockOffB("RasterizerVulkan::InnerInvalidation");
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemoryRanges(sequences);
    }
    {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
            pipeline_cache.InvalidateRegion(addr, size);
        }
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    NoteLightWork();
    DEBUG_ASSERT(addr != 0 || size != 0);
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    if (SplitOnB()) [[unlikely]] {
        // The proxy's and the GPU thread's deferred invalidations: the texture half in ring 3's
        // order, as OnCachedWrite's (T9c).
        InvalidateRegionB(addr, size, VideoCommon::Nx2Stages::CpuWriteCaches);
        return;
    }
    NoteLightWork();
    if (addr == 0 || size == 0) {
        return;
    }

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::OnCachedWrite(DAddr addr, u64 size) {
    if (SplitOnB()) [[unlikely]] {
        // GPU::InvalidateGPUCache on B outside a join (a submission's, the puller's, the proxy's):
        // the texture half C's, the buffer half's copies in a packet of B's (T9c, R2c review: the
        // old body records into C's scheduler beside C).
        InvalidateRegionB(addr, size, VideoCommon::Nx2Stages::CpuWriteCaches, true);
        return;
    }
    NoteLightWork();
    if (addr == 0 || size == 0) {
        return;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        // Copies just these bytes in: marking the pages CPU-modified instead re-uploads them
        // whole from guest memory, over what the GPU wrote there and never copied back.
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.CachedWriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

bool RasterizerVulkan::TryCacheInvalidation(DAddr addr, u64 size) {
    NoteLightWork();
    if (addr == 0 || size == 0) return true;
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) return false;
        texture_cache.WriteMemory(addr, size);
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return true;
}

void RasterizerVulkan::InvalidateGPUCache() {
    NoteLightWork();
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    NoteLightWork();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    NoteLightWork();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func) {
    // The puller's deferred fences come here on B without the proxy (T9c, R2 F3): FenceJoinsB.
    if (SplitOnB() && FenceJoins(FenceCall::Signal, true)) [[unlikely]] {
        JoinB([&] { SignalFence(std::move(func)); });
        return;
    }
    NoteLightWork();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::FencesSignaled, 1);
    fence_manager.SignalFence(std::move(func));
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    if (SplitOnB() && FenceJoins(FenceCall::Sync, true)) [[unlikely]] {
        JoinB([&] { SyncOperation(std::move(func)); });
        return;
    }
    NoteLightWork();
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    if (SplitOnB() && FenceJoins(FenceCall::SyncPoint, true)) [[unlikely]] {
        JoinB([&] { SignalSyncPoint(value); });
        return;
    }
    NoteLightWork();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::FencesSignaled, 1);
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    if (SplitOnB() && FenceJoins(FenceCall::Reference, true)) [[unlikely]] {
        JoinB([&] { SignalReference(); });
        return;
    }
    NoteLightWork();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::FencesSignaled, 1);
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    if (SplitOnB()) [[unlikely]] {
        if (FenceJoins(FenceCall::Release, force)) {
            JoinB([&] { ReleaseFences(force); });
            return;
        }
        // OnCommandListEnd's at each command list: WaitPendingFences(false) returns at once.
        NoteLightWork();
        return;
    }
    NoteLightWork();
    fence_manager.WaitPendingFences(force);
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    if (SplitOnB()) [[unlikely]] {
        WaitForIdleB();
        return;
    }
    NoteLightWork();
    WaitForIdleRecord();
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::WaitForIdleRecord() {
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    if (Tegra::Nx2MembarEnabled()) [[unlikely]] {
        Nx2Barrier(flags, false);
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([event = *wfi_event, flags](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetEvent(event, flags);
        cmdbuf.WaitEvents(event, flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, {}, {});
    });
}

// EDEN_NX2_MEMBAR (video_core/nx2.h; 57ffa55a8, there on AMD's Windows driver only): the puller's
// MEMBAR, WaitForIdle's stages and query notice. On B with cut c split as WaitForIdle is.
void RasterizerVulkan::Nx2Membar() {
    if (SplitOnB()) [[unlikely]] {
        Nx2MembarB();
        return;
    }
    NoteLightWork();
    Nx2MembarRecord();
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::Nx2MembarRecord() {
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }
    query_cache.NotifyWFI();
    Nx2Barrier(flags, true);
}

// The event wait with `flags` as both source and destination and a memory barrier (writes made
// available to all reads and writes). Counted, a line every 10 s (GPU back thread, or the GPU
// thread without the split: one thread).
void RasterizerVulkan::Nx2Barrier(VkPipelineStageFlags flags, bool membar) {
    static bool logged = false;
    static u64 membars = 0;
    static u64 idles = 0;
    static auto since = std::chrono::steady_clock::now();
    if (!logged) {
        logged = true;
        LOG_INFO(Render_Vulkan, "nx2 membar (EDEN_NX2_MEMBAR): MEM_OP_D MEMBAR and WaitForIdle "
                                "record a memory barrier to all their stages");
    }
    ++(membar ? membars : idles);
    if (const auto now = std::chrono::steady_clock::now(); now - since >= std::chrono::seconds{10}) {
        LOG_INFO(Render_Vulkan, "nx2 membar last {:.1f} s: {} MEMBARs, {} WaitForIdle",
                 std::chrono::duration<double>(now - since).count(), membars, idles);
        membars = 0;
        idles = 0;
        since = now;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([event = *wfi_event, flags](vk::CommandBuffer cmdbuf) {
        static constexpr VkMemoryBarrier memory_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.SetEvent(event, flags);
        cmdbuf.WaitEvents(event, flags, flags, memory_barrier, {}, {});
    });
}

void RasterizerVulkan::FragmentBarrier() {
    if (SplitOnB()) [[unlikely]] {
        // C's (the scheduler).
        NoteLightWork();
        StageToC([this] { FragmentBarrier(); });
        return;
    }
    NoteLightWork();
    // We already put barriers when a render pass finishes
    scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerVulkan::TiledCacheBarrier() {
    // TODO: Implementing tiled barriers requires rewriting a good chunk of the Vulkan backend
}

void RasterizerVulkan::FlushCommands() {
    if (SplitOnB()) [[unlikely]] {
        // C's (its draw counter and the scheduler).
        NoteLightWork();
        StageToC([this] { FlushCommands(); });
        return;
    }
    NoteLightWork();
    if (draw_counter == 0) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}

void RasterizerVulkan::TickFrame() {
    if (split_draws) [[unlikely]] {
        if (SplitOnB()) {
            // Never split (F4 / F5): Composite's TickFrame runs on C inside a ParkB (GPU::TickWork)
            // or on B the record owner (the proxy's joins); B outside one joins.
            JoinB([this] { TickFrame(); });
            return;
        }
        // Both stages at one point of the stream: the buffer cache's half here too (its garbage
        // collector's downloads never inside B's packet). B's next draw is set up in full and
        // finds the targets' device ranges again; nothing repairs with C's anchor.
        light_epoch_b.fetch_add(1, std::memory_order_seq_cst);
        stage_targets.valid = false;
        ReleaseAnchorC();
    }
    NoteLightWork();
    draw_counter = 0;
    guest_descriptor_queue.TickFrame();
    compute_pass_descriptor_queue.TickFrame();
    descriptor_buffer_ring.TickFrame();
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        CheckBufferLockOffB("RasterizerVulkan::TickFrame");
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
    if (Common::Nx2Reactive::StatsEnabled()) [[unlikely]] {
        ReportReactiveStats();
    }
}

void RasterizerVulkan::ReportReactiveStats() {
    namespace Reactive = Common::Nx2Reactive;
    Reactive::Add(Reactive::Counter::Frames, 1);
    const s64 now_ns = Reactive::NowNs();
    if (!Reactive::TakeReportDue(Reactive::GetState(), now_ns, 0)) {
        return;
    }
    {
        CheckBufferLockOffB("RasterizerVulkan::ReportReactiveStats");
        std::scoped_lock lock{buffer_cache.mutex};
        Reactive::SetGauge(Reactive::Gauge::ReadPages, buffer_cache.CpuReadPages());
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        Reactive::SetGauge(Reactive::Gauge::LatchedImages, texture_cache.ReadLatchedImages());
        texture_cache.Nx2ReportTop();
    }
    const u64 download_bytes = staging_pool.CachedBytes(MemoryUsage::Download);
    Reactive::SetGauge(Reactive::Gauge::StagingDownloadBytes, download_bytes);
    Reactive::SetGauge(Reactive::Gauge::StagingBytes,
                       download_bytes + staging_pool.CachedBytes(MemoryUsage::Upload) +
                           staging_pool.CachedBytes(MemoryUsage::DeviceLocal));
    Reactive::Report(now_ns);
}

bool RasterizerVulkan::AccelerateConditionalRendering() {
    NoteLightWork();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::ConditionalRenderings, 1);
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}

bool RasterizerVulkan::HasDrawTransformFeedback() {
    return device.IsTransformFeedbackDrawSupported();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    NoteLightWork();
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    return accelerate_dma;
}

namespace {
// The opt-in traces of an inline write (AccelerateInlineToMemory).
void TraceInlineWrite(Tegra::MemoryManager& memory_manager, GPUVAddr address, DAddr cpu_addr,
                      size_t copy_size, std::span<const u8> memory) {
    if (!memory_manager.UsesNativeTiling()) {
        return;
    }
    u32 first_word{};
    if (memory.size() >= sizeof(first_word)) std::memcpy(&first_word, memory.data(), sizeof(first_word));
    VideoCommon::TraceNativeQuery(11, memory_manager.GetID(), address, cpu_addr, 0, first_word,
                                  static_cast<u32>(copy_size));
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && address < watch + 16 && address + copy_size > watch) {
        LOG_INFO(Render_Vulkan, "Semaphore watch: inline write {:#x} size {} first word {:#x}",
                 address, copy_size, first_word);
    }
}

// EDEN_NX2_INLINE_DEFER=0: invalidate every cache at once (for comparisons).
bool InlineDeferEnabled() {
    static const bool defer = [] {
        const char* value = std::getenv("EDEN_NX2_INLINE_DEFER");
        return value == nullptr || value[0] != '0';
    }();
    return defer;
}
} // Anonymous namespace

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    if (SplitOnB()) [[unlikely]] {
        InlineToMemoryB(address, copy_size, memory);
        return;
    }
    namespace Profile = VideoCommon::Nx2Profile;
    NoteLightWork();
    const bool diagnose = Profile::Enabled();
    if (diagnose) {
        Diagnostics::NoteInlineWrite(maxwell3d, address, copy_size);
    }
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    TraceInlineWrite(*gpu_memory, address, cpu_addr.value_or(0), copy_size, memory);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    // Every cache is told below (or at the next FlushCaching): not a write around them.
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size, true);
    const bool defer = InlineDeferEnabled();
    // One cache at a time: holding both while waiting for either (std::lock) waited longer, the
    // fencing thread and CPU write traps holding one of them most of the time.
    bool uploaded;
    {
        CheckBufferLockOffB("RasterizerVulkan::AccelerateInlineToMemory");
        std::scoped_lock lock{buffer_cache.mutex};
        uploaded = buffer_cache.InlineMemory(*cpu_addr, copy_size, memory);
        if (!uploaded && !defer) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    // Pending query results must not land over the new words later: drop them now.
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
    if (!uploaded && defer) {
        // Title 1 makes thousands of small inline writes between draws (~10% of the GPU
        // thread invalidating every cache for each). Guest memory is written; the buffer,
        // texture and shader caches see the merged ranges at the next FlushCaching, which runs
        // before anything of theirs is used. Writes into GPU-written buffer data were uploaded
        // in place above, as before.
        gpu_memory->AccumulateInvalidation(address, copy_size);
        if (diagnose) {
            Diagnostics::NoteDeferredInline(*cpu_addr, copy_size);
        }
        return;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        const u64 image_hits = diagnose ? texture_cache.WriteHits() : 0;
        texture_cache.WriteMemory(*cpu_addr, copy_size);
        if (diagnose && texture_cache.WriteHits() != image_hits) {
            Profile::Add(Profile::Counter::InlineOnImages, 1);
        }
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
}

void RasterizerVulkan::AccelerateInlineRuns(std::span<const std::pair<GPUVAddr, u32>> runs,
                                            std::span<const u8> data) {
    // EDEN_NX2_RESET_BATCH: AccelerateInlineToMemory for each run, the same steps in the same
    // order, except that the buffer cache leaves its uploads into GPU-written buffer data in a
    // batch (BufferCache::InlineMemoryBatched): consecutive ones into one buffer are recorded as
    // one copy, from one staging buffer, between two barriers. The batch is recorded before this
    // returns, so before anything that may read the buffer: the steps it waits through write
    // guest memory, keep the caches' books (query, texture and shader invalidations, the
    // deferred ranges) and record only uploads of guest memory over the same bytes; the buffer
    // cache stays locked throughout (no other thread's download or bind comes in between), and
    // a run outside device memory (the memory manager's slow path) goes after what is batched.
    // One light epoch step stands for every run's (only its change is compared).
    namespace Profile = VideoCommon::Nx2Profile;
    NoteLightWork();
    const bool defer = InlineDeferEnabled();
    CheckBufferLockOffB("RasterizerVulkan::AccelerateInlineRuns");
    std::scoped_lock buffer_lock{buffer_cache.mutex};
    BufferCache::InlineBatch batch;
    u64 uploads = 0;
    u64 commands = 0;
    // On any way out, what is batched is recorded too.
    SCOPE_EXIT {
        buffer_cache.FlushInlineBatch(batch);
    };
    for (const auto& [address, size] : runs) {
        const std::span<const u8> bytes = data.first(size);
        const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(address);
        if (!cpu_addr) [[unlikely]] {
            commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
            AccelerateInlineToMemory(address, size, bytes);
            continue;
        }
        TraceInlineWrite(*gpu_memory, address, *cpu_addr, size, bytes);
        if (Profile::Enabled()) {
            Diagnostics::NoteInlineWrite(maxwell3d, address, size);
        }
        gpu_memory->WriteBlockUnsafe(address, bytes.data(), size, true);
        const size_t batched = batch.copies.size();
        const bool uploaded = buffer_cache.InlineMemoryBatched(*cpu_addr, size, bytes, batch);
        if (uploaded) {
            ++uploads;
            // Copies recorded on the way (the batch emptied or the unbatched way taken).
            commands += batch.copies.size() <= batched ? 1 : 0;
        } else if (!defer) {
            // EDEN_NX2_INLINE_DEFER=0: the CPU-modified mark makes a later synchronization in
            // this call upload the pages, recorded ahead of the batch: what is batched goes
            // first, so a batched copy can't land over bytes written after it.
            commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
            buffer_cache.WriteMemory(*cpu_addr, size);
        }
        query_cache.InvalidateRegion(*cpu_addr, size);
        if (!uploaded && defer) {
            gpu_memory->AccumulateInvalidation(address, size);
            if (Profile::Enabled()) {
                Diagnostics::NoteDeferredInline(*cpu_addr, size);
            }
            continue;
        }
        {
            std::scoped_lock lock{texture_cache.mutex};
            const u64 image_hits = Profile::Enabled() ? texture_cache.WriteHits() : 0;
            texture_cache.WriteMemory(*cpu_addr, size);
            if (Profile::Enabled() && texture_cache.WriteHits() != image_hits) {
                Profile::Add(Profile::Counter::InlineOnImages, 1);
            }
        }
        pipeline_cache.InvalidateRegion(*cpu_addr, size);
    }
    commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
    Profile::Add(Profile::Counter::ResetUploads, uploads);
    Profile::Add(Profile::Counter::ResetCopyCommands, commands);
    thread_local u64 calls{};
    thread_local u64 total_uploads{};
    thread_local u64 total_commands{};
    ++calls;
    total_uploads += uploads;
    total_commands += commands;
    if (calls == 1 || (calls & ((u64{1} << 10) - 1)) == 0) {
        LOG_INFO(HW_GPU,
                 "Indirect draw reset uploads batched: {} uploads in {} copy commands over {} "
                 "resets (EDEN_NX2_RESET_BATCH=0: one each)",
                 total_uploads, total_commands, calls);
    }
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return {};
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    thread_local u32 display_trace_count{};
    static const bool frame_capture = std::getenv("EDEN_NATIVE_FRAME_CAPTURE") != nullptr;
    if (frame_capture && display_trace_count++ < 64) {
        LOG_INFO(HW_GPU, "Native display source: address={:#x}, offset={:#x}, dimensions={}x{}, stride={}, accelerated={}, mask={:#x}",
                 framebuffer_addr, config.offset, config.width, config.height, pixel_stride,
                 image_view != nullptr, config.layer_stack_mask);
    }
    if (!image_view) {
        return {};
    }

    query_cache.NotifySegment(false);

    const auto& resolution = Settings::values.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr u32 DRAWS_TO_DISPATCH = 512;
    static constexpr u32 CHECK_MASK = 3;
#else
    // Every 32 draws (was 8): each dispatch woke the worker thread, and Switch 2 titles make
    // ~55,000 draws a second. A full chunk is dispatched on its own (Scheduler::Record).
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
    static constexpr u32 CHECK_MASK = 31;
#endif // __ANDROID__

    static_assert(DRAWS_TO_DISPATCH % (CHECK_MASK + 1) == 0);
    if ((++draw_counter & CHECK_MASK) != CHECK_MASK) {
        return;
    }
    if (draw_counter < DRAWS_TO_DISPATCH) {
        scheduler.DispatchWork();
        return;
    }
    scheduler.Flush();
    draw_counter = 0;
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    if (VideoCommon::Nx2Profile::Enabled()) {
        Diagnostics::NoteDmaWrite(src_address, amount * sizeof(u32));
    }
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    if (VideoCommon::Nx2Profile::Enabled()) {
        Diagnostics::NoteDmaWrite(dest_address, amount);
    }
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dest_address, amount);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    namespace Profile = VideoCommon::Nx2Profile;
    if (Profile::Enabled()) {
        // An upload writes an image (its pages are not looked at: size 0), a download the
        // buffer's guest memory.
        Diagnostics::NoteDmaWrite(IS_IMAGE_UPLOAD ? image_operand.address : buffer_operand.address,
                                  IS_IMAGE_UPLOAD ? 0
                                                  : u64{buffer_operand.pitch} * buffer_operand.height);
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    if constexpr (IS_IMAGE_UPLOAD) {
        Profile::Add(Profile::Counter::DmaOnImages, 1);
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

void RasterizerVulkan::UpdateDynamicStates() {
    auto& regs = maxwell3d->regs;
    auto& flags = maxwell3d->dirty.flags;
    const auto topology = maxwell3d->draw_manager.draw_state.topology;
    const bool topology_changed = state_tracker.ChangePrimitiveTopology(topology);
    if (topology_changed) {
        flags[Dirty::DepthBiasEnable] = true;
        flags[Dirty::PrimitiveRestartEnable] = true;
    }

    UpdateViewportsState(regs);
    UpdateScissorsState(regs);
    UpdateDepthBias(regs);
    UpdateBlendConstants(regs);
    UpdateDepthBounds(regs);
    UpdateStencilFaces(regs);
    UpdateLineWidth(regs);

    if (device.IsExtExtendedDynamicStateSupported()) {
        UpdateCullMode(regs);
        UpdateDepthCompareOp(regs);
        UpdateFrontFace(regs);
        UpdateStencilOp(regs);
        if (state_tracker.TouchStateEnable()) {
            UpdateDepthBoundsTestEnable(regs);
            UpdateDepthTestEnable(regs);
            UpdateDepthWriteEnable(regs);
            UpdateStencilTestEnable(regs);
        }
        if (topology_changed) {
            scheduler.Record([topology_vk = MaxwellToVK::PrimitiveTopology(device, topology)](
                                 vk::CommandBuffer cmdbuf) {
                cmdbuf.SetPrimitiveTopologyEXT(topology_vk);
            });
        }
    }

    if (device.IsExtExtendedDynamicState2Supported()) {
        UpdatePrimitiveRestartEnable(regs);
        UpdateRasterizerDiscardEnable(regs);
        UpdateDepthBiasEnable(regs);
    }

    if (device.IsExtExtendedDynamicState2ExtrasSupported()) {
        UpdateLogicOp(regs);
    }

    if (device.IsExtExtendedDynamicState3EnablesSupported()) {
        using namespace Tegra::Engines;
        // AMD Workaround: LogicOp incompatible with float render targets
        if (device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_OPEN_SOURCE ||
            device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_PROPRIETARY) {
            const auto has_float = std::any_of(
                regs.vertex_attrib_format.begin(), regs.vertex_attrib_format.end(),
                [](const auto& attrib) {
                    return attrib.type == Maxwell3D::Regs::VertexAttribute::Type::Float;
                }
            );
            if (regs.logic_op.enable) {
                regs.logic_op.enable = static_cast<u32>(!has_float);
            }
        }
        UpdateLogicOpEnable(regs);
        UpdateDepthClampEnable(regs);
        UpdateLineRasterizationMode(regs);
        UpdateLineStippleEnable(regs);
        UpdateConservativeRasterizationMode(regs);
        UpdateAlphaToCoverageEnable(regs);
        UpdateAlphaToOneEnable(regs);
    }

    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        UpdateBlending(regs);
    } else if (device.IsExtColorWriteEnableSupported()) {
        UpdateColorWriteEnable(regs);
    }

    if (device.IsExtVertexInputDynamicStateSupported()) {
        if (auto* gp = DynamicStatePipeline(); gp && gp->HasDynamicVertexInput()) {
            UpdateVertexInput(regs);
        }
    }
}

void RasterizerVulkan::HandleTransformFeedback() {
    static std::once_flag warn_unsupported;

    const auto& regs = maxwell3d->regs;
    // Once per draw recorded (a light draw needs it off: LightFeedbackOff).
    if (regs.transform_feedback_enabled != 0) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::TransformFeedbackDraws, 1);
    }
    if (!device.IsExtTransformFeedbackSupported()) {
        if (regs.transform_feedback_enabled != 0) {
            std::call_once(warn_unsupported, [&] {
                LOG_WARNING(Render_Vulkan, "Transform feedback requested by guest but VK_EXT_transform_feedback is unavailable; queries disabled");
            });
        } else {
            std::call_once(warn_unsupported, [&] {
                LOG_INFO(Render_Vulkan, "VK_EXT_transform_feedback not available on device");
            });
        }
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        // Log extension usage for transform feedback
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_transform_feedback", "HandleTransformFeedback");
        }
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchViewports()) {
        return;
    }

    maxwell3d->dirty.flags[Dirty::Scissors] = true;

    if (!regs.viewport_scale_offset_enabled) {
        float x = static_cast<float>(regs.surface_clip.x);
        float y = static_cast<float>(regs.surface_clip.y);
        float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
        float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y += height;
            height = -height;
        }
        VkViewport viewport{
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        scheduler.Record([this, viewport](vk::CommandBuffer cmdbuf) {
            const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkViewport, Maxwell::NumViewports> viewport_list{};
            viewport_list.fill(viewport);
            const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
            cmdbuf.SetViewport(0, viewports);
        });
        return;
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    const std::array viewport_list{
        GetViewportState(device, regs, 0, scale),  GetViewportState(device, regs, 1, scale),
        GetViewportState(device, regs, 2, scale),  GetViewportState(device, regs, 3, scale),
        GetViewportState(device, regs, 4, scale),  GetViewportState(device, regs, 5, scale),
        GetViewportState(device, regs, 6, scale),  GetViewportState(device, regs, 7, scale),
        GetViewportState(device, regs, 8, scale),  GetViewportState(device, regs, 9, scale),
        GetViewportState(device, regs, 10, scale), GetViewportState(device, regs, 11, scale),
        GetViewportState(device, regs, 12, scale), GetViewportState(device, regs, 13, scale),
        GetViewportState(device, regs, 14, scale), GetViewportState(device, regs, 15, scale),
    };
    scheduler.Record([this, viewport_list](vk::CommandBuffer cmdbuf) {
        const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
        cmdbuf.SetViewport(0, viewports);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchScissors()) {
        return;
    }
    if (!regs.viewport_scale_offset_enabled) {
        u32 x = regs.surface_clip.x;
        u32 y = regs.surface_clip.y;
        u32 width = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
        u32 height = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y = regs.surface_clip.height - (y + height);
        }
        VkRect2D scissor{};
        scissor.offset.x = static_cast<int32_t>(x);
        scissor.offset.y = static_cast<int32_t>(y);
        scissor.extent.width  = width;
        scissor.extent.height = height;
        scheduler.Record([this, scissor](vk::CommandBuffer cmdbuf) {
            const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkRect2D, Maxwell::NumViewports> scissor_list{};
            scissor_list.fill(scissor);
            const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
            cmdbuf.SetScissor(0, scissors);
        });
        return;
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    const std::array scissor_list{
        GetScissorState(regs, 0, up_scale, down_shift),
        GetScissorState(regs, 1, up_scale, down_shift),
        GetScissorState(regs, 2, up_scale, down_shift),
        GetScissorState(regs, 3, up_scale, down_shift),
        GetScissorState(regs, 4, up_scale, down_shift),
        GetScissorState(regs, 5, up_scale, down_shift),
        GetScissorState(regs, 6, up_scale, down_shift),
        GetScissorState(regs, 7, up_scale, down_shift),
        GetScissorState(regs, 8, up_scale, down_shift),
        GetScissorState(regs, 9, up_scale, down_shift),
        GetScissorState(regs, 10, up_scale, down_shift),
        GetScissorState(regs, 11, up_scale, down_shift),
        GetScissorState(regs, 12, up_scale, down_shift),
        GetScissorState(regs, 13, up_scale, down_shift),
        GetScissorState(regs, 14, up_scale, down_shift),
        GetScissorState(regs, 15, up_scale, down_shift),
    };
    scheduler.Record([this, scissor_list](vk::CommandBuffer cmdbuf) {
        const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
        cmdbuf.SetScissor(0, scissors);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBias()) {
        return;
    }
    float units = regs.depth_bias / 2.0f;
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;

    if (is_d24 && !device.SupportsD24DepthBuffer()) {
        static constexpr const size_t length = sizeof(NEEDS_D24) / sizeof(NEEDS_D24[0]);

        static constexpr const u64* start = NEEDS_D24;
        static constexpr const u64* end = NEEDS_D24 + length;

        const u64* it = std::find(start, end, program_id);

        if (it != end) {
            // the base formulas can be obtained from here:
            //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
            const double rescale_factor =
                static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
            units = static_cast<float>(static_cast<double>(units) * rescale_factor);
        }
    }

    scheduler.Record([constant = units, clamp = regs.depth_bias_clamp,
                      factor = regs.slope_scale_depth_bias, this](vk::CommandBuffer cmdbuf) {
        if (device.IsExtDepthBiasControlSupported()) {
            static VkDepthBiasRepresentationInfoEXT bias_info{
                .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
                .pNext = nullptr,
                .depthBiasRepresentation =
                    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
                .depthBiasExact = VK_FALSE,
            };

            cmdbuf.SetDepthBias(constant, clamp, factor, &bias_info);
        } else {
            cmdbuf.SetDepthBias(constant, clamp, factor);
        }
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlendConstants()) {
        return;
    }
    const std::array blend_color = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                    regs.blend_color.a};
    scheduler.Record(
        [blend_color](vk::CommandBuffer cmdbuf) { cmdbuf.SetBlendConstants(blend_color.data()); });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBounds()) {
        return;
    }
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBounds(min, max); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilProperties()) {
        return;
    }
    bool update_references = state_tracker.TouchStencilReference();
    bool update_write_mask = state_tracker.TouchStencilWriteMask();
    bool update_compare_masks = state_tracker.TouchStencilCompare();
    if (state_tracker.TouchStencilSide(regs.stencil_two_side_enable != 0)) {
        update_references = true;
        update_write_mask = true;
        update_compare_masks = true;
    }
    if (update_references) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_back_ref);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_front_ref);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_ref = regs.stencil_front_ref, back_ref = regs.stencil_back_ref,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_ref != back_ref;
                // Front face
                cmdbuf.SetStencilReference(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_ref);
                if (set_back) {
                    cmdbuf.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, back_ref);
                }
            });
        }();
    }
    if (update_write_mask) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_back_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_front_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_write_mask = regs.stencil_front_mask,
                              back_write_mask = regs.stencil_back_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_write_mask != back_write_mask;
                // Front face
                cmdbuf.SetStencilWriteMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_write_mask);
                if (set_back) {
                    cmdbuf.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, back_write_mask);
                }
            });
        }();
    }
    if (update_compare_masks) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_back_func_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_front_func_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_test_mask = regs.stencil_front_func_mask,
                              back_test_mask = regs.stencil_back_func_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_test_mask != back_test_mask;
                // Front face
                cmdbuf.SetStencilCompareMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                      : VK_STENCIL_FACE_FRONT_AND_BACK,
                                             front_test_mask);
                if (set_back) {
                    cmdbuf.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, back_test_mask);
                }
            });
        }();
    }
    state_tracker.ClearStencilReset();
}

void RasterizerVulkan::UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineWidth()) {
        return;
    }
    const float width =
        regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
    scheduler.Record([width](vk::CommandBuffer cmdbuf) { cmdbuf.SetLineWidth(width); });
}

void RasterizerVulkan::UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchCullMode()) {
        return;
    }
    scheduler.Record([enabled = regs.gl_cull_test_enabled,
                      cull_face = regs.gl_cull_face](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetCullModeEXT(enabled ? MaxwellToVK::CullFace(cull_face) : VK_CULL_MODE_NONE);
    });
}

void RasterizerVulkan::UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBoundsTestEnable()) {
        return;
    }
    bool enabled = regs.depth_bounds_enable;
    if (enabled && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
        enabled = false;
    }
    scheduler.Record([enable = enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBoundsTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthWriteEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_write_enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthWriteEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchPrimitiveRestartEnable()) {
        return;
    }

    bool enable = regs.primitive_restart.enabled != 0;
    if (device.IsMoltenVK()) {
        enable = true;
    } else if (enable) {
        const auto topology = MaxwellToVK::PrimitiveTopology(device, maxwell3d->draw_manager.draw_state.topology);
        enable = IsPrimitiveRestartSupported(device, topology);
    }

    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetPrimitiveRestartEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchRasterizerDiscardEnable()) {
        return;
    }
    scheduler.Record([disable = regs.rasterize_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetRasterizerDiscardEnableEXT(disable == 0);
    });
}

void RasterizerVulkan::UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchConservativeRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3ConservativeRasterizationMode()) {
        return;
    }

    scheduler.Record([enable = regs.conservative_raster_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetConservativeRasterizationModeEXT(
            enable ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                   : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
    });
}

void RasterizerVulkan::UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineStippleEnable()) {
        return;
    }

    if (!device.SupportsDynamicState3LineStippleEnable()) {
        return;
    }

    scheduler.Record([enable = regs.line_stipple_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineStippleEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!device.IsExtLineRasterizationSupported()) {
        return;
    }
    if (!state_tracker.TouchLineRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3LineRasterizationMode()) {
        static std::once_flag warn_missing_rect;
        std::call_once(warn_missing_rect, [] {
            LOG_WARNING(Render_Vulkan,
                        "Driver lacks rectangular line rasterization support; skipping dynamic "
                        "line state updates");
        });
        return;
    }

    const bool wants_smooth = regs.line_anti_alias_enable != 0;
    VkLineRasterizationModeEXT mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
    if (wants_smooth) {
        if (device.SupportsSmoothLines()) {
            mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        } else {
            static std::once_flag warn_missing_smooth;
            std::call_once(warn_missing_smooth, [] {
                LOG_WARNING(Render_Vulkan,
                            "Line anti-aliasing requested but smoothLines feature unavailable; "
                            "using rectangular rasterization");
            });
        }
    }
    scheduler.Record([mode](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineRasterizationModeEXT(mode);
    });
}

void RasterizerVulkan::UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBiasEnable()) {
        return;
    }
    constexpr size_t POINT = 0;
    constexpr size_t LINE = 1;
    constexpr size_t POLYGON = 2;
    static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
        POINT,   // Points
        LINE,    // Lines
        LINE,    // LineLoop
        LINE,    // LineStrip
        POLYGON, // Triangles
        POLYGON, // TriangleStrip
        POLYGON, // TriangleFan
        POLYGON, // Quads
        POLYGON, // QuadStrip
        POLYGON, // Polygon
        LINE,    // LinesAdjacency
        LINE,    // LineStripAdjacency
        POLYGON, // TrianglesAdjacency
        POLYGON, // TriangleStripAdjacency
        POLYGON, // Patches
    };
    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = u32(maxwell3d->draw_manager.draw_state.topology);
    const u32 enable = enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]];
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBiasEnableEXT(enable != 0); });
}

void RasterizerVulkan::UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOpEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3LogicOpEnable()) {
        return;
    }
    scheduler.Record([enable = regs.logic_op.enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLogicOpEnableEXT(enable != 0);
    });
}

void RasterizerVulkan::UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthClampEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3DepthClampEnable()) {
        return;
    }
    bool is_enabled = !(regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
    scheduler.Record(
        [is_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthClampEnableEXT(is_enabled); });
}

void RasterizerVulkan::UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (stage_dynamic.active && stage_dynamic.pipeline == nullptr) [[unlikely]] {
        // Cut c, no draw's pipeline (C's DrawTexture): the next draw sets it (flag kept).
        return;
    }
    if (!state_tracker.TouchAlphaToCoverageEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToCoverageEnable()) {
        return;
    }
    GraphicsPipeline* const pipeline = DynamicStatePipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToCoverage() &&
                        regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToCoverageEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (stage_dynamic.active && stage_dynamic.pipeline == nullptr) [[unlikely]] {
        return;
    }
    if (!state_tracker.TouchAlphaToOneEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToOneEnable()) {
        static std::once_flag warn_alpha_to_one;
        std::call_once(warn_alpha_to_one, [] {
            LOG_WARNING(Render_Vulkan,
                        "Alpha-to-one is not supported on this device; forcing it disabled");
        });
        return;
    }
    GraphicsPipeline* const pipeline = DynamicStatePipeline();
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToOne() &&
                        regs.anti_alias_alpha_control.alpha_to_one != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToOneEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthCompareOp()) {
        return;
    }
    scheduler.Record([func = regs.depth_test_func](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthCompareOpEXT(MaxwellToVK::ComparisonOp(func));
    });
}

void RasterizerVulkan::UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchFrontFace()) {
        return;
    }

    VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    scheduler.Record(
        [front_face](vk::CommandBuffer cmdbuf) { cmdbuf.SetFrontFaceEXT(front_face); });
}

void RasterizerVulkan::UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilOp()) {
        return;
    }
    const Maxwell::StencilOp::Op fail = regs.stencil_front_op.fail;
    const Maxwell::StencilOp::Op zfail = regs.stencil_front_op.zfail;
    const Maxwell::StencilOp::Op zpass = regs.stencil_front_op.zpass;
    const Maxwell::ComparisonOp compare = regs.stencil_front_op.func;
    if (regs.stencil_two_side_enable) {
        // Separate stencil op per face
        const Maxwell::StencilOp::Op back_fail = regs.stencil_back_op.fail;
        const Maxwell::StencilOp::Op back_zfail = regs.stencil_back_op.zfail;
        const Maxwell::StencilOp::Op back_zpass = regs.stencil_back_op.zpass;
        const Maxwell::ComparisonOp back_compare = regs.stencil_back_op.func;
        scheduler.Record([fail, zfail, zpass, compare, back_fail, back_zfail, back_zpass,
                          back_compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_BIT, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, MaxwellToVK::StencilOp(back_fail),
                                   MaxwellToVK::StencilOp(back_zpass),
                                   MaxwellToVK::StencilOp(back_zfail),
                                   MaxwellToVK::ComparisonOp(back_compare));
        });
    } else {
        // Front face defines the stencil op of both faces
        scheduler.Record([fail, zfail, zpass, compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_AND_BACK, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
        });
    }
}

void RasterizerVulkan::UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOp()) {
        return;
    }
    const auto op_value = static_cast<u32>(regs.logic_op.op);
    auto op = op_value >= 0x1500 && op_value < 0x1510 ? static_cast<VkLogicOp>(op_value - 0x1500)
                                                      : VK_LOGIC_OP_NO_OP;
    scheduler.Record([op](vk::CommandBuffer cmdbuf) { cmdbuf.SetLogicOpEXT(op); });
}

void RasterizerVulkan::UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlending()) {
        return;
    }

    if (state_tracker.TouchColorMask()) {
        std::array<VkColorComponentFlags, Maxwell::NumRenderTargets> setup_masks{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            auto& current = setup_masks[index];
            if (mask.R) {
                current |= VK_COLOR_COMPONENT_R_BIT;
            }
            if (mask.G) {
                current |= VK_COLOR_COMPONENT_G_BIT;
            }
            if (mask.B) {
                current |= VK_COLOR_COMPONENT_B_BIT;
            }
            if (mask.A) {
                current |= VK_COLOR_COMPONENT_A_BIT;
            }
        }
        scheduler.Record([setup_masks](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorWriteMaskEXT(0, setup_masks);
        });
    }

    if (state_tracker.TouchBlendEnable()) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            bool is_integer = false;
            if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                is_integer = IsPixelFormatInteger(format);
            }
            setup_enables[index] =
                (!is_integer && regs.blend.enable[index] != 0) ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
    }

    if (state_tracker.TouchBlendEquations()) {
        std::array<VkColorBlendEquationEXT, Maxwell::NumRenderTargets> setup_blends{};

        const auto blend_setup = [&](auto& host_blend, const auto& guest_blend) {
            host_blend.srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source);
            host_blend.dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest);
            host_blend.colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op);
            host_blend.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source);
            host_blend.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest);
            host_blend.alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op);
        };

        // Single blend equation for all targets
        if (!regs.blend_per_target_enabled) {
            // Temporary workaround for games that use iterated blending
            if (regs.iterated_blend.enable && Settings::values.use_squashed_iterated_blend) {
                setup_blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].colorBlendOp = VK_BLEND_OP_ADD;
                setup_blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                setup_blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                setup_blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                blend_setup(setup_blends[0], regs.blend);
            }

            // Copy first blend state to all other targets
            for (size_t index = 1; index < Maxwell::NumRenderTargets; index++) {
                setup_blends[index] = setup_blends[0];
            }
        } else {
            // Per-target blending
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                blend_setup(setup_blends[index], regs.blend_per_target[index]);
            }
        }

        scheduler.Record([setup_blends](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEquationEXT(0, setup_blends);
        });
    }
}

void RasterizerVulkan::UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchColorMask()) {
        return;
    }
    std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        setup_enables[index] = (mask.R || mask.G || mask.B || mask.A) ? VK_TRUE : VK_FALSE;
    }
    scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetColorWriteEnableEXT(setup_enables);
    });
}

void RasterizerVulkan::UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.stencil_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetStencilTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    auto& dirty{maxwell3d->dirty.flags};
    const bool vertex_input_dirty = dirty[Dirty::VertexInput];
    const bool vertex_buffers_dirty = dirty[VideoCommon::Dirty::VertexBuffers];
    if (!vertex_input_dirty && !vertex_buffers_dirty) {
        return;
    }
    dirty[Dirty::VertexInput] = false;

    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;

    const u32 max_attributes =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexAttributes,
                                          device.GetMaxVertexInputAttributes()));
    const u32 max_bindings =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexArrays,
                                          device.GetMaxVertexInputBindings()));


    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset = attribute.offset,
        });
    }

    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride = input_binding.stride,
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }

    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        dirty[Dirty::VertexAttribute0 + index] = false;
    }
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        dirty[Dirty::VertexBinding0 + index] = false;
    }

    scheduler.Record([bindings, attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetVertexInputEXT(bindings, attributes);
    });
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    NoteLightWork();
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    NoteLightWork();
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    if (VideoCommon::Nx2Profile::Enabled()) {
        Diagnostics::t_bound_maxwell3d = maxwell3d;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    NoteLightWork();
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

// Back stages, cut c (Kesim-2; PLAN.md §4.3 T7) ----------------------------------------------
//
// B (the memory stage) runs the engines' rasterizer calls; the draws and the events with a part
// for each stage split themselves there (SplitOnB). B's half: FlushCaching, the buffer cache
// (its Vulkan commands into a record list, BufferCacheRuntime::BeginRecords), the pipeline lookup
// and the light decision. C's half, in ring 3's order: B's list replayed, the texture cache, the
// query cache, dynamic state and the recording. B takes the buffer cache's lock through
// Nx2Record::CacheLock and writes ring 3 or joins holding none (K3).
//
// The light chain split (tasarim §3.7). B decides from what it sees (TryLightDraw's checks on
// its replica, its copy of the light epoch, the cached and same writes, the shader hits) and
// sends LightPlain (geometry binds only) or LightUniform (B's uniform buffer entries). C checks
// what only it sees before recording: (a) the command state, nothing recorded since its anchor
// (C's own work, a submission, a join) and the render pass active; (b) textures, no image written
// since. Either failing, C repairs with what it keeps of its anchor: ConfigureTextures with the
// anchor's descriptor entries and handles (plain) or with the packet's entries and the anchor's
// handles (uniform), then dynamic state. A repair needs nothing of B. The anchor's uniform data
// lives in B's staging pool under the anchor's seq: C holds that seq back (NoteStageSeq) while a
// light draw may still repair with it. B forces every vertex stream to bind again for each packet
// (the geometry mirror, §3.6/6: B's dirty flags do not see C's command buffer state).

namespace {

// EDEN_NX2_BACK_STAGES_LIGHT=0: with cut c every draw is set up in full (no light decision on B).
bool StageLightEnabled() {
    static std::atomic<s8> enabled{-1};
    s8 value = enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_BACK_STAGES_LIGHT");
        value = static_cast<s8>(env == nullptr || env[0] != '0');
        enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

// What C does for a range of B's invalidations (stage_c_ranges, the size word's top bits).
constexpr u64 StageRangeTexture = u64{1} << 62;
constexpr u64 StageRangeQuery = u64{1} << 63;
constexpr u64 StageRangeSize = StageRangeTexture - 1;
constexpr size_t StageRangesPerRecord = 1024;

u64 HandleBits(VkBuffer buffer) {
    u64 bits{};
    std::memcpy(&bits, &buffer, sizeof(buffer));
    return bits;
}

VkBuffer BufferOf(u64 bits) {
    VkBuffer buffer{};
    std::memcpy(&buffer, &bits, sizeof(buffer));
    return buffer;
}

// PrepareDraw's NX2_INDIRECT_RESET_PROGRAM (RESETFIX), for B.
constexpr std::array<u32, 44> StageResetCode{
    0xff007321, 0x0002fcff, 0x00000000, 0x000e2400, 0x00007a11, 0x00004400, 0x078018ff,
    0x001fca00, 0xff017624, 0x00004500, 0x000e06ff, 0x000fcc00, 0x00007381, 0x00000000,
    0x001eab00, 0x000ea200, 0x00037802, 0x00000001, 0x00000f00, 0x000fca00, 0x00007386,
    0x00000403, 0x0010a900, 0x004fe800, 0x00007386, 0x000000ff, 0x0010a900, 0x000fe800,
    0x00007386, 0x000008ff, 0x0010a900, 0x000fe800, 0x00007386, 0x00000cff, 0x0010a900,
    0x000fe800, 0x00007386, 0x000010ff, 0x0010a900, 0x000fe200, 0x0000794d, 0x00000000,
    0x03800000, 0x000fea00,
};

// Every vertex stream binds again: B's dirty flags do not see C's submissions and InvalidateState,
// which clear what the command buffer holds (RasterizerVulkan::ForceGeometryB, a join's old setup).
void ForceVertexBinds(Tegra::Engines::Maxwell3D& mb) {
    auto& flags = mb.dirty.flags;
    flags[VideoCommon::Dirty::VertexBuffers] = true;
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
    }
}

// EDEN_NX2_GEOMETRY_MIRROR=0 (cut c, C7): B records a draw packet's vertex and index binds into
// its list too, as before. Else they reach C in the packet's geometry slots alone and C records
// them after the list (ApplyGeometryC): B neither records nor copies them, a packet that recorded
// nothing else has no list, and C replays none.
bool GeometryMirrorBinds() {
    // Read at the first call, without a guarded static's check on every call (each draw packet).
    static std::atomic<s8> enabled{-1};
    s8 value = enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_GEOMETRY_MIRROR");
        value = static_cast<s8>(env == nullptr || env[0] != '0');
        enabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

// The streams of `streams` whose dirty flag is set.
u32 VertexDirtyMask(const Tegra::Engines::Maxwell3D::DirtyState::Flags& flags, u32 streams) {
    u32 mask = 0;
    for (u32 rest = streams; rest != 0; rest &= rest - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(rest));
        if (flags[VideoCommon::Dirty::VertexBuffer0 + index]) {
            mask |= 1u << index;
        }
    }
    return mask;
}

} // Anonymous namespace

Tegra::Engines::Maxwell3D& RasterizerVulkan::EngineB() {
    return VideoCommon::Nx2Stages::EngineFor(channel_state->channel,
                                             VideoCommon::Nx2Stages::Role::Memory);
}

template <typename Func>
void RasterizerVulkan::JoinB(Func&& func) {
    // What B has for C goes first: C runs it before it parks.
    FlushStageCWork();
    VideoCommon::Nx2Stages::JoinFromB([&] {
        const bool was_in_join = b_in_join;
        b_in_join = true;
        func();
        b_in_join = was_in_join;
        if (stage_query_later.Size() != 0) {
            // C parked: the counters the query cache no longer holds were written or cancelled.
            stage_query_later.DropIf([this](u64 begin, u64 end) {
                return !query_cache.HoldsQueries(begin, end - begin);
            });
        }
    });
    // C ran everything B sent before.
    stage_pools.Clear();
    stage_query_writes.Clear();
}

template <typename Func>
void RasterizerVulkan::JoinBLocked(Func&& func) {
    VideoCommon::Nx2Stages::JoinFromBLocked([&] {
        const bool was_in_join = b_in_join;
        b_in_join = true;
        func();
        b_in_join = was_in_join;
    });
}

template <typename Func>
void RasterizerVulkan::StageToC(const Func& func) {
    FlushStageCWork();
    if (OwnsRecordsB()) [[unlikely]] {
        // C parked: its half here, in its place in the stream.
        func();
        return;
    }
    VideoCommon::Nx2Stages::Post(VideoCommon::Nx2Stages::Ring::ToC, func);
}

void RasterizerVulkan::NoteQueryForWfi() noexcept {
    // T7c review finding 2 (nx2_stage_split.h).
    if (VideoCommon::Nx2Stages::QueryNotesWfi(
            split_draws, VideoCommon::Nx2Stages::OnStage(VideoCommon::Nx2Stages::Stage::C))) {
        stage_query_since_wfi = true;
    }
}

void RasterizerVulkan::GuardQueryWritesB(std::optional<DAddr> addr, u64 size, bool cancels) {
    // T7c review finding 3: C would write a report B sent before over B's later write of the same
    // bytes. A payload release C writes at once: C runs it first. A counter C writes from a fence
    // after it, which may pass before C runs B's invalidation of the bytes: cancelled first, as
    // the old path's invalidation does (QueryCacheBase::InvalidateRegion).
    const u64 ran = VideoCommon::Nx2Stages::GpuPages().RanSeq();
    const u64 begin = addr ? *addr : 0;
    const u64 end = begin + (std::max)(size, u64{1});
    // Not one device range: any write may meet a release; a counter it cannot cancel (B's
    // invalidation of each page reaches C later).
    const bool at_once =
        stage_query_writes.Any(ran) && (!addr || stage_query_writes.Overlaps(begin, end, ran));
    const bool later = cancels && addr && stage_query_later.Size() != 0 &&
                       Tegra::Nx2UntrackedWritten(begin, end - begin) &&
                       stage_query_later.Overlaps(begin, end, 0);
    if (!at_once && !later) [[likely]] {
        return;
    }
    const auto cancel = [&] {
        if (later) {
            query_cache.InvalidateRegion(begin, end - begin);
        }
    };
    if (Nx2Record::HeldLocks::Count() != 0 || buffer_cache_runtime.Target().list != nullptr)
        [[unlikely]] {
        // Not seen (B's guest writes come between packets): the join that keeps the mutex.
        VideoCommon::Nx2Stages::ReportStageHazard("guest write over a query inside B's packet");
        JoinBLocked(cancel);
        stage_query_writes.Clear();
    } else {
        JoinB(cancel);
    }
    if (later) {
        stage_query_later.Forget(begin, end);
    }
}

void RasterizerVulkan::ForceGeometryB(Tegra::Engines::Maxwell3D& mb) {
    // C binds again from its geometry mirror what its command buffer lost (BindGeometryC); B
    // sends every stream after binds the mirror did not see, and once per command buffer for
    // their use marks (GeometryForced).
    // Another channel or memory manager since: its streams resolved again (BindChannel's
    // InvalidateState sets only C's replica's flags; the buffer cache's streams are shared).
    const bool switched = &mb != geometry_engine_b || gpu_memory != geometry_memory_b;
    const u64 tick = scheduler.CurrentTick();
    if (VideoCommon::Nx2Stages::GeometryForced(geometry_reset_b || switched, tick,
                                               geometry_tick_b)) {
        ForceVertexBinds(mb);
        geometry_reset_b = false;
        geometry_tick_b = tick;
        geometry_engine_b = &mb;
        geometry_memory_b = gpu_memory;
    }
}

void RasterizerVulkan::ForceGeometryForJoin() {
    // The old setup binds straight to C's command buffer from B's replica, whose flags do not
    // see C's submissions (esdeger-2, as a Joined draw); B is parked or idle.
    if (!VideoCommon::Nx2Stages::Active()) {
        return;
    }
    ForceVertexBinds(EngineB());
    geometry_reset_b = true;
}

void RasterizerVulkan::BeginStagesHook(void* context) {
    // Start, before B and C start (every stage drained).
    auto* const rasterizer = static_cast<RasterizerVulkan*>(context);
    stage_rasterizer = rasterizer;
    std::scoped_lock lock{rasterizer->buffer_cache.mutex};
    rasterizer->buffer_cache_runtime.SetRecordOwnerHook(&StageRecordOwnerHook, rasterizer);
    rasterizer->buffer_cache_runtime.BeginStages();
    // B's next draw is set up in full and finds the targets' device ranges again; the first
    // WaitForIdle joins (query writes may be pending from before).
    rasterizer->light_epoch_b.fetch_add(1, std::memory_order_seq_cst);
    rasterizer->stage_targets.valid = false;
    rasterizer->stage_query_since_wfi = true;
    // Nothing in flight from a run before.
    rasterizer->stage_pools.Clear();
    rasterizer->stage_query_writes.Clear();
    rasterizer->stage_query_later.Clear();
    rasterizer->stage_c_ranges.clear();
    // C's command buffer holds no bind of B's yet: the first packet sends every vertex stream.
    rasterizer->geometry_reset_b = true;
    rasterizer->geometry_engine_b = nullptr;
    rasterizer->geometry_mirror_c.Clear();
    rasterizer->geometry_replayed_c = false;
}

void RasterizerVulkan::EndStagesHook(void* context) {
    // Stop, after B and C stopped: C's half of invalidations B had not sent yet runs here (nothing
    // runs behind), the anchor's seq is noted, then the runtime's pending one.
    auto* const rasterizer = static_cast<RasterizerVulkan*>(context);
    if (!rasterizer->stage_c_ranges.empty()) {
        rasterizer->RunStageInvalidations(rasterizer->stage_c_ranges.data(),
                                          static_cast<u32>(rasterizer->stage_c_ranges.size() / 2));
        rasterizer->stage_c_ranges.clear();
    }
    rasterizer->stage_pools.Clear();
    rasterizer->stage_query_writes.Clear();
    rasterizer->stage_query_later.Clear();
    rasterizer->ReleaseAnchorC();
    std::scoped_lock lock{rasterizer->buffer_cache.mutex};
    rasterizer->buffer_cache_runtime.EndStages();
}

void RasterizerVulkan::StageRecordOwnerHook(void* context, RecordList* records,
                                            VideoCommon::Nx2Stages::FunctionRef op) {
    // B inside a buffer cache operation (a packet, the buffer cache mutex held: K3 (a)): C parks
    // having run what B published before; B's list so far is recorded first, then `op` records
    // straight to the scheduler. Both use B's state alone (C's replica lags: JoinFromBLocked).
    auto* const rasterizer = static_cast<RasterizerVulkan*>(context);
    const auto run = [&] {
        if (records != nullptr) {
            rasterizer->scheduler.Replay(*records, records->Seq());
            // Its geometry binds, maybe in a command buffer the packet's draw will not be in.
            rasterizer->geometry_replayed_c = true;
        }
        op();
    };
    if (rasterizer->b_in_join) {
        // Already C parked (a retirement outside a packet inside B's join).
        run();
        return;
    }
    rasterizer->JoinBLocked(run);
}

template <typename Func>
void RasterizerVulkan::StageBufferWork(Func&& work) {
    if (buffer_cache_runtime.Target().list != nullptr) {
        // Inside B's packet: its records go with it.
        Nx2Record::CacheLock lock{buffer_cache.mutex};
        work();
        return;
    }
    if (OwnsRecordsB()) [[unlikely]] {
        // The runtime records straight to the scheduler.
        Nx2Record::CacheLock lock{buffer_cache.mutex};
        work();
        return;
    }
    u64 seq;
    RecordList* records;
    {
        Nx2Record::CacheLock lock{buffer_cache.mutex};
        seq = buffer_cache_runtime.BeginRecords();
        work();
        records = buffer_cache_runtime.EndRecords();
    }
    SendStageRecords(seq, records);
}

void RasterizerVulkan::FlushStageCWork() {
    namespace Stages = VideoCommon::Nx2Stages;
    if (stage_c_ranges.empty() || Nx2Record::HeldLocks::Count() != 0) {
        // With a cache lock held: at the next send (K3).
        return;
    }
    const size_t ranges = stage_c_ranges.size() / 2;
    if (OwnsRecordsB()) [[unlikely]] {
        // C parked: its half here.
        RunStageInvalidations(stage_c_ranges.data(), static_cast<u32>(ranges));
        stage_c_ranges.clear();
        return;
    }
    for (size_t first = 0; first < ranges; first += StageRangesPerRecord) {
        const size_t count = (std::min)(StageRangesPerRecord, ranges - first);
        u64* const out = Stages::Reserve(Stages::Ring::ToC, &StageInvalidateThunk,
                                         static_cast<u32>(1 + 2 * count));
        out[0] = reinterpret_cast<u64>(this);
        std::memcpy(out + 1, stage_c_ranges.data() + first * 2, count * 2 * sizeof(u64));
        Stages::Commit(Stages::Ring::ToC);
    }
    stage_c_ranges.clear();
}

void RasterizerVulkan::SendStageRecords(u64 seq, RecordList* records) {
    if (records == nullptr) {
        // C notes the seq with a later one.
        FlushStageCWork();
        return;
    }
    StageToC([this, records, seq] { RunStageRecords(records, seq); });
}

void RasterizerVulkan::SendStageDraw() {
    namespace Stages = VideoCommon::Nx2Stages;
    VideoCommon::Nx2Profile::FineScope scope{VideoCommon::Nx2Profile::Zone::PacketSend};
    FlushStageCWork();
    if (OwnsRecordsB()) [[unlikely]] {
        // C parked: the packet runs here (no draw is set up in a join's op so far).
        std::vector<u64> words(stage_writer.Words());
        stage_writer.WriteTo(words.data());
        StageDrawThunk(words.data(), static_cast<u32>(words.size()));
        return;
    }
    u64* const out = Stages::Reserve(Stages::Ring::ToC, &StageDrawThunk, stage_writer.Words());
    stage_writer.WriteTo(out);
    Stages::Commit(Stages::Ring::ToC);
}

u64 RasterizerVulkan::NextStageSeq() const noexcept {
    // While staged, what the runtime stamps with: the seq its next packet gets.
    return Nx2Record::SeqTicks::SeqOf(buffer_cache_runtime.UsageStamp());
}

void RasterizerVulkan::MarkStreamsB() {
    VideoCommon::Nx2Profile::FineScope scope{VideoCommon::Nx2Profile::Zone::MarkStreams};
    buffer_cache.MarkVertexStreamsUsed();
}

void RasterizerVulkan::NotePoolsB(const Tegra::Engines::Maxwell3D& mb, u64 seq) {
    // The same two pools draw after draw: InFlightRanges looks at its last entries first.
    const u64 ran = VideoCommon::Nx2Stages::GpuPages().RanSeq();
    const auto note = [&](GPUVAddr pool, u32 limit) {
        if (pool != 0) {
            stage_pools.Note(pool, VideoCommon::Nx2Stages::PoolEnd(pool, limit), seq, ran);
        }
    };
    const auto& regs = mb.regs;
    note(regs.tex_header.Address(), regs.tex_header.limit);
    note(regs.tex_sampler.Address(), SamplerPoolLimitB(mb));
}

u32 RasterizerVulkan::SamplerPoolLimitB(const Tegra::Engines::Maxwell3D& mb) {
    const auto& regs = mb.regs;
    return VideoCommon::Nx2Stages::SamplerPoolLimit(
        regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding, regs.tex_header.limit,
        regs.tex_sampler.limit);
}

bool RasterizerVulkan::WritesPoolsB(GPUVAddr address, u64 size) {
    return stage_pools.Overlaps(address, address + size,
                                VideoCommon::Nx2Stages::GpuPages().RanSeq());
}

bool RasterizerVulkan::WritesAnyPoolB(const Tegra::Engines::Maxwell3D& mb, GPUVAddr address,
                                      u64 size) {
    using VideoCommon::Nx2Stages::WritesPool;
    const auto& regs = mb.regs;
    return WritesPool(regs.tex_header.Address(), regs.tex_header.limit, address, size) ||
           WritesPool(regs.tex_sampler.Address(), SamplerPoolLimitB(mb), address, size) ||
           WritesPoolsB(address, size);
}

bool RasterizerVulkan::CbWriteCheckHook(void* context, Tegra::Engines::Maxwell3D& replica,
                                        GPUVAddr address, u64 size) {
    return static_cast<RasterizerVulkan*>(context)->CbWriteJoinsB(replica, address, size);
}

bool RasterizerVulkan::CbWriteJoinsB(Tegra::Engines::Maxwell3D& replica, GPUVAddr address,
                                     u64 size) {
    // omur-1's constant buffer data path (T9c's check, GpuHandler::CbData): B writes the data to
    // guest memory itself (Maxwell3D::StageCbWrite). Into a pool C may still read: the write in a
    // join, as InlineToMemoryB's.
    if (WritesAnyPoolB(replica, address, size)) {
        // GpuHandler::CbData joins at once (Nx2Stages::JoinFromB, not JoinB): what B has for C
        // goes first, and what C runs before it parks needs no tracking after (T7d review
        // cbhook-2: JoinB's bookkeeping).
        FlushStageCWork();
        stage_pools.Clear();
        stage_query_writes.Clear();
        return true;
    }
    // Over a payload release C has not written yet (qw-cbdata, T7c review finding 3): C writes it
    // first. A counter C writes from a later fence is cancelled where the old path cancels it: by
    // the invalidation at the memory manager's next FlushCaching (InnerInvalidationB, in ring 3's
    // order). A write over more than one device range: any release (cbhook-3).
    if (stage_query_writes.Size() != 0) [[unlikely]] {
        Tegra::MemoryManager& memory = replica.GetMemoryManager();
        GuardQueryWritesB(memory.IsContinuousRange(address, size) ? memory.GpuToCpuAddress(address)
                                                                  : std::nullopt,
                          size, false);
    }
    return false;
}

void RasterizerVulkan::PreSetTargets(Tegra::Engines::Maxwell3D& mb, u64 seq, bool light) {
    namespace Stages = VideoCommon::Nx2Stages;
    Stages::Nx2GpuPages& pages = Stages::GpuPages();
    if (!pages.Live()) {
        // Off: Holds says every page may hold data.
        return;
    }
    StageTargets& targets = stage_targets;
    const auto preset_ranges = [&] {
        for (const auto& [begin, size] : targets.ranges) {
            pages.PreSet(begin, size, seq);
        }
        if (targets.ranges.empty()) {
            // No target: only B's position (b_next, STAGE_CHECK's) moves; no page is held.
            pages.PreSet(0, 0, seq);
        }
    };
    // A light draw: its render target registers are the packet before's (they are
    // LightChange::State, which keeps a draw off the light way; a clear or a texture draw in
    // between is a Command), whose call found `key` for them (`current`) and its ranges; they
    // stand while the memory manager and its mappings do. ~30-90 ns a draw of building the key and
    // comparing it (k2hiz). STAGE_CHECK and =2 build it all the same and compare (below).
    const bool keep = light && targets.valid && targets.current &&
                      targets.gpu_memory == gpu_memory &&
                      targets.memory_id == gpu_memory->GetID() &&
                      targets.mapping_epoch == gpu_memory->MappingEpoch();
    if (keep && !pages.Checking()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::PresetsKept, 1);
        preset_ranges();
        return;
    }
    // The bound targets' GPU ranges, conservatively (D6): a layer's stride (pitch linear: pitch
    // times height; neither: 16 bytes a texel) times the layers or depth. A target larger than
    // MaxTargetBytes holds every page instead of a walk that long (R2 dogrulama-4). Not covered:
    // a target that is a layer or level of a larger image, of which C marks the whole image
    // (Nx2MarkImagePages) once it ran the packet, B only the target's range before.
    const auto& regs = mb.regs;
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    static constexpr u64 MaxTargetBytes = u64{256} << 20;
    const auto times = [](u64 a, u64 b) {
        return a != 0 && b > ~u64{0} / a ? ~u64{0} : a * b;
    };
    bool oversized = false;
    const auto target_bytes = [&](u64 stride, u64 linear, u64 width, u64 height, u64 layers,
                                  u64 texel) {
        u64 layer = (std::max)(stride, linear);
        if (layer == 0) {
            layer = times(times(width, height), texel);
        }
        const u64 bytes = times(layer, (std::max)(layers, u64{1}));
        oversized |= bytes > MaxTargetBytes;
        return (std::min)(bytes, MaxTargetBytes);
    };
    decltype(StageTargets::key) key{};
    const u32 count = (std::min)(static_cast<u32>(regs.rt_control.count),
                                 static_cast<u32>(Regs::NumRenderTargets));
    for (u32 index = 0; index < count; ++index) {
        // The targets C binds and writes: TextureCache::FindColorBuffer's rt[index] (Vulkan leaves
        // rt_control's map to OpenGL's draw buffers; the two change together).
        const auto& rt = regs.rt[index];
        if (rt.Address() == 0 || rt.format == Tegra::RenderTargetFormat::NONE) {
            continue;
        }
        const u64 linear = rt.tile_mode.is_pitch_linear ? u64{rt.width} * rt.height : 0;
        key[index] = {rt.Address(), target_bytes(u64{rt.array_pitch} * 4, linear, rt.width,
                                                 rt.height, rt.depth.Value(), 16)};
    }
    if (regs.zeta_enable != 0 && regs.zeta.Address() != 0) {
        const u64 layers = regs.zeta_size.dim_control ==
                                   Regs::ZetaSize::DimensionControl::DefineArraySize
                               ? u64{regs.zeta_size.depth.Value()}
                               : 1;
        key[Regs::NumRenderTargets] = {
            regs.zeta.Address(), target_bytes(u64{regs.zeta.array_pitch} * 4, 0,
                                              regs.zeta_size.width, regs.zeta_size.height,
                                              layers, 8)};
    }
    if (oversized) [[unlikely]] {
        targets.current = false;
        pages.PreSetAll(seq);
        return;
    }
    if (keep && targets.key != key) [[unlikely]] {
        Stages::ReportStageHazard("a light draw's render targets differ from the packet before's");
    }
    // Read before the walk: a mapping changed meanwhile walks again at the next packet.
    const u64 mapping_epoch = gpu_memory->MappingEpoch();
    if (!targets.valid || targets.gpu_memory != gpu_memory ||
        targets.memory_id != gpu_memory->GetID() || targets.mapping_epoch != mapping_epoch ||
        targets.key != key) {
        // Device pages found page by page, contiguous ones as one range (PreSetGpu's walk, kept
        // for the packets after while the registers stay).
        targets.ranges.clear();
        for (const auto& [address, size] : key) {
            DAddr run_begin = 0;
            DAddr run_end = 0;
            for (GPUVAddr page = address & ~GPUVAddr{0xfff}; size != 0 && page < address + size;
                 page += 0x1000) {
                const std::optional<DAddr> device_page = gpu_memory->GpuToCpuAddress(page);
                if (device_page && *device_page == run_end && run_end != run_begin) {
                    run_end += 0x1000;
                    continue;
                }
                if (run_end != run_begin) {
                    targets.ranges.emplace_back(run_begin, run_end - run_begin);
                }
                run_begin = device_page ? *device_page : 0;
                run_end = device_page ? *device_page + 0x1000 : 0;
            }
            if (run_end != run_begin) {
                targets.ranges.emplace_back(run_begin, run_end - run_begin);
            }
        }
        targets.key = key;
        targets.gpu_memory = gpu_memory;
        targets.memory_id = gpu_memory->GetID();
        targets.mapping_epoch = mapping_epoch;
        targets.valid = true;
    }
    targets.current = true;
    preset_ranges();
}

void RasterizerVulkan::StageInvalidateThunk(const u64* payload, u32 words) {
    auto* const rasterizer = reinterpret_cast<RasterizerVulkan*>(payload[0]);
    rasterizer->RunStageInvalidations(payload + 1, (words - 1) / 2);
}

void RasterizerVulkan::RunStageInvalidations(const u64* ranges, u32 count) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (u32 i = 0; i < count; ++i) {
            if ((ranges[i * 2 + 1] & StageRangeTexture) != 0) {
                texture_cache.WriteMemory(ranges[i * 2], ranges[i * 2 + 1] & StageRangeSize);
            }
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if ((ranges[i * 2 + 1] & StageRangeQuery) != 0) {
            query_cache.InvalidateRegion(ranges[i * 2], ranges[i * 2 + 1] & StageRangeSize);
        }
    }
}

void RasterizerVulkan::RunStageRecords(RecordList* records, u64 seq) {
    scheduler.Replay(*records, seq);
    NoteStageSeq(seq);
}

void RasterizerVulkan::NoteStageSeq(u64 seq) {
    LightAnchorC& anchor = light_anchor_c;
    anchor.last_seq = (std::max)(anchor.last_seq, seq);
    const u64 noted = anchor.hold_seq != 0 ? (std::min)(seq, anchor.hold_seq - 1) : seq;
    if (noted != 0) {
        scheduler.NoteSeqTick(noted);
    }
    // T8: C ran the work up to `seq` (its caches marked what it wrote): B's presets for it end.
    // The seq itself, the anchor's hold is the staging pool's alone.
    VideoCommon::Nx2Stages::GpuPages().Ran(seq);
}

void RasterizerVulkan::ReleaseAnchorC() {
    LightAnchorC& anchor = light_anchor_c;
    anchor.pipeline = nullptr;
    anchor.followable = false;
    anchor.payload.clear();
    if (anchor.hold_seq != 0) {
        anchor.hold_seq = 0;
        if (anchor.last_seq != 0) {
            scheduler.NoteSeqTick(anchor.last_seq);
        }
    }
}

void RasterizerVulkan::SaveAnchorC(const StageDrawReader& in, std::span<const u32> handles) {
    namespace Stages = VideoCommon::Nx2Stages;
    // A payload StageDrawReader reads: the head (no records, geometry or draw words), the
    // descriptor entries, the handles. Not from itself (a plain light draw keeps its anchor's).
    std::vector<u64>& payload = light_anchor_c.payload;
    const u64* const begin = payload.data();
    if (begin != nullptr && in.StageEntries(0) >= begin && in.StageEntries(0) < begin + payload.size())
        [[unlikely]] {
        return;
    }
    const u32 entry_words = in.EntryCount() * Stages::DrawEntryWords;
    const u32 handle_words = static_cast<u32>((handles.size() + 1) / 2);
    std::vector<u32> handle_copy(handles.begin(), handles.end());
    payload.assign(Stages::DrawHeadWords + entry_words + handle_words, 0);
    Stages::DrawToC head = in.Head();
    head.records = nullptr;
    head.handles = static_cast<u16>(handle_copy.size());
    head.geometry_slots = 0;
    head.draw_words = 0;
    std::memcpy(payload.data(), &head, sizeof(head));
    if (entry_words != 0) {
        std::memcpy(payload.data() + Stages::DrawHeadWords, in.StageEntries(0),
                    size_t{entry_words} * sizeof(u64));
    }
    if (!handle_copy.empty()) {
        std::memcpy(payload.data() + Stages::DrawHeadWords + entry_words, handle_copy.data(),
                    handle_copy.size() * sizeof(u32));
    }
}

void RasterizerVulkan::DrawB(bool is_indexed, u32 instance_count) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    Tegra::Engines::Maxwell3D& mb = EngineB();
    // B's replica holds the draw's side state (the proxy's event record).
    const auto& draw_state = mb.draw_manager.draw_state;
    const bool shape_ok =
        instance_count == 1 &&
        draw_state.draw_mode == Tegra::Engines::Maxwell3D::DrawManager::DrawMode::General &&
        draw_state.inline_index_draw_indexes.empty() && LightDrawShape(draw_state, is_indexed);
    const DrawParams draw_params{MakeDrawParams(draw_state, instance_count, is_indexed)};
    StageDrawParams params{};
    params.command = StageCommand::Direct;
    params.base_instance = draw_params.base_instance;
    params.num_instances = draw_params.num_instances;
    params.base_vertex = draw_params.base_vertex;
    params.num_vertices = draw_params.num_vertices;
    params.first_index = draw_params.first_index;
    params.is_indexed = draw_params.is_indexed;
    PrepareDrawB(is_indexed, false, shape_ok, instance_count, params);
}

void RasterizerVulkan::DrawIndirectB() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    Tegra::Engines::Maxwell3D& mb = EngineB();
    const auto& indirect = mb.draw_manager.indirect_state;
    const bool shape_ok = indirect.is_indexed && !indirect.include_count &&
                          !indirect.is_byte_count &&
                          LightDrawShape(mb.draw_manager.draw_state, true);
    StageDrawParams params{};
    params.command = indirect.is_byte_count   ? StageCommand::ByteCount
                     : indirect.include_count ? StageCommand::IndirectCount
                                              : StageCommand::Indirect;
    params.is_indexed = indirect.is_indexed;
    params.max_draw_counts = static_cast<u32>(indirect.max_draw_counts);
    params.stride = static_cast<u32>(indirect.stride);
    buffer_cache.SetDrawIndirect(&indirect);
    PrepareDrawB(indirect.is_indexed, true, shape_ok, 1, params);
    buffer_cache.SetDrawIndirect(nullptr);
}

RasterizerVulkan::LightLevel RasterizerVulkan::FollowableB(const GraphicsPipeline& pipeline,
                                                           const Tegra::Engines::Maxwell3D& mb,
                                                           bool shape_ok) const {
    // LightFollowable without its C part (render targets and views to repeat: C's anchor).
    namespace Profile = VideoCommon::Nx2Profile;
    if (!shape_ok) {
        Profile::Add(Profile::Counter::LightRefusedShape, 1);
        return LightLevel::None;
    }
    if (mb.regs.transform_feedback_enabled != 0) {
        Profile::Add(Profile::Counter::LightRefusedFeedback, 1);
        return LightLevel::None;
    }
    if (pipeline.WritesMemory()) {
        Profile::Add(Profile::Counter::LightRefusedWrites, 1);
        return LightUniformMode() != 0 ? LightLevel::Uniform : LightLevel::None;
    }
    return LightLevel::Full;
}

bool RasterizerVulkan::LightUniformOkB(GraphicsPipeline& pipeline, Tegra::Engines::Maxwell3D& mb,
                                       bool invalidated, bool& rebind_storage) {
    // LightUniformOk on B's replica and anchor. The images written since (texture_cache's write
    // hits) are C's to check (b).
    namespace Profile = VideoCommon::Nx2Profile;
    const LightAnchorB& last = light_anchor_b;
    const auto& light = mb.light;
    const u64 cached_writes = gpu_memory->CachedWrites() - last.cached_writes;
    const u32 ranges = light.written_count;
    if (cached_writes != light.cb_writes - last.cb_writes || ranges > light.written.size() ||
        (invalidated && cached_writes == 0)) {
        Profile::Add(Profile::Counter::LightUniformMemory, 1);
        return false;
    }
    const std::span<const std::pair<GPUVAddr, u32>> written{light.written.data(), ranges};
    const u64 sparse_low = Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed);
    const u64 sparse_high = Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed);
    for (const auto& [address, size] : written) {
        if (address + size > sparse_low && address < sparse_high) {
            Profile::Add(Profile::Counter::LightUniformMemory, 1);
            return false;
        }
    }
    if (pipeline_cache.InvalidationHits() != last.shader_hits) {
        Profile::Add(Profile::Counter::LightUniformCaches, 1);
        return false;
    }
    const u32 storage_mode = LightStorageMode();
    const GraphicsPipeline::LightWords words = pipeline.CheckDescriptorWords(
        written, light.rebound, LightUniformValues(), storage_mode != 0 || Profile::Enabled());
    const bool storage_changed = (words.changed & GraphicsPipeline::StorageWords) != 0;
    if (storage_changed && Profile::Enabled()) {
        Profile::Add(Profile::Counter::LightStorageChanged, 1);
    }
    const u32 refusing =
        storage_mode != 0 ? words.changed & GraphicsPipeline::HandleWords : words.changed;
    if (refusing != 0) {
        Profile::Add((words.rebound_changed & refusing) != 0 ? Profile::Counter::LightUniformBinds
                     : (refusing & GraphicsPipeline::HandleWords) != 0
                         ? Profile::Counter::LightUniformHandles
                         : Profile::Counter::LightUniformStorage,
                     1);
        return false;
    }
    const auto& regs = mb.regs;
    const auto overlaps = [&](GPUVAddr begin, GPUVAddr end) {
        for (const auto& [address, size] : written) {
            if (address < end && begin < address + size) {
                return true;
            }
        }
        return false;
    };
    const auto entries_written = [&](GPUVAddr pool, u32 limit, std::span<const u32> used) {
        if (!overlaps(pool, pool + (u64{limit} + 1) * 32)) {
            return false;
        }
        for (const u32 index : used) {
            const GPUVAddr entry = pool + u64{index} * 32;
            if (overlaps(entry, entry + 32)) {
                return true;
            }
        }
        return false;
    };
    if (entries_written(regs.tex_header.Address(), regs.tex_header.limit,
                        pipeline.UsedTextureHeaders())) {
        Profile::Add(Profile::Counter::LightUniformTic, 1);
        return false;
    }
    const u32 tsc_limit = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding
                              ? regs.tex_header.limit
                              : regs.tex_sampler.limit;
    if (entries_written(regs.tex_sampler.Address(), tsc_limit, pipeline.UsedSamplers())) {
        Profile::Add(Profile::Counter::LightUniformTsc, 1);
        return false;
    }
    rebind_storage = storage_changed;
    return true;
}

void RasterizerVulkan::PrepareDrawB(bool is_indexed, bool indirect, bool shape_ok,
                                    u32 instance_count, StageDrawParams& params) {
    namespace Profile = VideoCommon::Nx2Profile;
    namespace Stages = VideoCommon::Nx2Stages;
    using LightChange = Tegra::Engines::Maxwell3D::LightChange;
    Tegra::Engines::Maxwell3D& mb = EngineB();
    params.topology = static_cast<u8>(mb.draw_manager.draw_state.topology);
    bool invalidated;
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        invalidated = gpu_memory->FlushCaching();
    }
    // As PrepareDraw's anchor, taken before this draw binds anything (B's fields).
    LightAnchorB anchor{
        .maxwell3d = &mb,
        .gpu_memory = gpu_memory,
        .epoch = light_epoch_b.load(std::memory_order_seq_cst),
        .cached_writes = gpu_memory->CachedWrites(),
        .same_writes = mb.light.same_writes,
        .cb_writes = mb.light.cb_writes,
        .shader_hits = pipeline_cache.InvalidationHits(),
        .topology = mb.draw_manager.draw_state.topology,
        .index_format = mb.draw_manager.draw_state.index_buffer.format,
        .is_indexed = is_indexed,
        .indirect = indirect,
    };
    LightLevel follows = LightLevel::None;
    SCOPE_EXIT {
        anchor.level = follows;
        light_anchor_b = anchor;
        auto& light = mb.light;
        light.changes = 0;
        light.first_blocking = 0;
        light.first_blocking_uniform = 0;
        light.written_count = 0;
        light.rebound.fill(0);
        light.armed = follows == LightLevel::Full;
    };
    // The arguments may be what a dispatch or draw just wrote: recorded in the packet before
    // the draw (DrawIndirect's barrier). A light draw follows no recorded command (TryLightDraw).
    const bool indirect_barrier = indirect && buffer_cache.TakeIndirectBarrier();
    const auto record_indirect_barrier = [&] {
        if (!indirect_barrier) {
            return;
        }
        Profile::Add(Profile::Counter::IndirectBarriers, 1);
        RecordTarget& target = buffer_cache_runtime.Target();
        target.RequestOutsideRenderPassOperationContext();
        target.Record([](vk::CommandBuffer cmdbuf) {
            static constexpr VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, barrier);
        });
    };
    // DrawIndirect's draw_func reads these after the setup.
    const auto take_indirect = [&] {
        if (!indirect) {
            return;
        }
        const auto [buffer, offset] = buffer_cache.GetDrawIndirectBuffer();
        params.indirect_buffer = HandleBits(buffer->Handle());
        params.indirect_offset = offset;
        if (params.command == StageCommand::IndirectCount) {
            const auto [count_buffer, count_offset] = buffer_cache.GetDrawIndirectCount();
            params.count_buffer = HandleBits(count_buffer->Handle());
            params.count_offset = count_offset;
        }
    };
    Stages::StageDrawWriter& out = stage_writer;
    const auto finish_head = [&](u64 seq, RecordList* records) {
        Profile::FineScope scope{Profile::Zone::PacketBuild};
        Stages::DrawToC& head = out.Head();
        head.seq = seq;
        head.records = records;
        head.light = {.epoch = anchor.epoch};
        // The geometry mirror: the packet's binds and the streams B keeps live (C binds them
        // again where its command buffer lost them, BindGeometryC). The runtime's own slots,
        // valid until its next BeginRecords: SendStageDraw copies them to ring 3.
        out.SetGeometry(buffer_cache_runtime.PacketGeometry());
        head.geometry_mirrored = buffer_cache_runtime.GeometryMirrored();
        head.vertex_live = buffer_cache.EnabledVertexBuffersMask();
        // Packed by command (a direct draw's 72 bytes in 24).
        std::array<u64, Stages::MaxDrawArgWords> packed;
        out.AddDrawWords(packed.data(), Stages::PackDrawArgs(params, packed.data()));
    };

    // The light decision: TryLightDraw's checks on what B sees.
    const LightAnchorB& last = light_anchor_b;
    bool try_plain = false;
    bool try_uniform = false;
    bool rebind_storage = false;
    if (LightDrawEnabled() && StageLightEnabled()) {
        Profile::FineScope decide_scope{Profile::Zone::LightDecide};
        const auto& light = mb.light;
        const auto& draw_state = mb.draw_manager.draw_state;
        const u32 uniform_mode = LightUniformMode();
        const u32 allowed = uniform_mode != 0 ? LightChange::Uniform : LightChange::Light;
        if (last.level == LightLevel::None) {
            Profile::Add(Profile::Counter::LightNotArmed, 1);
        } else if ((light.changes & ~allowed) != 0) {
            Profile::Add((light.changes & LightChange::Command) != 0
                             ? Profile::Counter::LightOtherWork
                             : Profile::Counter::LightStateChanged,
                         1);
        } else if (last.maxwell3d != &mb || last.gpu_memory != gpu_memory ||
                   last.epoch != anchor.epoch || indirect_barrier) {
            Profile::Add(Profile::Counter::LightOtherWork, 1);
        } else if (!shape_ok || indirect != last.indirect || is_indexed != last.is_indexed ||
                   draw_state.topology != last.topology ||
                   (is_indexed && draw_state.index_buffer.format != last.index_format) ||
                   mb.regs.transform_feedback_enabled != 0) {
            Profile::Add(Profile::Counter::LightShape, 1);
        } else {
            const u64 cached_writes = anchor.cached_writes - last.cached_writes;
            const u64 same_writes = light.same_writes - last.same_writes;
            try_plain = last.level == LightLevel::Full &&
                        (light.changes & ~LightChange::Light) == 0 &&
                        cached_writes == same_writes && !(invalidated && cached_writes == 0);
            if (!try_plain) {
                if (uniform_mode == 0) {
                    Profile::Add(Profile::Counter::LightOtherWork, 1);
                } else {
                    try_uniform = LightUniformOkB(*last.pipeline, mb, invalidated, rebind_storage);
                }
            }
        }
    }
    if (try_plain || try_uniform) {
        GraphicsPipeline& pipeline = *last.pipeline;
        bool light_ok = false;
        u64 seq;
        RecordList* records;
        out.Begin();
        {
            Profile::Scope scope{Profile::Zone::LightDraw};
            Nx2Record::CacheLock lock{buffer_cache.mutex};
            seq = buffer_cache_runtime.BeginRecords(GeometryMirrorBinds());
            ForceGeometryB(mb);
            if (try_plain) {
                // ConfigureImpl's geometry buffers (TryLightDraw); an upload needs the full
                // setup's barrier.
                Profile::FineScope geometry_scope{Profile::Zone::LightGeometry};
                if (buffer_cache.UpdateGeometryBuffers(is_indexed)) {
                    // EDEN_NX2_LIGHT_SYNC_SKIP, as TryLightDraw: plain here by B's checks (its
                    // replica's light changes, its memory manager's cached and same-value writes,
                    // light_epoch_b), so B's chain synchronized every enabled stream after the
                    // last memory change; only streams flagged dirty bind anew (ForceGeometryB's
                    // flag them all), a clean stream whose range may hold CPU-modified or
                    // untracked pages since is synchronized anyway (UnsetSince). Without it each
                    // plain light draw synchronized its ~12 clean streams again (85: twice K1's
                    // buffer syncs a draw).
                    buffer_cache.plain_light_draw = VideoCommon::Nx2LightSyncSkip() != 0;
                    buffer_cache.BindHostGeometryBuffers(is_indexed);
                    buffer_cache.plain_light_draw = false;
                    light_ok = !buffer_cache.any_buffer_uploaded;
                }
                if (light_ok) {
                    Stages::DrawToC& head = out.Head();
                    head.kind = Stages::DrawKind::LightPlain;
                    head.pipeline = &pipeline;
                    head.is_indexed = is_indexed;
                } else {
                    Profile::Add(Profile::Counter::LightFellBack, 1);
                }
            } else {
                // The anchor's SetEngine holds (same replica and memory manager).
                Profile::FineScope uniform_scope{Profile::Zone::LightUniform};
                light_ok = pipeline.ConfigureUniformsBuffers(is_indexed, rebind_storage, out);
                if (!light_ok) {
                    Profile::Add(Profile::Counter::LightUniformFellBack, 1);
                    if (rebind_storage) {
                        Profile::Add(Profile::Counter::LightStorageFellBack, 1);
                    }
                }
            }
            if (light_ok) {
                // Every stream the draw reads, bound again or not (geo-markusage-1).
                MarkStreamsB();
                take_indirect();
                // EDEN_NX2_GPU_PROFILE: the streams this draw bound, as TryLightDraw counts them.
                buffer_cache.CommitVertexStreamNote();
            }
            records = buffer_cache_runtime.EndRecords();
        }
        if (light_ok) {
            finish_head(seq, records);
            {
                Profile::FineScope presets_scope{Profile::Zone::StagePresets};
                NotePoolsB(mb, seq);
                PreSetTargets(mb, seq, true);
                if (pipeline.WritesMemory()) {
                    VideoCommon::Nx2Stages::GpuPages().PreSetAll(seq);
                }
            }
            SendStageDraw();
            if (try_plain) {
                Profile::Add(Profile::Counter::LightDraws, 1);
                if (indirect) {
                    Profile::Add(Profile::Counter::LightIndirectDraws, 1);
                }
            } else {
                Profile::Add(Profile::Counter::LightUniformDraws, 1);
                if (indirect) {
                    Profile::Add(Profile::Counter::LightUniformIndirectDraws, 1);
                }
                if (rebind_storage) {
                    Profile::Add(Profile::Counter::LightStorageDraws, 1);
                }
            }
            // A light draw leaves the setup it followed.
            anchor.pipeline = last.pipeline;
            follows = last.level;
            return;
        }
        // The full setup follows; what was bound and uploaded so far stays valid (C replays it
        // first) and any_buffer_uploaded stays for its barrier. Its binds go without the geometry
        // mirror (a records packet; mirrored, C7, they are not in it at all): the full setup sends
        // every vertex stream, and each indexed or quad draw its index buffer.
        geometry_reset_b = true;
        SendStageRecords(seq, records);
    }

    GraphicsPipeline* const pipeline{[&] {
        Profile::Scope scope{Profile::Zone::PipelineLookup};
        return pipeline_cache.CurrentGraphicsPipeline();
    }()};
    if (!pipeline) {
        thread_local u32 logged{};
        if (logged < 16) {
            ++logged;
            LOG_WARNING(HW_GPU, "Draw skipped (no pipeline) on the memory stage");
        }
        // The barrier taken is recorded all the same, as DrawIndirect records it before
        // PrepareDraw (esdeger-4).
        if (indirect_barrier) {
            StageBufferWork(record_indirect_barrier);
        }
        FlushStageCWork();
        return;
    }
    if (StageResetProgram(mb, is_indexed)) {
        if (indirect_barrier) {
            StageBufferWork(record_indirect_barrier);
        }
        StageResetStores(mb);
        return;
    }
    if (pipeline->HasDynamicVertexInput()) {
        // FixedPipelineState::Refresh leaves it to UpdateVertexInput, which is C's on C's replica:
        // B's flag is cleared here, after the key read it (esdeger-3).
        mb.dirty.flags[Dirty::VertexInput] = false;
    }
    // Update the engine: B's replica (the channel may be different).
    pipeline->SetEngine(&mb, gpu_memory);
    if (pipeline->SplitJoins()) {
        // Texture / image buffers or transform feedback: the old setup, C parked (DrawKind::
        // Joined, done here: no packet). Whatever B sent before C ran first.
        if (indirect_barrier) {
            StageBufferWork(record_indirect_barrier);
        }
        FlushStageCWork();
        JoinB([&] {
            std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
            FlushWork();
            maxwell3d->draw_manager.draw_state.topology = mb.draw_manager.draw_state.topology;
            // B's replica's flags set up the buffers here too: C's submissions do not reach them
            // (esdeger-2). C's geometry mirror does not see these binds: B's next packet sends
            // every vertex stream (geometri-3).
            ForceVertexBinds(mb);
            geometry_reset_b = true;
            if (Profile::Scope scope{Profile::Zone::Configure}; !pipeline->Configure(is_indexed)) {
                return;
            }
            // Configure bound every vertex stream and cleared B's flag (UpdateVertexBuffers); C's
            // replica's, set by the deltas the join passed, is cleared too: no vertex input
            // recorded again for it alone (kayit-2, as PrepareDrawC after B's list).
            maxwell3d->dirty.flags[VideoCommon::Dirty::VertexBuffers] = false;
            {
                // The draw's pipeline, not the cache's current one (kayit-1).
                Profile::Scope scope{Profile::Zone::DynamicState};
                stage_dynamic = {.active = true, .pipeline = pipeline};
                UpdateDynamicStates();
                stage_dynamic = {};
            }
            query_cache.NotifySegment(true);
            HandleTransformFeedback();
            query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                                      maxwell3d->regs.zpass_pixel_count_enable);
            take_indirect();
            Profile::Scope scope{Profile::Zone::DrawRecord};
            RecordStageDraw(params);
        });
        return;
    }
    u64 seq;
    RecordList* records;
    out.Begin();
    {
        Nx2Record::CacheLock lock{buffer_cache.mutex};
        seq = buffer_cache_runtime.BeginRecords(GeometryMirrorBinds());
        record_indirect_barrier();
        ForceGeometryB(mb);
        GraphicsPipeline::BufferHalf half;
        {
            Profile::Scope scope{Profile::Zone::Configure};
            half = pipeline->ConfigureBuffers(is_indexed, out);
        }
        if (half == GraphicsPipeline::BufferHalf::Ready) {
            // Every stream the draw reads, bound again or not (geo-markusage-1).
            MarkStreamsB();
            take_indirect();
            buffer_cache.CommitVertexStreamNote();
        } else {
            // SplitJoins said no: nothing was done.
            LOG_ERROR(HW_GPU, "ConfigureBuffers joined a draw SplitJoins kept split");
            out.Begin();
            params.command = StageCommand::None;
        }
        records = buffer_cache_runtime.EndRecords();
    }
    if (params.command != StageCommand::None) {
        anchor.pipeline = pipeline;
        follows = LightDrawEnabled() && StageLightEnabled() ? FollowableB(*pipeline, mb, shape_ok)
                                                            : LightLevel::None;
    }
    finish_head(seq, records);
    {
        Profile::FineScope presets_scope{Profile::Zone::StagePresets};
        NotePoolsB(mb, seq);
        if (params.command != StageCommand::None) {
            // T8 (PLAN §7/25): what C may write for this packet, before it is sent.
            PreSetTargets(mb, seq);
            if (pipeline->WritesMemory()) {
                // Storage images / global stores B does not place.
                VideoCommon::Nx2Stages::GpuPages().PreSetAll(seq);
            }
        }
    }
    SendStageDraw();
}

bool RasterizerVulkan::StageResetProgram(Tegra::Engines::Maxwell3D& mb, bool is_indexed) {
    // PrepareDraw's RESETFIX detection, its answers kept here (B's thread).
    static const bool reset_fix = [] {
        const char* value = std::getenv("EDEN_NX2_RESETFIX");
        return value == nullptr || value[0] != '0';
    }();
    if (!reset_fix || is_indexed || !mb.GetNativeMacroEngine().native_turing) {
        return false;
    }
    static const bool reset_last = [] {
        const char* value = std::getenv("EDEN_NX2_RESET_LAST");
        return value == nullptr || value[0] != '0';
    }();
    const auto& regs = mb.regs;
    const GPUVAddr program =
        (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
    thread_local std::unordered_map<GPUVAddr, bool> reset_programs;
    thread_local GPUVAddr last_program{};
    thread_local bool last_known{};
    thread_local bool last_is_reset{};
    if (reset_last && last_known && program == last_program) {
        return last_is_reset;
    }
    auto [entry, inserted] = reset_programs.try_emplace(program, false);
    if (inserted) {
        std::array<u32, 44> code{};
        const GPUVAddr code_address = program + 128;
        if (gpu_memory->IsFullyMappedRange(code_address, sizeof(code))) {
            gpu_memory->ReadBlockUnsafe(code_address, code.data(), sizeof(code));
            entry->second = code == StageResetCode;
        }
        if (entry->second) {
            LOG_INFO(HW_GPU, "Indirect draw reset program at {:#x}: its stores are done here (B)",
                     program);
        }
    }
    last_program = program;
    last_known = true;
    last_is_reset = entry->second;
    return entry->second;
}

void RasterizerVulkan::StageResetStores(Tegra::Engines::Maxwell3D& mb) {
    // PrepareDraw's RESETFIX stores (its runs, sorted when the records share no byte), B's half:
    // the list flushed in a join, the inline writes in a packet (InlineRunsB).
    namespace Profile = VideoCommon::Nx2Profile;
    const auto& reset_draw = mb.draw_manager.draw_state;
    const auto& cbuf0 = mb.state.shader_stages[0].const_buffers[0];
    const GPUVAddr list = gpu_memory->Read<u64>(cbuf0.address + 0x110);
    const u32 count = (std::min)(reset_draw.vertex_buffer.count, 1u << 20);
    const GPUVAddr list_begin = list + u64{reset_draw.vertex_buffer.first} * sizeof(u64);
    const size_t list_bytes = size_t{count} * sizeof(u64);
    if (count == 0 || !gpu_memory->IsFullyMappedRange(list_begin, list_bytes)) {
        FlushStageCWork();
        return;
    }
    Profile::Add(Profile::Counter::ResetFixLists, 1);
    if (gpu_memory->IsMemoryDirty(list_begin, list_bytes, VideoCommon::CacheType::BufferCache)) {
        Profile::Add(Profile::Counter::ResetFixFlushes, 1);
        // A download records and waits: C parked, no cache lock held (K3).
        FlushStageCWork();
        JoinB([&] {
            gpu_memory->FlushRegion(list_begin, list_bytes, VideoCommon::CacheType::BufferCache);
        });
    }
    std::vector<u64> pointers(count);
    gpu_memory->ReadBlockUnsafe(list_begin, pointers.data(), list_bytes);
    static const bool runs_enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESET_RUNS");
        return value == nullptr || value[0] != '0';
    }();
    static const bool sort_enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESET_SORT");
        return value == nullptr || value[0] != '0';
    }();
    static constexpr u32 RecordSize = 5 * sizeof(u32);
    static constexpr u32 MaxRun = 4096 / RecordSize;
    static const auto run_words = [] {
        std::array<u32, 5 * MaxRun> words{};
        for (u32 i = 0; i < MaxRun; ++i) {
            words[i * 5 + 1] = 1;
        }
        return words;
    }();
    std::vector<u64> records;
    records.reserve(pointers.size());
    for (const u64 pointer : pointers) {
        if (pointer != 0 && gpu_memory->IsFullyMappedRange(pointer, RecordSize)) {
            records.push_back(pointer);
        }
    }
    if (runs_enabled && sort_enabled && !std::is_sorted(records.begin(), records.end())) {
        std::vector<u64> ordered(records);
        std::sort(ordered.begin(), ordered.end());
        const bool disjoint =
            std::adjacent_find(ordered.begin(), ordered.end(), [](u64 lower, u64 upper) {
                return upper - lower < RecordSize;
            }) == ordered.end();
        if (disjoint) {
            records = std::move(ordered);
        }
    }
    std::vector<std::pair<GPUVAddr, u32>> runs;
    runs.reserve(records.size());
    GPUVAddr run_start = 0;
    u32 run_records = 0;
    for (const u64 pointer : records) {
        if (runs_enabled && run_records != 0 && run_records < MaxRun &&
            pointer == run_start + u64{run_records} * RecordSize &&
            (run_start >> 12) == ((pointer + RecordSize - 1) >> 12)) {
            ++run_records;
            continue;
        }
        if (run_records != 0) {
            runs.emplace_back(run_start, run_records * RecordSize);
        }
        run_start = pointer;
        run_records = 1;
    }
    if (run_records != 0) {
        runs.emplace_back(run_start, run_records * RecordSize);
    }
    NoteLightWork();
    if (stage_query_writes.Any(VideoCommon::Nx2Stages::GpuPages().RanSeq()) ||
        stage_query_later.Size() != 0) [[unlikely]] {
        // Reports in flight: the runs that meet one (InlineRunsB writes inside the packet).
        for (const auto& [address, size] : runs) {
            GuardQueryWritesB(gpu_memory->GpuToCpuAddress(address), size, true);
        }
    }
    StageBufferWork([&] {
        InlineRunsB(runs, {reinterpret_cast<const u8*>(run_words.data()), sizeof(run_words)});
    });
    FlushStageCWork();
    thread_local u32 reset_logs{};
    if (reset_logs < 5) {
        ++reset_logs;
        LOG_INFO(HW_GPU, "Indirect draw reset (B): list {:#x}, {} records ({} written in {} runs)",
                 list_begin, count, records.size(), runs.size());
    }
}

void RasterizerVulkan::InlineRunsB(std::span<const std::pair<GPUVAddr, u32>> runs,
                                   std::span<const u8> data) {
    // AccelerateInlineRuns' steps with the buffer cache locked inside a packet: guest memory,
    // the buffer cache (its copies into B's list) and the shaders here, the query and texture
    // invalidations to C (stage_c_ranges, sent before the packet's records).
    namespace Profile = VideoCommon::Nx2Profile;
    const bool defer = InlineDeferEnabled();
    static const bool batch_enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESET_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    const bool batched = batch_enabled && !Settings::IsGPUFenceBehaviorAccurate();
    BufferCache::InlineBatch batch;
    u64 uploads = 0;
    u64 commands = 0;
    SCOPE_EXIT {
        buffer_cache.FlushInlineBatch(batch);
    };
    for (const auto& [address, size] : runs) {
        const std::span<const u8> bytes = data.first(size);
        const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(address);
        if (!cpu_addr) [[unlikely]] {
            // Not one device range: written now, every cache told at the next FlushCaching.
            commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
            gpu_memory->WriteBlockUnsafe(address, bytes.data(), size, true);
            gpu_memory->AccumulateInvalidation(address, size);
            continue;
        }
        gpu_memory->WriteBlockUnsafe(address, bytes.data(), size, true);
        bool uploaded;
        if (batched) {
            const size_t before = batch.copies.size();
            uploaded = buffer_cache.InlineMemoryBatched(*cpu_addr, size, bytes, batch);
            commands += uploaded && batch.copies.size() <= before ? 1 : 0;
        } else {
            uploaded = buffer_cache.InlineMemory(*cpu_addr, size, bytes);
            commands += uploaded ? 1 : 0;
        }
        uploads += uploaded ? 1 : 0;
        if (!uploaded && !defer) {
            commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
            buffer_cache.WriteMemory(*cpu_addr, size);
        }
        const bool deferred = !uploaded && defer;
        stage_c_ranges.push_back(*cpu_addr);
        stage_c_ranges.push_back(u64{size} | StageRangeQuery | (deferred ? 0 : StageRangeTexture));
        if (deferred) {
            gpu_memory->AccumulateInvalidation(address, size);
            continue;
        }
        pipeline_cache.InvalidateRegion(*cpu_addr, size);
    }
    commands += buffer_cache.FlushInlineBatch(batch) ? 1 : 0;
    Profile::Add(Profile::Counter::ResetUploads, uploads);
    Profile::Add(Profile::Counter::ResetCopyCommands, commands);
}

void RasterizerVulkan::InlineToMemoryB(GPUVAddr address, size_t copy_size,
                                       std::span<const u8> memory) {
    // AccelerateInlineToMemory's halves: guest memory, the buffer cache and the shaders here,
    // the query and texture invalidations C's (in ring 3's order, before anything after).
    {
        // eszaman-4 / omur-1: C reads the texture header and sampler pools from guest memory for
        // draws B sent before this write (the pools they used: stage_pools; B's current ones).
        // One into them is made in a join: C parked having run those, the old body here, its
        // invalidations in order. Not covered here: images C uploads from guest memory (a
        // summary of their pages from C). Constant buffer data B writes: CbWriteJoinsB.
        if (WritesAnyPoolB(EngineB(), address, copy_size)) {
            JoinB([&] { AccelerateInlineToMemory(address, copy_size, memory); });
            return;
        }
    }
    NoteLightWork();
    const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(address);
    GuardQueryWritesB(cpu_addr, copy_size, true);
    if (!cpu_addr) [[unlikely]] {
        // WriteBlock would invalidate C's caches from here: written now, every cache told at
        // the next FlushCaching (which runs before anything of theirs is used).
        gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size, true);
        gpu_memory->AccumulateInvalidation(address, copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size, true);
    const bool defer = InlineDeferEnabled();
    bool deferred = false;
    StageBufferWork([&] {
        const bool uploaded = buffer_cache.InlineMemory(*cpu_addr, copy_size, memory);
        if (!uploaded && !defer) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
        deferred = !uploaded && defer;
        stage_c_ranges.push_back(*cpu_addr);
        stage_c_ranges.push_back(u64{copy_size} | StageRangeQuery |
                                 (deferred ? 0 : StageRangeTexture));
        if (deferred) {
            gpu_memory->AccumulateInvalidation(address, copy_size);
        }
    });
    FlushStageCWork();
    if (!deferred) {
        pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
    }
}

void RasterizerVulkan::InnerInvalidationB(
    std::span<const std::pair<DAddr, std::size_t>> sequences) {
    // InnerInvalidation's halves: the buffer cache and the shaders here, the texture and query
    // caches C's.
    for (const auto& [addr, size] : sequences) {
        stage_c_ranges.push_back(addr);
        stage_c_ranges.push_back(u64{size} | StageRangeTexture | StageRangeQuery);
    }
    StageBufferWork([&] { buffer_cache.WriteMemoryRanges(sequences); });
    FlushStageCWork();
    for (const auto& [addr, size] : sequences) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InvalidateRegionB(DAddr addr, u64 size, VideoCommon::CacheType which,
                                         bool cached_write) {
    // InvalidateRegion's halves (SplitCaches): the buffer cache and the shaders here, the texture
    // and query caches C's (before anything B sends after).
    using VideoCommon::CacheType;
    NoteLightWork();
    if (addr == 0 || size == 0) {
        return;
    }
    // The memory manager's WriteBlock invalidates each range before it writes it.
    GuardQueryWritesB(addr, size, True(which & CacheType::QueryCache));
    const VideoCommon::Nx2Stages::CacheHalves halves = VideoCommon::Nx2Stages::SplitCaches(which);
    const u64 c_bits = (True(halves.c & CacheType::TextureCache) ? StageRangeTexture : 0) |
                       (True(halves.c & CacheType::QueryCache) ? StageRangeQuery : 0);
    if (c_bits != 0) {
        stage_c_ranges.push_back(addr);
        stage_c_ranges.push_back(size | c_bits);
    }
    if (True(halves.b & CacheType::BufferCache)) {
        StageBufferWork([&] {
            if (cached_write) {
                buffer_cache.CachedWriteMemory(addr, size);
            } else {
                buffer_cache.WriteMemory(addr, size);
            }
        });
    }
    FlushStageCWork();
    if (True(halves.b & CacheType::ShaderCache)) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::FlushRegionB(DAddr addr, u64 size, VideoCommon::CacheType which) {
    // A download from C's caches and B's records and waits: the old body in a join, only where a
    // cache may hold GPU data (C's through the page summary, whole pages as HoldsGpuWrittenData).
    if (addr != 0 && size != 0) {
        const DAddr begin = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
        const size_t span = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE) - begin;
        if (HoldsGpuWrittenDataB(begin, span)) {
            JoinB([&] { FlushRegion(addr, size, which); });
            return;
        }
    }
    NoteLightWork();
}

bool RasterizerVulkan::MustFlushRegionB(DAddr addr, u64 size, VideoCommon::CacheType which) {
    // MustFlushRegion with C's texture cache through the page summary (conservative).
    namespace Stages = VideoCommon::Nx2Stages;
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        Nx2Record::CacheLock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh() || False(which & VideoCommon::CacheType::TextureCache)) {
        return false;
    }
    const DAddr begin = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
    const size_t span = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE) - begin;
    if (Stages::Get().mode == 2) [[unlikely]] {
        // =2's lockstep: the cache's answer at the same point of the stream (HoldsGpuWrittenDataB).
        bool modified;
        {
            std::scoped_lock lock{texture_cache.mutex};
            modified = texture_cache.IsRegionGpuModified(addr, size);
        }
        return Stages::GpuPages().CheckHolds(begin, span, modified);
    }
    return Stages::GpuPages().Holds(begin, span);
}

bool RasterizerVulkan::HoldsGpuWrittenDataB(DAddr begin, size_t span) {
    // C's caches (images, pending query results, writes around them) through T8's page summary,
    // lock-free (PLAN §7/25); the buffer cache is B's.
    namespace Stages = VideoCommon::Nx2Stages;
    Stages::Nx2GpuPages& pages = Stages::GpuPages();
    bool holds;
    if (Stages::Get().mode == 2) [[unlikely]] {
        // =2's lockstep: C ran everything B sent (B waits for each record of ring 3) and waits,
        // so the caches' own answer is at the same point of the stream (STAGE_CHECK counts a
        // page the summary misses: HazardPages).
        bool caches_hold = query_cache.HoldsQueries(begin, span);
        if (!caches_hold) {
            std::scoped_lock lock{texture_cache.mutex};
            caches_hold = texture_cache.IsRegionGpuModified(begin, span);
        }
        holds = pages.CheckHolds(begin, span, caches_hold);
    } else {
        holds = pages.Holds(begin, span);
    }
    if (holds) {
        return true;
    }
    Nx2Record::CacheLock lock{buffer_cache.mutex};
    if (buffer_cache.IsRegionGpuModified(begin, span)) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::HoldsGpuWrittenBuffer, 1);
        return true;
    }
    return false;
}

void RasterizerVulkan::ClearB(u32 layer_count) {
    // B: Clear's FlushCaching (C's half of its invalidations before the clear); C the rest.
    NoteLightWork();
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::Clears, 1);
    gpu_memory->FlushCaching();
    // T8: the clear writes the bound targets, before the next packet.
    PreSetTargets(EngineB(), NextStageSeq());
    StageToC([this, layer_count] {
        VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::Clear};
        FlushWork();
        ClearRecord(layer_count);
    });
}

void RasterizerVulkan::DrawTextureB() {
    // All C's: the texture cache and the recording, with the side state of B's replica.
    NoteLightWork();
    const auto state = EngineB().draw_manager.draw_texture_state;
    // T8: the blit writes the bound targets, before the next packet.
    PreSetTargets(EngineB(), NextStageSeq());
    NotePoolsB(EngineB(), NextStageSeq());
    StageToC([this, state] {
        maxwell3d->draw_manager.draw_texture_state = state;
        // The blit binds its own pipeline: no draw's pipeline-dependent state now (kayit-1).
        stage_dynamic = {.active = true, .pipeline = nullptr};
        DrawTexture();
        stage_dynamic = {};
    });
}

void RasterizerVulkan::WaitForIdleB() {
    if (stage_query_since_wfi) {
        // F3: the query cache's NotifyWFI may sync query writes now (SyncValues: the buffer
        // cache's), so the old body runs in a join, C parked.
        stage_query_since_wfi = false;
        JoinB([this] { WaitForIdle(); });
        return;
    }
    // No query since: B the ordering signal (the Vulkan fence manager: the buffer cache's flushes
    // to gather); C the query cache (nothing to sync) and the wait recorded.
    NoteLightWork();
    fence_manager.SignalOrdering();
    StageToC([this] { WaitForIdleRecord(); });
}

void RasterizerVulkan::Nx2MembarB() {
    // EDEN_NX2_MEMBAR: its query notice as WaitForIdle's (F3), the barrier recorded by C.
    if (stage_query_since_wfi) {
        stage_query_since_wfi = false;
        JoinB([this] { Nx2Membar(); });
        return;
    }
    NoteLightWork();
    fence_manager.SignalOrdering();
    StageToC([this] { Nx2MembarRecord(); });
}

// C takes from the pipeline cache only what the packet names, the GraphicsPipeline (DrawToC::
// pipeline; no ShaderInfo* or other entry of B's caches, T9c): B's
// pipeline_cache.InvalidateRegion never frees one (graphics_cache only grows). An eviction would
// have to wait for the seq C ran.
static_assert(
    std::is_same_v<decltype(VideoCommon::Nx2Stages::DrawToC::pipeline), GraphicsPipeline*>);

void RasterizerVulkan::StageDrawThunk(const u64* payload, u32 words) {
    const StageDrawReader in{payload, words};
    const StageDrawParams params = VideoCommon::Nx2Stages::UnpackDrawArgs(in.DrawWords());
    stage_rasterizer->PrepareDrawC(in, params);
}

void RasterizerVulkan::PrepareDrawC(const StageDrawReader& in, const StageDrawParams& params) {
    // PrepareDraw's recording half: B's list replayed, the texture half (ConfigureTextures), or
    // a light draw checked and, failing, repaired; dynamic state, queries and the draw.
    namespace Profile = VideoCommon::Nx2Profile;
    namespace Stages = VideoCommon::Nx2Stages;
    using Stages::DrawKind;
    const Stages::DrawToC& head = in.Head();
    SCOPE_EXIT {
        gpu.TickWork();
    };
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
    }
    LightAnchorC& anchor = light_anchor_c;
    auto& mc = *maxwell3d;
    if (params.command == StageCommand::None) {
        // A draw cut short on B: only its records.
        const u64 replay_tick = scheduler.CurrentTick();
        if (head.records != nullptr) {
            scheduler.Replay(*head.records, head.seq);
        }
        ApplyGeometryC(in, replay_tick);
        mc.dirty.flags[VideoCommon::Dirty::VertexBuffers] = false;
        ReleaseAnchorC();
        NoteStageSeq(head.seq);
        return;
    }
    const bool light = head.kind == DrawKind::LightPlain || head.kind == DrawKind::LightUniform;
    // (a) the command state the anchor left and (b) its images, before B's list adds to them.
    const u64 image_hits = texture_cache.WriteHits();
    const bool state_ok = !light || (anchor.followable && anchor.pipeline == head.pipeline &&
                                     scheduler.RecordCount() == anchor.record_count);
    const bool textures_ok = !light || image_hits == anchor.image_hits;
    mc.draw_manager.draw_state.topology =
        static_cast<Maxwell::PrimitiveTopology>(params.topology);
    std::unique_lock lock{texture_cache.mutex};
    const u64 replay_tick = scheduler.CurrentTick();
    if (head.records != nullptr) {
        scheduler.Replay(*head.records, head.seq);
    }
    ApplyGeometryC(in, replay_tick);
    // B's list bound the vertex streams (BufferCache::UpdateVertexBuffers clears this flag before
    // UpdateDynamicStates on one replica; C's is set by register deltas and InvalidateState): no
    // vertex input recorded again for it alone (esdeger-3).
    mc.dirty.flags[VideoCommon::Dirty::VertexBuffers] = false;
    GraphicsPipeline& pipeline = *head.pipeline;
    const auto repair = [&](bool plain) {
        Profile::Scope scope{Profile::Zone::LightRepair};
        Profile::Add(state_ok ? Profile::Counter::LightRepairsTexture
                              : Profile::Counter::LightRepairsState,
                     1);
        if (!state_ok) {
            Profile::Add(!anchor.followable                ? Profile::Counter::RepairsNotFollowable
                         : anchor.pipeline != head.pipeline ? Profile::Counter::RepairsPipeline
                                                            : Profile::Counter::RepairsRecords,
                         1);
        }
        if (anchor.pipeline != &pipeline || (plain && anchor.payload.empty())) {
            // No anchor here to repair with (its setup failed, or the stages just began).
            return false;
        }
        if (plain) {
            const StageDrawReader anchor_in{anchor.payload.data(),
                                            static_cast<u32>(anchor.payload.size())};
            return pipeline.ConfigureTextures(anchor_in, mc);
        }
        // B's light decision says the handles are the anchor's.
        return pipeline.ConfigureTextures(in.WithHandles(pipeline.LastHandles()), mc);
    };
    bool set_up = true;
    bool configured = false;
    switch (head.kind) {
    case DrawKind::Full: {
        Profile::Scope scope{Profile::Zone::Configure};
        configured = pipeline.ConfigureTextures(in, mc);
        break;
    }
    case DrawKind::LightPlain:
        if (state_ok && textures_ok && scheduler.IsRenderPassActive()) {
            set_up = false;
            configured = true;
        } else {
            configured = repair(true);
        }
        break;
    case DrawKind::LightUniform:
        if (state_ok && textures_ok && pipeline.RecordUniformsFrom(in)) {
            set_up = false;
            configured = true;
        } else {
            configured = repair(false);
        }
        break;
    case DrawKind::Joined:
        LOG_ERROR(HW_GPU, "Joined draw packet on the recording stage");
        break;
    }
    if (!configured) {
        thread_local u32 logged{};
        if (logged < 16) {
            ++logged;
            LOG_WARNING(HW_GPU, "Draw skipped (configure failed) on the recording stage{}",
                        light ? ", a light draw without its anchor" : "");
        }
        // B's chain would go on from an anchor C does not have: its next draw in full.
        light_epoch_b.fetch_add(1, std::memory_order_seq_cst);
        ReleaseAnchorC();
        NoteStageSeq(head.seq);
        return;
    }
    if (set_up) {
        // The packet's pipeline: the pipeline cache is B's (kayit-1 / esdeger-1).
        Profile::Scope scope{Profile::Zone::DynamicState};
        stage_dynamic = {.active = true, .pipeline = &pipeline};
        UpdateDynamicStates();
        stage_dynamic = {};
    }
    if (Stages::Get().mode == 2) [[unlikely]] {
        // The double setup parks B: not with the texture cache held (B's joins take it). Before
        // the queries begin, as the old path checks (R2 dogrulama-2): a render pass the old setup
        // ends would end them, and the draw would record outside.
        const SplitCheckBase base{
            .image_hits = image_hits,
            .view_epoch = texture_cache.ImageViewEpoch(),
            .light_epoch = head.light.epoch,
        };
        lock.unlock();
        CheckSplitSetup(pipeline, head.kind == DrawKind::Full ? in.Handles() : pipeline.LastHandles(),
                        head.is_indexed, light, !set_up, base);
        lock.lock();
    }
    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              mc.regs.zpass_pixel_count_enable);
    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
        BindGeometryC(head);
        RecordStageDraw(params);
    }
    // The anchor: what the next light draw is checked against and repaired with. A plain light
    // draw keeps the descriptor entries (and the seq holding their uniform data) it followed.
    if (head.kind == DrawKind::Full || head.kind == DrawKind::LightUniform) {
        SaveAnchorC(in, head.kind == DrawKind::Full ? in.Handles() : pipeline.LastHandles());
        anchor.hold_seq = head.seq;
    }
    anchor.pipeline = &pipeline;
    anchor.image_hits = image_hits;
    anchor.record_count = scheduler.RecordCount();
    if (set_up) {
        // EDEN_NX2_RX_RT_FIT=3: C's light draws record no feedback barrier; a draw with one is
        // repaired (set up in full: ConfigureDraw records it).
        anchor.followable = !texture_cache.RenderTargetsNeedRepeat() &&
                            !pipeline.ViewsNeedRepeat() && !texture_cache.Nx2InPassFeedback();
        if (!anchor.followable) {
            Profile::Add(Profile::Counter::LightRefusedTargets, 1);
            // B does not see this part of LightFollowable: its next draw in full. The light
            // draws it sends meanwhile are repaired with the anchor's payload, so its seq stays
            // held (esdeger-5 / omur-2): only a new anchor or ReleaseAnchorC (TickFrame, a broken
            // chain) lets go.
            light_epoch_b.fetch_add(1, std::memory_order_seq_cst);
        }
    }
    auto& light_state = mc.light;
    light_state.changes = 0;
    light_state.written_count = 0;
    NoteStageSeq(head.seq);
}

void RasterizerVulkan::ApplyGeometryC(const StageDrawReader& in, u64 replay_tick) {
    // The streams the packet bound are in the command buffer now, unless B's record owner hook
    // replayed them in a join before (a submission may have come after) or a submission came
    // while they were replayed (not expected: a packet's Finish, STAGE_CHECK's hazard): bound
    // again then.
    // A packet B mirrored (C7) has its vertex and index binds in its geometry slots alone: C
    // records them here, after the list, in this command buffer.
    const u64 tick = scheduler.CurrentTick();
    bool early = std::exchange(geometry_replayed_c, false) || tick != replay_tick;
    if (in.Head().geometry_mirrored) {
        RecordMirroredGeometry(scheduler, device, in.Geometry());
        early = false;
    }
    const u32 bound =
        geometry_mirror_c.Apply(in.Geometry(), in.Head().vertex_live, tick, early);
    auto& flags = maxwell3d->dirty.flags;
    for (u32 mask = bound; mask != 0; mask &= mask - 1) {
        flags[VideoCommon::Dirty::VertexBuffer0 + std::countr_zero(mask)] = early;
    }
}

void RasterizerVulkan::BindGeometryC(const VideoCommon::Nx2Stages::DrawToC& head) {
    // R2 kayit-4 / T3 R2 birakma-1: B's binds were replayed before the texture half; since, a
    // submission (FlushWork, a join's) or InvalidateState (a blit's pipeline leaves the dynamic
    // strides undefined) may have dropped them. C's replica's vertex flags say which (both set
    // them); those of the live streams bind again from the mirror, as B's runtime bound them.
    namespace Stages = VideoCommon::Nx2Stages;
    auto& flags = maxwell3d->dirty.flags;
    const u32 bindings = device.GetMaxVertexInputBindings();
    const u32 live = head.vertex_live & (bindings >= 32 ? ~0u : (1u << bindings) - 1);
    const u32 dirty = VertexDirtyMask(flags, live);
    const u32 rebind = geometry_mirror_c.Rebind(live, dirty);
    for (u32 mask = rebind; mask != 0; mask &= mask - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(mask));
        RecordMirroredGeometry(scheduler, device, {&geometry_mirror_c.Vertex(index), 1});
        flags[VideoCommon::Dirty::VertexBuffer0 + index] = false;
    }
    if (const u32 missing = geometry_mirror_c.Missing(live, dirty); missing != 0) [[unlikely]] {
        // Not expected: B sends every stream after binds outside a packet (ForceGeometryB).
        Stages::ReportStageHazard("a live vertex stream the geometry mirror does not know");
        thread_local u32 logged{};
        if (logged < 16) {
            ++logged;
            LOG_ERROR(HW_GPU, "Geometry mirror: live vertex streams {:#x} unknown, not bound",
                      missing);
        }
    }
    const bool rebind_index = geometry_mirror_c.RebindIndex(scheduler.CurrentTick());
    if (rebind_index) {
        RecordMirroredGeometry(scheduler, device, {&geometry_mirror_c.Index(), 1});
    }
    if (rebind != 0 || rebind_index) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::GeometryReplays, 1);
    }
}

void RasterizerVulkan::RecordStageDraw(const StageDrawParams& params) {
    switch (params.command) {
    case StageCommand::None:
        return;
    case StageCommand::Direct: {
        const DrawParams draw_params{
            .base_instance = params.base_instance,
            .num_instances = params.num_instances,
            .base_vertex = params.base_vertex,
            .num_vertices = params.num_vertices,
            .first_index = params.first_index,
            .is_indexed = params.is_indexed,
        };
        scheduler.Record([draw_params](vk::CommandBuffer cmdbuf) {
            if (draw_params.is_indexed) {
                cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                   draw_params.first_index, draw_params.base_vertex,
                                   draw_params.base_instance);
            } else {
                cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                            draw_params.base_vertex, draw_params.base_instance);
            }
        });
        return;
    }
    case StageCommand::Indirect:
        scheduler.Record([buffer = BufferOf(params.indirect_buffer), offset = params.indirect_offset,
                          count = params.max_draw_counts, stride = params.stride,
                          indexed = params.is_indexed](vk::CommandBuffer cmdbuf) {
            if (indexed) {
                cmdbuf.DrawIndexedIndirect(buffer, offset, count, stride);
            } else {
                cmdbuf.DrawIndirect(buffer, offset, count, stride);
            }
        });
        return;
    case StageCommand::IndirectCount:
        scheduler.Record([buffer = BufferOf(params.indirect_buffer), offset = params.indirect_offset,
                          count_buffer = BufferOf(params.count_buffer),
                          count_offset = params.count_offset, count = params.max_draw_counts,
                          stride = params.stride,
                          indexed = params.is_indexed](vk::CommandBuffer cmdbuf) {
            if (indexed) {
                cmdbuf.DrawIndexedIndirectCount(buffer, offset, count_buffer, count_offset, count,
                                                stride);
            } else {
                cmdbuf.DrawIndirectCount(buffer, offset, count_buffer, count_offset, count, stride);
            }
        });
        return;
    case StageCommand::ByteCount:
        scheduler.Record([buffer = BufferOf(params.indirect_buffer), offset = params.indirect_offset,
                          stride = params.stride](vk::CommandBuffer cmdbuf) {
            cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer, offset, 0, stride);
        });
        return;
    }
}

void RasterizerVulkan::CheckSplitSetup(GraphicsPipeline& pipeline, std::span<const u32> handles,
                                       bool is_indexed, bool light, bool followed,
                                       const SplitCheckBase& base) {
    // EDEN_NX2_BACK_STAGES=2: the split setup's snapshot, then the old one (Configure, no
    // SetEngine: =2's lockstep keeps B at the end of this packet, its replica this draw's) with B
    // parked, compared. The draw records with the old setup. A light draw that differs is an
    // error (the split chain took a light way the full setup would not match); the reverse (the
    // old chain light where the split one set up in full) is not seen here. Besides the
    // descriptors (CompareSetups), as CheckLightVerify (esdeger-6): the pipeline the cache looks up
    // for B's replica (B parked: safe here) and, for a draw that followed without a setup (R2
    // dogrulama-1 / kayit-1: a setup makes them again, feedback loops and aliases end the render
    // pass each time), a render pass the old setup begins and the dynamic state it records after
    // it. A difference after another thread wrote memory in between (dogrulama-3: the texture
    // cache is let go for the ParkB) is counted apart, not as an error. Not compared: the geometry
    // binds (C's mirror binds them right before the draw, BindGeometryC) and the rescaling and
    // render area outside EDEN_NX2_LIGHT_UNIFORM=2 (TakeSetupSnapshot, T6).
    GraphicsPipeline::SetupSnapshot split;
    pipeline.TakeSetupSnapshot(split, handles);
    GraphicsPipeline::SetupSnapshot full;
    bool configured = false;
    GraphicsPipeline* looked_up = nullptr;
    u64 render_passes = 0;
    u64 dynamic_records = 0;
    bool raced = false;
    VideoCommon::Nx2Stages::ParkB([&] {
        CheckBufferLockOffB("RasterizerVulkan::CheckSplitSetup");
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        // A guest CPU write (OnCPUWrite: WriteMemory under this lock), an unmap (image views
        // deleted) or a buffer side note since the split setup.
        raced = texture_cache.WriteHits() != base.image_hits ||
                texture_cache.ImageViewEpoch() != base.view_epoch ||
                light_epoch_b.load(std::memory_order_seq_cst) != base.light_epoch;
        looked_up = pipeline_cache.CurrentGraphicsPipeline();
        const u64 render_pass_begins = scheduler.RenderPassBegins();
        // Vertex streams the old setup binds straight to C's command buffer (those B's flags have
        // dirty; none in lockstep): C's mirror does not see them, B's next packet sends them all.
        const auto& b_flags = EngineB().dirty.flags;
        const u32 b_dirty = VertexDirtyMask(b_flags, ~0u);
        configured = pipeline.Configure(is_indexed);
        if ((b_dirty & ~VertexDirtyMask(b_flags, ~0u)) != 0) {
            geometry_reset_b = true;
        }
        if (!configured) {
            return;
        }
        // The old path's UpdateVertexBuffers clears it on its one replica before its dynamic
        // state (kayit-1): C's, set by InvalidateState, would record the vertex input again.
        maxwell3d->dirty.flags[VideoCommon::Dirty::VertexBuffers] = false;
        pipeline.TakeSetupSnapshot(full, {});
        const u64 records = scheduler.RecordCount();
        stage_dynamic = {.active = true, .pipeline = &pipeline};
        UpdateDynamicStates();
        stage_dynamic = {};
        dynamic_records = scheduler.RecordCount() - records;
        render_passes = scheduler.RenderPassBegins() - render_pass_begins;
    });
    if (!configured) {
        return;
    }
    // Uniform data streamed by B lives in B's staging pool, which this does not reach (T3): such
    // an entry at another slice compares as different.
    const auto bytes = [&](const DescriptorUpdateEntry& entry, VkDeviceSize& range) {
        if (pipeline.UsesDescriptorBuffer()) {
            range = entry.address.range;
            return staging_pool.MappedBytesAt(entry.address.address, range);
        }
        range = entry.buffer.range;
        return staging_pool.MappedBytes(entry.buffer.buffer, entry.buffer.offset, range);
    };
    // Counted here, not with the replica checks (VerifyChecks): no counter of its own yet (T0).
    thread_local u64 checks{};
    thread_local u64 mismatches{};
    thread_local u64 light_errors{};
    thread_local u64 raced_differences{};
    ++checks;
    std::string differences = pipeline.CompareSetups(split, full, bytes);
    if (looked_up != &pipeline) {
        differences += fmt::format(" pipeline: the cache looks up {} for the packet's {};",
                                   fmt::ptr(looked_up), fmt::ptr(&pipeline));
    }
    if (followed && render_passes != 0) {
        differences += fmt::format(" the old setup began {} render passes;", render_passes);
    }
    if (followed && dynamic_records != 0) {
        differences += fmt::format(" the old setup recorded {} dynamic state commands after it;",
                                   dynamic_records);
    }
    if (!differences.empty()) {
        if (raced) {
            ++raced_differences;
        } else {
            ++mismatches;
            light_errors += light ? 1 : 0;
        }
    }
    if ((checks & (checks - 1)) == 0 && checks >= 1024) {
        LOG_INFO(HW_GPU,
                 "Split setups checked: {}, differing {} (light draws {}), after a write in "
                 "between {}",
                 checks, mismatches, light_errors, raced_differences);
    }
    if (differences.empty()) {
        return;
    }
    if (raced) {
        LOG_DEBUG(HW_GPU, "Split setup differs after a write in between: {}", differences);
        return;
    }
    // Apart: warnings must not use up the errors' lines.
    thread_local u32 logged_errors{};
    thread_local u32 logged_warnings{};
    if (light) {
        if (logged_errors < 16) {
            ++logged_errors;
            LOG_ERROR(HW_GPU, "Split light draw differs from the full setup: {}", differences);
        }
    } else if (logged_warnings < 16) {
        ++logged_warnings;
        LOG_WARNING(HW_GPU, "Split setup differs from the old one: {}", differences);
    }
}

} // namespace Vulkan
