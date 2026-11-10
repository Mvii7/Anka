// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/assert.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "shader_recompiler/ir_opt/nx2_root.h"
#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_back_cpu.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/nx2_stage_proxy.h"
#include "video_core/nx2_stage_split.h"
#include "video_core/nx2_stages.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

// The back stages' core (T1): the stages below A (StagePipeline, nx2_stage_ring.h) run with the
// GPU's handler here, A's side of the records and joins, the owner checks, Start and Stop. A's
// loop is GpuSplit::RunStagedBack (gpu_split.cpp).
namespace VideoCommon::Nx2Stages {

// JoinFromA counts Counter::JoinsAEngine + reason.
static_assert(static_cast<size_t>(Nx2Profile::Counter::JoinsAOther) -
                  static_cast<size_t>(Nx2Profile::Counter::JoinsAEngine) + 1 ==
              static_cast<size_t>(JoinReason::Count));
static_assert(StageIdA == static_cast<u8>(Stage::A) && StageIdB == static_cast<u8>(Stage::B) &&
              StageIdC == static_cast<u8>(Stage::C));
static_assert(std::is_same_v<RecordThunk, Thunk> && std::is_same_v<RecordRing1Thunk, Ring1Thunk>);

namespace {

using Nx2Profile::Counter;
using Nx2Profile::Zone;
using Tegra::Engines::Maxwell3D;

template <Zone Which>
struct ZoneScope : Nx2Profile::Scope {
    ZoneScope() : Scope{Which} {}
};

Core::System* g_system{};
VideoCore::RasterizerInterface* g_real{};
bool g_check{};
/// The stage recording Vulkan commands (RecordOwner): B, C with cut c, A in its joins.
std::atomic<u8> g_owner{StageIdB};
u8 g_default_owner{StageIdB};
/// The default owner runs records (CheckRecordOwner: the front may use the scheduler otherwise).
/// A line of its own: C stores it around each of its batches, B reads g_owner and the hooks
/// around it for each record and packet (k2hiz: the line moved between them).
struct alignas(64) RecordBusy {
    std::atomic<bool> value{};
};
RecordBusy g_record_busy_line;
std::atomic<bool>& g_record_busy = g_record_busy_line.value;
/// The record side's begin and end of the stages (SetStageHooks).
void (*g_hook_begin)(void*){};
void (*g_hook_end)(void*){};
/// SetCbWriteCheck's (B's CbData records with cut c).
CbWriteCheck g_cb_write_check{};
void* g_cb_write_context{};
bool CbWriteJoins(Maxwell3D& replica, u64 address, u32 count);
void* g_hook_context{};
/// EDEN_NX2_PRESENT_DIAG=1 or EDEN_NX2_MODE_DIAG=1: with cut c, B's joins and C's ParkBs timed,
/// and a "nx2 stages diag" line a second from B while the stages run (JoinDiagReport). Set by
/// Start before B and C start (the switch file is in the environment by then; not at static
/// initialization, before main loads nx2_switches.txt).
bool g_join_diag{};
void JoinDiagReport();

/// What B and C do with the records (StagePipeline's handler).
struct GpuHandler {
    struct Scopes {
        using InWait = ZoneScope<Zone::StageInWait>;
        using OutWait = ZoneScope<Zone::StageOutWait>;
        using JoinWait = ZoneScope<Zone::JoinWait>;
        using ParkWait = ZoneScope<Zone::ParkWait>;
    };

    /// The primary Maxwell3D each stage's deltas are for (Target records).
    std::array<Maxwell3D*, 3> targets{};

    Maxwell3D* Replica(u8 stage) const {
        Maxwell3D* const target = targets[stage];
        return target != nullptr ? ReplicaOf(*target, static_cast<Stage>(stage)) : nullptr;
    }

