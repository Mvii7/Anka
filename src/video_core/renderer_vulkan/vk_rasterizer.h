// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <span>
#include <utility>
#include <vector>

#include <boost/container/static_vector.hpp>

#include "common/common_types.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2_gpu_pages.h"
#include "video_core/nx2_stage_draw.h"
#include "video_core/nx2_stage_split.h"
#include "video_core/nx2_stages.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {

namespace Engines {
class Maxwell3D;
}

} // namespace Tegra

namespace Vulkan {

struct FramebufferTextureInfo;

class StateTracker;

class AccelerateDMA : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache,
                           Scheduler& scheduler);

    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;

    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;

    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;

    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& src,
                            const Tegra::DMA::ImageOperand& dst);

    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    Scheduler& scheduler;
};

class RasterizerVulkan final : public VideoCore::RasterizerInterface,
                               protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                              Tegra::MaxwellDeviceMemoryManager& device_memory_,
                              const Device& device_, MemoryAllocator& memory_allocator_,
                              StateTracker& state_tracker_, Scheduler& scheduler_);
    ~RasterizerVulkan() override;

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    std::optional<u32> PendingSemaphoreRelease(DAddr addr) override;
    u32 BoundGeometryBuffer(u32 index) override {
        std::scoped_lock lock{buffer_cache.mutex};
        return buffer_cache.BoundGeometryBuffer(index).Value();
    }
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool HoldsGpuWrittenData(DAddr addr, u64 size) override;
    void NoteEngineWork() override {
        NoteLightWork();
    }
    [[nodiscard]] u64 WorkEpoch() const noexcept override {
        if (split_draws && VideoCommon::Nx2Stages::OnStage(VideoCommon::Nx2Stages::Stage::B))
            [[unlikely]] {
            // Cut c: B's copy (S12), what B's own events moved, in B's order.
            return light_epoch_b.load(std::memory_order_seq_cst);
        }
        return light_epoch.load(std::memory_order_seq_cst);
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(DAddr addr, u64 size) override;
    void OnCachedWrite(DAddr addr, u64 size) override;
    bool TryCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void Nx2Membar() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateConditionalRendering() override;
    bool HasDrawTransformFeedback() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;

    /// One debris-reset record, Lichii build 31's store: the 20 bytes {0, 1, 0, 0, 0} copied
    /// into a buffer that already holds them, or left in guest memory for the next upload
    /// when none does. The caller holds the buffer cache lock. `force_registered_upload`
    /// copies a buffer that holds the record even when its page is not marked GPU-modified.
    void StoreResetRecord(GPUVAddr address);
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;

    void InitializeChannel(Tegra::Control::ChannelState& channel) override;

    void BindChannel(Tegra::Control::ChannelState& channel) override;

    void ReleaseChannel(s32 channel_id) override;
    std::optional<FramebufferTextureInfo> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                            VAddr framebuffer_addr,
                                                            u32 pixel_stride);

