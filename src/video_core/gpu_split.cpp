// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_back_cpu.h"
#include "video_core/nx2_busy_clock.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stages.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (Nx2Stages::Active()) [[unlikely]] {
        // Back stages: the stage recording (RecordOwner) owns the scheduler.
        Nx2Stages::Detail::CheckRecordOwner(where);
        return;
    }
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    // EDEN_NX2_WRITE_BATCH=0: every macro write is a ring entry of its own.
    static const bool batch = [] {
        const char* value = std::getenv("EDEN_NX2_WRITE_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    batch_writes = batch;
    // EDEN_NX2_BATCH_DIRECT=0: a Batch entry's writes go to the engine one virtual call each
    // (Execute); otherwise through its CallMethodBatch, the same loop with its own calls.
    static const bool direct = [] {
        const char* value = std::getenv("EDEN_NX2_BATCH_DIRECT");
        return value == nullptr || value[0] != '0';
    }();
    batch_direct = direct;
    pending_count = 0;
    ring = std::make_unique<u64[]>(RingWords);
    // EDEN_NX2_BACK_STAGES: the stages below the back start first (nothing runs behind the
    // front yet); the back thread then runs as stage A (BackLoop).
    Nx2Stages::Start(system_);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    // EDEN_NX2_BACK_STAGES: the stages below A stop after it (they may read ring 1, =3).
    Nx2Stages::Stop();
    // EDEN_NX2_BACK_CPU: the core the back had is the process's again.
    Nx2BackCpu::StopBack();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    ++session;
    write_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    pending_count = 0;
    unbatched_position = 0;
    unbatched_published = 0;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    // Reserve runs with no macro write held (ReserveEntry, WritePending, MacroWrite unbatched).
    PublishRing();
    Nx2BackCpu::NoteFront();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::Publish() {
    FlushPending();
    unbatched_published = unbatched_position;
    PublishRing();
}

void GpuSplit::PublishRing() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

bool GpuSplit::Drain() {
    FlushPending();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position) {
        return false;
    }
    Publish();
    Nx2BackCpu::NoteFront();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            publish_every = 8;
            return true;
        }
        if (spins < 16384) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    // The held writes are the previous engine's: they go before the switch.
    u64* const entry = ReserveEntry(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = ReserveEntry(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::WritePending() {
    // Cleared first: Reserve may publish (WaitForSpace), and Publish sends what is held.
    const u32 count = pending_count;
    pending_count = 0;
    u64* const entry = Reserve(1 + size_t{count});
    entry[0] = static_cast<u64>(Kind::Batch) | (u64{count} << 32);
    std::memcpy(entry + 1, pending.data(), size_t{count} * sizeof(u64));
    // Counted as they were held (Count): no publish here.
    write_position += 1 + size_t{count};
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry, u64 published_left) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            // Macro writes to the engine's run methods (Maxwell3D: constant buffer data) that
            // follow each other go to it together: published, before the ring's end, one word
            // each (no segment).
            using Tegra::Engines::EngineInterface;
            if (method - engine->method_run_begin < engine->method_run_count && published_left > 1) {
                const size_t to_end = RingWords - static_cast<size_t>(entry - ring.get());
                const size_t limit = static_cast<size_t>(std::min<u64>(
                    std::min<u64>(published_left, to_end), EngineInterface::MaxMethodRun));
                u32 methods[EngineInterface::MaxMethodRun];
                u32 arguments[EngineInterface::MaxMethodRun];
                methods[0] = method;
                arguments[0] = argument;
                size_t count = 1;
                for (; count < limit; ++count) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method ||
                        (next & (Direct | HasSegment)) != Direct ||
                        next_method - engine->method_run_begin >= engine->method_run_count) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                }
                if (count > 1) {
                    engine->CallMethodRun(*system, methods, arguments, static_cast<u32>(count));
                    return count;
                }
            }
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Batch: {
        // EDEN_NX2_WRITE_BATCH: for each write what a Method entry with IsLast and Direct does,
        // in order. The method sink grows only from non-Direct Method entries (CallMethod never
        // adds to it), so after the first ConsumeSink the ones before the later writes found it
        // empty. Runs of the engine's run methods go to CallMethodRun as from consecutive
        // entries (at most MaxMethodRun; CallMethodRun takes any such run word by word if it
        // must).
        const u32 count = static_cast<u32>(head >> 32);
        const u64* const writes = entry + 1;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if (batch_direct) {
            engine->CallMethodBatch(*system, writes, count);
            return 1 + size_t{count};
        }
        using Tegra::Engines::EngineInterface;
        for (u32 i = 0; i < count;) {
            const u32 method = static_cast<u32>(writes[i] & 0xffff);
            const u32 argument = static_cast<u32>(writes[i] >> 32);
            if (method - engine->method_run_begin < engine->method_run_count) {
                u32 methods[EngineInterface::MaxMethodRun];
                u32 arguments[EngineInterface::MaxMethodRun];
                u32 run = 0;
                for (; run < EngineInterface::MaxMethodRun && i + run < count; ++run) {
                    const u32 next = static_cast<u32>(writes[i + run] & 0xffff);
                    if (next - engine->method_run_begin >= engine->method_run_count) {
                        break;
                    }
                    methods[run] = next;
                    arguments[run] = static_cast<u32>(writes[i + run] >> 32);
                }
                if (run > 1) {
                    engine->CallMethodRun(*system, methods, arguments, run);
                    i += run;
                    continue;
                }
            }
            engine->CallMethod(*system, method, argument, true);
            ++i;
        }
        return 1 + size_t{count};
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    if (Nx2Stages::Active()) {
        RunStagedBack(stop_token);
        return;
    }
    Common::SetCurrentThreadName("GPUBack");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    Nx2Stages::SetBufferRecordRole(2);
    // EDEN_NX2_BACK_CPU: a physical core of its own (nx2_back_cpu.h).
    Nx2BackCpu::StartBack();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    const bool busy_clock = Nx2BusyClock::Tracked();
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested()) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        g_back_busy.store(true, std::memory_order_relaxed);
        Nx2BackCpu::NoteBack();
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size = Execute(&ring[index], available - read_position);
            read_position += size != 0 ? size : RingWords - index;
            // Hand ring space back now and then while working through a long batch.
            if (read_position - last_store >= 4096 && read_position != available) {
                consumed.store(read_position, std::memory_order_release);
                last_store = read_position;
                Nx2BackCpu::NoteBack();
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        consumed.store(read_position, std::memory_order_release);
        if (busy_clock) {
            // EDEN_NX2_GPU_BUSY_CLOCK: caught up while the front waits for a command?
            Nx2BusyClock::BackStored();
        }
    }
}