    u32 PendingDeltas() {
        return Capture().PendingWords();
    }
    void TakeDeltas(u64* out, u32 count) {
        Capture().TakeWords(out);
        Nx2Profile::Add(Counter::DeltaWords, count);
    }
    void Apply(u8 stage, const u64* words, u32 count) {
        if (Maxwell3D* const replica = Replica(stage)) {
            Nx2Profile::Scope scope{Zone::ReplicaApply};
            ApplyDelta(*replica, words, count);
        }
    }
    void Target(u8 stage, u64 primary) {
        targets[stage] = reinterpret_cast<Maxwell3D*>(primary);
    }
    void CbData(u8 stage, u64 address, const u32* data, u32 count) {
        if (Maxwell3D* const replica = Replica(stage)) {
            Nx2Profile::FineScope scope{Zone::CbData};
            if (g_cb_write_check != nullptr && stage == StageIdB &&
                CbWriteJoins(*replica, address, count)) [[unlikely]] {
                // Into a pool C may still read for work B sent before (omur-1): C parked having
                // run it, B the record owner.
                JoinFromB([&] { StageCbWrite(*replica, address, data, count); });
                return;
            }
            StageCbWrite(*replica, address, data, count);
        }
    }
    void ExecA(u64 position, u64 words) {
        GpuSplit::Get().StageExecute(position, words);
    }
    void Mark(u8, u64 position, bool caught_up) {
        GpuSplit::Get().StageConsumed(position, caught_up);
    }
    void Verify(u8 stage) {
        if (Maxwell3D* const target = targets[stage]) {
            VerifyReplica(*target, static_cast<Stage>(stage));
        }
    }
    void BadSequence(u8 stage, u16 expected, u16 seen) {
        static std::atomic<u32> reports{};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_CRITICAL(HW_GPU, "GPU stages: {} read record {} where {} was due",
                         Nx2Profile::stage_report_names[stage], seen, expected);
        }
    }
    void ThreadStart(u8 stage) {
        Detail::t_stage = stage;
        if (stage == StageIdB) {
            // The stage a frame waits on (S19): Critical, and the back's core of its own
            // (EDEN_NX2_BACK_CPU); A and C are kept off it with the process's other threads.
            Common::SetCurrentThreadName("GPUMem");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
            Common::SetCurrentThreadToPerformanceCores();
            Nx2BackCpu::StartBack();
        } else {
            Common::SetCurrentThreadName("GPURec");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
            Common::SetCurrentThreadToPerformanceCores();
        }
        g_system->RegisterHostThread();
    }
    void Report(u8 stage) {
        Nx2Profile::Report(Nx2Profile::stage_report_names[stage]);
        if (g_join_diag && stage == StageIdB) {
            JoinDiagReport();
        }
    }
    void Busy(u8 stage, bool busy) {
        if (stage == g_default_owner) {
            g_record_busy.store(busy, busy ? std::memory_order_relaxed : std::memory_order_release);
        }
    }
    void Wrote(u8 to_stage, size_t bytes) {
        Nx2Profile::Add(to_stage == StageIdB ? Counter::RingBytesB : Counter::RingBytesC, bytes);
    }
    void Parked() {
        Nx2Profile::Add(Counter::Parks, 1);
    }
    void Hazard(u8 stage, StageHazard what, u64 a, u64 b);
    void JoinStall(u64 c_record, u64 target, u64 ms, bool stuck);
    /// C at a record boundary, asked (StageTickWork): the GPU's sync requests, in a ParkB.
    void BoundaryWork() {
        g_system->GPU().TickWork();
    }
};

GpuHandler g_handler;
StagePipeline<GpuHandler> g_pipeline{g_handler};

bool CbWriteJoins(Maxwell3D& replica, u64 address, u32 count) {
    return g_pipeline.Options().cut_c &&
           g_cb_write_check(g_cb_write_context, replica, address, u64{count} * sizeof(u32));
}

/// C runs a ParkB's op (B parked): where it may take the buffer cache mutex (K3, CheckBufferLock)
/// and run the GPU's sync requests (StageTickWork).
thread_local bool t_park_op{};
/// B inside JoinFromBLocked (the buffer cache mutex held): JoinStall's report says so.
thread_local bool t_locked_join{};
/// The same for the other threads (CheckRecordOwner): C uses the caches and the scheduler, the
/// front may not, caught up or not (C holds the mark until the op ran: T9b review protokol-1).
std::atomic<bool> g_c_park_op{};

/// g_join_diag's counts this second: B's joins (B's thread alone) and C's ParkBs (C adds, B
/// takes them for the line).
struct JoinDiag {
    u64 joins{};
    u64 locked{};      ///< JoinFromBLocked's of them
    u64 wait_ns{};     ///< B asked until C parked (the op began)
    u64 wait_max_ns{};
    u64 op_ns{}; ///< the ops, C parked
    std::atomic<u64> parks{};
    std::atomic<u64> park_wait_ns{}; ///< C asked until B parked (C runs ring 3 on meanwhile)
    std::atomic<u64> park_wait_max_ns{};
    u64 second_start_ns{};
    u64 second_presents{};
};
JoinDiag g_join_stats;

u64 DiagNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// B after a join it asked for at `asked`, its op begun at `entered` (C parked).
void NoteJoinB(u64 asked, u64 entered, bool locked) {
    JoinDiag& d = g_join_stats;
    const u64 wait = entered - asked;
    ++d.joins;
    d.locked += locked ? 1 : 0;
    d.wait_ns += wait;
    d.wait_max_ns = (std::max)(d.wait_max_ns, wait);
    d.op_ns += DiagNowNs() - entered;
}

/// C in a ParkB's op, `wait` after it asked.
void NoteParkB(u64 wait) {
    JoinDiag& d = g_join_stats;
    d.parks.fetch_add(1, std::memory_order_relaxed);
    d.park_wait_ns.fetch_add(wait, std::memory_order_relaxed);
    if (wait > d.park_wait_max_ns.load(std::memory_order_relaxed)) {
        d.park_wait_max_ns.store(wait, std::memory_order_relaxed);
    }
}

/// B's loop at each pass (GpuHandler::Report), with cut c: the line once a second. A frame is a
/// present of the guest's (Core::g_nx2_present_count); "-" when none came.
void JoinDiagReport() {
    if (!g_pipeline.Options().cut_c) {
        return;
    }
    JoinDiag& d = g_join_stats;
    const u64 now = DiagNowNs();
    const u64 presents = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    if (d.second_start_ns == 0) {
        d.second_start_ns = now;
        d.second_presents = presents;
        return;
    }
    if (now - d.second_start_ns < 1'000'000'000) {
        return;
    }
    const u64 frames = presents - d.second_presents;
    const u64 parks = d.parks.exchange(0, std::memory_order_relaxed);
    const u64 park_wait = d.park_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 park_max = d.park_wait_max_ns.exchange(0, std::memory_order_relaxed);
    const auto per_frame = [frames](u64 count) {
        return frames != 0
                   ? fmt::format("{:.1f}", static_cast<double>(count) / static_cast<double>(frames))
                   : std::string{"-"};
    };
    const auto avg_us = [](u64 ns, u64 count) {
        return count != 0 ? static_cast<double>(ns) / 1e3 / static_cast<double>(count) : 0.0;
    };
    LOG_INFO(HW_GPU,
             "nx2 stages diag {:.2f} s: {} presents; joins from B {} ({} a frame, {} locked), "
             "wait for C {:.0f} us avg {:.0f} us max, C parked {:.0f} us avg, {:.2f} ms in all; "
             "ParkB {} ({} a frame), wait for B {:.0f} us avg {:.0f} us max",
             static_cast<double>(now - d.second_start_ns) / 1e9, frames, d.joins,
             per_frame(d.joins), d.locked, avg_us(d.wait_ns, d.joins),
             static_cast<double>(d.wait_max_ns) / 1e3, avg_us(d.op_ns, d.joins),
             static_cast<double>(d.wait_ns + d.op_ns) / 1e6, parks, per_frame(parks),
             avg_us(park_wait, parks), static_cast<double>(park_max) / 1e3);
    d.joins = d.locked = d.wait_ns = d.wait_max_ns = d.op_ns = 0;
    d.second_start_ns = now;
    d.second_presents = presents;
}

/// The first reports of a check (CheckStage, CheckRecordOwner), with the stack (as
/// GpuSplit::CheckOwner) on stderr, and in short in the log (the measuring scripts read the log
/// alone): where, the thread's name and the first frames.
void ReportStack(const char* check, const char* where) {
#ifdef _WIN32
    constexpr u32 MaxReports = 20;
    constexpr USHORT LogFrames = 6;
    static std::atomic<u32> reports{};
    const u32 report = reports.fetch_add(1, std::memory_order_relaxed);
    if (report >= MaxReports) {
        return;
    }
    const u8 stage = Detail::t_stage;
    const char* const stage_name =
        stage < 3 ? Nx2Profile::stage_report_names[stage] : "no stage";
    const char* const owner_name =
        Nx2Profile::stage_report_names[g_owner.load(std::memory_order_relaxed)];
    char thread_name[64] = "?";
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    if (PWSTR description{}; get_description != nullptr &&
                             SUCCEEDED(get_description(GetCurrentThread(), &description))) {
        if (description[0] != L'\0') {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name),
                                nullptr, nullptr);
            thread_name[sizeof(thread_name) - 1] = '\0';
        }
        LocalFree(description);
    }
    std::fprintf(stderr, "GPU %s: %s on thread %lu %s (%s%s), the record owner is %s\n", check,
                 where, GetCurrentThreadId(), thread_name, stage_name,
                 Detail::t_in_join ? ", in a join" : "", owner_name);
    std::string log_frames;
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
        const auto offset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                            reinterpret_cast<uintptr_t>(module));
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name, offset);
        if (i < LogFrames) {
            log_frames += fmt::format("{}{}+{:#x}", i == 0 ? "" : " < ",
                                      base_name ? base_name + 1 : name, offset);
        }
    }
    std::fflush(stderr);
    LOG_WARNING(HW_GPU, "GPU {} ({}/{}): {} on thread {} '{}' ({}{}), the record owner is {}; {}",
                check, report + 1, MaxReports, where, GetCurrentThreadId(), thread_name,
                stage_name, Detail::t_in_join ? ", in a join" : "", owner_name, log_frames);
