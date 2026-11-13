// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Common::Net {

struct Asset {
    std::string name;
    std::string url;
    std::string path;
    std::string filename;
    // Lowercase hex SHA-256 published by the host, empty if unknown.
    std::string sha256;
};

struct Release {
    std::string title;
    std::string body;
    std::string tag;
    std::string base_download_url;
    std::string html_url;
    std::string host;

    std::vector<std::string> assets;
    // SHA-256 of each asset, keyed by its entry in `assets`.
    std::map<std::string, std::string> digests;

    u64 id;
    u64 published;
    bool prerelease;

    // Get the relevant list of assets for the current platform.
    std::vector<Asset> GetPlatformAssets() const;

    static std::optional<Release> FromJson(const nlohmann::json& json, const std::string &host, const std::string& repo);
    static std::optional<Release> FromJson(const std::string_view& json, const std::string &host, const std::string& repo);
    static std::vector<Release> ListFromJson(const nlohmann::json &json, const std::string &host, const std::string &repo);
    static std::vector<Release> ListFromJson(const std::string_view &json, const std::string &host, const std::string &repo);
};

// Make a request via httplib, and return the response body if applicable.
std::optional<std::string> MakeRequest(const std::string &url, const std::string &path);

// Get all of the latest stable releases.
std::vector<Release> GetReleases();

// Get all of the latest stable releases as text.
std::optional<std::string> GetReleasesBody();

// Compare `head` against `base` (tags or commits) on the host. Returns the status, such as
// "ahead", "behind", "identical" or "diverged" (head relative to base), or nothing on failure.
std::optional<std::string> GetCompareStatus(const std::string& base, const std::string& head);

// Get the latest release of the current channel.
std::optional<Release> GetLatestRelease();

}
