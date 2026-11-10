// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "memory_manager.h"
#include "video_core/guest_memory.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stages.h"
#include "video_core/host1x/host1x.h"
#include "video_core/invalidation_accumulator.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/texture_cache/util.h"

namespace Tegra {
namespace {
// EDEN_NX2_STAGE_CHECK: the paths below reach the rasterizer's caches (through the proxy from A,
// nx2_stage_proxy.h), which belong to the stages below A. From A they must run inside a join
// (JoinFromA); outside one (Forwarding) they are reported as a call off B's thread and counted
// (Counter::StageViolations). CPU threads, the front and A inside a join are not reported. Off
// the stages: one thread-local byte compared (Forwarding is false off A).
void CheckNotForwarding(const char* where) {
    if (VideoCommon::Nx2Stages::Forwarding() && VideoCommon::Nx2Stages::Get().check) [[unlikely]] {
        VideoCommon::Nx2Stages::CheckStage(VideoCommon::Nx2Stages::Stage::B, where);
    }
}
} // Anonymous namespace

bool MemoryManager::IsShuttingDown() const {
    return system.IsShuttingDown();
}

using Tegra::Memory::GuestMemoryFlags;

namespace {
// Small reads (texture and sampler descriptors, bindless texture handles: several per draw, ~2.4%
// of the split back thread in title 1 spent translating their pages) remember the host pointer of
// their 4 KiB page, per thread. The entries are dropped when the GPU page table (MappingEpoch) or
// the device mapping (DeviceMemoryManager::TranslationGeneration) changed: until then
// GpuToCpuAddress and the device's GetPointer answer the same for that page, and every GPU page
// (4 KiB or larger) maps a 4 KiB-aligned block to one host page, so the pointer is the same.
// EDEN_NX2_DESC_XLAT=0 translates every read. =2 remembers them as by default and checks each
// small ReadBlockUnsafe read against the page walk (ReadBlockImpl, what every read took before;
// logs "EDEN_NX2_DESC_XLAT: small read ... differs from the page walk", first 16). Read at the
// first use, as BufferCache's ReadNx2Mode: after main loads nx2_switches.txt, and without a
// function-local static's initialization check on every read.
std::atomic<s8> g_nx2_desc_xlat_mode{-1};

int DescXlatMode() {
    s8 value = g_nx2_desc_xlat_mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_DESC_XLAT");
        value = static_cast<s8>(env == nullptr ? 1 : env[0] == '0' ? 0 : env[0] == '2' ? 2 : 1);
        g_nx2_desc_xlat_mode.store(value, std::memory_order_relaxed);
    }
    return value;
}

// Small writes within one 4 KiB page (macros' constant buffer words: four per draw in title 1,
// ~2% of the split back thread walking the page tables in WriteBlockImpl for each; inline records)
// go through the same remembered page pointers. EDEN_NX2_SMALL_WRITE=0 walks for every write.
const bool g_nx2_small_write = [] {
    const char* value = std::getenv("EDEN_NX2_SMALL_WRITE");
    return value == nullptr || value[0] != '0';
}();

struct SmallReadCache {
    static constexpr std::size_t NumSlots = 64;
    std::size_t owner = ~std::size_t{0}; ///< MemoryManager::GetID of the entries
    u64 mapping_epoch = 0;
    u64 device_generation = 0;
    std::array<u64, NumSlots> pages{};          ///< 4 KiB page number + 1; 0 is an empty slot
    std::array<const u8*, NumSlots> pointers{}; ///< Host pointer of the page's first byte
};
thread_local SmallReadCache t_small_reads{};
} // Anonymous namespace

std::atomic<size_t> MemoryManager::unique_identifier_generator{};

MemoryManager::MemoryManager(Core::System& system_, MaxwellDeviceMemoryManager& memory_, u64 address_space_bits_, GPUVAddr split_address_, u64 big_page_bits_, u64 page_bits_)
    : system{system_}, memory{memory_}, address_space_bits{address_space_bits_}
    , split_address{split_address_}, page_bits{page_bits_}, big_page_bits{big_page_bits_}
    , entries{}, big_entries{}
    , page_table{address_space_bits, address_space_bits + page_bits - 38, page_bits != big_page_bits ? page_bits : 0}
    , kind_map{PTEKind::INVALID}, unique_identifier{unique_identifier_generator.fetch_add(1, std::memory_order_acq_rel)}
    , accumulator{}
{
    address_space_size = 1ULL << address_space_bits;
    page_size = 1ULL << page_bits;
    page_mask = page_size - 1ULL;
    big_page_size = 1ULL << big_page_bits;
    big_page_mask = big_page_size - 1ULL;
    const u64 page_table_bits = address_space_bits - page_bits;
    const u64 big_page_table_bits = address_space_bits - big_page_bits;
    const u64 page_table_size = 1ULL << page_table_bits;
    const u64 big_page_table_size = 1ULL << big_page_table_bits;
    page_table_mask = page_table_size - 1;
    big_page_table_mask = big_page_table_size - 1;

    big_page_table_dev.ResizeAndClear(big_page_table_size);
    big_entries.resize(big_page_table_size / 32, 0);
    big_page_continuous.resize(big_page_table_size / continuous_bits, 0);
    entries.resize(page_table_size / 32, 0);
}

MemoryManager::MemoryManager(Core::System& system_, u64 address_space_bits_, GPUVAddr split_address_, u64 big_page_bits_, u64 page_bits_)
    : MemoryManager(system_, system_.Host1x().MemoryManager(), address_space_bits_, split_address_, big_page_bits_, page_bits_)
{}

void MemoryManager::SetNativeSyncpointShim(GPUVAddr address) {
    native_syncpoint_shim.store(address, std::memory_order_release);
}