#else
    LOG_ERROR(HW_GPU, "GPU {}: {}", check, where);
#endif
}

/// A record's producer (K4, K9): never A inside a join, and 1 + payload words at most
/// MaxBodyWords. The asserts only when one fails: Reserve runs for each record.
void CheckRecord(u32 payload_words) {
    if (InJoin() || payload_words >= MaxBodyWords) [[unlikely]] {
        ASSERT_MSG(!InJoin(), "GPU stages: a record from A inside a join");
        ASSERT_MSG(1 + payload_words <= MaxBodyWords, "GPU stages: a record of {} words",
                   1 + payload_words);
    }
}

void GpuHandler::Hazard(u8 stage, StageHazard what, u64 a, u64 b) {
    // A broken K3 rule (never in a correct run): counted, the first ones logged with their stack.
    Nx2Profile::Add(Counter::StageViolations, 1);
    const char* const text = what == StageHazard::ParkInRecord
                                 ? "ParkB inside a record at =1 (C cannot run on to where B parks)"
                                 : "a join from B acked short of its target";
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_CRITICAL(HW_GPU, "GPU stages: {} on {} ({:#x}, {:#x})", text,
                     Nx2Profile::stage_report_names[stage], a, b);
    }
    if (g_check) {
        ReportStack("stage hazard", text);
    }
}

