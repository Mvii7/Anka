// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <type_traits>

#include "common/common_types.h"
#include "video_core/nx2_stage_ring.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class Maxwell3D;
}

// EDEN_NX2_BACK_STAGES: the split GPU thread's back (gpu_split.h) as a pipeline of stages.
// A ("GPUBack") runs the engines against their primary registers; their rasterizer is a proxy
// (nx2_stage_proxy.h) that sends what the caches and Vulkan do to B ("GPUMem": memory, buffer
// cache) and C ("GPURec": textures, the Vulkan scheduler) through rings of records, or runs it on
// A while the stages below are parked (a join). B and C read per-channel replicas of Maxwell3D,
// brought up to date with register deltas (nx2_reg_replica.h). Cut a (Kesim-1): A | BC, B does
// everything C would; cut c (Kesim-2): A | B | C. Off (=0): no thread, ring or replica; nothing
// behaves differently. Contract of the parallel tasks (PLAN.md §2.4); owner of the bodies: T1.
namespace VideoCommon::Nx2Stages {

/// The stages. With cut a alone, B is "BC" (memory and record).
enum class Stage : u8 { A, B, C };

/// What a cache reads Maxwell3D for. Memory (buffer cache, pipeline cache): B's replica. Record
/// (rasterizer, texture cache, query cache, StateTracker): C's replica with cut c, else B's.
enum class Role : u8 { Memory, Record };

struct Config {
    /// EDEN_NX2_BACK_STAGES: 0 off, 1 on, 2 verify (lockstep, depth 1, replicas compared),
    /// 3 pass-through (A forwards ring 1 ranges for BC to run with GpuSplit::Execute; no channel
    /// is staged and the proxy is not installed: BC runs the primaries as the unstaged back, K1)
    u8 mode{};
    bool cut_a{};        ///< EDEN_NX2_BACK_CUTS holds 'a' (default with mode != 0: "a" in K1)
    bool cut_c{};        ///< ... holds 'c' (K2)
    bool check{};        ///< EDEN_NX2_STAGE_CHECK=1: owner checks and hazard counters at full speed
    u32 verify_every{1}; ///< EDEN_NX2_BACK_STAGES_EVERY: =2 compares every n-th record
};

/// Read once, at the first call (GpuSplit::MaybeStart).
const Config& Get();

namespace Detail {
inline std::atomic<bool> g_active{};
inline constexpr u8 NoStage = 0xff;
/// The stage the thread runs (set by the stage threads; A in RunStagedBack).
inline thread_local u8 t_stage = NoStage;
/// A runs a join's work (JoinFromA): the stages below are parked at its point of the stream.
inline thread_local bool t_in_join{};

// T1's own, between A (gpu_split.cpp) and the stages (nx2_stages.cpp); not for other callers.
/// A, =3: B runs `words` words of ring 1 from `ring1_position` with GpuSplit::Execute (ExecA).
void PostExecA(u64 ring1_position, u64 words);
/// A, the end of its batch: a Mark (MarkCaughtUp) for `ring1_position`, then ring 2 published.
void PostBatchEnd(u64 ring1_position);
/// GpuSplit::CheckOwner while Active(): reports a thread other than RecordOwner()'s using the
/// scheduler (the front only while the record owner runs records; A outside a join counted as
/// StageUnproxied too).
void CheckRecordOwner(const char* where);
} // namespace Detail

/// Whether the stages run (Start to Stop), =3 included. Any thread. What depends on staged
/// channels tests them, never Active() alone (K1): Maxwell3D's hooks its stage_capture
/// (nx2_stage_api.h), the front's BindChannel tag ChannelStaged and Proxy() (nx2_stage_proxy.h).
inline bool Active() {
    return Detail::g_active.load(std::memory_order_relaxed);
}
inline bool OnStage(Stage stage) {
    return Detail::t_stage == static_cast<u8>(stage);
}
/// On A, inside JoinFromA (or a join A makes at ring level, T1).
inline bool InJoin() {
    return Detail::t_in_join;
}
/// What the proxy forwards from: A outside a join. From any other thread (B or C, the front
/// after a Drain, A inside a join) it calls the real rasterizer. Code choosing between a join, a
/// record and the real call tests Forwarding(), never OnStage(Stage::A) alone (K4).
inline bool Forwarding() {
    return OnStage(Stage::A) && !InJoin();
}

enum class Ring : u8 { ToB, ToC };

/// A Call record's function: gets the record's payload (after the thunk word) and its words.
using Thunk = void (*)(const u64* payload, u32 words);
/// A ring 1 Call entry's function (GpuSplit::CallThunk), of Forward records (S18: calling it as a
/// Thunk would be undefined).
using Ring1Thunk = void (*)(const u64* payload);

/// Producer (A for ToB, B for ToC): room for a Call record running `thunk` with `payload_words`
/// words; returns where the payload goes. From A, the deltas A's capture holds go in the same
/// record, before it (S5/1). From B (K2, K11; T9): the delta words of ring 2 records B applied to
/// its replica and has not passed on yet go in it the same way, in their order, so C's replica
/// follows the same stream; B passes a Target record on to ring 3 as it consumes it (the deltas
/// pending for the old target in it), and a record B consumes alone (CbData, a B event) leaves
/// its deltas for B's next ring 3 record. The consumer sees it from Commit on (published as
/// GpuSplit does).
/// Only while Active(). A inside a join writes no record (K4): Reserve, Post, Forward,
/// PostCbData, PostTarget and PostMark assert !InJoin(). 1 + `payload_words` is at most
/// MaxBodyWords (asserted; K9, nx2_stage_ring.h).
u64* Reserve(Ring ring, Thunk thunk, u32 payload_words);
void Commit(Ring ring);
/// Makes what was committed visible to the consumer now (the end of a batch, before a wait).
void Publish(Ring ring);

template <typename F>
void PostThunk(const u64* payload, [[maybe_unused]] u32 words) {
    (*reinterpret_cast<const F*>(payload))();
}

/// Runs `func` on the consumer after everything sent before it (a Call record).
template <typename F>
void Post(Ring ring, const F& func) {
    static_assert(std::is_trivially_copyable_v<F> && alignof(F) <= alignof(u64));
    constexpr u32 words = static_cast<u32>((sizeof(F) + sizeof(u64) - 1) / sizeof(u64));
    u64* const payload = Reserve(ring, &PostThunk<F>, words);
    std::memcpy(payload, &func, sizeof(F));
    Commit(ring);
}

/// A: a ring 1 Call entry tagged DeferTarget::Owner, copied as it is (thunk and payload, D2).
void Forward(Ring ring, Ring1Thunk thunk, const u64* payload, u32 payload_words);
/// A: constant buffer data ProcessCBMultiData sends while staged (T2), for the consumer's replica
/// of `primary` (StageCbWrite). First Capture().SelectTarget(primary): CallMultiMethod's cb data
/// passes no ProcessDirtyRegisters, so after a channel change no Note may have sent its Target
/// (K6). Joins the CbData record just before (S5/3) only when that is the last one written, not
/// published yet, ends at `address`, the sum fits one record (MaxBodyWords) and no delta is
/// pending (Capture().PendingWords() == 0: a register written between the two writes applies
/// before the second, K6); else a new record. A write longer than one record goes in several, in
/// order (K9). const_buffer.offset: A advances the primary's without noting it, StageCbWrite the
/// replica's (a noted offset would leave a delta pending after every write and nothing would
/// ever join, K6).
void PostCbData(Ring ring, Tegra::Engines::Maxwell3D& primary, GPUVAddr address, const u32* data,
                u32 count);
/// A: the primary Maxwell3D the deltas after this record are for (the consumer takes its own
/// replica, ReplicaOf). RegCapture::SelectTarget sends it when the engine A writes for changes
/// (Note, PostCbData); the deltas pending until then go in this record, for the previous target.
void PostTarget(Ring ring, Tegra::Engines::Maxwell3D* primary);
/// A ran ring 1 up to `ring1_position` (the end of each batch, every 4096 words). The last stage
/// (K1: B, K2: C) stores it to GpuSplit's consumed, then Nx2BackCpu::NoteBack and, caught up,
/// Nx2BusyClock::BackStored (D1): Drain then means every stage ran everything (E7). K2: C holds
/// it while its ParkB or boundary work is pending (StageTickWork's requests run after it in the
/// stream), until their op ran. So the queries and semaphore releases B sent C before a mark are
/// in C's query cache when the front's Drain ends, written or pending there: the puller's
/// PendingSemaphoreRelease after its Drain sees them (a DMA read waits for the releases before
/// it, PLAN §7/19). No stage holds a release of work before a mark outside the query cache. A
/// release behind a fence (IsAFence) B runs in a join instead (T7d: the fence manager takes the
/// buffer cache mutex, never in a record of C's): in the query cache's pending releases once B
/// passed it, before the mark, after what B sent C before it (the join's).
void PostMark(Ring ring, u64 ring1_position);

/// Why A joins (counted: Counter::JoinsAEngine + reason).
enum class JoinReason : u8 {
    Engine,    ///< Executable methods of an engine other than Maxwell3D (ring level, T1; S9/2)
    Channel,   ///< A channel bound first or changed (StageChannel, S3/S9)
    Value,     ///< A rasterizer member returning a value (MustFlushRegion, GetFlushArea, ...)
    Flush,     ///< FlushRegion / FlushAll / FlushAndInvalidateRegion from A (S7)
    Condition, ///< Conditional rendering (ProcessQueryCondition, its rasterizer call; S1)
    Upload,    ///< Block linear inline upload writing guest memory from A (T5)
    Tagged,    ///< A ring 1 Call tagged DeferTarget::Join (RunCommand, dma_pusher's thunk)
    Other,
    Count,
};

/// A non-owning reference to a callable, called before the function taking it returns.
class FunctionRef {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>)
    FunctionRef(F&& func) noexcept
        : object{const_cast<void*>(static_cast<const void*>(std::addressof(func)))},
          call{[](void* callable) { (*static_cast<std::remove_reference_t<F>*>(callable))(); }} {}