MemoryManager::~MemoryManager() = default;

MemoryManager::EntryType MemoryManager::GetEntry(size_t position, bool is_big_page) const {
    if (is_big_page) {
        position = position >> big_page_bits;
        const u64 entry_mask = big_entries[position / 32];
        const size_t sub_index = position % 32;
        return static_cast<EntryType>((entry_mask >> (2 * sub_index)) & 0x03ULL);
    } else {
        position = position >> page_bits;
        const u64 entry_mask = entries[position / 32];
        const size_t sub_index = position % 32;
        return static_cast<EntryType>((entry_mask >> (2 * sub_index)) & 0x03ULL);
    }
}

void MemoryManager::SetEntry(size_t position, MemoryManager::EntryType entry, bool is_big_page) {
    if (is_big_page) {
        position = position >> big_page_bits;
        const u64 entry_mask = big_entries[position / 32];
        const size_t sub_index = position % 32;
        big_entries[position / 32] =
            (~(3ULL << sub_index * 2) & entry_mask) | (static_cast<u64>(entry) << sub_index * 2);
    } else {
        position = position >> page_bits;
        const u64 entry_mask = entries[position / 32];
        const size_t sub_index = position % 32;
        entries[position / 32] =
            (~(3ULL << sub_index * 2) & entry_mask) | (static_cast<u64>(entry) << sub_index * 2);
    }
}

PTEKind MemoryManager::GetPageKind(GPUVAddr gpu_addr) const {
    std::unique_lock<std::mutex> lock(guard);
    return kind_map.GetValueAt(gpu_addr);
}

inline bool MemoryManager::IsBigPageContinuous(size_t big_page_index) const {
    const u64 entry_mask = big_page_continuous[big_page_index / continuous_bits];
    const size_t sub_index = big_page_index % continuous_bits;
    return ((entry_mask >> sub_index) & 0x1ULL) != 0;
}

inline void MemoryManager::SetBigPageContinuous(size_t big_page_index, bool value) {
    const u64 continuous_mask = big_page_continuous[big_page_index / continuous_bits];
    const size_t sub_index = big_page_index % continuous_bits;
    big_page_continuous[big_page_index / continuous_bits] =
        (~(1ULL << sub_index) & continuous_mask) | (value ? 1ULL << sub_index : 0);
}

GPUVAddr MemoryManager::PageTableOp(GPUVAddr gpu_addr, [[maybe_unused]] DAddr dev_addr, size_t size, PTEKind kind, MemoryManager::EntryType entry_type) {
    [[maybe_unused]] u64 remaining_size{size};
    if (entry_type == EntryType::Mapped) {
        page_table.ReserveRange(gpu_addr, size);
    }
    // The caches hear about each run of pages whose entry changed in one call, not page by page
    // (a 1.45 GB Switch 2 region made ~370,000 texture cache scans).
    GPUVAddr modified_start{};
    u64 modified_size{};
    const auto flush_modified = [&] {
        if (modified_size != 0) {
            rasterizer->ModifyGPUMemory(unique_identifier, modified_start, modified_size);
            modified_size = 0;
        }
    };
    for (u64 offset{}; offset < size; offset += page_size) {
        const GPUVAddr current_gpu_addr = gpu_addr + offset;
        [[maybe_unused]] const auto current_entry_type = GetEntry(current_gpu_addr, false);
        SetEntry(current_gpu_addr, entry_type, false);
        if (current_entry_type != entry_type) {
            if (modified_size == 0) {
                modified_start = current_gpu_addr;
            }
            modified_size += page_size;
        } else {
            flush_modified();
        }
        if (entry_type == EntryType::Mapped) {
            const DAddr current_dev_addr = dev_addr + offset;
            const auto index = PageEntryIndex(current_gpu_addr, false);
            const u32 sub_value = static_cast<u32>(current_dev_addr >> cpu_page_bits);
            page_table[index] = sub_value;
        }
        remaining_size -= page_size;
    }
    flush_modified();
    {
        // As BigPageTableOp: GetPageKind and GetMemoryLayoutSize read the map from other threads.
        std::unique_lock<std::mutex> lock(guard);
        kind_map.Map(gpu_addr, gpu_addr + size, kind);
    }
    mapping_epoch.fetch_add(1, std::memory_order_release);
    return gpu_addr;
}

GPUVAddr MemoryManager::BigPageTableOp(GPUVAddr gpu_addr, [[maybe_unused]] DAddr dev_addr, size_t size, PTEKind kind, MemoryManager::EntryType entry_type) {
    [[maybe_unused]] u64 remaining_size{size};
    GPUVAddr modified_start{};
    u64 modified_size{};
    const auto flush_modified = [&] {
        if (modified_size != 0) {
            rasterizer->ModifyGPUMemory(unique_identifier, modified_start, modified_size);
            modified_size = 0;
        }
    };
    for (u64 offset{}; offset < size; offset += big_page_size) {
        const GPUVAddr current_gpu_addr = gpu_addr + offset;
        [[maybe_unused]] const auto current_entry_type = GetEntry(current_gpu_addr, true);
        SetEntry(current_gpu_addr, entry_type, true);
        if (current_entry_type != entry_type) {
            if (modified_size == 0) {
                modified_start = current_gpu_addr;
            }
            modified_size += big_page_size;
        } else {
            flush_modified();
        }
        if (entry_type == EntryType::Mapped) {
            const DAddr current_dev_addr = dev_addr + offset;
            const auto index = PageEntryIndex(current_gpu_addr, true);
            const u32 sub_value = static_cast<u32>(current_dev_addr >> cpu_page_bits);
            big_page_table_dev.Set(index, sub_value);
            const bool is_continuous = ([&] {
                uintptr_t base_ptr{
                    reinterpret_cast<uintptr_t>(memory.GetPointer<u8>(current_dev_addr))};
                if (base_ptr == 0) {
                    return false;
                }
                for (DAddr start_cpu = current_dev_addr + page_size;
                     start_cpu < current_dev_addr + big_page_size; start_cpu += page_size) {
                    base_ptr += page_size;
                    auto next_ptr = reinterpret_cast<uintptr_t>(memory.GetPointer<u8>(start_cpu));
                    if (next_ptr == 0 || base_ptr != next_ptr) {
                        return false;
                    }
                }
                return true;
            })();
            SetBigPageContinuous(index, is_continuous);
        }
        remaining_size -= big_page_size;
    }
    flush_modified();
    {
        std::unique_lock<std::mutex> lock(guard);
        kind_map.Map(gpu_addr, gpu_addr + size, kind);
    }
    mapping_epoch.fetch_add(1, std::memory_order_release);
    return gpu_addr;
}