void GpuHandler::JoinStall(u64 c_record, u64 target, u64 ms, bool stuck) {
    // STAGE_CHECK, on B while its join still waits (T9b review kilit-4). A deadlock never ends:
    // C waiting on a lock B holds on its way to the join's target (K3 (a): the buffer cache mutex
    // in a locked join), on one record. A C behind (a pipeline built on C, a wait for the GPU)
    // goes on from record to record: a slow join, lines of its own and no stack, so slow joins
    // leave the deadlock reports to a deadlock (R2c review).
    const char* const locked = t_locked_join ? "locked " : "";
    if (!stuck) {
        static std::atomic<u32> slow{};
        if (slow.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_WARNING(HW_GPU,
                        "GPU stages: a {}join from B has waited {} ms for C, at the record at "
                        "{:#x} (the join's target {:#x}): a slow join",
                        locked, ms, c_record, target);
        }
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 8) {
        return;
    }
    LOG_WARNING(HW_GPU,
                "GPU stages: a {}join from B has waited {} ms for C, on the record at {:#x} since "
                "the report before (the join's target {:#x}): K3 deadlock suspect",
                locked, ms, c_record, target);
    // B's stack once a wait: it does not change while B waits.
    thread_local u64 t_stack_target = ~u64{0};
    if (std::exchange(t_stack_target, target) != target) {
        ReportStack("join from B waits", t_locked_join ? "JoinFromBLocked" : "JoinFromB");
    }
}

