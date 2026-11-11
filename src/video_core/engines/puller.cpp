// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <optional>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/nx2_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_acquire_peek.h"
#include "video_core/nx2_front_diag_hooks.h"
#include "video_core/nx2_mode_diag.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        dma_pusher.rasterizer->FlushRegion(*device, sizeof(u32),
                                           VideoCommon::CacheType::BufferCache);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

namespace Peek = VideoCommon::Nx2AcquirePeek;

// EDEN_NX2_ACQUIRE_PEEK (nx2_acquire_peek.h): =1 an acquire guest memory meets goes on without a
// drain where Decide allows it; =2 drains as before and logs where =1 would have gone on early.
Peek::Mode PeekMode() {
    static const Peek::Mode mode = Peek::ParseMode(std::getenv("EDEN_NX2_ACQUIRE_PEEK"));
    return mode;
}

// EDEN_NX2_SEMEXEC_DEFER=1 (ProcessSemaphoreExecute): read once.
bool SemExecuteDefer() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_SEMEXEC_DEFER");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Switch 2 acquires and the puller's releases are tracked with the split on, for the switches or
// while profiling (the acquire peek line). Front thread only.
bool PeekTracking() {
    return (PeekMode() != Peek::Mode::Off || SemExecuteDefer() ||
            VideoCommon::Nx2Profile::Enabled()) &&
           VideoCommon::GpuSplit::Get().Active();
}

// Started over with each split session (GpuSplit::Session): the next may be another game, at the
// same device addresses.
Peek::Tracker& PeekTracker() {
    static Peek::Tracker tracker;
    static u64 session = ~u64{0};
    if (const u64 now = VideoCommon::GpuSplit::Get().Session(); now != session) {
        tracker = Peek::Tracker{};
        session = now;
    }
    return tracker;
}

// The caller is counted while profiling (EDEN_NX2_GPU_PROFILE, Nx2Profile::CountDrain).
void DrainBack(VideoCommon::Nx2Profile::DrainSite site) {
    VideoCommon::Nx2FrontDiag::CountDrain(VideoCommon::GpuSplit::Get(), site);
    if (PeekTracking()) {
        PeekTracker().Drained();
    }
}

// A release the puller queued for the back (PeekTracking). A 16-byte one also writes a zero
// word after the payload and the timestamp after that.
void NoteRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload, bool timestamp) {
    if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
        VideoCommon::Nx2FrontDiag::NoteWrite(dma_pusher.memory_manager,
                                             VideoCommon::Nx2FrontDiag::WriteKind::Release,
                                             address, timestamp ? 16 : 4);
    }
    if (!PeekTracking()) {
        return;
    }
    auto& tracker = PeekTracker();
    const s32 channel = dma_pusher.channel_state.bind_id;
    for (u32 word = 0; word < (timestamp ? 4U : 1U); ++word) {
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address + word * 4)) {
            tracker.NoteRelease(*device, channel, word == 0 ? payload : 0, word < 2);
        }
    }
}

// The acquire peek line (EDEN_NX2_GPU_PROFILE): counted on the front, logged for the front's
// profile window once the next one has begun.
struct PeekStats {
    u64 window{}; ///< Nx2Profile's window start these are counted in
    std::array<u64, 3> acquires{};      ///< Equal, greater or equal, mask
    std::array<u64, 3> met_in_memory{}; ///< Of them: met by guest memory before the drain
    std::array<u64, static_cast<size_t>(Peek::Verdict::Count)> verdicts{}; ///< Of those
    u64 skipped{};        ///< Went on without a drain (=1)
    u64 met_then_unmet{}; ///< Met in memory before the drain, not after it
    u64 may_skip_unmet{}; ///< Of them: Decide said Skip
    u64 resumes{}; ///< Checks of a pending acquire (CanResumeAcquire: the scheduler's, Step's)
    u64 resumes_met{};    ///< Of them: met in memory before the drain
    u64 resumes_may_skip{};
    u64 resumes_latched{}; ///< Resumed without a drain, latched by a release (=1)
    u64 resumes_skipped{}; ///< Resumed without a drain, met in memory (=1)
    u64 sem_execute_deferred{}; ///< SEM_EXECUTE acquires checked on the back (SEMEXEC_DEFER)
};

