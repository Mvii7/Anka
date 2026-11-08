// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_front_diag.h"
#include "video_core/nx2_front_same.h"
#include "video_core/nx2_profile.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

// EDEN_NX2_GPU_PROFILE: the split GPU thread's front diag lines (nx2_front_diag.h), counted where
// the front sends work and logged for the front's profile window once the next one began:
// - "front macros": Maxwell3D front macro calls by start address (sampled time, JIT / HLE);
// - "front sends": DmaPusher method calls and macro writes, time spent sending them (sampled);
// - "front register writes": 3D register writes by method, of the value held or not sent;
// - "front drains": each drain caller's waits by length;
// - "MME DMA reads": what was pending of each read when it drained (PendingWrites), whether the
//   drain changed the words it read.
// Front thread only, behind On(); the counters touch nothing the emulation reads, the timed paths
// are out of line (the untimed ones keep their one call). Also EDEN_NX2_PROBE_FRONT_US=n: the
// front spins n microseconds per command list (any profile).
namespace VideoCommon::Nx2FrontDiag {

// EDEN_NX2_FRONT_DIAG=0: these lines off with the profile on (their counters cost the front a few
// percent more). Read at the first call, as Nx2Profile::Enabled() is.
inline std::atomic<s8> g_on{-1};

YUZU_NO_INLINE inline bool ReadOn() {
    const char* value = std::getenv("EDEN_NX2_FRONT_DIAG");
    const bool on = Nx2Profile::Enabled() && (value == nullptr || value[0] != '0');
    g_on.store(on ? 1 : 0, std::memory_order_relaxed);
    if (on) {
        LOG_INFO(HW_GPU,
                 "Front diag lines on with the profile (EDEN_NX2_FRONT_DIAG=0 turns them off)");
    }
    return on;
}

inline bool On() {
    const s8 value = g_on.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        return ReadOn();
    }
    return value != 0;
}

/// EDEN_NX2_FRONT_DIAG_REGS=0: the register table off (Maxwell3D's front reads it once; its same
/// value filter stays armed then).
inline bool RegistersOn() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_FRONT_DIAG_REGS");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

/// Who a DmaPusher method call went to.
enum class SendKind : u8 {
    Puller,       ///< Puller methods (semaphores, binds)
    Front3D,      ///< Maxwell3D's front (FrontCallMethod / FrontCallMultiMethod)
    FrontCompute, ///< KeplerCompute's front
    Split,        ///< Another engine, sent to the back as it is
    Direct,       ///< The split off: run on this thread
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(SendKind::Count)> send_kind_names{
    "puller", "3D front", "compute front", "other engines", "unsplit",
};

inline constexpr u32 MacroSampleBits = 3; ///< One macro call in 8 timed
inline constexpr u32 SendSampleBits = 4;  ///< One method call or macro write in 16 timed

/// MME DMA reads by class and what was pending.
struct ReadStats {
    u64 reads{};
    u64 fifoed{};
    u64 words{};
    u64 pages{};
    u64 waited{};
    u64 flushed{};
    u64 changed{};
    u64 entries{}; ///< Pending writes at the reads, summed
    u64 untracked{};
    u64 unmapped{};
    std::array<u64, static_cast<size_t>(ReadClass::Count)> by_class{};
    std::array<u64, static_cast<size_t>(ReadClass::Count)> changed_by_class{};
    std::array<u64, static_cast<size_t>(WriteKind::Count)> hits{};
    u64 dropped_hits{};
    /// By WriteKind, then gpu, untracked, none.
    std::array<u64, static_cast<size_t>(WriteKind::Count) + 3> first_writer{};
    std::unordered_map<u32, std::array<u64, 2>> work_masks; ///< Work reads: mask -> reads, changed
};

/// An MME DMA read before its drain: what was pending of it, its pages (GPU-written, written
/// around the caches), the first writer of its first page and its class.
struct ReadProbe {
    bool active{};
    bool idle{};
    bool gpu_page{};
    bool untracked{};
    bool unmapped{};
    u32 pages{};
    u8 first_writer{};
    PendingWrites::Pending pending;
    ReadClass read_class{ReadClass::Free};
};

struct State {
    // The window counted in (Nx2Profile's), and the split session (positions start over).
    u64 window{};
    u64 window_ticks{};
    u64 session{~u64{0}};