    void operator()() const {
        call(object);
    }

private:
    void* object;
    void (*call)(void*);
};

/// A: the stages below run everything sent before this point and park (B writes a Join to ring
/// 3 and waits for C's park with cut c); then `op` runs on A with every cache, the scheduler and
/// the replicas, A being the record owner (InJoin, RecordOwner, CheckOwner); then they go on.
/// There the replicas' registers equal the primaries' (the Join record takes the pending deltas);
/// their side state may lag, it goes only with draws (K2, D4): `op` must not depend on it. The
/// ack wait spins briefly, then sleeps (an atomic wait); with STAGE_CHECK one past 50 ms is
/// logged. =2 lockstep: for a Join record the producer waits for the park ack, never for the
/// record's end (the consumer ends it only once A releases; S8).
/// Nested and elsewhere (K4): from A inside a join (InJoin(), e.g. engine_upload's block linear
/// path in a ring level join of KeplerMemory), from any other thread and with the stages off or
/// at =3, `op` runs at once: no record, no wait, not counted. Inside a join A sends nothing (see
/// Reserve); RegCapture::Note only collects there, its words go with the first record after.
void JoinFromA(JoinReason reason, FunctionRef op);
// K2: joins and parks between B and C (K1 calls none: BC owns every cache). The rules (S13; K3
// as decided after R2, choice (a): the buffer runtime's join keeps B's buffer cache mutex):
// - A request is a flag outside the rings, not a record. The one asked answers it at a park
//   point: it acks, waits for the release and runs nothing meanwhile.
// - B's park points (for ParkB): each record boundary of ring 2 and its wait for ring 2, nothing
//   else. There B has no packet open, holds no cache lock and sent C its work up to that point:
//   it passes its pending deltas and publishes ring 3 before it acks. B's other waits on C (room
//   on ring 3, =2's end of a record, C's ack of a Join B passes on, JoinFromB, JoinFromBLocked)
//   are made inside a record of ring 2, maybe inside a packet or holding the buffer cache mutex:
//   B serves no ParkB there, and C, waiting for its own ParkB, goes on running ring 3 meanwhile,
//   so neither waits on the other (R2 F6).
// - C's park points (for JoinFromB, JoinFromBLocked): each record boundary of ring 3, its wait
//   for ring 3 and its wait in ParkB, only once it ran everything B sent before asking (its read
//   position at B's join target; R2 F1), never short of it.
// - The buffer cache mutex (recursive) is B's. B takes it for its own work and holds it across
//   JoinFromBLocked alone: never at a park point, when it calls JoinFromB, or while it writes or
//   waits on ring 3. C takes it only inside a ParkB `op` (B parked, holding none); a record C runs
//   never takes it, else a JoinFromBLocked waiting on C to reach its target would never end. A
//   join's `op` runs on the joiner and may take it there: A in JoinFromA (B and C parked, holding
//   none), B in JoinFromB, B again in JoinFromBLocked (the same thread; recursive). Threads that
//   are no stage (the guest CPU's, the fence thread) take it as before and wait for a
//   JoinFromBLocked to end; while they wait for it they hold no other cache lock (C may need
//   the texture or query cache's to reach the join's target; one std::scoped_lock of several
//   mutexes waits holding none of them, taking one inside another's scope does not).
// - Every other cache lock (texture, query, the scheduler's): no stage holds one at a park point,
//   when it asks (JoinFromB, JoinFromBLocked, ParkB) or while it writes or waits on ring 3. So an
//   `op` may take any lock: the parked stage holds none.
// - An `op` waits on no ring and no stage. The joiner writes no record inside its `op` (K4 for A;
//   B inside a JoinFromB / JoinFromBLocked `op` is the record owner, RecordOwner() == Stage::B:
//   it runs C's half of its work itself instead of sending it).
// - Events: the proxy sends each to B, whose rasterizer runs it or its B half there and sends C
//   the rest; B notes the engine work at its own point (NoteLightWork: B's light draws, S12)
//   also for an event it sends on to C.
// - EDEN_NX2_PRESENT_DIAG=1 or EDEN_NX2_MODE_DIAG=1: a "nx2 stages diag" line a second from B
//   while the stages run with cut c: joins from B (JoinFromB and JoinFromBLocked, as
//   Counter::JoinsB) a frame, B's wait for C to park, the time C stays parked, and C's ParkBs a
//   frame with C's wait for B. A frame is a present of the guest's.
// - STAGE_CHECK: CheckBufferLock where the mutex is taken off B; ReportStageHazard; a ParkB from
//   inside a record at =1; a JoinFromB / JoinFromBLocked still waiting after 50 ms, reported
//   while it waits (then every second): a slow join while C goes on from record to record, a K3
//   deadlock suspect with B's stack once C stayed on one record from a report to the next.