thread_local PeekStats t_peek_stats;

size_t OperationIndex(u32 operation) {
    return operation == Peek::OpEqual ? 0 : operation == Peek::OpGequal ? 1 : 2;
}

void LogPeekStats(const PeekStats& stats, u64 window_ns) {
    const double per_second = 1e9 / static_cast<double>(window_ns);
    const auto rate = [per_second](u64 value) { return static_cast<double>(value) * per_second; };
    const auto verdict = [&](Peek::Verdict which) {
        return rate(stats.verdicts[static_cast<size_t>(which)]);
    };
    const auto& tracker = PeekTracker();
    LOG_INFO(HW_GPU,
             "GPU thread acquire peek per second: acquires {:.0f} (equal {:.0f}, greater or "
             "equal {:.0f}, mask {:.0f}), met in memory before the drain {:.0f} ({:.0f}/{:.0f}/"
             "{:.0f}): may skip {:.0f}, skipped {:.0f}; drain kept for mask {:.0f}, wrapped "
             "{:.0f}, queued release {:.0f}, not monotonic {:.0f}, new {:.0f}, not advanced "
             "{:.0f}, untracked {:.0f}; "
             "met in memory but not after the drain {:.0f} (may skip {:.0f}) | resume checks {:.0f} "
             "(met in memory {:.0f}, may skip {:.0f}; skipped: latched {:.0f}, met {:.0f}) | "
             "sem execute acquires deferred {:.0f} | words {} ({} went down), releases queued {}",
             rate(stats.acquires[0] + stats.acquires[1] + stats.acquires[2]),
             rate(stats.acquires[0]), rate(stats.acquires[1]), rate(stats.acquires[2]),
             rate(stats.met_in_memory[0] + stats.met_in_memory[1] + stats.met_in_memory[2]),
             rate(stats.met_in_memory[0]), rate(stats.met_in_memory[1]),
             rate(stats.met_in_memory[2]), verdict(Peek::Verdict::Skip), rate(stats.skipped),
             verdict(Peek::Verdict::Operation), verdict(Peek::Verdict::Wrapped),
             verdict(Peek::Verdict::QueuedRelease), verdict(Peek::Verdict::NotMonotonic),
             verdict(Peek::Verdict::New), verdict(Peek::Verdict::NotAdvanced),
             verdict(Peek::Verdict::Untracked),
             rate(stats.met_then_unmet), rate(stats.may_skip_unmet), rate(stats.resumes),
             rate(stats.resumes_met), rate(stats.resumes_may_skip), rate(stats.resumes_latched),
             rate(stats.resumes_skipped), rate(stats.sem_execute_deferred), tracker.Words(),
             tracker.WordsNotMonotonic(), tracker.QueuedReleases());
}

PeekStats& PeekStatsNow() {
    auto& stats = t_peek_stats;
    const u64 window = VideoCommon::Nx2Profile::t_data.window_start;
    if (stats.window != window) {
        if (stats.window != 0 && window > stats.window) {
            LogPeekStats(stats, window - stats.window);
        }
        stats = PeekStats{};
        stats.window = window;
    }
    return stats;
}

void CountPeek(u64 PeekStats::*counter) {
    if (VideoCommon::Nx2Profile::Enabled()) {
        ++(PeekStatsNow().*counter);
    }
}

enum class PeekSite { Acquire, Resume, SemExecute };

// An acquire's word in guest memory before the drain (PeekTracking), and what Decide says of it.
struct PeekRead {
    bool tracked{};
    std::optional<u64> device;
    u32 raw{};
    Peek::Verdict verdict{Peek::Verdict::Untracked};
};