void MemoryManager::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

GPUVAddr MemoryManager::Map(GPUVAddr gpu_addr, DAddr dev_addr, std::size_t size, PTEKind kind, bool is_big_pages) {
    if (is_big_pages)
        return BigPageTableOp(gpu_addr, dev_addr, size, kind, EntryType::Mapped);
    return PageTableOp(gpu_addr, dev_addr, size, kind, EntryType::Mapped);
}

GPUVAddr MemoryManager::MapSparse(GPUVAddr gpu_addr, std::size_t size, bool is_big_pages) {
    if (is_big_pages)
        return BigPageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Reserved);
    return PageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Reserved);
}

void MemoryManager::Unmap(GPUVAddr gpu_addr, std::size_t size) {
    if (size == 0) {
        return;
    }
    GetSubmappedRangeImpl<false>(gpu_addr, size, page_stash);

    for (const auto& [map_addr, map_size] : page_stash) {
        rasterizer->UnmapMemory(map_addr, map_size);
    }
    page_stash.clear();

    BigPageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Free);
    PageTableOp(gpu_addr, 0, size, PTEKind::INVALID, EntryType::Free);
}

std::optional<DAddr> MemoryManager::GpuToCpuAddress(GPUVAddr gpu_addr) const {
    if (!IsWithinGPUAddressRange(gpu_addr)) [[unlikely]] {
        return std::nullopt;
    }
    if (GetEntry(gpu_addr, true) != EntryType::Mapped) [[unlikely]] {
        if (GetEntry(gpu_addr, false) != EntryType::Mapped)
            return std::nullopt;
        const DAddr dev_addr_base = DAddr(page_table[PageEntryIndex(gpu_addr, false)]) << cpu_page_bits;
        return dev_addr_base + (gpu_addr & page_mask);
    }
    const DAddr dev_addr_base = DAddr(big_page_table_dev[PageEntryIndex(gpu_addr, true)]) << cpu_page_bits;
    return dev_addr_base + (gpu_addr & big_page_mask);
}

std::optional<DAddr> MemoryManager::GpuToCpuAddress(GPUVAddr addr, std::size_t size) const {
    size_t page_index{addr >> page_bits};
    const size_t page_last{(addr + size + page_size - 1) >> page_bits};
    while (page_index < page_last) {
        const auto page_addr{GpuToCpuAddress(page_index << page_bits)};
        if (page_addr) {
            return page_addr;
        }
        ++page_index;
    }
    return std::nullopt;
}

const u8* MemoryManager::SmallReadPointer(GPUVAddr gpu_addr) const {
    if (DescXlatMode() == 0 || page_bits < cpu_page_bits) {
        return GetPointer(gpu_addr);
    }
    SmallReadCache& cache = t_small_reads;
    // Read before translating: a change made meanwhile leaves entries that are dropped next time.
    const u64 epoch = mapping_epoch.load(std::memory_order_acquire);
    const u64 generation = memory.TranslationGeneration();
    if (cache.owner != unique_identifier || cache.mapping_epoch != epoch ||
        cache.device_generation != generation) {
        cache.owner = unique_identifier;
        cache.mapping_epoch = epoch;
        cache.device_generation = generation;
        cache.pages.fill(0);
    }
    const u64 page = (gpu_addr >> cpu_page_bits) + 1;
    const std::size_t slot = static_cast<std::size_t>(page) & (SmallReadCache::NumSlots - 1);
    const u64 offset = gpu_addr & ((u64{1} << cpu_page_bits) - 1);
    if (cache.pages[slot] == page) {
        return cache.pointers[slot] + offset;
    }
    const u8* const pointer = GetPointer(gpu_addr);
    if (pointer != nullptr) {
        // Unmapped pages are not remembered: their reads keep taking the full path.
        cache.pages[slot] = page;
        cache.pointers[slot] = pointer - offset;
    }
    return pointer;
}

template <typename T>
T MemoryManager::Read(GPUVAddr addr) const {
    if constexpr (sizeof(T) == sizeof(u32)) {
        const auto shim = native_syncpoint_shim.load(std::memory_order_acquire);
        if (shim && addr >= shim && addr - shim < NumShimSyncpoints * sizeof(u32) &&
            Common::IsAligned(addr - shim, sizeof(u32))) {
            return static_cast<T>(system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(
                static_cast<u32>((addr - shim) / sizeof(u32))));
        }
    }

    if (auto page_pointer{SmallReadPointer(addr)}; page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        T value;
        std::memcpy(&value, page_pointer, sizeof(T));
        return value;
    }

    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 16) {
        LOG_ERROR(HW_GPU, "Read{} of unmapped GPU address {:#x}", sizeof(T) * 8, addr);
    }

    return {};
}