u64* BeginRecord(Ring ring, RecordKind kind, u32 body_words, u32 flags = 0) {
    return ring == Ring::ToB ? g_pipeline.BeginB(kind, body_words, flags)
                             : g_pipeline.BeginC(kind, body_words, flags);
}

void EndRecord(Ring ring) {
    if (ring == Ring::ToB) {
        g_pipeline.EndB();
    } else {
        g_pipeline.EndC();
    }
}

} // Anonymous namespace

const Config& Get() {
    static const Config config = [] {
        const StageOptions options = ParseStageOptions(
            std::getenv("EDEN_NX2_BACK_STAGES"), std::getenv("EDEN_NX2_BACK_CUTS"),
            std::getenv("EDEN_NX2_STAGE_CHECK"), std::getenv("EDEN_NX2_BACK_STAGES_EVERY"));
        return Config{
            .mode = options.mode,
            .cut_a = options.mode != 0,
            .cut_c = options.cut_c,
            .check = options.check,
            .verify_every = options.verify_every,
        };
    }();
    return config;
}

u64* Reserve(Ring ring, Thunk thunk, u32 payload_words) {
    CheckRecord(payload_words);
    u64* const body = BeginRecord(ring, RecordKind::Call, 1 + payload_words);
    body[0] = reinterpret_cast<u64>(thunk);
    return body + 1;
}

void Commit(Ring ring) {
    EndRecord(ring);
}

void Publish(Ring ring) {
    if (ring == Ring::ToB) {
        g_pipeline.PublishB();
    } else {
        g_pipeline.PublishC();
    }
}

void Forward(Ring ring, Ring1Thunk thunk, const u64* payload, u32 payload_words) {
    CheckRecord(payload_words);
    u64* const body = BeginRecord(ring, RecordKind::Forward, 1 + payload_words);
    body[0] = reinterpret_cast<u64>(thunk);
    std::memcpy(body + 1, payload, size_t{payload_words} * sizeof(u64));
    EndRecord(ring);
}

void PostCbData(Ring ring, Tegra::Engines::Maxwell3D& primary, GPUVAddr address, const u32* data,
                u32 count) {
    CheckRecord(0);
    Capture().SelectTarget(primary);
    if (ring == Ring::ToB) {
        g_pipeline.PostCbData(address, data, count);
        return;
    }
    do {
        const u32 part = (std::min)(count, decltype(g_pipeline)::MaxCbDataCount);
        const u32 bytes = part * static_cast<u32>(sizeof(u32));
        const u32 data_words = (bytes + 7) / 8;
        u64* const body = BeginRecord(ring, RecordKind::CbData, 2 + data_words);
        body[0] = address;
        body[1] = bytes;
        if (data_words != 0) {
            body[1 + data_words] = 0;
        }
        std::memcpy(body + 2, data, bytes);
        EndRecord(ring);
        address += bytes;
        data += part;
        count -= part;
    } while (count != 0);
}

void PostTarget(Ring ring, Tegra::Engines::Maxwell3D* primary) {
    // The stages off (a capture without them, YUZU_TESTS' unit test): no ring to write to.
    if (!Active()) {
        return;
    }
    CheckRecord(1);
    u64* const body = BeginRecord(ring, RecordKind::Target, 1);
    body[0] = reinterpret_cast<u64>(primary);
    EndRecord(ring);
}

void PostMark(Ring ring, u64 ring1_position) {
    CheckRecord(1);
    u64* const body = BeginRecord(ring, RecordKind::Mark, 1);
    body[0] = ring1_position;
    EndRecord(ring);
}