void GpuSplit::StageConsumed(u64 position, bool caught_up) {
    consumed.store(position, std::memory_order_release);
    Nx2BackCpu::NoteBack();
    if (caught_up && Nx2BusyClock::Tracked()) {
        // EDEN_NX2_GPU_BUSY_CLOCK: caught up while the front waits for a command?
        Nx2BusyClock::BackStored();
    }
}

void GpuSplit::StageExecute(u64 position, u64 words) {
    const u64 end = position + words;
    u64 last_store = position;
    while (position != end) {
        const size_t index = position & RingMask;
        const size_t size = Execute(&ring[index], end - position);
        position += size != 0 ? size : RingWords - index;
        if (position - last_store >= 4096 && position != end) {
            consumed.store(position, std::memory_order_release);
            last_store = position;
        }
    }
    Nx2Profile::Add(Nx2Profile::Counter::PassThrough, words);
}

namespace {

/// KeplerCompute's launch method: of the engines whose methods do not run on A, only its
/// execution mask holds it (MaxwellDMA 0xC0, KeplerMemory 0x6C-0x6D, Fermi2D 0x880 on).
constexpr size_t KeplerComputeLaunch = KEPLER_COMPUTE_REG_INDEX(launch);

/// Whether the entry (Method, Multi or Batch) runs an executable method of `engine`.
bool EntryExecutes(const Tegra::Engines::EngineInterface& engine, u64 head, const u64* entry,
                   bool batch) {
    if (!batch) {
        return engine.execution_mask[(head >> 8) & 0xffff];
    }
    const u32 count = static_cast<u32>(head >> 32);
    for (u32 i = 0; i < count; ++i) {
        if (engine.execution_mask[entry[1 + i] & 0xffff]) {
            return true;
        }
    }
    return false;
}

void CountJoinCandidate(const Tegra::Engines::EngineInterface& engine) {
    if (Nx2Profile::Enabled()) {
        Nx2Profile::Add(Nx2Profile::Counter::JoinCandidates, 1);
        if (engine.execution_mask[KeplerComputeLaunch]) {
            Nx2Profile::Add(Nx2Profile::Counter::JoinCandidatesCompute, 1);
        }
    }
}

} // Anonymous namespace

