// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "frontend_common/nx_archive.h"

namespace FrontendCommon::NxArchive {

namespace {

namespace fs = std::filesystem;

std::string LowerName(const fs::path& path) {
    const auto u8 = path.filename().generic_u8string();
    std::string name{u8.begin(), u8.end()};
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

fs::path FindDirectory(const fs::path& parent, std::string_view name) {
    std::error_code ec;
    if (parent.empty() || !fs::is_directory(parent, ec)) {
        return {};
    }
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (std::error_code type_ec; it->is_directory(type_ec) && LowerName(it->path()) == name) {
            return it->path();
        }
    }
    return {};
}

std::optional<u64> ParseDecimal(const std::string& name) {
    u64 value{};
    const auto* end = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(name.data(), end, value);
    if (name.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// The exefs folder a dump boots from: the newest updates/<version>, else base, else the root.
// Each must hold main, main.npdm and a romfs folder next to it or under base.
bool HasBootableExeFS(const fs::path& root) {
    const auto bootable = [](const fs::path& exefs) {
        std::error_code ec;
        return !exefs.empty() && fs::is_regular_file(exefs / "main", ec) &&
               fs::is_regular_file(exefs / "main.npdm", ec);
    };
    fs::path exefs;
    std::optional<u64> best_version;
    if (const auto updates = FindDirectory(root, "updates"); !updates.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(updates, ec), end; !ec && it != end; it.increment(ec)) {
            const auto version = ParseDecimal(LowerName(it->path()));
            const auto candidate = FindDirectory(it->path(), "exefs");
            if (version && bootable(candidate) && (!best_version || *version > *best_version)) {
                best_version = version;
                exefs = candidate;
            }
        }
    }
    const auto base = FindDirectory(root, "base");
    if (exefs.empty()) {
        exefs = FindDirectory(base, "exefs");
    }
    if (exefs.empty()) {
        exefs = FindDirectory(root, "exefs");
    }
    if (!bootable(exefs)) {
        return false;
    }
    return !FindDirectory(exefs.parent_path(), "romfs").empty() ||
           !FindDirectory(base, "romfs").empty();
}

} // namespace

std::string WithNxExtension(std::string path) {
    constexpr std::string_view extension = ".nx";
    const bool has_extension =
        path.size() >= extension.size() &&
        std::equal(extension.begin(), extension.end(), path.end() - extension.size(),
                   [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
    if (!has_extension) {
        path += extension;
    }
    return path;
}

std::vector<std::filesystem::path> RegularFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (std::error_code type_ec; it->is_regular_file(type_ec)) {
            files.push_back(it->path());
        }
    }
    return files;
}

bool IsTitleEdit(std::string_view edited, std::string_view shown) {
    return !edited.empty() && edited != shown;
}

std::filesystem::path UpdateFolder(const std::filesystem::path& chosen) {
    std::error_code ec;
    auto folder = chosen;
    for (int depth = 0; depth < 3 && !std::filesystem::is_directory(folder / "exefs", ec);
         ++depth) {
        std::optional<std::filesystem::path> only;
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (std::error_code type_ec; !it->is_directory(type_ec)) {
                continue;
            }
            if (only) {
                return chosen;
            }
            only = it->path();
        }
        if (ec || !only) {
            return chosen;
        }
        folder = *only;
    }
    return std::filesystem::is_directory(folder / "exefs", ec) ? folder : chosen;
}

std::optional<std::filesystem::path> DumpRootFromMain(const std::filesystem::path& main) {
    // The Qt game list stores the exefs folder rather than exefs/main.
    std::error_code ec;
    const auto exefs = fs::is_directory(main, ec) ? main : main.parent_path();
    if (LowerName(exefs) != "exefs") {
        return std::nullopt;
    }
    auto root = exefs.parent_path();
    const auto name = LowerName(root);
    if (name == "base") {
        root = root.parent_path();
    } else if (ParseDecimal(name) && LowerName(root.parent_path()) == "updates") {
        root = root.parent_path().parent_path();
    }
    if (!HasBootableExeFS(root)) {
        return std::nullopt;
    }
    return root;
}

} // namespace FrontendCommon::NxArchive