template <typename T>
void MemoryManager::Write(GPUVAddr addr, T data) {
    // Semaphore and report words: no cache is told.
    MarkUntracked(addr, sizeof(T));
    if (auto page_pointer{GetPointer(addr)}; page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        std::memcpy(page_pointer, &data, sizeof(T));
        return;
    }
    // A Switch 2 sparse block nobody wrote yet gets its memory now.
    Tegra::Nx2BackSparseWrite(addr, sizeof(T));
    if (auto page_pointer{GetPointer(addr)}; page_pointer) {
        std::memcpy(page_pointer, &data, sizeof(T));
        return;
    }

    ASSERT(false);
}

template u8 MemoryManager::Read<u8>(GPUVAddr addr) const;
template u16 MemoryManager::Read<u16>(GPUVAddr addr) const;
template u32 MemoryManager::Read<u32>(GPUVAddr addr) const;
template u64 MemoryManager::Read<u64>(GPUVAddr addr) const;
template void MemoryManager::Write<u8>(GPUVAddr addr, u8 data);
template void MemoryManager::Write<u16>(GPUVAddr addr, u16 data);
template void MemoryManager::Write<u32>(GPUVAddr addr, u32 data);
template void MemoryManager::Write<u64>(GPUVAddr addr, u64 data);

u8* MemoryManager::GetPointer(GPUVAddr gpu_addr) {
    const auto address{GpuToCpuAddress(gpu_addr)};
    if (!address) {
        return {};
    }

    return memory.GetPointer<u8>(*address);
}

const u8* MemoryManager::GetPointer(GPUVAddr gpu_addr) const {
    const auto address{GpuToCpuAddress(gpu_addr)};
    if (!address) {
        return {};
    }

    return memory.GetPointer<u8>(*address);
}

#if defined(_MSC_VER) && !defined(__clang__) // no need for gcc / clang but msvc's compiler is more conservative with inlining.
#pragma inline_recursion(on)
#endif

template <typename FuncMapped, typename FuncReserved, typename FuncUnmapped>
inline void MemoryManager::MemoryOperation(GPUVAddr gpu_src_addr, std::size_t size, bool is_big_page, FuncMapped&& func_mapped, FuncReserved&& func_reserved, FuncUnmapped&& func_unmapped) const {
    using FuncMappedReturn =
        typename std::invoke_result<FuncMapped, std::size_t, std::size_t, std::size_t>::type;
    using FuncReservedReturn =
        typename std::invoke_result<FuncReserved, std::size_t, std::size_t, std::size_t>::type;
    using FuncUnmappedReturn =
        typename std::invoke_result<FuncUnmapped, std::size_t, std::size_t, std::size_t>::type;
    static constexpr bool BOOL_BREAK_MAPPED = std::is_same_v<FuncMappedReturn, bool>;
    static constexpr bool BOOL_BREAK_RESERVED = std::is_same_v<FuncReservedReturn, bool>;
    static constexpr bool BOOL_BREAK_UNMAPPED = std::is_same_v<FuncUnmappedReturn, bool>;
    u64 used_page_size;
    u64 used_page_mask;
    u64 used_page_bits;
    if (is_big_page) {
        used_page_size = big_page_size;
        used_page_mask = big_page_mask;
        used_page_bits = big_page_bits;
    } else {
        used_page_size = page_size;
        used_page_mask = page_mask;
        used_page_bits = page_bits;
    }
    std::size_t remaining_size{size};
    std::size_t page_index{gpu_src_addr >> used_page_bits};
    std::size_t page_offset{gpu_src_addr & used_page_mask};
    GPUVAddr current_address = gpu_src_addr;

    while (remaining_size > 0) {
        const std::size_t copy_amount{
            (std::min)(static_cast<std::size_t>(used_page_size) - page_offset, remaining_size)};
        auto entry = GetEntry(current_address, is_big_page);
        if (entry == EntryType::Mapped) [[likely]] {
            if constexpr (BOOL_BREAK_MAPPED) {
                if (func_mapped(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_mapped(page_index, page_offset, copy_amount);
            }

        } else if (entry == EntryType::Reserved) {
            if constexpr (BOOL_BREAK_RESERVED) {
                if (func_reserved(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_reserved(page_index, page_offset, copy_amount);
            }

        } else [[unlikely]] {
            if constexpr (BOOL_BREAK_UNMAPPED) {
                if (func_unmapped(page_index, page_offset, copy_amount)) {
                    return;
                }
            } else {
                func_unmapped(page_index, page_offset, copy_amount);
            }
        }
        page_index++;
        page_offset = 0;
        remaining_size -= copy_amount;
        current_address += copy_amount;
    }
}

void MemoryManager::ReadBlockImpl(GPUVAddr gpu_src_addr, void* dest_buffer, std::size_t size, [[maybe_unused]] VideoCommon::CacheType which, bool unsafe) const {
    auto set_to_zero = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset, std::size_t copy_amount) {
        std::memset(dest_buffer, 0, copy_amount);
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
        }
        // A device page without memory (a sparse block nobody wrote yet) reads zero.
        if (const u8* physical = memory.GetPointer<u8>(dev_addr_base)) [[likely]] {
            std::memcpy(dest_buffer, physical, copy_amount);
        } else {
            std::memset(dest_buffer, 0, copy_amount);
        }
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
        }
        if (!IsBigPageContinuous(page_index)) [[unlikely]] {
            memory.ReadBlockUnsafe(dev_addr_base, dest_buffer, copy_amount);
        } else {
            u8* physical = memory.GetPointer<u8>(dev_addr_base);
            std::memcpy(dest_buffer, physical, copy_amount);
        }
        dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
    };
    auto read_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, set_to_zero, set_to_zero);
    };
    MemoryOperation(gpu_src_addr, size, true, mapped_big, set_to_zero, read_short_pages);
}