    // Per window.
    MacroTable macros;
    RegisterWrites registers;
    std::array<DrainHistogram, static_cast<size_t>(Nx2Profile::DrainSite::Count)> drains{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_calls{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_sampled{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_ticks{};
    u64 macro_writes{};
    u64 macro_writes_sampled{};
    u64 macro_write_ticks{};
    ReadStats reads;
    u64 indirect_lookups{}; ///< FrontRunMacro's indirect draw checks looked up
    u64 indirect_decided{}; ///< ... for a macro decided already
    FrontRuns runs;

    // Kept.
    Sampler sampler;
    PendingWrites pending;
    FirstWriters first{u64{1} << (36 - FirstWriters::PageBits)};
    u32 current_macro{~0U};
    std::vector<u32> snapshot;
    // The last cb data write's GPU range end and device range end (consecutive words share the
    // translation), per address space.
    const Tegra::MemoryManager* last_memory{};
    u64 last_gpu_end{};
    u64 last_device_end{};
    // The last range noted page by page and the span of its device runs, for the same range
    // again (an upload's words, each sent on its own).
    const Tegra::MemoryManager* repeat_memory{};
    WriteKind repeat_kind{};
    GPUVAddr repeat_address{};
    u64 repeat_size{};
    u64 repeat_begin{};
    u64 repeat_end{};
    // The MME DMA read's pages that were mapped for its snapshot (bit per page from its first).
    u32 snapshot_pages{};
    // Registers of the engines whose writes DmaPusher's methods start (NoteEngineMethod).
    struct EngineShadow {
        const void* engine;
        std::array<u32, 0x200> regs;
        std::bitset<0x200> known; ///< Registers written since (not by a macro since)
    };
    std::vector<EngineShadow> engines;
    // RESETFIX: vertex programs by address, the indirect draw reset program or not (read again
    // each window).
    std::unordered_map<u64, bool> reset_programs;
    u64 last_program{};
    bool last_is_reset{};
    bool last_program_known{};
    // A draw ended (draw.end): if in an instance loop, the back draws it at the next method
    // outside the loop or at the front macro's end (DrawManager::DrawDeferred).
    bool draw_deferred{};
};

inline State& GetState() {
    static State state;
    return state;
}

inline void LogWindow(State& state, u64 window_ns, double ns_per_tick);

/// The state, the previous window logged and its counters cleared once Nx2Profile began another
/// (Report, at the top of the GPU thread's loop), the kept ones cleared with a new split session.
inline State& Window() {
    State& state = GetState();
    const auto& data = Nx2Profile::t_data;
    // A new split session first (another game; positions start over): the last one's counters
    // are dropped, not logged for the time between them.
    if (const u64 session = GpuSplit::Get().Session(); session != state.session) [[unlikely]] {
        state.pending.Reset();
        state.first.Clear();
        state.engines.clear();
        state.last_memory = nullptr;
        state.repeat_memory = nullptr;
        state.draw_deferred = false;
        state.window = 0;
        state.session = session;
    }
    if (state.window != data.window_start) [[unlikely]] {
        if (state.window != 0 && data.window_start > state.window &&
            data.window_start_ticks > state.window_ticks) {
            const u64 window_ns = data.window_start - state.window;
            LogWindow(state, window_ns,
                      static_cast<double>(window_ns) /
                          static_cast<double>(data.window_start_ticks - state.window_ticks));
        }
        state.macros.Clear();
        state.registers.Clear();
        state.drains = {};
        state.send_calls = {};
        state.send_sampled = {};
        state.send_ticks = {};
        state.macro_writes = 0;
        state.macro_writes_sampled = 0;
        state.macro_write_ticks = 0;
        state.reads = ReadStats{};
        state.indirect_lookups = 0;
        state.indirect_decided = 0;
        state.runs = FrontRuns{};
        state.reset_programs.clear();
        state.last_program_known = false;
        // The translation shortcuts go stale with a remap: at most a window.
        state.last_memory = nullptr;
        state.repeat_memory = nullptr;
        state.window = data.window_start;
        state.window_ticks = data.window_start_ticks;
    }
    return state;
}

/// At the top of the GPU thread's loop after Nx2Profile::Report (On()): the window that ended
/// logged there, outside the zones.
YUZU_NO_INLINE inline void Report() {
    static_cast<void>(Window());
}

/// GPU addresses at or past this are register garbage, not noted page by page.
inline constexpr u64 GpuAddressEnd = u64{1} << 48;

/// Everything sent so far is in the ring by this position (SentEndBound).
inline u64 Bound() {
    const GpuSplit& split = GpuSplit::Get();
    return SentEndBound(split.DiagWritePosition(), split.DiagHeldWrites(), GpuSplit::DiagRingWords,
                        GpuSplit::DiagMaxHeld);
}

/// A write of [begin, begin + size) device addresses just sent to the back.
inline void NoteDeviceWrite(WriteKind kind, u64 begin, u64 size) {
    if (size == 0 || !GpuSplit::Get().Active()) {
        return;
    }
    State& state = Window();
    if (size > MaxNotedBytes) {
        state.pending.NoteWork(WorkKind::Unranged, Bound());
        return;
    }
    state.pending.NoteWrite(kind, begin, begin + size, Bound());
    state.first.Note(kind, begin, begin + size);
}

/// A write of [address, address + size) GPU addresses of `memory` just sent to the back, page by
/// page into device ranges (unmapped pages write nothing, but in a Switch 2 sparse span the write
/// may back them first: unranged work).
inline void NoteWrite(const Tegra::MemoryManager& memory, WriteKind kind, GPUVAddr address,
                      u64 size) {
    if (size == 0 || !GpuSplit::Get().Active()) {
        return;
    }
    State& state = Window();
    const u64 bound = Bound();
    if (size > MaxNotedBytes || address >= GpuAddressEnd) {
        state.pending.NoteWork(WorkKind::Unranged, bound);
        return;
    }
    const u64 end = address + size;
    // A word right after the last one within its page: the same translation.
    if (kind == WriteKind::CbData && &memory == state.last_memory &&
        address == state.last_gpu_end && (address & 0xfff) != 0 && ((end - 1) >> 12) == (address >> 12)) {
        state.pending.NoteWrite(kind, state.last_device_end, state.last_device_end + size, bound);
        state.first.Note(kind, state.last_device_end, state.last_device_end + size);
        state.last_gpu_end = end;
        state.last_device_end += size;
        return;
    }
    // The range noted last again: the span of its device runs, which covers them (their first
    // writer is in already).
    if (&memory == state.repeat_memory && kind == state.repeat_kind &&
        address == state.repeat_address && size == state.repeat_size) {
        state.pending.NoteWrite(kind, state.repeat_begin, state.repeat_end, bound);
        return;
    }
    u64 run_begin = 0;
    u64 run_end = 0;
    u64 span_begin = ~u64{0};
    u64 span_end = 0;
    bool last_mapped = false;
    bool sparse = false;
    for (GPUVAddr page = address; page < end;) {
        const GPUVAddr next = std::min<GPUVAddr>((page | 0xfff) + 1, end);
        const auto device = memory.GpuToCpuAddress(page);
        last_mapped = device.has_value();
        if (!device && page < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed) &&
            next > Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed)) {
            sparse = true;
        }
        if (device) {
            if (run_end != *device) {
                if (run_begin != run_end) {
                    state.pending.NoteWrite(kind, run_begin, run_end, bound);
                    state.first.Note(kind, run_begin, run_end);
                }
                run_begin = *device;
            }
            run_end = *device + (next - page);
            span_begin = std::min(span_begin, *device);
            span_end = std::max(span_end, run_end);
        }
        page = next;
    }
    if (run_begin != run_end) {
        state.pending.NoteWrite(kind, run_begin, run_end, bound);
        state.first.Note(kind, run_begin, run_end);
    }
    if (sparse) {
        state.pending.NoteWork(WorkKind::Unranged, bound);
    }
    // The next word goes the short way only after a mapped one.
    state.last_memory = kind == WriteKind::CbData && last_mapped ? &memory : nullptr;
    state.last_gpu_end = end;
    state.last_device_end = run_end;
    state.repeat_memory = sparse ? nullptr : &memory;
    state.repeat_kind = kind;
    state.repeat_address = address;
    state.repeat_size = size;
    state.repeat_begin = span_begin < span_end ? span_begin : 0;
    state.repeat_end = span_begin < span_end ? span_end : 0;
}

/// Work whose writes the front cannot tell, just sent to the back.
inline void NoteWork(WorkKind kind) {
    if (!GpuSplit::Get().Active()) {
        return;
    }
    State& state = Window();
    state.pending.NoteWork(kind, Bound());
}

// RESETFIX's indirect draw reset program (vk_rasterizer.cpp, NX2_INDIRECT_RESET_PROGRAM), at
// its vertex program's address + 128.
inline constexpr std::array<u32, 44> IndirectResetProgram{
    0xff007321, 0x0002fcff, 0x00000000, 0x000e2400, 0x00007a11, 0x00004400, 0x078018ff,
    0x001fca00, 0xff017624, 0x00004500, 0x000e06ff, 0x000fcc00, 0x00007381, 0x00000000,
    0x001eab00, 0x000ea200, 0x00037802, 0x00000001, 0x00000f00, 0x000fca00, 0x00007386,
    0x00000403, 0x0010a900, 0x004fe800, 0x00007386, 0x000000ff, 0x0010a900, 0x000fe800,
    0x00007386, 0x000008ff, 0x0010a900, 0x000fe800, 0x00007386, 0x00000cff, 0x0010a900,
    0x000fe800, 0x00007386, 0x000010ff, 0x0010a900, 0x000fe200, 0x0000794d, 0x00000000,
    0x03800000, 0x000fea00,
};

/// Whether a write still pending for the back meets [address, address + size) of `memory`
/// (its mapped pages).
inline bool WritePending(const Tegra::MemoryManager& memory, State& state, GPUVAddr address,
                         u64 size) {
    const u64 consumed = GpuSplit::Get().DiagConsumed();
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        const GPUVAddr begin = std::max(page, address);
        const GPUVAddr end = std::min<GPUVAddr>(page + 0x1000, address + size);
        if (const auto device = memory.GpuToCpuAddress(begin)) {
            const auto pending = state.pending.Query(*device, *device + (end - begin), consumed);
            if (pending.writes != 0 || pending.overflow) {
                return true;
            }
        }
    }
    return false;
}

/// A draw ended with the vertex program at `program`: RESETFIX writes records for it.
inline void NoteDrawEnd(const Tegra::MemoryManager& memory, GPUVAddr program) {
    State& state = Window();
    if (!state.last_program_known || program != state.last_program) {
        bool is_reset = false;
        bool known = true;
        if (const auto it = state.reset_programs.find(program); it != state.reset_programs.end()) {
            is_reset = it->second;
        } else {
            std::array<u32, 44> code{};
            const GPUVAddr code_address = program + 128;
            if (memory.IsFullyMappedRange(code_address, sizeof(code))) {
                memory.ReadBlockUnsafe(code_address, code.data(), sizeof(code));
                is_reset = code == IndirectResetProgram;
            }
            // Another program as guest memory holds it now: kept once no write still queued
            // for the back (its upload) may change it.
            known = is_reset || !WritePending(memory, state, code_address, sizeof(code));
            if (known) {
                state.reset_programs.emplace(program, is_reset);
            }
        }
        state.last_program = program;
        state.last_is_reset = is_reset;
        state.last_program_known = known;
    }
    if (state.last_is_reset) {
        state.pending.NoteWork(WorkKind::ResetFix, Bound());
    }
    state.draw_deferred = true;
}

/// A 3D method just sent by the front; `instance_loop`: one of an instance loop's
/// (DrawManager::IsInstanceLoopMethod), `executable` or not. After a draw ended, the back draws
/// the loop's deferred instances at a method outside the loop it processes: an executable one
/// (sent or with the sink before it), a state write once the sink runs (later, or never if the
/// value is held), or `macro_end` (the front macro's FinishFrontMacro). Draw work is noted at each
/// until a sure one.
inline void NoteDeferredDraw(bool instance_loop, bool executable, bool macro_end) {
    State& state = Window();
    if (!state.draw_deferred || (instance_loop && !macro_end) || !GpuSplit::Get().Active()) {
        return;
    }
    if (executable || macro_end) {
        state.draw_deferred = false;
    }
    state.pending.NoteWork(WorkKind::Draw, Bound());
    if (state.last_is_reset) {
        state.pending.NoteWork(WorkKind::ResetFix, Bound());
    }
}

/// Methods DmaPusher just sent to an engine other than Maxwell3D's front: `count` words to
/// `method` (a run of one method). Their own registers, kept from the methods, give what the
/// uploads (KeplerMemory, KeplerCompute: Upload::Registers at 0x60, exec 0x6C, data 0x6D) and
/// MaxwellDMA's launches (0xC0; offset_out 0x102, pitch_out 0x105, line length and count 0x106-7,
/// semaphore 0x90-0x92) write, once the methods set the registers they use (unranged work until
/// then); compute launches and native dispatches (0xAE-0xB0), inline QMDs (0x107), report
/// semaphores (0x6C3), compute macros (their writes run on either side, unseen here: the registers
/// are not known after one) and Fermi2D's executable methods are work. Each word of a run of DMA
/// launches is a launch.
inline void NoteEngineMethod(const Tegra::MemoryManager& memory, const void* engine,
                             Tegra::Engines::EngineTypes type, u32 method, const u32* args,
                             u32 count, bool executable) {
    using Tegra::Engines::EngineTypes;
    if (count == 0 || !GpuSplit::Get().Active()) {
        return;
    }
    if (type == EngineTypes::Fermi2D) {
        if (executable) {
            NoteWork(WorkKind::Copy2D);
        }
        return;
    }
    if (type != EngineTypes::KeplerMemory && type != EngineTypes::KeplerCompute &&
        type != EngineTypes::MaxwellDMA) {
        return;
    }
    State& state = Window();
    auto it = std::find_if(
        state.engines.begin(), state.engines.end(),
        [engine](const State::EngineShadow& shadow) { return shadow.engine == engine; });
    if (it == state.engines.end()) {
        it = state.engines.insert(state.engines.end(), State::EngineShadow{engine, {}});
    }
    auto& regs = it->regs;
    auto& known = it->known;
    if (type == EngineTypes::KeplerCompute) {
        // What ProcessNativeMethod and the launch act on besides the value (native_plain_methods).
        if (method >= 0xE00) {
            NoteWork(WorkKind::Compute);
            known.reset();
            return;
        }
        if ((method >= 0xAE && method <= 0xB0) || method == 0x107 || method == 0x6C3) {
            NoteWork(WorkKind::Compute);
            return;
        }
    }
    if (type == EngineTypes::MaxwellDMA && method == 0xC0 && count > 1) {
        for (u32 i = 0; i < count; ++i) {
            NoteEngineMethod(memory, engine, type, method, args + i, 1, executable);
        }
        return;
    }
    if (method < regs.size()) {
        regs[method] = args[count - 1];
        known.set(method);
    }
    const auto address = [&regs](u32 high, u32 low, u32 high_mask) {
        return (GPUVAddr{regs[high] & high_mask} << 32) | GPUVAddr{regs[low]};
    };
    const auto all_known = [&known](std::initializer_list<u32> methods) {
        return std::all_of(methods.begin(), methods.end(), [&known](u32 m) { return known[m]; });
    };
    if (type == EngineTypes::MaxwellDMA) {
        if (method != 0xC0) {
            return;
        }
        const u32 launch = regs[0xC0];
        const bool multi_line = ((launch >> 9) & 1) != 0;
        const DmaWrite write = DmaRange(address(0x102, 0x103, 0xff), regs[0x105], regs[0x106],
                                        regs[0x107], multi_line, ((launch >> 7) & 1) != 0,
                                        ((launch >> 8) & 1) != 0, ((launch >> 10) & 1) != 0);
        if (!write.known) {
            NoteWork(WorkKind::DmaOther);
        } else if (all_known({0x102, 0x103, 0x106}) && (!multi_line || all_known({0x105, 0x107}))) {
            NoteWrite(memory, WriteKind::Dma, write.begin, write.end - write.begin);
        } else {
            NoteWork(WorkKind::Unranged);
        }
        if (const u32 semaphore = (launch >> 3) & 3; semaphore == 1 || semaphore == 2) {
            if (all_known({0x90, 0x91})) {
                NoteWrite(memory, WriteKind::Release, address(0x90, 0x91, 0xff),
                          semaphore == 1 ? 4 : 16);
            } else {
                NoteWork(WorkKind::Unranged);
            }
        }
        return;
    }
    if (method == 0x6D) {
        const bool linear = (regs[0x6C] & 1) != 0;
        if (!all_known({0x60, 0x61, 0x62, 0x63, 0x6C}) ||
            !(linear ? all_known({0x64}) : all_known({0x65, 0x66, 0x67, 0x68}))) {
            NoteWork(WorkKind::Unranged);
            return;
        }
        const auto [begin, end] = InlineRange(
            address(0x62, 0x63, ~0U), regs[0x60], regs[0x61], regs[0x64], linear, regs[0x66],
            regs[0x67], regs[0x68], (regs[0x65] >> 4) & 0xf, (regs[0x65] >> 8) & 0xf);
        NoteWrite(memory, WriteKind::Inline, begin, end - begin);
    }
}

/// A drain counted as Nx2Profile::CountDrain does, its wait into the caller's histogram.
template <typename Split>
inline bool CountDrain(Split& split, Nx2Profile::DrainSite site) {
    if (!On()) {
        return Nx2Profile::CountDrain(split, site);
    }
    const u64 start = Nx2Profile::Ticks();
    const bool waited = Nx2Profile::CountDrain(split, site);
    const u64 ticks = Nx2Profile::Ticks() - start;
    Window().drains[static_cast<size_t>(site)].Note(waited, ticks);
    return waited;
}

/// A timed stretch of the front, nested profile zones out (their time goes to t_data.child_ticks
/// of the zone around it as they end).
struct Timer {
    u64 start{};
    u64 child{};
    bool sampled{};
};

inline Timer StartTimer(State& state, u32 bits) {
    Timer timer;
    if (state.sampler.Next(bits)) {
        timer.sampled = true;
        timer.child = Nx2Profile::t_data.child_ticks;
        timer.start = Nx2Profile::Ticks();
    }
    return timer;
}

/// Exclusive and nested ticks of a sampled timer.
inline std::pair<u64, u64> StopTimer(const Timer& timer) {
    const u64 elapsed = Nx2Profile::Ticks() - timer.start;
    const u64 child = Nx2Profile::t_data.child_ticks - timer.child;
    return {elapsed > child ? elapsed - child : 0, child};
}

/// A front macro call of the macro at `start` begins (On()).
YUZU_NO_INLINE inline Timer MacroBegin(u32 start) {
    State& state = Window();
    state.current_macro = start;
    return StartTimer(state, MacroSampleBits);
}

/// ... and ended; `hle`: FrontIndirectDraw took it; the JIT's `emits` and register `reads`.
YUZU_NO_INLINE inline void MacroEnd(const Timer& timer, u32 start, bool hle, u32 emits, u32 reads) {
    State& state = GetState(); // MacroBegin took the window
    MacroTable::Entry& entry = state.macros.Find(start);
    ++entry.calls;
    entry.hle_calls += hle ? 1 : 0;
    entry.emits += emits;
    entry.reads += reads;
    if (timer.sampled) {
        const auto [exclusive, child] = StopTimer(timer);
        ++entry.sampled;
        entry.ticks += exclusive;
        entry.child_ticks += child;
    }
    state.current_macro = ~0U;
}

/// Whom the DmaPusher method call being sent went to: DmaPusher sets it on its way (a plain
/// store whether timed or not; GPU thread).
alignas(64) inline std::atomic<SendKind> g_send_kind{SendKind::Split};

inline void SetSendKind(SendKind kind) {
    g_send_kind.store(kind, std::memory_order_relaxed);
}

/// A DmaPusher method call, `call`, timed for whom it went to (On()).
template <typename Call>
YUZU_NO_INLINE void TimedSend(const Call& call) {
    const Timer timer = StartTimer(Window(), SendSampleBits);
    SetSendKind(SendKind::Split);
    call();
    State& state = GetState(); // StartTimer took the window
    const auto index = static_cast<size_t>(g_send_kind.load(std::memory_order_relaxed));
    ++state.send_calls[index];
    if (timer.sampled) {
        ++state.send_sampled[index];
        state.send_ticks[index] += StopTimer(timer).first;
    }
}

/// A macro write FrontWrite sends, `write`, timed (On()).
template <typename Write>
YUZU_NO_INLINE void TimedMacroWrite(const Write& write) {
    const Timer timer = StartTimer(Window(), SendSampleBits);
    write();
    State& state = GetState(); // StartTimer took the window
    ++state.macro_writes;
    if (timer.sampled) {
        ++state.macro_writes_sampled;
        state.macro_write_ticks += StopTimer(timer).first;
    }
}

/// A 3D register write FrontWrite got: the value held already, not sent, a macro's (On()).
inline void NoteRegisterWrite(u32 method, bool same, bool skipped, bool from_macro) {
    Window().registers.Note(method, same, skipped, from_macro);
}

/// FrontRunMacro looked up whether a macro is NVN's indirect draw: `decided` already (On()).
inline void NoteIndirectLookup(bool decided) {
    State& state = Window();
    ++state.indirect_lookups;
    state.indirect_decided += decided ? 1 : 0;
}

/// A command header DmaPusher starts to Maxwell3D's front (FrontRuns::Note, On()).
YUZU_NO_INLINE inline void NoteFrontRun(bool increase_once, bool macro, u32 words, bool in_list) {
    Window().runs.Note(increase_once, macro, words, in_list);
}

/// EDEN_NX2_JIT_SAME's line, per profile window whether front diag is on or not.
struct SameStats {
    u64 window{};
    u32 mode{};
    u64 calls{};
    u64 hits{};
    std::array<u64, static_cast<size_t>(Nx2FrontSame::Disarm::Count)> calls_by{};
    u64 mismatches{};
    u64 missed{};
};

inline void LogSame(const SameStats& stats, u64 window_ns) {
    namespace Same = Nx2FrontSame;
    const double per_second = 1e9 / static_cast<double>(window_ns);
    const auto rate = [per_second](u64 value) { return static_cast<double>(value) * per_second; };
    const auto by = [&](Same::Disarm why) {
        return rate(stats.calls_by[static_cast<size_t>(why)]);
    };
    LOG_INFO(HW_GPU,
             "GPU thread front same filter per second: hits {:.0f}, disarmed calls {:.0f} (replay "
             "{:.0f} / check {:.0f} / diag {:.0f} / gap {:.0f} / skip {:.0f}) of {:.0f}, verify "
             "mismatch {}, verify missed {} | EDEN_NX2_JIT_SAME={}",
             rate(stats.hits), rate(stats.calls - stats.calls_by[0]), by(Same::Disarm::ShadowReplay),
             by(Same::Disarm::Check), by(Same::Disarm::Diag), by(Same::Disarm::Gap),
             by(Same::Disarm::Skip), rate(stats.calls), stats.mismatches, stats.missed, stats.mode);
}

/// A front macro call with the filter ended (with the profile): its hits, why it was disarmed,
/// its =2 mismatches and misses. A window is logged at the first call of a later one.
YUZU_NO_INLINE inline void NoteSameCall(u32 mode, Nx2FrontSame::Disarm why, u64 hits,
                                        u64 mismatches, u64 missed) {
    static SameStats stats;
    const u64 window = Nx2Profile::t_data.window_start;
    if (window != stats.window) [[unlikely]] {
        if (stats.window != 0 && window > stats.window && stats.calls != 0) {
            // Report's windows last 10 s (a little more); longer, windows without calls followed.
            constexpr u64 nominal = 10'000'000'000ULL;
            const u64 length = window - stats.window;
            LogSame(stats, length < nominal * 3 / 2 ? length : nominal);
        }
        stats = {};
        stats.window = window;
    }
    stats.mode = mode;
    ++stats.calls;
    stats.hits += hits;
    ++stats.calls_by[static_cast<size_t>(why)];
    stats.mismatches += mismatches;
    stats.missed += missed;
}

/// An MME DMA read of `words` words at `source` (On()), before its drain: what is pending of it,
/// and the words as guest memory holds them now.
inline ReadProbe DmaReadBefore(const Tegra::MemoryManager& memory, GPUVAddr source, u32 words) {
    State& state = Window();
    ReadProbe probe;
    probe.active = true;
    state.snapshot_pages = 0;
    const GpuSplit& split = GpuSplit::Get();
    const u64 consumed = split.DiagConsumed();
    probe.idle = consumed >= Bound();
    // Work pending and the pending writes do not depend on the range.
    const auto all = state.pending.Query(0, 0, consumed);
    probe.pending.work = all.work;
    probe.pending.entries = all.entries;
    const u64 bytes = u64{words} * sizeof(u32);
    bool first_page = true;
    for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + bytes; page += 0x1000) {
        ++probe.pages;
        const GPUVAddr begin = std::max(page, source);
        const GPUVAddr end = std::min<GPUVAddr>(page + 0x1000, source + bytes);
        const auto device = memory.GpuToCpuAddress(begin);
        if (!device) {
            probe.unmapped = true;
            continue;
        }
        state.snapshot_pages |= 1U << (probe.pages - 1);
        const auto pending = state.pending.Query(*device, *device + (end - begin), consumed);
        probe.pending.writes |= pending.writes;
        probe.pending.overflow |= pending.overflow;
        probe.gpu_page |= Tegra::Nx2IsGpuWritten(*device);
        probe.untracked |= Tegra::Nx2UntrackedWritten(*device, end - begin);
        if (first_page) {
            first_page = false;
            const u8 writer = state.first.Of(*device);
            probe.first_writer = writer != 0 ? static_cast<u8>(writer - 1)
                                 : Tegra::Nx2IsGpuWritten(*device)
                                     ? static_cast<u8>(WriteKind::Count)
                                 : Tegra::Nx2UntrackedWritten(*device, end - begin)
                                     ? static_cast<u8>(static_cast<u8>(WriteKind::Count) + 1)
                                     : static_cast<u8>(static_cast<u8>(WriteKind::Count) + 2);
        }
    }
    if (first_page) {
        probe.first_writer = static_cast<u8>(static_cast<u8>(WriteKind::Count) + 2);
    }
    // Pages written around the noted paths too (untracked: release words), and unmapped ones
    // (the syncpoint shim reads syncpoints, a sparse block gets memory as the back writes it, a
    // map may be queued): never free.
    probe.read_class =
        Classify(probe.idle, probe.pending, probe.gpu_page || probe.untracked || probe.unmapped);
    state.snapshot.assign(words, 0);
    for (u32 i = 0; i < words; ++i) {
        const GPUVAddr word = source + u64{i} * sizeof(u32);
        if ((state.snapshot_pages >> ((word >> 12) - (source >> 12))) & 1) {
            state.snapshot[i] = memory.Read<u32>(word);
        }
    }
    return probe;
}

