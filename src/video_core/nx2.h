// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// EDEN_NX2_MEMBAR=1 (off by default, any vendor): the Ampere puller's MEM_OP_D MEMBAR (operation 5)
// records a real memory barrier (RasterizerInterface::Nx2Membar) and WaitForIdle waits with every
// stage it waited for as the destination, plus a memory barrier, instead of TOP_OF_PIPE without
// one (57ffa55a8 does both on AMD's Windows driver only). An experiment against the black core and
// white halo (H1): draws after a barrier must see GPU-written vertex, index and indirect data.
inline bool Nx2MembarEnabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("EDEN_NX2_MEMBAR");
        return text != nullptr && text[0] != '0';
    }();
    return enabled;
}

// The running title, for the few places that must behave differently per game.
inline std::atomic<u64> g_nx2_program_id{0};

// Donkey Kong Bananza (the id Loader::Nx2DkDynamicFps patches), whose frame pacing
// EDEN_NX2_MODE_PACE and EDEN_NX2_PRESENT_INTERVAL_WORD are for.
inline bool Nx2IsBananza() {
    return g_nx2_program_id.load(std::memory_order_relaxed) == 0x040074A01BF12000ULL;
}

// Titles whose guest side reads GPU-written buffer data through a path the buffer cache does not
// see, so their buffers must be copied back to guest memory at every fence
// (BufferCache::AsyncBufferDownloads, EDEN_NX2_BUFFER_DOWNLOADS). 0400A3A01EE4C000 is Pokemon
// Pokopia: with the copy-back off its sky is black or flickers. Open work: find what reads it,
// then the list goes.
inline bool Nx2NeedsBufferDownloads() {
    switch (g_nx2_program_id.load(std::memory_order_relaxed)) {
    case 0x0400A3A01EE4C000ULL:
        return true;
    default:
        return false;
    }
}

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};

/// EDEN_NX2_MARK_WORDS=2 (Nx2MarkGpuWritten): `mask` is the bits word `index` gets 64 pages at a
/// time, `pages` the bits worked out page by page. Logs the first 16 words where they differ and,
/// at each power of two from 1024 on, how many words were checked and how many differed
/// (dma_pusher.cpp).
void Nx2CheckMarkWord(u64 index, u64 mask, u64 pages);

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    // EDEN_NX2_MARK_WORDS=0: the bits are set a page at a time. A written storage buffer is marked
    // whole on every draw that binds it (BufferCache::MarkWrittenBuffer; a native binding is a
    // window of up to 8 MiB, 2,048 pages): a word of 64 pages at a time sets the same bits, each
    // word written only if one of them is still clear, as each page was. =2: each word's bits are
    // also worked out page by page as =0 sets them, those are set, and the two are compared
    // (Nx2CheckMarkWord: the log names the first words that differ; none should). The switch is
    // read at the first call, without a guarded static's check on every call.
    static std::atomic<s8> mark_mode{-1};
    s8 mode = mark_mode.load(std::memory_order_relaxed);
    if (mode < 0) [[unlikely]] {
        const char* value = std::getenv("EDEN_NX2_MARK_WORDS");
        mode = static_cast<s8>(value == nullptr  ? 1
                               : value[0] == '0' ? 0
                               : value[0] == '2' ? 2
                                                 : 1);
        mark_mode.store(mode, std::memory_order_relaxed);
    }
    const u64 first = device_address >> Nx2WrittenPageBits;
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    if (mode != 0) {
        for (u64 index = first / 64; first <= last && index <= last / 64; ++index) {
            const u64 low = index == first / 64 ? first % 64 : 0;
            const u64 high = index == last / 64 ? last % 64 : 63;
            u64 mask = (~u64{0} >> (63 - high)) & (~u64{0} << low);
            if (mode == 2) [[unlikely]] {
                u64 pages = 0;
                for (u64 page = (std::max)(first, index * 64);
                     page <= (std::min)(last, index * 64 + 63); ++page) {
                    pages |= u64{1} << (page % 64);
                }
                Nx2CheckMarkWord(index, mask, pages);
                mask = pages;
            }
            auto& word = g_nx2_gpu_written_pages[index];
            if ((word.load(std::memory_order_relaxed) & mask) != mask) {
                word.fetch_or(mask, std::memory_order_relaxed);
            }
        }
        return;
    }
    for (u64 page = first; page <= last; ++page) {
        auto& word = g_nx2_gpu_written_pages[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word.load(std::memory_order_relaxed) & bit) == 0) {
            word.fetch_or(bit, std::memory_order_relaxed);
        }
    }
}