void MemoryManager::ReadBlock(GPUVAddr gpu_src_addr, void* dest_buffer, std::size_t size, VideoCommon::CacheType which) const {
    CheckNotForwarding("MemoryManager::ReadBlock");
    ReadBlockImpl(gpu_src_addr, dest_buffer, size, which, false);
}

void MemoryManager::ReadWordsUnsafe(GPUVAddr gpu_src_addr, u32* dest, std::size_t count) const {
    const std::size_t size = count * sizeof(u32);
    const auto shim = native_syncpoint_shim.load(std::memory_order_acquire);
    const GPUVAddr shim_end = shim + GPUVAddr{NumShimSyncpoints} * sizeof(u32);
    // A range that starts before the shim overlaps it when it reaches the shim. Adding the
    // size onto the address would wrap.
    const bool overlaps_shim = shim && count != 0 && gpu_src_addr < shim_end &&
                               (gpu_src_addr >= shim || size > shim - gpu_src_addr);
    if (overlaps_shim) {
        for (std::size_t i = 0; i < count; ++i) {
            dest[i] = Read<u32>(gpu_src_addr + i * sizeof(u32));
        }
        return;
    }
    ReadBlockUnsafe(gpu_src_addr, dest, size);
}

void MemoryManager::ReadBlockUnsafe(GPUVAddr gpu_src_addr, void* dest_buffer, const std::size_t size) const {
    // Small reads within one 4 KiB page (texture and sampler descriptors, several per draw): one
    // translation and a copy, as Read<T> does, instead of the page walk (~3% of the GPU thread
    // in title 1). Anything else, unmapped or sparse pages included, takes the full path.
    constexpr std::size_t SmallPage = 0x1000;
    if (size <= SmallPage && (gpu_src_addr & (SmallPage - 1)) + size <= SmallPage) {
        if (const u8* const pointer = SmallReadPointer(gpu_src_addr)) {
            std::memcpy(dest_buffer, pointer, size);
            if (DescXlatMode() == 2) [[unlikely]] {
                CheckSmallRead(gpu_src_addr, dest_buffer, size);
            }
            return;
        }
    }
    ReadBlockImpl(gpu_src_addr, dest_buffer, size, VideoCommon::CacheType::None, true);
}

void MemoryManager::CheckSmallRead(GPUVAddr gpu_addr, const void* read, std::size_t size) const {
    static std::atomic<u64> checks{};
    static std::atomic<u32> reports{};
    if (checks.fetch_add(1, std::memory_order_relaxed) == 0) {
        LOG_INFO(HW_GPU, "EDEN_NX2_DESC_XLAT=2: checking small reads against the page walk");
    }
    std::array<u8, 0x1000> walked;
    ReadBlockImpl(gpu_addr, walked.data(), size, VideoCommon::CacheType::None, true);
    const u8* const fast = static_cast<const u8*>(read);
    const u8* const at = std::mismatch(fast, fast + size, walked.data()).first;
    if (at == fast + size) {
        return;
    }
    // A guest write landing between the two reads shows here as well.
    if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
        const std::size_t offset = static_cast<std::size_t>(at - fast);
        LOG_ERROR(HW_GPU,
                  "EDEN_NX2_DESC_XLAT: small read of {} bytes at {:#x} differs from the page walk "
                  "from byte {} ({:#04x} vs {:#04x}; after {} checks)",
                  size, gpu_addr, offset, fast[offset], walked[offset],
                  checks.load(std::memory_order_relaxed));
    }
}

void MemoryManager::WriteBlockImpl(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size, [[maybe_unused]] VideoCommon::CacheType which, bool unsafe) {
    // Give Switch 2 sparse pages the write lands on their memory first (otherwise it is dropped).
    Tegra::Nx2BackSparseWrite(gpu_dest_addr, size);
    auto just_advance = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset, std::size_t copy_amount) {
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
        }
        if (u8* physical = memory.GetPointer<u8>(dev_addr_base)) [[likely]] {
            std::memcpy(physical, src_buffer, copy_amount);
        }
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (!unsafe) {
            rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
        }
        if (!IsBigPageContinuous(page_index)) [[unlikely]] {
            memory.WriteBlockUnsafe(dev_addr_base, src_buffer, copy_amount);
        } else {
            u8* physical = memory.GetPointer<u8>(dev_addr_base);
            std::memcpy(physical, src_buffer, copy_amount);
        }
        src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
    };
    auto write_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, just_advance, just_advance);
    };
    MemoryOperation(gpu_dest_addr, size, true, mapped_big, just_advance, write_short_pages);
}

bool MemoryManager::WriteSmall(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size) {
    constexpr std::size_t SmallPage = std::size_t{1} << cpu_page_bits;
    if (!g_nx2_small_write || size == 0 || page_bits < cpu_page_bits ||
        big_page_bits < cpu_page_bits || (gpu_dest_addr & (SmallPage - 1)) + size > SmallPage ||
        !IsWithinGPUAddressRange(gpu_dest_addr)) {
        return false;
    }
    // WriteBlockImpl skips a reserved big page; GpuToCpuAddress would look at the small pages
    // under it. Under a mapped or free big page both translate the address the same way.
    if (GetEntry(gpu_dest_addr, true) == EntryType::Reserved) {
        return false;
    }
    // As WriteBlockImpl, first: a Switch 2 sparse block the write lands on gets its memory (when
    // this returns false WriteBlockImpl asks again, and blocks with memory are left as they are).
    Tegra::Nx2BackSparseWrite(gpu_dest_addr, size);
    // The host page WriteBlockImpl writes through (its GetPointer, or the device WriteBlockUnsafe
    // for a 4 KiB block of a big page); none where it would write nothing or log, which it then
    // does itself. The pointer is device memory, written as WriteBlockImpl writes it.
    const u8* const pointer = SmallReadPointer(gpu_dest_addr);
    if (pointer == nullptr) {
        return false;
    }
    std::memcpy(const_cast<u8*>(pointer), src_buffer, size);
    return true;
}