/// The pages of [source, source + bytes) mapped now, bit per page from its first.
inline u32 MappedPages(const Tegra::MemoryManager& memory, GPUVAddr source, u64 bytes) {
    u32 mapped = 0;
    u32 index = 0;
    for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + bytes; page += 0x1000) {
        if (memory.GpuToCpuAddress(std::max(page, source))) {
            mapped |= 1U << index;
        }
        ++index;
    }
    return mapped;
}

/// ... and after it: `waited` for the back, `flushed` from the buffer cache.
inline void DmaReadAfter(const ReadProbe& probe, const Tegra::MemoryManager& memory,
                         GPUVAddr source, u32 words, bool fifoed, bool waited, bool flushed) {
    State& state = Window();
    // Words on pages mapped before and after compared; a page the drain mapped or unmapped
    // changed them.
    const u32 mapped = MappedPages(memory, source, u64{words} * sizeof(u32));
    bool changed = mapped != state.snapshot_pages;
    for (u32 i = 0; i < words && i < state.snapshot.size() && !changed; ++i) {
        const GPUVAddr word = source + u64{i} * sizeof(u32);
        if ((mapped >> ((word >> 12) - (source >> 12))) & 1) {
            changed = state.snapshot[i] != memory.Read<u32>(word);
        }
    }
    ReadStats& reads = state.reads;
    ++reads.reads;
    reads.fifoed += fifoed ? 1 : 0;
    reads.words += words;
    reads.pages += probe.pages;
    reads.waited += waited ? 1 : 0;
    reads.flushed += flushed ? 1 : 0;
    reads.changed += changed ? 1 : 0;
    reads.entries += probe.pending.entries;
    reads.untracked += probe.untracked ? 1 : 0;
    reads.unmapped += probe.unmapped ? 1 : 0;
    const auto index = static_cast<size_t>(probe.read_class);
    ++reads.by_class[index];
    reads.changed_by_class[index] += changed ? 1 : 0;
    for (size_t kind = 0; kind < reads.hits.size(); ++kind) {
        reads.hits[kind] += (probe.pending.writes >> kind) & 1;
    }
    reads.dropped_hits += probe.pending.overflow ? 1 : 0;
    ++reads.first_writer[std::min<size_t>(probe.first_writer, reads.first_writer.size() - 1)];
    if (probe.read_class == ReadClass::Work) {
        auto& entry = reads.work_masks[probe.pending.work];
        ++entry[0];
        entry[1] += changed ? 1 : 0;
    }
    if (state.current_macro != ~0U) {
        MacroTable::Entry& entry = state.macros.Find(state.current_macro);
        ++entry.dma_reads;
        entry.dma_words += words;
    }
}