private:
    /// EDEN_NX2_REACTIVE_STATS: counts the frame and, every ~10 s, sets the gauges (CPU-read
    /// buffer pages, staging pool) and writes the "nx2 reactive:" line. From TickFrame.
    void ReportReactiveStats();

    /// EDEN_NX2_MEMBAR: WaitForIdle's event wait with `flags` as the destination and a memory
    /// barrier; `membar`: for Nx2Membar (counted apart). The caller signals the ordering (on B
    /// with cut c, where C records this).
    void Nx2Barrier(VkPipelineStageFlags flags, bool membar);
    /// Nx2Membar's query notice and barrier (C's half with cut c, as WaitForIdleRecord).
    void Nx2MembarRecord();

    static constexpr const u64 NEEDS_D24[] = {
        0x01006A800016E000ULL, // SSBU
        0x0100E95004038000ULL, // XC2
        0x0100A6301214E000ULL, // FE:Engage
    };
    static constexpr size_t MAX_TEXTURES = 192;
    static constexpr size_t MAX_IMAGES = 48;
    static constexpr size_t MAX_IMAGE_VIEWS = MAX_TEXTURES + MAX_IMAGES;

    static constexpr VkDeviceSize DEFAULT_BUFFER_SIZE = 4 * sizeof(float);

    template <typename Func>
    void PrepareDraw(bool is_indexed, bool indirect, Func&&);

    // EDEN_NX2_LIGHT_DRAW (PrepareDraw): a draw that changed only vertex/index buffer addresses
    // and draw parameters since the last one binds those buffers and draws; the rest of the last
    // draw's setup is still in the command buffer. EDEN_NX2_LIGHT_UNIFORM: one that also changed
    // constant buffer data or binds sets up its buffers and descriptors again, nothing else.
    /// What may follow a draw (LightFollowable).
    enum class LightLevel : u8 {
        None,    ///< A full setup only
        Uniform, ///< A light draw with uniforms (its pipeline writes memory)
        Full,    ///< A light draw of either kind
    };
    /// What a draw left behind, for the next one.
    struct LightAnchor {
        Tegra::Engines::Maxwell3D* maxwell3d{};
        Tegra::MemoryManager* gpu_memory{};
        GraphicsPipeline* pipeline{}; ///< What it drew with (set up in full by it or before)
        u64 record_count{};  ///< Scheduler::RecordCount once it was recorded
        u64 epoch{};         ///< light_epoch when its setup began
        u64 cached_writes{}; ///< gpu_memory->CachedWrites() when its setup began
        u64 same_writes{};   ///< maxwell3d->light.same_writes when its setup began
        u64 cb_writes{};     ///< maxwell3d->light.cb_writes when its setup began
        u64 image_hits{};    ///< texture_cache.WriteHits() when its setup began
        u64 shader_hits{};   ///< pipeline_cache.InvalidationHits() when its setup began
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology{};
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format{};
        bool is_indexed{};
        bool indirect{};
        LightLevel level{}; ///< Set up in full or light with a shape a light draw can follow
    };
    /// Draw / DrawIndirect for PrepareDraw.
    struct LightEntry {
        u64 record_count{}; ///< Scheduler::RecordCount before the draw recorded anything
        bool shape_ok{};    ///< The draw's own shape allows the light setup
    };
    void BeginLightDraw(bool indirect, bool shape_ok);
    template <typename Func>
    bool TryLightDraw(bool is_indexed, bool indirect, bool invalidated, Func& draw_func);
    /// TryLightDraw: whether nothing a full setup reads changed but constant buffer data the
    /// uniform way sets up again (counts why not, profiling). rebind_storage: storage buffer
    /// descriptors hold other values too, which ConfigureUniforms then binds again
    /// (EDEN_NX2_LIGHT_STORAGE).
    [[nodiscard]] bool LightUniformOk(GraphicsPipeline& pipeline, bool invalidated,
                                      bool& rebind_storage);
    [[nodiscard]] bool LightFeedbackOff() const;
    /// What may follow this fully set up draw (counts why not a plain light draw, profiling).
    [[nodiscard]] LightLevel LightFollowable(const GraphicsPipeline& pipeline);
    /// EDEN_NX2_LIGHT_UNIFORM=2: compares a full setup with what TryLightDraw decided.
    void CheckLightVerify(GraphicsPipeline& pipeline, u64 render_pass_begins, u64 dynamic_records);
    /// EDEN_NX2_GPU_PROFILE: for a full setup that only fixed function state kept off the uniform
    /// way, counts what setting up its pipeline and dynamic state alone (the EDEN_NX2_LIGHT_PIPELINE
    /// design, not implemented) would depend on. Reads only.
    void NoteLightPipeline(const GraphicsPipeline& pipeline, bool is_indexed, bool indirect,
                           bool invalidated);
    /// Work outside draws that may change what the next draw would bind (any thread).
    void NoteLightWork() noexcept {
        light_epoch.fetch_add(1, std::memory_order_seq_cst);
        if (split_draws && !VideoCommon::Nx2Stages::OnStage(VideoCommon::Nx2Stages::Stage::C))
            [[unlikely]] {
            // S12: every event C will run that notes work passes B first and moves B's copy
            // there; C's own notes (its half of the same event) move only the shared one.
            light_epoch_b.fetch_add(1, std::memory_order_seq_cst);
        }
    }

    // Back stages, cut c (Kesim-2, EDEN_NX2_BACK_STAGES with 'c'; PLAN.md §4.3 T7). On B the
    // draws and the two-part events split themselves: B does the memory half (FlushCaching, the
    // buffer cache, the pipeline lookup, the light decision; its Vulkan commands into a record
    // list) and sends C the rest in ring 3, C (the only stage that records) replays B's list and
    // does the texture half, the recording and the draw. Everywhere else (C, A in a join, other
    // threads, B the record owner in a join's op) the old bodies run; PrepareDraw and
    // TryLightDraw are unchanged and unused on B.
    using StageDrawReader = VideoCommon::Nx2Stages::StageDrawReader;

    [[nodiscard]] bool SplitOnB() const noexcept {
        return split_draws && VideoCommon::Nx2Stages::OnStage(VideoCommon::Nx2Stages::Stage::B) &&
               !OwnsRecordsB();
    }
    /// K3 (EDEN_NX2_STAGE_CHECK) where the buffer cache mutex is taken and the caller may be off
    /// B: C may take it only inside a ParkB `op` (a record C runs never), A only in its join.
    void CheckBufferLockOffB(const char* where) const {
        if (split_draws) [[unlikely]] {
            VideoCommon::Nx2Stages::CheckBufferLock(where);
        }
    }
    /// B the record owner, C parked: inside a join's `op` (JoinB, the record owner hook, a
    /// JoinFromB made elsewhere: the proxy's TickFrame, SignalSyncPoint, fences). The old bodies
    /// run there and C's half of B's own work runs at once: nothing goes to ring 3.
    [[nodiscard]] bool OwnsRecordsB() const noexcept {
        return b_in_join ||
               VideoCommon::Nx2Stages::RecordOwner() == VideoCommon::Nx2Stages::Stage::B;
    }

    /// What C records for a packet (StageDrawParams::command).
    using StageCommand = VideoCommon::Nx2Stages::DrawCommand;
    /// The packet's draw words (DrawToC's tail part 4, packed: Nx2Stages::PackDrawArgs): what
    /// Draw / DrawIndirect's draw_func read, taken by B from its replica and its buffer cache.
    using StageDrawParams = VideoCommon::Nx2Stages::DrawArgs;
    /// The rasterizer C runs draw packets for (StageDrawThunk): set by BeginStagesHook, the
    /// stages' one record side (SetStageHooks).
    static inline RasterizerVulkan* stage_rasterizer{};

    /// B's replica of the bound channel's Maxwell3D (Role::Memory; maxwell3d is C's).
    [[nodiscard]] Tegra::Engines::Maxwell3D& EngineB();
    /// B: Draw / DrawIndirect.
    void DrawB(bool is_indexed, u32 instance_count);
    void DrawIndirectB();
    /// B: PrepareDraw's memory half and the light decision; sends the packet. `params` holds the
    /// direct draw's parameters (the indirect ones are taken after the setup).
    void PrepareDrawB(bool is_indexed, bool indirect, bool shape_ok, u32 instance_count,
                      StageDrawParams& params);
    /// B: what may follow a draw it set up in full, by what B sees (C adds its own: render
    /// targets and views to repeat).
    [[nodiscard]] LightLevel FollowableB(const GraphicsPipeline& pipeline,
                                         const Tegra::Engines::Maxwell3D& mb, bool shape_ok) const;
    /// B: LightUniformOk without C's part (images written: C checks the write hits).
    [[nodiscard]] bool LightUniformOkB(GraphicsPipeline& pipeline, Tegra::Engines::Maxwell3D& mb,
                                       bool invalidated, bool& rebind_storage);
    /// B, RESETFIX (Donkey Kong Bananza's terrain reset program, PrepareDraw): whether this draw
    /// runs that program (its answers kept in B's statics); StageResetStores then makes its
    /// stores, the list flushed first in a join (no lock held), the writes in a packet of their
    /// own, and the draw is cut short. False leaves the draw to run: the list did not fit its
    /// descriptor.
    [[nodiscard]] bool StageResetProgram(Tegra::Engines::Maxwell3D& mb, bool is_indexed);
    [[nodiscard]] bool StageResetStores(Tegra::Engines::Maxwell3D& mb, u32 instance_count);
    /// B, inside a packet with the buffer cache locked: AccelerateInlineRuns' memory half.
    void InlineRunsB(std::span<const std::pair<GPUVAddr, u32>> runs, std::span<const u8> data);
    void InlineToMemoryB(GPUVAddr address, size_t copy_size, std::span<const u8> memory);
    void InnerInvalidationB(std::span<const std::pair<DAddr, std::size_t>> sequences);
    /// B: the memory manager's guest writes, reads and dirty checks (WriteBlock, ReadBlock,
    /// IsMemoryDirty on B's replicas' memory managers). InvalidateRegionB also takes the CPU's
    /// writes (OnCacheInvalidation; OnCachedWrite: `cached_write`, the buffer cache copying just
    /// those bytes in).
    void InvalidateRegionB(DAddr addr, u64 size, VideoCommon::CacheType which,
                           bool cached_write = false);
    void FlushRegionB(DAddr addr, u64 size, VideoCommon::CacheType which);
    [[nodiscard]] bool MustFlushRegionB(DAddr addr, u64 size, VideoCommon::CacheType which);
    [[nodiscard]] bool HoldsGpuWrittenDataB(DAddr begin, size_t span);
    /// Where this thread may not take the buffer cache mutex (C in a record:
    /// Nx2Stages::MayTakeBufferLock): whether the buffer cache holds GPU-written data at
    /// [addr, addr + size), without waiting for it. None: B holds it.
    [[nodiscard]] std::optional<bool> BufferGpuModifiedNoWait(DAddr addr, u64 size);
    void ClearB(u32 layer_count);
    void DrawTextureB();
    void WaitForIdleB();
    /// EDEN_NX2_MEMBAR's Nx2Membar on B, as WaitForIdleB.
    void Nx2MembarB();
    /// B: a join of its own: C parked, the old bodies run here (b_in_join).
    template <typename Func>
    void JoinB(Func&& func);
    /// B inside a packet holding the buffer cache mutex: JoinFromBLocked (b_in_join).
    template <typename Func>
    void JoinBLocked(Func&& func);
    /// A query report or counter reset's old body (A or B in a join): B's next WaitForIdle joins.
    void NoteQueryForWfi() noexcept;
    /// B, before it writes guest memory at device address `addr` (none: not one device range):
    /// a query report it sent C that would land there after it does not (T7c review finding 3).
    /// One C writes at once runs first (a join); one C writes from a later fence is cancelled
    /// first, in the join, where the write invalidates the query cache (`cancels`).
    void GuardQueryWritesB(std::optional<DAddr> addr, u64 size, bool cancels);
    /// B: buffer cache work with the cache locked (Nx2Record::CacheLock), its records in a packet
    /// of their own (sent when not empty) unless one is open; the record owner records them.
    template <typename Func>
    void StageBufferWork(Func&& work);
    /// B, no cache lock held: C's half of an event, to ring 3 after B's pending invalidations;
    /// the record owner runs it at once.
    template <typename Func>
    void StageToC(const Func& func);
    /// Nx2Stages::SetStageHooks (constructor): Start stages the buffer runtime (BeginStages, its
    /// record owner hook bound to JoinFromBLocked) before B and C start, Stop ends it after they
    /// stopped; nothing runs behind either.
    static void BeginStagesHook(void* context);
    static void EndStagesHook(void* context);
    static void StageRecordOwnerHook(void* context, RecordList* records,
                                     VideoCommon::Nx2Stages::FunctionRef op);
    /// B, no cache lock held: C's half of B's invalidations so far (texture WriteMemory, query
    /// InvalidateRegion), then a packet's records, to ring 3 (the record owner: at once).
    void FlushStageCWork();
    void SendStageRecords(u64 seq, RecordList* records);
    /// B, no cache lock held: the draw packet in stage_writer.
    void SendStageDraw();
    /// B (T8): the pages C may write for work `seq` (or, between packets, the next packet's): the
    /// bound render targets and depth only (every page for a target too large to walk). A caller
    /// whose pipeline writes memory it does not place (storage images, global stores) adds
    /// GpuPages().PreSetAll(seq) itself. `light`: a light draw's packet (no render target
    /// register changed since the packet before: the ranges found then are kept).
    void PreSetTargets(Tegra::Engines::Maxwell3D& mb, u64 seq, bool light = false);
    /// B: the seq of the next packet (work sent between packets runs before it).
    [[nodiscard]] u64 NextStageSeq() const noexcept;
    /// B, inside a draw packet: every vertex stream the draw reads marked used
    /// (BufferCache::MarkVertexStreamsUsed, geo-markusage-1).
    void MarkStreamsB();
    /// B (omur-1): the texture header and sampler pools C reads for work `seq` (a packet, or
    /// DrawTexture before the next one), kept until C ran it.
    void NotePoolsB(const Tegra::Engines::Maxwell3D& mb, u64 seq);
    /// B: whether a write of [address, address + size) hits a pool C may still read for work
    /// B sent (the ones C ran dropped first).
    [[nodiscard]] bool WritesPoolsB(GPUVAddr address, u64 size);
    /// B: the same, or `mb`'s current pools.
    [[nodiscard]] bool WritesAnyPoolB(const Tegra::Engines::Maxwell3D& mb, GPUVAddr address,
                                      u64 size);
    /// The sampler pool's limit C reads with `mb`'s registers (SamplerPoolLimit).
    [[nodiscard]] static u32 SamplerPoolLimitB(const Tegra::Engines::Maxwell3D& mb);
    /// Nx2Stages::SetCbWriteCheck (constructor), B: constant buffer data B writes to guest memory
    /// (a CbData record) joins where it meets a pool (omur-1); over a release C has not written
    /// yet, C writes it first (qw-cbdata).
    static bool CbWriteCheckHook(void* context, Tegra::Engines::Maxwell3D& replica,
                                 GPUVAddr address, u64 size);
    [[nodiscard]] bool CbWriteJoinsB(Tegra::Engines::Maxwell3D& replica, GPUVAddr address,
                                     u64 size);
    /// B, before a draw packet's buffer half: every vertex stream binds again where
    /// GeometryForced says so (nx2_stage_split.h), else only those B's replica has dirty.
    void ForceGeometryB(Tegra::Engines::Maxwell3D& mb);
    /// An old Draw / DrawIndirect with the stages on (A or B in a join, the front after a drain):
    /// B's replica binds every vertex stream, and B's next packet sends them all (C's mirror did
    /// not see these binds).
    void ForceGeometryForJoin();

    /// C: ring 3's records.
    static void StageDrawThunk(const u64* payload, u32 words);
    static void StageInvalidateThunk(const u64* payload, u32 words);
    void PrepareDrawC(const StageDrawReader& in, const StageDrawParams& params);
    void RunStageRecords(RecordList* records, u64 seq);
    void RunStageInvalidations(const u64* ranges, u32 count);
    void RecordStageDraw(const StageDrawParams& params);
    /// C, once a packet's records were replayed (from `replay_tick` on): its geometry binds into
    /// the mirror; a mirrored packet's (DrawToC::geometry_mirrored, C7) recorded first.
    void ApplyGeometryC(const StageDrawReader& in, u64 replay_tick);
    /// C, right before the draw: what the command buffer lost since binds again (kayit-4).
    void BindGeometryC(const VideoCommon::Nx2Stages::DrawToC& head);
    void ClearRecord(u32 layer_count);
    void WaitForIdleRecord();
    /// C: a packet's seq recorded; the anchor's held back while a light draw may repair with
    /// its descriptor entries (their uniform data in B's staging pool).
    void NoteStageSeq(u64 seq);
    void ReleaseAnchorC();
    /// C: the anchor's packet as a light draw's repair reads it.
    void SaveAnchorC(const StageDrawReader& in, std::span<const u32> handles);
    /// EDEN_NX2_BACK_STAGES=2: what another thread's write between the split setup and the old
    /// one moves (C lets the texture cache go for the ParkB; R2 dogrulama-3).
    struct SplitCheckBase {
        u64 image_hits;  ///< texture_cache.WriteHits() before the split setup
        u64 view_epoch;  ///< texture_cache.ImageViewEpoch() once it was done
        u64 light_epoch; ///< light_epoch_b at B's decision (head.light.epoch), before its buffer
                         ///< half; in lockstep only other threads move it until the check
    };
    /// EDEN_NX2_BACK_STAGES=2: the split setup against the old one (Configure, B parked).
    /// `followed`: drawn without a setup (a light draw that needed no repair).
    void CheckSplitSetup(GraphicsPipeline& pipeline, std::span<const u32> handles, bool is_indexed,
                         bool light, bool followed, const SplitCheckBase& base);

    void FlushWork();

    /// EDEN_NX2_RESET_BATCH: AccelerateInlineToMemory for each run (address, size), in order,
    /// a run's bytes being the first `size` of `data`. Called with the buffer and texture
    /// caches locked (PrepareDraw).
    void AccelerateInlineRuns(std::span<const std::pair<GPUVAddr, u32>> runs,
                              std::span<const u8> data);

    void UpdateDynamicStates();
    /// UpdateDynamicStates' pipeline: the pipeline cache's current one, with cut c stage_dynamic's.
    [[nodiscard]] GraphicsPipeline* DynamicStatePipeline() {
        if (stage_dynamic.active) [[unlikely]] {
            return stage_dynamic.pipeline;
        }
        return pipeline_cache.CurrentGraphicsPipeline();
    }

    void HandleTransformFeedback();

    void UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineStipple(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;

    const Device& device;
    MemoryAllocator& memory_allocator;
    StateTracker& state_tracker;
    Scheduler& scheduler;

    StagingBufferPool staging_pool;
    DescriptorPool descriptor_pool;
    GuestDescriptorQueue guest_descriptor_queue;
    ComputePassDescriptorQueue compute_pass_descriptor_queue;
    DescriptorBufferRing descriptor_buffer_ring;
    BlitImageHelper blit_image;
    RenderPassCache render_pass_cache;

    TextureCacheRuntime texture_cache_runtime;
    TextureCache texture_cache;
    BufferCacheRuntime buffer_cache_runtime;
    BufferCache buffer_cache;
    QueryCacheRuntime query_cache_runtime;
    QueryCache query_cache;
    PipelineCache pipeline_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;

    vk::Event wfi_event;

    boost::container::static_vector<u32, MAX_IMAGE_VIEWS> image_view_indices;
    std::array<VideoCommon::ImageViewId, MAX_IMAGE_VIEWS> image_view_ids;
    boost::container::static_vector<VkSampler, MAX_TEXTURES> sampler_handles;

    u32 draw_counter = 0;
    /// Instance count of the draw PrepareDraw is setting up (Draw). Indirect draws leave it.
    u32 draw_instances = 0;

    LightAnchor light_anchor;
    LightEntry light_entry;
    std::atomic<u64> light_epoch{};
    /// EDEN_NX2_LIGHT_UNIFORM=2: a draw decided light with uniforms, then set up in full.
    struct LightVerify {
        bool active{};
        GraphicsPipeline* pipeline{};
        std::vector<DescriptorUpdateEntry> payload; ///< What ConfigureUniforms built
        std::vector<u32> values; ///< The pipeline's descriptor word values it left
    };
    LightVerify light_verify;

    // Back stages, cut c (T7).
    bool split_draws{}; ///< Set once (constructor): EDEN_NX2_BACK_STAGES with cut c
    bool b_in_join{};   ///< B runs a join of its own (JoinB, the record owner hook)
    /// B: a Query / ResetCounter went to C since the last WaitForIdle made in a join (F3): the
    /// next one may sync query writes (SyncValues takes the buffer cache) and joins.
    bool stage_query_since_wfi{true};
    /// B's copy of light_epoch (S12): moved by B's events and other threads' notes, not by C's.
    std::atomic<u64> light_epoch_b{};
    /// B's light anchor: LightAnchor's fields B sees.
    struct LightAnchorB {
        Tegra::Engines::Maxwell3D* maxwell3d{};
        Tegra::MemoryManager* gpu_memory{};
        GraphicsPipeline* pipeline{};
        u64 epoch{};
        u64 cached_writes{};
        u64 same_writes{};
        u64 cb_writes{};
        u64 shader_hits{};
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology{};
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format{};
        bool is_indexed{};
        bool indirect{};
        LightLevel level{};
    };
    LightAnchorB light_anchor_b;
    /// C's light anchor: what C checks a light packet against and repairs it with.
    struct LightAnchorC {
        GraphicsPipeline* pipeline{};
        u64 record_count{}; ///< Scheduler::RecordCount once the anchor was recorded
        u64 image_hits{};   ///< texture_cache.WriteHits() then
        u64 hold_seq{};     ///< The anchor's seq, not noted while held (0: none)
        u64 last_seq{};     ///< The last packet seq C saw
        bool followable{};  ///< No render target or view to repeat (LightFollowable's C part)
        std::vector<u64> payload; ///< Its packet: head, descriptor entries, handles
    };
    /// C's per draw members apart from B's (each written for every draw by one of them): lines of
    /// their own (k2hiz: B's anchor and C's shared one).
    alignas(64) LightAnchorC light_anchor_c;
    alignas(64) VideoCommon::Nx2Stages::StageDrawWriter stage_writer; ///< B
    /// B: C's half of B's invalidations not sent yet, two words each: the device address, the
    /// size with what in the top bits (vk_rasterizer.cpp: StageRangeTexture, StageRangeQuery).
    std::vector<u64> stage_c_ranges;
    /// Cut c: the pipeline UpdateDynamicStates sets its pipeline-dependent state for (vertex input,
    /// alpha to coverage / one) while `active`: the draw's (C's packet, B's join), or none (C's
    /// DrawTexture: those steps wait, their dirty flags kept). The pipeline cache is B's.
    struct StageDynamicPipeline {
        bool active{};
        GraphicsPipeline* pipeline{};
    };
    alignas(64) StageDynamicPipeline stage_dynamic; ///< C (B in its joins)
    /// B: the device ranges of the bound render targets and depth (T8's PreSet for each packet),
    /// found again when the registers, the memory manager (GetID) or its page table
    /// (MappingEpoch) change; TickFrame and Start drop them too.
    struct StageTargets {
        Tegra::MemoryManager* gpu_memory{};
        size_t memory_id{};
        u64 mapping_epoch{};
        std::array<std::pair<GPUVAddr, u64>, Tegra::Engines::Maxwell3D::Regs::NumRenderTargets + 1>
            key{};
        bool valid{};
        /// The last call's registers gave `key` (not a target too large to walk).
        bool current{};
        std::vector<std::pair<DAddr, u64>> ranges;
    };
    alignas(64) StageTargets stage_targets;
    /// B (omur-1): the pool ranges (GPU addresses) of work C may not have run yet, each with the
    /// last seq that reads it; dropped once C ran that seq (Nx2GpuPages::RanSeq) and at B's joins.
    VideoCommon::Nx2Stages::InFlightRanges stage_pools;
    /// B (T7c review finding 3): the guest memory query reports B sent C write ([address,
    /// address + 16)): C writes a payload release at once, so a write of B's own there waits.
    VideoCommon::Nx2Stages::InFlightRanges stage_query_writes;
    /// B: the counters B sent C, which C writes from a fence after them (QueryWritesAtOnce): kept
    /// until a join finds the query cache no longer holds them, or B cancels them before a write.
    VideoCommon::Nx2Stages::InFlightRanges stage_query_later;
    /// B (geometry mirror): the next draw packet sends every vertex stream (GeometryForced).
    bool geometry_reset_b{true};
    /// B: C's command buffer tick at the last packet that sent every vertex stream.
    u64 geometry_tick_b{};
    /// B: the replica and memory manager of that packet. The buffer cache's vertex streams are
    /// one set for every channel and a channel's bind sets only C's replica's flags.
    const Tegra::Engines::Maxwell3D* geometry_engine_b{};
    const Tegra::MemoryManager* geometry_memory_b{};
    /// C: the geometry mirror (nx2_stage_split.h).
    alignas(64) VideoCommon::Nx2Stages::GeometryMirror geometry_mirror_c;
    /// C: B's record owner hook replayed B's list in a join since C's last draw packet: that
    /// packet's binds may be in an older command buffer. Written by B with C parked.
    bool geometry_replayed_c{};
};

} // namespace Vulkan
