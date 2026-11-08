// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "common/common_types.h"

// "nx2 device fault" lines: what the driver says of a device loss through VK_EXT_device_fault
// (Device::ReportFault), once per process.
namespace Vulkan::Nx2DeviceFault {

/// VkDeviceFaultAddressTypeEXT (vulkan_device.cpp checks the values).
constexpr std::string_view AddressTypeName(u32 type) {
    switch (type) {
    case 0:
        return "none";
    case 1:
        return "read invalid";
    case 2:
        return "write invalid";
    case 3:
        return "execute invalid";
    case 4:
        return "instruction pointer unknown";
    case 5:
        return "instruction pointer invalid";
    case 6:
        return "instruction pointer fault";
    default:
        return "unknown type";
    }
}

/// A driver's fixed size text, up to its first NUL; all of it if it has none.
constexpr std::string_view Text(const char* text, std::size_t size) {
    std::size_t length = 0;
    while (length < size && text[length] != '\0') {
        ++length;
    }
    return {text, length};
}

/// The first device loss to report wins: the others (the threads that see it too) log nothing.
class Once {
public:
    bool First() {
        return !done.exchange(true, std::memory_order_acq_rel);
    }

private:
    std::atomic<bool> done{};
};

} // namespace Vulkan::Nx2DeviceFault