/// EDEN_NX2_PROBE_FRONT_US=n (read once): microseconds the front spins per command list.
inline u32 ProbeFrontUs() {
    static const u32 us = [] {
        const char* value = std::getenv("EDEN_NX2_PROBE_FRONT_US");
        const u32 parsed =
            value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        if (parsed != 0) {
            LOG_INFO(HW_GPU,
                     "EDEN_NX2_PROBE_FRONT_US={}: the GPU thread's front spins {} us per command "
                     "list it pushes",
                     parsed, parsed);
        }
        return parsed;
    }();
    return us;
}

inline void SpinFor(u32 us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds{us};
    while (std::chrono::steady_clock::now() < until) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

inline void LogWindow(State& state, u64 window_ns, double ns_per_tick) {
    const double per_second = 1e9 / static_cast<double>(window_ns);
    const auto rate = [per_second](u64 value) { return static_cast<double>(value) * per_second; };
    const auto ms = [&](double ticks) { return ticks * ns_per_tick * per_second / 1e6; };

    // Macros.
    u64 calls = 0;
    u64 hle = 0;
    double ticks = 0;
    double child = 0;
    std::string top;
    for (const MacroTable::Entry* entry : state.macros.Top(~size_t{0})) {
        calls += entry->calls;
        hle += entry->hle_calls;
        ticks += entry->EstimatedTicks();
        if (entry->sampled != 0) {
            child += static_cast<double>(entry->child_ticks) * static_cast<double>(entry->calls) /
                     static_cast<double>(entry->sampled);
        }
    }
    for (const MacroTable::Entry* entry : state.macros.Top(16)) {
        const double per_call = entry->sampled != 0
                                    ? static_cast<double>(entry->ticks) * ns_per_tick /
                                          static_cast<double>(entry->sampled)
                                    : 0.0;
        const double nested = entry->sampled != 0
                                  ? static_cast<double>(entry->child_ticks) * ns_per_tick /
                                        static_cast<double>(entry->sampled)
                                  : 0.0;
        const char* kind = entry->hle_calls == 0               ? "J"
                           : entry->hle_calls == entry->calls ? "H"
                                                              : "JH";
        const u64 jit_calls = entry->calls - entry->hle_calls;
        const auto per_jit_call = [jit_calls](u64 value) {
            return jit_calls != 0 ? static_cast<double>(value) / static_cast<double>(jit_calls)
                                  : 0.0;
        };
        top += fmt::format("{}{:#x} {} {:.0f} x {:.0f} ns (nested {:.0f} ns) dma {:.0f} emit "
                           "{:.1f} read {:.1f}",
                           top.empty() ? "" : ", ", entry->start, kind, rate(entry->calls),
                           per_call, nested, rate(entry->dma_reads), per_jit_call(entry->emits),
                           per_jit_call(entry->reads));
    }
    LOG_INFO(HW_GPU,
             "GPU thread front macros per second: calls {:.0f} ({} starts; JIT {:.0f}, HLE {:.0f}, "
             "interpreter 0) taking {:.1f} ms exclusive, {:.1f} ms nested (sampled 1/{}) | first "
             "16 (start kind calls x exclusive ns, per JIT call emits and register reads): {} | "
             "indirect draw checks looked up {:.0f}, decided already {:.1f}%",
             rate(calls), state.macros.Used(), rate(calls - hle), rate(hle), ms(ticks), ms(child),
             1U << MacroSampleBits, top.empty() ? "-" : top, rate(state.indirect_lookups),
             state.indirect_lookups != 0 ? 100.0 * static_cast<double>(state.indirect_decided) /
                                               static_cast<double>(state.indirect_lookups)
                                         : 0.0);

    // Sends.
    std::string sends;
    u64 send_calls = 0;
    double send_ticks = 0;
    for (size_t i = 0; i < state.send_calls.size(); ++i) {
        const double estimated =
            state.send_sampled[i] != 0 ? static_cast<double>(state.send_ticks[i]) *
                                             static_cast<double>(state.send_calls[i]) /
                                             static_cast<double>(state.send_sampled[i])
                                       : 0.0;
        send_calls += state.send_calls[i];
        send_ticks += estimated;
        sends += fmt::format("{}{} {:.0f} in {:.1f} ms", sends.empty() ? "" : ", ",
                             send_kind_names[i], rate(state.send_calls[i]), ms(estimated));
    }
    const double macro_write_ticks =
        state.macro_writes_sampled != 0
            ? static_cast<double>(state.macro_write_ticks) *
                  static_cast<double>(state.macro_writes) /
                  static_cast<double>(state.macro_writes_sampled)
            : 0.0;
    const FrontRuns& runs = state.runs;
    std::string run_lengths;
    for (const u64 count : runs.state_runs) {
        run_lengths += fmt::format("{}{:.0f}", run_lengths.empty() ? "" : "/", rate(count));
    }
    LOG_INFO(HW_GPU,
             "GPU thread front sends per second: method calls {:.0f} taking {:.1f} ms ({}) | macro "
             "writes {:.0f} taking {:.1f} ms (sampled 1/{}, nested zones out) | 3D front "
             "increasing runs (1/2/3-4/5-8/more words): {} ({:.0f} words), into macros {:.0f} | "
             "increase once macro calls: one send {:.0f}, two sends {:.0f}, split {:.0f}; state "
             "{:.0f}",
             rate(send_calls), ms(send_ticks), sends, rate(state.macro_writes),
             ms(macro_write_ticks), 1U << SendSampleBits, run_lengths, rate(runs.state_words),
             rate(runs.macro_runs), rate(runs.once_one), rate(runs.once_two),
             rate(runs.once_split), rate(runs.once_state));

    // Register writes.
    if (!RegistersOn()) {
        LOG_INFO(HW_GPU,
                 "GPU thread front register writes per second: off (EDEN_NX2_FRONT_DIAG_REGS=0)");
    } else {
        const auto totals = state.registers.Totals();
        std::string same;
        for (const auto& row : state.registers.Top(16)) {
            same += fmt::format("{}{:#x} {:.0f}/{:.0f} ({:.0f} not sent)", same.empty() ? "" : ", ",
                                row.method, rate(row.same), rate(row.total), rate(row.skipped));
        }
        const auto& pushed = state.registers.Origin(false);
        const auto& macro = state.registers.Origin(true);
        LOG_INFO(HW_GPU,
                 "GPU thread front register writes per second: {:.0f} ({:.0f} same value, {:.0f} "
                 "not sent) | same value by method (first 16, same/all): {} | by origin (all/same "
                 "value/not sent): macro {:.0f}/{:.0f}/{:.0f}, push {:.0f}/{:.0f}/{:.0f}",
                 rate(totals[0]), rate(totals[1]), rate(totals[2]), same.empty() ? "-" : same,
                 rate(macro[0]), rate(macro[1]), rate(macro[2]), rate(pushed[0]), rate(pushed[1]),
                 rate(pushed[2]));
    }

    // Drains.
    std::string drains;
    for (size_t site = 0; site < state.drains.size(); ++site) {
        const DrainHistogram& histogram = state.drains[site];
        if (histogram.Calls() == 0) {
            continue;
        }
        const auto bands = histogram.ToBands(ns_per_tick);
        std::string text;
        for (const u64 band : bands) {
            text += fmt::format("{}{:.0f}", text.empty() ? "" : "/", rate(band));
        }
        drains += fmt::format("{}{} {:.0f}: {}", drains.empty() ? "" : ", ",
                              Nx2Profile::drain_site_names[site], rate(histogram.Calls()), text);
    }
    LOG_INFO(HW_GPU,
             "GPU thread front drains per second (calls: waits under 2/8/32/128/512/2048/8192 us "
             "and over): {}",
             drains.empty() ? "-" : drains);

    // MME DMA reads.
    const ReadStats& reads = state.reads;
    if (reads.reads == 0) {
        LOG_INFO(HW_GPU, "GPU thread MME DMA reads per second: 0");
        return;
    }
    std::string classes;
    for (size_t i = 0; i < reads.by_class.size(); ++i) {
        classes += fmt::format("{}{} {:.1f} ({:.1f} changed)", classes.empty() ? "" : ", ",
                               read_class_names[i], rate(reads.by_class[i]),
                               rate(reads.changed_by_class[i]));
    }
    std::string hits;
    for (size_t i = 0; i < reads.hits.size(); ++i) {
        hits += fmt::format("{}{} {:.0f}", hits.empty() ? "" : ", ", write_kind_names[i],
                            rate(reads.hits[i]));
    }
    std::vector<std::pair<u32, std::array<u64, 2>>> masks(reads.work_masks.begin(),
                                                          reads.work_masks.end());
    std::sort(masks.begin(), masks.end(),
              [](const auto& a, const auto& b) { return a.second[0] > b.second[0]; });
    std::string work;
    for (size_t i = 0; i < masks.size() && i < 8; ++i) {
        std::string name;
        for (size_t kind = 0; kind < work_kind_names.size(); ++kind) {
            if ((masks[i].first >> kind) & 1) {
                name += fmt::format("{}{}", name.empty() ? "" : "+", work_kind_names[kind]);
            }
        }
        work += fmt::format("{}{} {:.1f} ({:.1f} changed)", work.empty() ? "" : ", ", name,
                            rate(masks[i].second[0]), rate(masks[i].second[1]));
    }
    std::string writers;
    for (size_t i = 0; i < reads.first_writer.size(); ++i) {
        const char* name = i < write_kind_names.size()        ? write_kind_names[i]
                           : i == write_kind_names.size()     ? "gpu"
                           : i == write_kind_names.size() + 1 ? "untracked"
                                                              : "none";
        writers += fmt::format("{}{} {:.0f}", writers.empty() ? "" : ", ", name,
                               rate(reads.first_writer[i]));
    }
    const auto share = [&](u64 value) {
        return 100.0 * static_cast<double>(value) / static_cast<double>(reads.reads);
    };
    const u64 idle = reads.by_class[static_cast<size_t>(ReadClass::Idle)];
    const u64 free_reads = reads.by_class[static_cast<size_t>(ReadClass::Free)];
    const u64 work_reads = reads.by_class[static_cast<size_t>(ReadClass::Work)];
    LOG_INFO(HW_GPU,
             "GPU thread MME DMA reads per second: {:.1f} ({:.1f} fifoed; {:.1f} words, {:.2f} "
             "pages each), waited {:.1f}, flushed {:.1f}, changed by the drain {:.1f} | by class: "
             "{} | no pending write {:.1f}% (with work pending {:.1f}%), unchanged {:.1f}% | write "
             "hits: {}, dropped {:.0f} | work pending (first 8): {} | first writer of the first "
             "page: {} | untracked pages {:.1f} | unmapped pages {:.1f} | pending writes {:.1f} per "
             "read",
             rate(reads.reads), rate(reads.fifoed),
             static_cast<double>(reads.words) / static_cast<double>(reads.reads),
             static_cast<double>(reads.pages) / static_cast<double>(reads.reads),
             rate(reads.waited), rate(reads.flushed), rate(reads.changed), classes,
             share(idle + free_reads), share(idle + free_reads + work_reads),
             share(reads.reads - reads.changed),
             hits, rate(reads.dropped_hits), work.empty() ? "-" : work, writers,
             rate(reads.untracked), rate(reads.unmapped),
             static_cast<double>(reads.entries) / static_cast<double>(reads.reads));
}

} // namespace VideoCommon::Nx2FrontDiag