/// K2, B inside a record of ring 2, outside any packet and holding no cache lock (a draw's
/// joined setup, RESETFIX's FlushRegion, a WaitForIdle with a query sync pending, an event whose
/// halves are not split yet): B passes C the deltas it has not passed yet (a record of their own;
/// its wait there serves no ParkB), publishes ring 3 and asks; C runs everything B sent before and
/// parks; `op` runs on B, the record owner (RecordOwner, CheckOwner), with every cache and the
/// scheduler, C's replica's registers equal to B's; then C goes on. Nested (B inside a join's
/// `op`), without cut c or from another thread, `op` runs at once.
void JoinFromB(FunctionRef op);
/// K2, B inside a packet holding the buffer cache mutex: the buffer runtime's record owner hook
/// (BufferCacheRuntime::JoinRecordOwner: Wait, Finish, a multi-range binding; R2 F6). B keeps the
/// mutex (K3 (a): the CPU threads' OnCPUWrite / GetFlushArea wait meanwhile, so a download's "copy,
/// wait, write back" keeps its order) and writes nothing to ring 3: the deltas it has not passed
/// stay pending and go with its next ring 3 record. It publishes ring 3, asks and waits for C's
/// ack (serving no ParkB); C runs everything B published before and parks; `op` runs on B under
/// its lock (locking the mutex again is the same thread's), B the record owner; then C goes on.
/// C's replica lags B's by those deltas: `op` uses B's state alone (B's list so far replayed, the
/// scheduler's waits), never C's replica nor C's caches. Nested, without cut c or from another
/// thread, `op` runs at once.
void JoinFromBLocked(FunctionRef op);
/// K2, C at a record boundary of ring 3 (the stages' own work there: StageTickWork): B parks at
/// its next park point (a record boundary of ring 2, its wait for ring 2); C runs ring 3 on up to
/// where B parked; then `op` runs on C with every cache, the buffer cache mutex included: B's
/// caches and C's recorded commands stand at the same point of the stream, the replicas' registers
/// equal (R2 F2). Then B goes on. Until B parked C goes on running ring 3 and answers B's joins
/// once reached (F1): a B blocked on C (room, a join) never waits on this request. =2 alone, also
/// from inside a record (CheckSplitSetup): B waits for that record's end there (the lockstep, no
/// packet open, no lock), so `op` runs at once. Inside a record at =1 it is a STAGE_CHECK
/// violation: C cannot run on to where B parks. Without cut c, once B stopped or from another
/// thread, `op` runs at once.
void ParkB(FunctionRef op);
/// K2 (cut c), GPU::TickWork (gpu.cpp): the guest CPU's sync requests (a reactive FlushRegion, a
/// preserving InvalidateRegion, RequestFlush, Composite with its TickFrame, captures) touch B's
/// buffer cache and C's caches and scheduler alike. They run on C at a record boundary of ring 3
/// inside a ParkB (`run` its `op`; R2 F4), never inside a record (PrepareDrawC's TickWork) nor on
/// B. True when the stages took them: `run` ran so, or waits for C's next record boundary, which
/// calls GPU::TickWork again (from C inside a record, B, A outside a join), or `idle` (no request
/// queued: nothing to run, no boundary work and no ParkB, whatever EDEN_NX2_TICK_SKIP says; one
/// queued meanwhile runs at the next tick, as with the skip). False when the caller runs `run`
/// now: cut c off (K1, =0, =3), C inside a ParkB op or A inside its join (the others parked
/// already), a thread that is no stage.
bool StageTickWork(bool idle, FunctionRef run);