void JoinFromA(JoinReason reason, FunctionRef op) {
    // Nested, from another thread, off or =3 (K4): at once, no record, no wait, not counted.
    if (!Forwarding() || !Active() || g_pipeline.Options().mode == 3) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsA, 1);
    Nx2Profile::Add(
        static_cast<Counter>(static_cast<u32>(Counter::JoinsAEngine) + static_cast<u32>(reason)),
        1);
    const auto start =
        g_check ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const u32 join = g_pipeline.JoinABegin();
    if (g_check) {
        const auto waited = std::chrono::steady_clock::now() - start;
        if (waited > std::chrono::milliseconds{50}) {
            LOG_WARNING(HW_GPU,
                        "GPU stages: a join from A waited {} ms for the stages to park "
                        "(reason {})",
                        std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                        static_cast<u32>(reason));
        }
    }
    Detail::t_in_join = true;
    g_owner.store(StageIdA, std::memory_order_release);
    op();
    g_owner.store(g_default_owner, std::memory_order_release);
    Detail::t_in_join = false;
    g_pipeline.JoinAEnd(join);
}

void JoinFromB(FunctionRef op) {
    // Nested: B inside a join's op is the record owner with C parked already.
    if (!Active() || !OnStage(Stage::B) || !g_pipeline.Options().cut_c ||
        g_owner.load(std::memory_order_relaxed) == StageIdB) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsB, 1);
    const u64 asked = g_join_diag ? DiagNowNs() : 0;
    u64 entered = 0;
    g_pipeline.JoinFromB([&] {
        if (g_join_diag) {
            entered = DiagNowNs();
        }
        g_owner.store(StageIdB, std::memory_order_release);
        {
            Nx2Profile::FineScope scope{Zone::JoinOp};
            op();
        }
        g_owner.store(g_default_owner, std::memory_order_release);
    });
    if (g_join_diag) {
        NoteJoinB(asked, entered, false);
    }
}

void JoinFromBLocked(FunctionRef op) {
    if (!Active() || !OnStage(Stage::B) || !g_pipeline.Options().cut_c ||
        g_owner.load(std::memory_order_relaxed) == StageIdB) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsB, 1);
    // B holds the buffer cache mutex while it waits: C reaching the join's target must not take
    // it (K3 (a)). STAGE_CHECK reports a wait that lasts while it lasts (GpuHandler::JoinStall).
    t_locked_join = true;
    const u64 asked = g_join_diag ? DiagNowNs() : 0;
    u64 entered = 0;
    g_pipeline.JoinFromBLocked([&] {
        if (g_join_diag) {
            entered = DiagNowNs();
        }
        t_locked_join = false;
        g_owner.store(StageIdB, std::memory_order_release);
        {
            Nx2Profile::FineScope scope{Zone::JoinOp};
            op();
        }
        g_owner.store(g_default_owner, std::memory_order_release);
    });
    t_locked_join = false;
    if (g_join_diag) {
        NoteJoinB(asked, entered, true);
    }
}

void ParkB(FunctionRef op) {
    if (!Active() || !OnStage(Stage::C)) {
        op();
        return;
    }
    const u64 asked = g_join_diag ? DiagNowNs() : 0;
    g_pipeline.ParkB([&] {
        if (g_join_diag) {
            NoteParkB(DiagNowNs() - asked);
        }
        const bool was_park_op = t_park_op;
        t_park_op = true;
        if (g_check) {
            g_c_park_op.store(true, std::memory_order_seq_cst);
        }
        op();
        if (g_check) {
            g_c_park_op.store(was_park_op, std::memory_order_seq_cst);
        }
        t_park_op = was_park_op;
    });
}

bool StageTickWork(bool idle, FunctionRef run) {
    if (!Active() || !g_pipeline.Options().cut_c) {
        return false;
    }
    if (idle) {
        // T9b review protokol-3: no ParkB for an empty queue (TICK_SKIP=0 ticks every draw).
        return true;
    }
    if (OnStage(Stage::C)) {
        if (t_park_op) {
            // B parked already: the requests run here.
            return false;
        }
        if (g_pipeline.CInRecord()) {
            // At the record boundary after this one (the boundary work calls TickWork again).
            g_pipeline.RequestBoundaryWork();
            return true;
        }
        ParkB(run);
        return true;
    }
    if (OnStage(Stage::B) || Forwarding()) {
        g_pipeline.RequestBoundaryWork();
        return true;
    }
    // A inside its join (B and C parked), a thread that is no stage.
    return false;
}