PeekRead PeekBefore(DmaPusher& dma_pusher, GPUVAddr address, u32 operation, u32 value,
                    PeekSite site) {
    PeekRead read;
    if (!PeekTracking()) {
        return read;
    }
    read.tracked = true;
    read.raw = dma_pusher.memory_manager.Read<u32>(address);
    read.device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    const bool met = Peek::Matches(operation, read.raw, value);
    if (read.device) {
        auto& tracker = PeekTracker();
        tracker.Observe(*read.device, read.raw);
        read.verdict = tracker.Decide(*read.device, dma_pusher.channel_state.bind_id, operation,
                                      value, read.raw);
    } else {
        read.verdict = met ? Peek::Verdict::Untracked : Peek::Verdict::Unmet;
    }
    if (!VideoCommon::Nx2Profile::Enabled()) {
        return read;
    }
    auto& stats = PeekStatsNow();
    if (site == PeekSite::Resume) {
        ++stats.resumes;
        stats.resumes_met += met ? 1 : 0;
        stats.resumes_may_skip += read.verdict == Peek::Verdict::Skip ? 1 : 0;
    } else if (site == PeekSite::Acquire) {
        const size_t index = OperationIndex(operation);
        ++stats.acquires[index];
        if (met) {
            ++stats.met_in_memory[index];
            ++stats.verdicts[static_cast<size_t>(read.verdict)];
        }
    }
    return read;
}

// The word as the drain left it (`current`). Where it no longer meets an acquire memory met
// before, a skip would have let the channel go on early: logged where Decide said Skip.
void PeekAfter(DmaPusher& dma_pusher, const PeekRead& read, GPUVAddr address, u32 operation,
               u32 value, u32 current) {
    if (!read.tracked) {
        return;
    }
    if (read.device) {
        PeekTracker().Observe(*read.device, current);
    }
    if (!Peek::Matches(operation, read.raw, value) || Peek::Matches(operation, current, value)) {
        return;
    }
    const bool may_skip = read.verdict == Peek::Verdict::Skip;
    if (VideoCommon::Nx2Profile::Enabled()) {
        auto& stats = PeekStatsNow();
        ++stats.met_then_unmet;
        stats.may_skip_unmet += may_skip ? 1 : 0;
    }
    if (!may_skip) {
        return;
    }
    static u64 reported{};
    if (++reported <= 16) {
        LOG_WARNING(HW_GPU,
                    "Acquire peek: channel {} semaphore {:#x} (op {}, {:#x}) met in memory "
                    "({:#x}) but not after the drain ({:#x})",
                    dma_pusher.channel_state.bind_id, address, operation, value, read.raw,
                    current);
    } else if ((reported & (reported - 1)) == 0) {
        LOG_WARNING(HW_GPU, "Acquire peek: {} acquires met in memory but not after the drain",
                    reported);
    }
}

bool SemExecuteReady(u32 operation, u32 word, u32 payload) {
    return operation == 0   ? word == payload
           : operation == 2 ? word >= payload
           : operation == 3 ? static_cast<s32>(word - payload) >= 0
           : operation == 4 ? (word & payload) != 0
           : operation == 5 ? (word & payload) == 0
                            : true;
}

// A SEM_EXECUTE acquire's check: it only logs a value it finds unmet.
void CheckSemExecute(u32 operation, GPUVAddr address, u32 word, u32 payload) {
    if (!SemExecuteReady(operation, word, payload)) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
}

// An acquire was met: Decide's next skip on the word for this channel must wait for more.
void PeekMet(DmaPusher& dma_pusher, const PeekRead& read, u32 value) {
    if (read.tracked && read.device) {
        PeekTracker().Met(*read.device, dma_pusher.channel_state.bind_id, value);
    }
}