/// EDEN_NX2_STAGE_CHECK: reports (the first ones, with the stack, as GpuSplit::CheckOwner) and
/// counts (Counter::StageViolations) a call of `where` from a thread other than `stage`'s; A
/// inside a join may do anything.
void CheckStage(Stage stage, const char* where);
/// EDEN_NX2_STAGE_CHECK, K3: call where the buffer cache mutex is taken off B (C's side of the
/// rasterizer, the query cache's SyncValues, ...): reports and counts (StageViolations) taking
/// it on C outside a ParkB `op` and on A outside a join.
void CheckBufferLock(const char* where);
/// K3: whether this thread may take the buffer cache mutex now (BufferLockAllowed,
/// nx2_stage_split.h): always without cut c; with it C only inside a ParkB `op`, A only inside its
/// join. Where it may not, a caller that needs the buffer cache does without it (the Vulkan
/// rasterizer's FlushRegion on C: T7d review).
bool MayTakeBufferLock();
/// EDEN_NX2_STAGE_CHECK: reports (with the stack, as CheckStage) and counts (StageViolations) a
/// hazard its caller found: a download inside B's packet (a Finish through JoinFromBLocked:
/// right, but the CPU threads wait on the mutex for it), a record list released out of order...
void ReportStageHazard(const char* what);
/// The stage recording Vulkan commands, whose use of the scheduler GpuSplit::CheckOwner checks:
/// K1 B, K2 C; A inside its join; B inside a JoinFromB / JoinFromBLocked `op`.
Stage RecordOwner();

