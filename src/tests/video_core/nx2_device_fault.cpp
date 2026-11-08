// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/nx2_device_fault.h"

using namespace Vulkan::Nx2DeviceFault;

TEST_CASE("Nx2DeviceFault: address types", "[video_core]") {
    REQUIRE(AddressTypeName(0) == "none");
    REQUIRE(AddressTypeName(1) == "read invalid");
    REQUIRE(AddressTypeName(2) == "write invalid");
    REQUIRE(AddressTypeName(3) == "execute invalid");
    REQUIRE(AddressTypeName(4) == "instruction pointer unknown");
    REQUIRE(AddressTypeName(5) == "instruction pointer invalid");
    REQUIRE(AddressTypeName(6) == "instruction pointer fault");
    REQUIRE(AddressTypeName(7) == "unknown type");
    REQUIRE(AddressTypeName(0x7FFFFFFF) == "unknown type");
}

TEST_CASE("Nx2DeviceFault: the driver's text", "[video_core]") {
    const std::array<char, 8> short_text{'p', 'a', 'g', 'e', '\0', 'x', 'y', 'z'};
    REQUIRE(Text(short_text.data(), short_text.size()) == "page");
    // No NUL: the whole array, nothing past it
    const std::array<char, 4> full{'f', 'u', 'l', 'l'};
    REQUIRE(Text(full.data(), full.size()) == "full");
    const std::array<char, 4> empty{};
    REQUIRE(Text(empty.data(), empty.size()).empty());
}

TEST_CASE("Nx2DeviceFault: reported once", "[video_core]") {
    Once once;
    REQUIRE(once.First());
    REQUIRE_FALSE(once.First());
    REQUIRE_FALSE(once.First());
}