void MemoryManager::WriteBlock(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size, VideoCommon::CacheType which) {
    CheckNotForwarding("MemoryManager::WriteBlock");
    WriteBlockImpl(gpu_dest_addr, src_buffer, size, which, false);
}

void MemoryManager::MarkUntracked(GPUVAddr gpu_addr, std::size_t size) const {
    if (size == 0) {
        return;
    }
    for (GPUVAddr page = gpu_addr & ~GPUVAddr{0xfff}; page < gpu_addr + size; page += 0x1000) {
        if (const std::optional<DAddr> device = GpuToCpuAddress(page)) {
            Tegra::Nx2MarkUntrackedWrite(*device, 1);
        }
    }
}

void MemoryManager::WriteBlockUnsafe(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size,
                                     bool caches_told) {
    if (!caches_told) {
        MarkUntracked(gpu_dest_addr, size);
    }
    if (WriteSmall(gpu_dest_addr, src_buffer, size)) {
        return;
    }
    WriteBlockImpl(gpu_dest_addr, src_buffer, size, VideoCommon::CacheType::None, true);
}

void MemoryManager::WriteBlockCached(GPUVAddr gpu_dest_addr, const void* src_buffer, std::size_t size) {
    if (!WriteSmall(gpu_dest_addr, src_buffer, size)) {
        WriteBlockImpl(gpu_dest_addr, src_buffer, size, VideoCommon::CacheType::None, true);
    }
    ++cached_writes;
    accumulator.Add(gpu_dest_addr, size);
}

void MemoryManager::FlushRegion(GPUVAddr gpu_addr, size_t size,
                                VideoCommon::CacheType which) const {
    CheckNotForwarding("MemoryManager::FlushRegion");
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) {};

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        rasterizer->FlushRegion(dev_addr_base, copy_amount, which);
    };
    auto flush_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, flush_short_pages);
}

bool MemoryManager::IsMemoryDirty(GPUVAddr gpu_addr, size_t size,
                                  VideoCommon::CacheType which) const {
    CheckNotForwarding("MemoryManager::IsMemoryDirty");
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MemoryDirtyQueries, 1);
    bool result = false;
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) { return false; };

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        result |= rasterizer->MustFlushRegion(dev_addr_base, copy_amount, which);
        return result;
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        result |= rasterizer->MustFlushRegion(dev_addr_base, copy_amount, which);
        return result;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
        return result;
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, check_short_pages);
    return result;
}

size_t MemoryManager::MaxContinuousRange(GPUVAddr gpu_addr, size_t size) const {
    std::optional<DAddr> old_page_addr{};
    size_t range_so_far = 0;
    bool result{false};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    std::size_t copy_amount) {
        result = true;
        return true;
    };
    auto short_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = true;
            return true;
        }
        range_so_far += copy_amount;
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto big_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            return true;
        }
        range_so_far += copy_amount;
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, short_check, fail, fail);
        return result;
    };
    MemoryOperation(gpu_addr, size, true, big_check, fail, check_short_pages);
    return range_so_far;
}

size_t MemoryManager::GetMemoryLayoutSize(GPUVAddr gpu_addr, size_t max_size) const {
    std::unique_lock<std::mutex> lock(guard);
    return kind_map.GetContinuousSizeFrom(gpu_addr);
}

void MemoryManager::InvalidateRegion(GPUVAddr gpu_addr, size_t size,
                                     VideoCommon::CacheType which) const {
    CheckNotForwarding("MemoryManager::InvalidateRegion");
    auto do_nothing = [&]([[maybe_unused]] std::size_t page_index,
                          [[maybe_unused]] std::size_t offset,
                          [[maybe_unused]] std::size_t copy_amount) {};

    auto mapped_normal = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(page_table[page_index]) << cpu_page_bits) + offset;
        rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
    };
    auto mapped_big = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base = (DAddr(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        rasterizer->InvalidateRegion(dev_addr_base, copy_amount, which);
    };
    auto invalidate_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, mapped_normal, do_nothing, do_nothing);
    };
    MemoryOperation(gpu_addr, size, true, mapped_big, do_nothing, invalidate_short_pages);
}

void MemoryManager::CopyBlock(GPUVAddr gpu_dest_addr, GPUVAddr gpu_src_addr, std::size_t size,
                              VideoCommon::CacheType which) {
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::SafeReadWrite> data(
        *this, gpu_src_addr, size);
    data.SetAddressAndSize(gpu_dest_addr, size);
    FlushRegion(gpu_dest_addr, size, which);
}

bool MemoryManager::IsGranularRange(GPUVAddr gpu_addr, std::size_t size) const {
    if (GetEntry(gpu_addr, true) == EntryType::Mapped) [[likely]] {
        size_t page_index = gpu_addr >> big_page_bits;
        if (IsBigPageContinuous(page_index)) [[likely]] {
            const std::size_t page{(page_index & big_page_mask) + size};
            return page <= big_page_size;
        }
        const std::size_t page{(gpu_addr & Core::DEVICE_PAGEMASK) + size};
        return page <= Core::DEVICE_PAGESIZE;
    }
    if (GetEntry(gpu_addr, false) != EntryType::Mapped) {
        return false;
    }
    const std::size_t page{(gpu_addr & Core::DEVICE_PAGEMASK) + size};
    return page <= Core::DEVICE_PAGESIZE;
}