/// Whether this thread may record a buffer upload. The record owner (and A inside a join) while
/// the stages run; the back thread while the split runs without them; the GPU thread otherwise.
/// A CPU thread writing guest memory is not one of these.
bool OwnsBufferRecords();
/// 1: the GPU thread. 2: the unstaged back thread. The stages do not consult this.
void SetBufferRecordRole(u8 role);

/// The record side's begin and end of the stages (the Vulkan rasterizer: its buffer runtime's
/// BeginStages and record owner hook, EndStages; T7): with cut c, Start calls `begin` before B
/// and C start and Stop calls `end` after they stopped; nothing runs behind either. One pair, set
/// by its owner before the stages start; it outlives them. Null: none.
void SetStageHooks(void (*begin)(void* context), void (*end)(void* context), void* context);

/// K2 (cut c), B: whether constant buffer data B writes to guest memory at [address, address +
/// size) for `replica`'s channel (a CbData record: StageCbWrite) hits what C may still read
/// there for work B sent before, the texture header and sampler pools (T9c, omur-1; the
/// rasterizer's check for its inline writes, RasterizerVulkan::WritesPoolsB). Then the write runs
/// in a JoinFromB: C parked, having run that work.
using CbWriteCheck = bool (*)(void* context, Tegra::Engines::Maxwell3D& replica, GPUVAddr address,
                              u64 size);
/// Sets the check (the Vulkan rasterizer: T7d), as SetStageHooks: by its owner before the stages
/// start, outliving them. Null: no CbData write joins.
void SetCbWriteCheck(CbWriteCheck check, void* context);

/// GpuSplit::MaybeStart, before the back thread starts (nothing runs behind the front): when
/// Get().mode != 0, makes the rings and starts B (and C); at =1 and =2 it also installs the proxy
/// and stages every channel (InstallProxy, S3), at =3 neither (K1: BC runs the primaries with
/// GpuSplit::Execute, a staged channel's hooks would send records from BC to its own ring and its
/// caches would read replicas no delta reaches). Active() is true from here on and the back
/// thread runs A (RunStagedBack). The real rasterizer's NoteEngineWork: no light draw follows one
/// from before (S18).
void Start(Core::System& system);
/// GpuSplit::Stop, after the back thread (A) stopped: B and C run what they hold and stop (at
/// shutdown, with the stop request alone, they stop without it: NotifyShutdown, gpu_thread.cpp),
/// the channels are unstaged (UninstallProxy, =1 and =2), the rings freed, NoteEngineWork as at
/// Start; Active() false.
void Stop();

} // namespace VideoCommon::Nx2Stages