bool GpuSplit::StagedJoinStarts(const u64* entry, Nx2Stages::JoinReason& reason) {
    const u64 head = entry[0];
    const Kind kind = static_cast<Kind>(head & 0xf);
    if (kind == Kind::Call) {
        if (CallTarget(head) != DeferTarget::Join) {
            return false;
        }
        reason = Nx2Stages::JoinReason::Tagged;
        return true;
    }
    if (kind != Kind::Method && kind != Kind::Multi && kind != Kind::Batch) {
        return false;
    }
    const auto& engine = *back_engine;
    if (engine.stage_methods_on_a || !EntryExecutes(engine, head, entry, kind == Kind::Batch)) {
        // Maxwell3D's methods run on A (they send what they do); state writes of the other
        // engines too (method sink, registers).
        return false;
    }
    CountJoinCandidate(engine);
    reason = Nx2Stages::JoinReason::Engine;
    return true;
}

bool GpuSplit::StagedJoinContinues(const u64* entry) const {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return true;
    case Kind::Engine:
        return !reinterpret_cast<const Tegra::Engines::EngineInterface*>(entry[1])
                    ->stage_methods_on_a;
    case Kind::Call:
        return CallTarget(head) == DeferTarget::Join;
    case Kind::Method:
    case Kind::Multi:
    case Kind::Batch: {
        const auto& engine = *back_engine;
        if (engine.stage_methods_on_a) {
            return false;
        }
        if (EntryExecutes(engine, head, entry, static_cast<Kind>(head & 0xf) == Kind::Batch)) {
            CountJoinCandidate(engine);
        }
        return true;
    }
    }
    return false;
}