// Device pages written around the caches: semaphore and query results, unsafe block writes
// (cache downloads, DMA fills), DMA copies through the buffer cache, in-place texture swizzles. A
// cache may hold older bytes there, which an invalidation of the page brings up to date. The
// light path counts no constant buffer write there as a same write (Maxwell3D::IsLightSameWrite),
// and EDEN_NX2_SAME_SKIP invalidates a skipped one if its page gets written so before the next
// FlushCaching (MemoryManager::ReviewSkippedWrites). Same layout as the GPU-written bits; the
// bits never clear within a session.
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64>
    g_nx2_untracked_pages{};

inline void Nx2MarkUntrackedWrite(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = (std::min)((device_address + size - 1) >> Nx2WrittenPageBits,
                                u64{g_nx2_untracked_pages.size() * 64 - 1});
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        auto& word = g_nx2_untracked_pages[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word.load(std::memory_order_relaxed) & bit) == 0) {
            word.fetch_or(bit, std::memory_order_relaxed);
        }
    }
}

[[nodiscard]] inline bool Nx2UntrackedWritten(u64 device_address, u64 size) {
    const u64 first = device_address >> Nx2WrittenPageBits;
    const u64 last = (device_address + (std::max)(size, u64{1}) - 1) >> Nx2WrittenPageBits;
    for (u64 page = first; page <= last; ++page) {
        if (page >= g_nx2_untracked_pages.size() * 64) {
            return true;
        }
        if ((g_nx2_untracked_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
            return true;
        }
    }
    return false;
}

// FlushCaching calls so far, of every GPU address space (EDEN_NX2_SAME_SKIP).
inline std::atomic<u64> g_nx2_flushes{};

/// A new emulation session starts: no page has been written by its GPU yet.
inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_untracked_pages) {
        word.store(0, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;
inline std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions> g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        if (region.load() == start) {
            return;
        }
    }
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = 0;
        if (region.compare_exchange_strong(expected, start)) {
            return;
        }
    }
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = start;
        region.compare_exchange_strong(expected, 0);
    }
}

/// A new emulation session starts: no queue has a region yet.
inline void Nx2ClearSemaphoreRegions() {
    for (auto& region : g_nx2_semaphore_regions) {
        region.store(0);
    }
}

// Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and
// layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the
// GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover;
// the GPU memory manager calls Nx2BackSparseWrite before each write.
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size)>;
inline std::recursive_mutex g_nx2_sparse_backing_mutex;
inline Nx2SparseBackingHandler g_nx2_sparse_backing;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    g_nx2_sparse_backing = std::move(handler);
    g_nx2_sparse_low.store(~u64{0});
    g_nx2_sparse_high.store(0);
}

/// Widens the span that writes are checked against (a new lazily backed region).
inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    u64 low = g_nx2_sparse_low.load();
    while (gpu_address < low && !g_nx2_sparse_low.compare_exchange_weak(low, gpu_address)) {
    }
    u64 high = g_nx2_sparse_high.load();
    while (gpu_address + size > high &&
           !g_nx2_sparse_high.compare_exchange_weak(high, gpu_address + size)) {
    }
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size);
    }
}

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
};
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {size, block_size,
                                       std::vector<bool>((size + block_size - 1) / block_size)};
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

/// A new emulation session: the previous game's regions are gone (nvdrv's Module clears them).
inline void Nx2ClearSparseRegions() {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.clear();
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address); it != g_nx2_sparse_state.end()) {
        it->second.written[block] = true;
    }
}

/// True when [gpu_address, +size) lies in one sparse region.
inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second.size;
}

/// The GPU renders into [gpu_address, +size): count its blocks as written, so images made there
/// later get memory (their contents live in the host image, not in guest memory).
inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    for (auto& [start, region] : g_nx2_sparse_state) {
        const u64 end = start + region.size;
        if (gpu_address >= end || gpu_address + size <= start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = ((std::min)(gpu_address + size, end) - 1 - start) / region.block_size;
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

/// True when [gpu_address, +size) lies in one sparse region and none of it has been written.
inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    if (gpu_address + size > start + region.size) {
        return false;
    }
    const u64 last = (gpu_address + size - 1 - start) / region.block_size;
    for (u64 block = (gpu_address - start) / region.block_size; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

/// Channels suspended on a semaphore acquire. A console channel polls and sees every release;
/// Eden writes releases in bursts when fences signal and resumes waiters only between command
/// lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.
class Nx2SemaphoreWaiters {
public:
    // Puller GpuSemaphoreOperation values.
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    /// A GPU write of `value` to the u32 at `device_address`.
    void OnWrite(u64 device_address, u32 value) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        for (auto& w : waiters) {
            if (w.address == device_address && !w.latched && Matches(w, value)) {
                w.latched = true;
            }
        }
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
};

} // namespace Tegra