// =1: what an acquire Decide let go on without a drain may see. The word was written on the back
// after the work before its release; the front's reads of guest memory after this one stay after
// it (a compiler barrier: the order of the host's stores and loads is x86-64's).
void PeekSkipped() {
    std::atomic_thread_fence(std::memory_order_acquire);
}

} // Anonymous namespace

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    const auto rescued = [&](u32 now) {
        if (static std::atomic<u32> count{};
            !pending_acquire->Matches(now) && count.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, now);
        }
    };
    const PeekRead peek = PeekBefore(dma_pusher, pending_acquire->address,
                                     pending_acquire->operation, pending_acquire->value,
                                     PeekSite::Resume);
    if (peek.tracked && PeekMode() == Peek::Mode::Skip) {
        // A release that met the value latched the channel, which no drain undoes; or memory
        // meets it where Decide allows.
        if (Nx2SemaphoreWaiters::Get().Latched(this)) {
            CountPeek(&PeekStats::resumes_latched);
            rescued(peek.raw);
            return true;
        }
        if (peek.verdict == Peek::Verdict::Skip) {
            CountPeek(&PeekStats::resumes_skipped);
            PeekSkipped();
            return true;
        }
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    DrainBack(VideoCommon::Nx2Profile::DrainSite::PullerResume);
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    PeekAfter(dma_pusher, peek, pending_acquire->address, pending_acquire->operation,
              pending_acquire->value, current);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        rescued(current);
        return true;
    }
    if (pending_acquire->Matches(current)) {
        return true;
    }
    // A release the GPU already processed may only wait for the host GPU to write it (accurate
    // GPU mode): work after this acquire runs after the work before that release on the host.
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address)) {
        if (const auto pending = dma_pusher.rasterizer->PendingSemaphoreRelease(*device);
            pending && pending_acquire->Matches(*pending)) {
            return true;
        }
    }
    if (!pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since > std::chrono::seconds{2}) {
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for 2 s; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current, device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    if (PeekTracking()) {
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address)) {
            PeekTracker().Met(*device, dma_pusher.channel_state.bind_id, pending_acquire->value);
        }
    }
    pending_acquire.reset();
    Nx2SemaphoreWaiters::Get().Remove(this);
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        VideoCommon::GpuSplit::Get().NoteSync();
        if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
            VideoCommon::Nx2FrontDiag::NoteWork(VideoCommon::Nx2FrontDiag::WorkKind::Fences);
        }
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
            VideoCommon::Nx2FrontDiag::NoteWork(VideoCommon::Nx2FrontDiag::WorkKind::Syncpoint);
        }
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, properties, payload] {
            VideoCommon::Nx2ModeDiag::SourceScope diag_source{
                VideoCommon::Nx2ModeDiag::Source::PullerLong};
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
        });
        NoteRelease(dma_pusher, sequence_address, payload,
                    properties == VideoCommon::QueryPropertiesFlags::HasTimeout);
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireChecks, 1);
        const PeekRead peek =
            PeekBefore(dma_pusher, acquire.address, acquire.operation, acquire.value,
                       PeekSite::Acquire);
        if (peek.verdict == Peek::Verdict::Skip && PeekMode() == Peek::Mode::Skip) {
            CountPeek(&PeekStats::skipped);
            PeekMet(dma_pusher, peek, acquire.value);
            PeekSkipped();
            VideoCommon::GpuSplit::Get().NoteSync();
            return;
        }
        DrainBack(VideoCommon::Nx2Profile::DrainSite::PullerAcquire);
        const u32 current = ReadSemaphoreWord(dma_pusher, acquire.address);
        PeekAfter(dma_pusher, peek, acquire.address, acquire.operation, acquire.value, current);
        if (acquire.Matches(current)) {
            PeekMet(dma_pusher, peek, acquire.value);
            VideoCommon::GpuSplit::Get().NoteSync();
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        dma_pusher.rasterizer->ReleaseFences();
        if (!waiters.Latched(this) && !acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            pending_acquire = acquire;
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
        } else {
            waiters.Remove(this);
            PeekMet(dma_pusher, peek, acquire.value);
        }
        VideoCommon::GpuSplit::Get().NoteSync();
    } else {
        DrainBack(VideoCommon::Nx2Profile::DrainSite::PullerAcquire);
        do {
            u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            // A release the GPU already processed may only wait for the host GPU to write it
            // (accurate GPU mode). Work after this acquire runs after the work before that
            // release on the host anyway: no need to wait for the host GPU.
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(
                    regs.semaphore_address.SemaphoreAddress())) {
                if (const auto pending = dma_pusher.rasterizer->PendingSemaphoreRelease(*device)) {
                    const bool satisfies =
                        op == GpuSemaphoreOperation::AcquireEqual
                            ? *pending == regs.semaphore_sequence
                        : op == GpuSemaphoreOperation::AcquireGequal
                            ? *pending >= regs.semaphore_sequence
                            : false;
                    if (satisfies) {
                        word = *pending;
                    }
                }
            }
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
    if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
        VideoCommon::Nx2FrontDiag::NoteWrite(dma_pusher.memory_manager,
                                             VideoCommon::Nx2FrontDiag::WriteKind::Release,
                                             sequence_address, 4);
    }
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack(VideoCommon::Nx2Profile::DrainSite::PullerAcquire);
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        dma_pusher.rasterizer->ReleaseFences();
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    if (VideoCommon::Nx2ModeDiag::Enabled()) [[unlikely]] {
        VideoCommon::Nx2ModeDiag::SemExecute(argument, address);
    }
    if (operation == 1) { // RELEASE
        OnBack([rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        NoteRelease(dma_pusher, address, payload, false);
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    // The check only logs: with EDEN_NX2_SEMEXEC_DEFER=1 and the split on, the back releases the
    // fences and checks after what was sent before, as after a drain, and the front goes on. Only
    // where the acquire peek would let such an acquire go on (Decide): the release ran, so the
    // work before it ran too (the pages it wrote are marked for the front's command list reads),
    // as the drain had it. Equal as equal, both greater-or-equal kinds as greater or equal (the
    // circular one is met only within 2^31: Wrapped); masks drain. 6 and 7 wait for nothing.
    const u32 peek_operation = operation == 0                     ? Peek::OpEqual
                               : operation == 2 || operation == 3 ? Peek::OpGequal
                                                                  : 0;
    PeekRead peek;
    if (SemExecuteDefer() && VideoCommon::GpuSplit::Get().Active()) {
        if (peek_operation != 0) {
            peek = PeekBefore(dma_pusher, address, peek_operation, payload, PeekSite::SemExecute);
        }
        if (operation >= 6 || peek.verdict == Peek::Verdict::Skip) {
            PeekMet(dma_pusher, peek, payload);
            CountPeek(&PeekStats::sem_execute_deferred);
            OnBack([rasterizer = dma_pusher.rasterizer,
                    memory_manager = &dma_pusher.memory_manager, operation, address, payload] {
                rasterizer->ReleaseFences();
                CheckSemExecute(operation, address, memory_manager->Read<u32>(address), payload);
            });
            if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
                VideoCommon::Nx2FrontDiag::NoteWork(VideoCommon::Nx2FrontDiag::WorkKind::Fences);
            }
            return;
        }
    }
    DrainBack(VideoCommon::Nx2Profile::DrainSite::PullerExecute);
    dma_pusher.rasterizer->ReleaseFences();
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    PeekAfter(dma_pusher, peek, address, peek_operation, payload, word);
    if (SemExecuteReady(operation, word, payload)) {
        PeekMet(dma_pusher, peek, payload);
    }
    CheckSemExecute(operation, address, word, payload);
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        VideoCommon::GpuSplit::Get().NoteSync();
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
        break;
    }
    case BufferMethods::MemOpC: {
        LOG_TRACE(HW_GPU, "Memory Operation C: argument={:#x}", method_call.argument);
        break;
    }
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation D: argument={:#x}", method_call.argument);
        // MEM_OP_D runs the operation MEM_OP_A-C set up; operation 5 (bits 27-31) is MEMBAR
        // (57ffa55a8). EDEN_NX2_MEMBAR: records a real barrier.
        constexpr u32 OperationShift = 27;
        constexpr u32 OperationMask = 0x1f;
        constexpr u32 MembarOperation = 5;
        if (g_nx2_ampere && Nx2MembarEnabled() &&
            ((method_call.argument >> OperationShift) & OperationMask) == MembarOperation) {
            OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->Nx2Membar(); });
        }
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