bool MemoryManager::IsContinuousRange(GPUVAddr gpu_addr, std::size_t size) const {
    std::optional<DAddr> old_page_addr{};
    bool result{true};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    std::size_t copy_amount) {
        result = false;
        return true;
    };
    auto short_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = false;
            return true;
        }
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto big_check = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr && *old_page_addr != dev_addr_base) {
            result = false;
            return true;
        }
        old_page_addr = {dev_addr_base + copy_amount};
        return false;
    };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset,
                                 std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, short_check, fail, fail);
        return !result;
    };
    MemoryOperation(gpu_addr, size, true, big_check, fail, check_short_pages);
    return result;
}

bool MemoryManager::IsFullyMappedRange(GPUVAddr gpu_addr, std::size_t size) const {
    bool result{true};
    auto fail = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    [[maybe_unused]] std::size_t copy_amount) {
        result = false;
        return true;
    };
    auto pass = [&]([[maybe_unused]] std::size_t page_index, [[maybe_unused]] std::size_t offset,
                    [[maybe_unused]] std::size_t copy_amount) { return false; };
    auto check_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, pass, pass, fail);
        return !result;
    };
    MemoryOperation(gpu_addr, size, true, pass, fail, check_short_pages);
    return result;
}

boost::container::small_vector<std::pair<GPUVAddr, std::size_t>, 32>
MemoryManager::GetSubmappedRange(GPUVAddr gpu_addr, std::size_t size) const {
    boost::container::small_vector<std::pair<GPUVAddr, std::size_t>, 32> result{};
    GetSubmappedRangeImpl<true>(gpu_addr, size, result);
    return result;
}

template <bool is_gpu_address>
void MemoryManager::GetSubmappedRangeImpl(GPUVAddr gpu_addr, std::size_t size, boost::container::small_vector<std::pair<std::conditional_t<is_gpu_address, GPUVAddr, DAddr>, std::size_t>, 32>& result)
    const {
    std::optional<std::pair<std::conditional_t<is_gpu_address, GPUVAddr, DAddr>, std::size_t>> last_segment{};
    std::optional<DAddr> old_page_addr{};
    const auto split = [&last_segment, &result]([[maybe_unused]] std::size_t page_index,
                                                [[maybe_unused]] std::size_t offset,
                                                [[maybe_unused]] std::size_t copy_amount) {
        if (last_segment) {
            result.push_back(*last_segment);
            last_segment = std::nullopt;
        }
    };
    const auto extend_size_big = [this, &split, &old_page_addr,
                                  &last_segment](std::size_t page_index, std::size_t offset,
                                                 std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(big_page_table_dev[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr) {
            if (*old_page_addr != dev_addr_base) {
                split(0, 0, 0);
            }
        }
        old_page_addr = {dev_addr_base + copy_amount};
        if (!last_segment) {
            if constexpr (is_gpu_address) {
                const GPUVAddr new_base_addr = (page_index << big_page_bits) + offset;
                last_segment = {new_base_addr, copy_amount};
            } else {
                last_segment = {dev_addr_base, copy_amount};
            }
        } else {
            last_segment->second += copy_amount;
        }
    };
    const auto extend_size_short = [this, &split, &old_page_addr,
                                    &last_segment](std::size_t page_index, std::size_t offset,
                                                   std::size_t copy_amount) {
        const DAddr dev_addr_base =
            (static_cast<DAddr>(page_table[page_index]) << cpu_page_bits) + offset;
        if (old_page_addr) {
            if (*old_page_addr != dev_addr_base) {
                split(0, 0, 0);
            }
        }
        old_page_addr = {dev_addr_base + copy_amount};
        if (!last_segment) {
            if constexpr (is_gpu_address) {
                const GPUVAddr new_base_addr = (page_index << page_bits) + offset;
                last_segment = {new_base_addr, copy_amount};
            } else {
                last_segment = {dev_addr_base, copy_amount};
            }
        } else {
            last_segment->second += copy_amount;
        }
    };
    auto do_short_pages = [&](std::size_t page_index, std::size_t offset, std::size_t copy_amount) {
        GPUVAddr base = (page_index << big_page_bits) + offset;
        MemoryOperation(base, copy_amount, false, extend_size_short, split, split);
    };
    MemoryOperation(gpu_addr, size, true, extend_size_big, split, do_short_pages);
    split(0, 0, 0);
}

bool MemoryManager::NoteSkippedWrite(GPUVAddr gpu_addr, DAddr device_addr, u32 size) {
    if (skipped_count == skipped_writes.size()) {
        return false;
    }
    if (skipped_count == 0) {
        skipped_epoch = rasterizer->WorkEpoch();
        skipped_flushes = Tegra::g_nx2_flushes.load(std::memory_order_relaxed);
    }
    skipped_writes[skipped_count++] = {gpu_addr, device_addr, size};
    return true;
}

void MemoryManager::ReviewSkippedWrites() {
    // A skipped write's invalidation would run now. When it was skipped it would have changed
    // nothing (Maxwell3D::IsLightSameWrite: the bytes memory held, on pages not written around
    // the caches, no cache holding GPU-written data or a query result there). What could change
    // that since is work: what writes through the caches does so only after a FlushCaching (draws
    // and everything else run here or in another address space) or bumps the rasterizer's
    // WorkEpoch (CPU writes, flushes, unmaps, fences, clears, dispatches, copies, queries), and
    // what writes around them marks its pages (semaphores too, which bump nothing). If either
    // counter moved since the first skip, every skipped write is invalidated as if it had been
    // made: what happened before. Otherwise only those on pages written around the caches since.
    // Each counts as a cached write that is no same write, so no light draw follows it.
    // Back stages, cut c (S12): on B, WorkEpoch is B's copy of the light epoch (C's events pass
    // B first and move it there), and what C writes around the caches since (query results,
    // images) only C's page summary knows: B's HoldsGpuWrittenData reads it (T8).
    const bool work = rasterizer->WorkEpoch() != skipped_epoch ||
                      Tegra::g_nx2_flushes.load(std::memory_order_relaxed) != skipped_flushes;
    const bool stage_b = VideoCommon::Nx2Stages::OnStage(VideoCommon::Nx2Stages::Stage::B) &&
                         VideoCommon::Nx2Stages::Get().cut_c;
    for (u32 i = 0; i < skipped_count; ++i) {
        const SkippedWrite& write = skipped_writes[i];
        if (work || Tegra::Nx2UntrackedWritten(write.device_addr, write.size) ||
            (stage_b && rasterizer->HoldsGpuWrittenData(write.device_addr, write.size))) {
            ++cached_writes;
            accumulator.Add(write.gpu_addr, write.size);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameWritesInvalidatedLater, 1);
        }
    }
    skipped_count = 0;
}