size_t GpuSplit::ExecuteStagedA(const u64* entry, u64 published_left) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            // Macro writes to the engine's run methods (Maxwell3D: constant buffer data) that
            // follow each other go to it together: published, before the ring's end, one word
            // each (no segment).
            using Tegra::Engines::EngineInterface;
            if (method - engine->method_run_begin < engine->method_run_count && published_left > 1) {
                const size_t to_end = RingWords - static_cast<size_t>(entry - ring.get());
                const size_t limit = static_cast<size_t>(std::min<u64>(
                    std::min<u64>(published_left, to_end), EngineInterface::MaxMethodRun));
                u32 methods[EngineInterface::MaxMethodRun];
                u32 arguments[EngineInterface::MaxMethodRun];
                methods[0] = method;
                arguments[0] = argument;
                size_t count = 1;
                for (; count < limit; ++count) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method ||
                        (next & (Direct | HasSegment)) != Direct ||
                        next_method - engine->method_run_begin >= engine->method_run_count) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                }
                if (count > 1) {
                    engine->CallMethodRun(*system, methods, arguments, static_cast<u32>(count));
                    return count;
                }
            }
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Batch: {
        // EDEN_NX2_WRITE_BATCH: as Execute.
        const u32 count = static_cast<u32>(head >> 32);
        const u64* const writes = entry + 1;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if (batch_direct) {
            engine->CallMethodBatch(*system, writes, count);
            return 1 + size_t{count};
        }
        using Tegra::Engines::EngineInterface;
        for (u32 i = 0; i < count;) {
            const u32 method = static_cast<u32>(writes[i] & 0xffff);
            const u32 argument = static_cast<u32>(writes[i] >> 32);
            if (method - engine->method_run_begin < engine->method_run_count) {
                u32 methods[EngineInterface::MaxMethodRun];
                u32 arguments[EngineInterface::MaxMethodRun];
                u32 run = 0;
                for (; run < EngineInterface::MaxMethodRun && i + run < count; ++run) {
                    const u32 next = static_cast<u32>(writes[i + run] & 0xffff);
                    if (next - engine->method_run_begin >= engine->method_run_count) {
                        break;
                    }
                    methods[run] = next;
                    arguments[run] = static_cast<u32>(writes[i + run] >> 32);
                }
                if (run > 1) {
                    engine->CallMethodRun(*system, methods, arguments, run);
                    i += run;
                    continue;
                }
            }
            engine->CallMethod(*system, method, argument, true);
            ++i;
        }
        return 1 + size_t{count};
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        const auto thunk = reinterpret_cast<void (*)(const u64*)>(entry[1]);
        const DeferTarget target = CallTarget(head);
        if (target == DeferTarget::OnA || Nx2Stages::InJoin()) {
            // Register and draw manager work (or anything while the stages are parked).
            thunk(entry + 2);
        } else if (target == DeferTarget::Join) {
            Nx2Stages::JoinFromA(Nx2Stages::JoinReason::Tagged, [&] { thunk(entry + 2); });
        } else {
            // Owner: the stage owning the caches runs it, in the stream's order (D2).
            Nx2Stages::Forward(Nx2Stages::Ring::ToB, thunk, entry + 2, static_cast<u32>(payload));
        }
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::RunStagedBack(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    // Stage A is Highest (S19): B, the stage a frame waits on, has Critical and the back's core
    // (EDEN_NX2_BACK_CPU); A runs off that core with the process's other threads.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
    Nx2Stages::Detail::t_stage = static_cast<u8>(Nx2Stages::Stage::A);
    const bool pass_through = Nx2Stages::Get().mode == 3;
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report(Nx2Profile::stage_report_names[0]);
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested()) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        if (pass_through) {
            // =3 (D7): B runs the range with Execute; the stage costs alone (ring 2, wake, mark).
            Nx2Stages::Detail::PostExecA(read_position, available - read_position);
            read_position = available;
            Nx2Stages::Detail::PostBatchEnd(read_position);
            continue;
        }
        u64 last_mark = read_position;
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            Nx2Stages::JoinReason reason{};
            if (StagedJoinStarts(&ring[index], reason)) [[unlikely]] {
                // With the stages parked: this entry and what follows of the engines whose
                // methods do not run on A, and of Call entries tagged Join (S9/2).
                Nx2Stages::JoinFromA(reason, [&] {
                    do {
                        const size_t at = read_position & RingMask;
                        const size_t size = ExecuteStagedA(&ring[at], available - read_position);
                        read_position += size != 0 ? size : RingWords - at;
                    } while (read_position != available &&
                             StagedJoinContinues(&ring[read_position & RingMask]));
                });
            } else {
                const size_t size = ExecuteStagedA(&ring[index], available - read_position);
                read_position += size != 0 ? size : RingWords - index;
            }
            // How far A ran, now and then while working through a long batch: the last stage
            // hands ring space back with it (D1: A never stores `consumed`).
            if (read_position - last_mark >= 4096 && read_position != available) {
                Nx2Stages::PostMark(Nx2Stages::Ring::ToB, read_position);
                last_mark = read_position;
            }
        }
        Nx2Stages::Detail::PostBatchEnd(read_position);
    }
}

} // namespace VideoCommon