void CheckStage(Stage stage, const char* where) {
    if (!g_check || OnStage(stage) || (OnStage(Stage::A) && InJoin())) {
        return;
    }
    Nx2Profile::Add(Counter::StageViolations, 1);
    ReportStack("stage check", where);
}

bool MayTakeBufferLock() {
    return BufferLockAllowed(Active() && g_pipeline.Options().cut_c, OnStage(Stage::C), t_park_op,
                             Forwarding());
}

void CheckBufferLock(const char* where) {
    if (!g_check || MayTakeBufferLock()) {
        return;
    }
    Nx2Profile::Add(Counter::StageViolations, 1);
    ReportStack("buffer lock check", where);
}

void ReportStageHazard(const char* what) {
    if (!g_check) {
        return;
    }
    Nx2Profile::Add(Counter::StageViolations, 1);
    ReportStack("stage hazard", what);
}

Stage RecordOwner() {
    return static_cast<Stage>(g_owner.load(std::memory_order_acquire));
}

namespace {
thread_local u8 t_buffer_record_role = 0;
}

bool OwnsBufferRecords() {
    if (Active()) {
        return OnStage(RecordOwner()) || InJoin();
    }
    if (t_buffer_record_role == 2) {
        return true;
    }
    // The GPU thread records only while the back thread does not.
    return t_buffer_record_role == 1 && !GpuSplit::Get().Active();
}

void SetBufferRecordRole(u8 role) {
    t_buffer_record_role = role;
}

void SetStageHooks(void (*begin)(void*), void (*end)(void*), void* context) {
    g_hook_begin = begin;
    g_hook_end = end;
    g_hook_context = context;
}

void SetCbWriteCheck(CbWriteCheck check, void* context) {
    g_cb_write_check = check;
    g_cb_write_context = context;
}

void Start(Core::System& system) {
    const Config& config = Get();
    if (config.mode == 0 || Active()) {
        return;
    }
    g_system = &system;
    g_real = system.GPU().Renderer().ReadRasterizer();
    g_check = config.check;
    if (g_check) {
        // The scheduler's owner checks (GpuSplit::CheckOwner) check the record owner.
        GpuSplit::check_owner = true;
    }
    const StageOptions options{
        .mode = config.mode,
        .cut_c = config.cut_c && config.mode != 3,
        .check = config.check,
        .verify_every = config.verify_every,
    };
    g_default_owner = options.cut_c ? StageIdC : StageIdB;
    g_owner.store(g_default_owner, std::memory_order_relaxed);
    // B and C stopped: the diag line's second starts over with them.
    g_join_diag = [] {
        const auto on = [](const char* name) {
            const char* text = std::getenv(name);
            return text != nullptr && text[0] != '0';
        };
        return on("EDEN_NX2_PRESENT_DIAG") || on("EDEN_NX2_MODE_DIAG");
    }();
    g_join_stats.joins = g_join_stats.locked = g_join_stats.wait_ns = 0;
    g_join_stats.wait_max_ns = g_join_stats.op_ns = g_join_stats.second_start_ns = 0;
    g_join_stats.parks.store(0, std::memory_order_relaxed);
    g_join_stats.park_wait_ns.store(0, std::memory_order_relaxed);
    g_join_stats.park_wait_max_ns.store(0, std::memory_order_relaxed);
    g_record_busy.store(false, std::memory_order_relaxed);
    g_handler.targets = {};
    Capture().Reset();
    // No light draw follows one from before (S18).
    g_real->NoteEngineWork();
    // EDEN_NX2_BACK_CPU: the front on a core of its own too (S19); before B starts and pins.
    Nx2BackCpu::StartFront();
    if (options.cut_c && g_hook_begin != nullptr) {
        g_hook_begin(g_hook_context);
    }
    g_pipeline.Start(options);
    // =1 and =2 only (K1): =3 runs the primaries on BC as the unstaged back did, with no proxy
    // and no staged channel (a staged channel's hooks would send records from BC to its own ring).
    if (config.mode != 3) {
        InstallProxy(system, *g_real);
    }
    Detail::g_active.store(true, std::memory_order_seq_cst);
    LOG_INFO(HW_GPU, "GPU back stages: A | {}{} (EDEN_NX2_BACK_STAGES={}{}{})",
             options.cut_c ? "B | C" : "BC",
             config.mode == 2   ? ", lockstep with replica checks"
             : config.mode == 3 ? ", pass-through"
                                : "",
             config.mode, options.cut_c ? ", cuts ac" : "", config.check ? ", STAGE_CHECK" : "");
    // K2 (cut c) runs B | C before T9 classifies the events that need both: a trial of the
    // plumbing, not for play or measuring. Once per process (Start runs again after a Stop).
    static std::atomic_flag cut_c_warned{};
    if (options.cut_c && !cut_c_warned.test_and_set(std::memory_order_relaxed)) {
        LOG_WARNING(HW_GPU, "GPU back stages: cut c is a plumbing trial only until T9 "
                            "(EDEN_NX2_BACK_CUTS holds 'c'): not for play or measuring");
    }
    // EDEN_NX2_ROOT_TABLES (perf/hafif 53ac7bee8) keeps the root tables in the primary Maxwell3D:
    // A runs their methods, the replicas B and C draw with never get them (StageCopyFrom, the
    // deltas), so no unbound bank 17 is streamed from table 0. =3 draws with the primaries.
    static std::atomic_flag root_tables_warned{};
    if (config.mode != 3 && Shader::Optimization::Nx2Root::TablesEnabled() &&
        !root_tables_warned.test_and_set(std::memory_order_relaxed)) {
        LOG_WARNING(HW_GPU, "GPU back stages: EDEN_NX2_ROOT_TABLES does not reach the replicas "
                            "(no bank 17 from root table 0); use it with EDEN_NX2_BACK_STAGES=0");
    }
}