bool MemoryManager::FlushCaching() {
    if (skipped_count != 0) [[unlikely]] {
        ReviewSkippedWrites();
    }
    Tegra::g_nx2_flushes.fetch_add(1, std::memory_order_relaxed);
    // EDEN_NX2_INVAL_SKIP=0: every accumulated range translated page by page and handed to the
    // caches as collected.
    static const bool merge_ranges = [] {
        const char* value = std::getenv("EDEN_NX2_INVAL_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    if (!merge_ranges) {
        // Flush from the invalidate accumulator
        if (accumulator.InvalidateAll([this](GPUVAddr addr, size_t size) {
            GetSubmappedRangeImpl<false>(addr, size, page_stash2);
        })) {
            rasterizer->InnerInvalidation(page_stash2);
            page_stash2.clear();
            return true;
        }
        return false;
    }
    // This runs before every draw (~5% of the GPU thread on title 1). The accumulator hands out
    // overlapping ranges (it rounds to 32 bytes from the unaligned start, so words written one by
    // one and 20-byte inline records come out as several ranges over the same bytes). Every
    // cache's invalidation is a set operation on the bytes, so the sorted, merged union
    // invalidates the same (the buffer cache clears pending downloads over the whole union when
    // any of it is GPU-modified); ranges within one page (nearly all) are translated directly,
    // as GetSubmappedRangeImpl would.
    boost::container::small_vector<std::pair<GPUVAddr, std::size_t>, 16> ranges;
    if (!accumulator.InvalidateAll([&ranges](GPUVAddr addr, size_t size) {
            if (size != 0) {
                ranges.emplace_back(addr, size);
            }
        })) {
        return false;
    }
    const size_t collected = ranges.size();
    if (collected > 1) {
        if (!std::is_sorted(ranges.begin(), ranges.end())) {
            std::sort(ranges.begin(), ranges.end());
        }
        size_t last = 0;
        for (size_t i = 1; i < collected; ++i) {
            const auto [addr, size] = ranges[i];
            auto& [last_addr, last_size] = ranges[last];
            if (addr <= last_addr + last_size) {
                last_size = (std::max)(last_size, static_cast<std::size_t>(addr + size - last_addr));
            } else {
                ranges[++last] = ranges[i];
            }
        }
        ranges.resize(last + 1);
    }
    u64 direct = 0;
    for (const auto& [addr, size] : ranges) {
        if (IsWithinGPUAddressRange(addr) && size <= address_space_size - addr) {
            const EntryType big_entry = GetEntry(addr, true);
            if (big_entry == EntryType::Mapped && (addr & big_page_mask) + size <= big_page_size) {
                page_stash2.emplace_back(
                    (static_cast<DAddr>(big_page_table_dev[addr >> big_page_bits]) << cpu_page_bits) +
                        (addr & big_page_mask),
                    size);
                ++direct;
                continue;
            }
            if (big_entry == EntryType::Free && GetEntry(addr, false) == EntryType::Mapped &&
                (addr & page_mask) + size <= page_size) {
                page_stash2.emplace_back(
                    (static_cast<DAddr>(page_table[addr >> page_bits]) << cpu_page_bits) +
                        (addr & page_mask),
                    size);
                ++direct;
                continue;
            }
        }
        GetSubmappedRangeImpl<false>(addr, size, page_stash2);
    }
    thread_local u64 stat_calls{}, stat_collected{}, stat_merged{}, stat_direct{}, stat_device{};
    ++stat_calls;
    stat_collected += collected;
    stat_merged += ranges.size();
    stat_direct += direct;
    stat_device += page_stash2.size();
    if ((stat_calls & ((u64{1} << 20) - 1)) == 0) {
        LOG_INFO(HW_GPU, "Pre-draw invalidations: {} calls, {} ranges merged to {} ({} translated directly), {} device ranges",
                 stat_calls, stat_collected, stat_merged, stat_direct, stat_device);
        stat_calls = stat_collected = stat_merged = stat_direct = stat_device = 0;
    }
    if (!page_stash2.empty()) {
        rasterizer->InnerInvalidation(page_stash2);
        page_stash2.clear();
    }
    return true;
}

const u8* MemoryManager::GetSpan(const GPUVAddr src_addr, const std::size_t size) const {
    if (!IsContinuousRange(src_addr, size)) {
        return nullptr;
    }
    auto dev_addr = GpuToCpuAddress(src_addr);
    if (dev_addr) {
        return memory.GetSpan(*dev_addr, size);
    }
    return nullptr;
}

u8* MemoryManager::GetSpan(const GPUVAddr src_addr, const std::size_t size) {
    if (!IsContinuousRange(src_addr, size)) {
        return nullptr;
    }
    auto dev_addr = GpuToCpuAddress(src_addr);
    if (dev_addr) {
        return memory.GetSpan(*dev_addr, size);
    }
    return nullptr;
}

} // namespace Tegra