void Stop() {
    if (!Active()) {
        return;
    }
    // A stopped (GpuSplit::Stop joined it): B and C stop at their next record boundary, with
    // nothing left after a drain, and at shutdown without running what they hold.
    g_pipeline.Stop();
    if (g_pipeline.Options().cut_c && g_hook_end != nullptr) {
        g_hook_end(g_hook_context);
    }
    if (Get().mode != 3) {
        // As Start: only what it installed (K1).
        UninstallProxy(*g_system);
    }
    Capture().Reset();
    g_handler.targets = {};
    g_real->NoteEngineWork();
    g_owner.store(StageIdB, std::memory_order_relaxed);
    g_record_busy.store(false, std::memory_order_relaxed);
    Detail::g_active.store(false, std::memory_order_seq_cst);
}

namespace Detail {

void PostExecA(u64 ring1_position, u64 words) {
    u64* const body = g_pipeline.BeginB(RecordKind::ExecA, 2);
    body[0] = ring1_position;
    body[1] = words;
    g_pipeline.EndB();
}

void PostBatchEnd(u64 ring1_position) {
    u64* const body = g_pipeline.BeginB(RecordKind::Mark, 1, MarkCaughtUp);
    body[0] = ring1_position;
    g_pipeline.EndB();
    g_pipeline.PublishB();
}

void CheckRecordOwner(const char* where) {
    const u8 owner = g_owner.load(std::memory_order_acquire);
    if (t_stage == owner) {
        return;
    }
    if (OnStage(Stage::A) && !InJoin()) {
        // A reached the real rasterizer outside a join (S3).
        Nx2Profile::Add(Counter::StageUnproxied, 1);
    } else if (t_stage == NoStage && !g_c_park_op.load(std::memory_order_seq_cst) &&
               (!g_record_busy.load(std::memory_order_acquire) || GpuSplit::Get().BackCaughtUp())) {
        // The front after a drain, CPU threads: the stages are idle (as GpuSplit::CheckOwner).
        // Caught up, the last stage may still end its batch (busy) after the mark it stored, a
        // check (=2), a release: it touches no cache and no scheduler then (R1 eszaman-3). Not
        // while C runs a ParkB's op: it does, past the marks it ran on to.
        return;
    }
    ReportStack("stages owner check", where);
}

} // namespace Detail

} // namespace VideoCommon::Nx2Stages
