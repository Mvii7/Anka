// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_auto_script.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>

namespace Nx2AutoScript {
namespace {

struct KeyName {
    std::string_view name;
    int id;
};

// Ids are VirtualGamepad::VirtualButton values (yuzu.cpp checks them).
constexpr std::array<KeyName, 14> Keys{{
    {"A", 0},
    {"B", 1},
    {"X", 2},
    {"Y", 3},
    {"L", 6},
    {"R", 7},
    {"ZL", 8},
    {"ZR", 9},
    {"Plus", 10},
    {"Minus", 11},
    {"Left", 12},
    {"Up", 13},
    {"Right", 14},
    {"Down", 15},
}};

bool SameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

std::vector<std::string_view> Split(std::string_view line) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        const size_t begin = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            ++i;
        }
        if (i > begin) {
            words.push_back(line.substr(begin, i - begin));
        }
    }
    return words;
}

std::optional<double> Number(std::string_view word) {
    // strtod: libc++ (macOS) has no floating-point std::from_chars.
    const std::string text{word};
    char* end{};
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || errno != 0 || end != text.c_str() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<Anchor> AnchorNamed(std::string_view name) {
    if (SameText(name, "boot")) {
        return Anchor::Boot;
    }
    if (SameText(name, "present")) {
        return Anchor::Present;
    }
    if (SameText(name, "album")) {
        return Anchor::Album;
    }
    return std::nullopt;
}

// Returns an error message, or an empty string when the line was taken. A press without an '@'
// anchor goes on the file's clock: its index is added to file_clock, for once the file is read.
std::string ParseLine(std::vector<std::string_view> words, Script& script, bool& anchor_set,
                      std::vector<size_t>& file_clock) {
    if (SameText(words[0], "anchor")) {
        if (words.size() != 2) {
            return "expected 'anchor boot', 'anchor present' or 'anchor album'";
        }
        const auto anchor = AnchorNamed(words[1]);
        if (!anchor) {
            return "unknown anchor (boot, present or album)";
        }
        if (anchor_set) {
            return "a second anchor line";
        }
        anchor_set = true;
        script.anchor = *anchor;
        return {};
    }
    if (SameText(words[0], "until")) {
        if (words.size() != 2 || !SameText(words[1], "album")) {
            return "expected 'until album'";
        }
        script.until_album = true;
        return {};
    }
    std::optional<Anchor> own_anchor;
    if (words[0].starts_with('@')) {
        own_anchor = AnchorNamed(words[0].substr(1));
        if (!own_anchor) {
            return "unknown anchor after '@' (boot, present or album)";
        }
        words.erase(words.begin());
        if (words.empty()) {
            return "expected a press after the anchor";
        }
    }
    const auto start = Number(words[0]);
    if (!start || *start < 0.0) {
        return "the time is not a number of seconds >= 0";
    }
    Event event{.anchor = own_anchor.value_or(Anchor::Present), .start = *start, .duration = 0.0,
                .stick = false, .id = 0, .x = 0.0f, .y = 0.0f, .name = {}};
    size_t duration_word;
    if (words.size() >= 2 && SameText(words[1], "stick")) {
        if (words.size() != 6) {
            return "expected '<seconds> stick <L|R> <x> <y> <held seconds>'";
        }
        if (SameText(words[2], "L")) {
            event.id = 0;
        } else if (SameText(words[2], "R")) {
            event.id = 1;
        } else {
            return "unknown stick (L or R)";
        }
        const auto x = Number(words[3]);
        const auto y = Number(words[4]);
        if (!x || !y || std::abs(*x) > 1.0 || std::abs(*y) > 1.0) {
            return "stick x and y must be numbers in [-1, 1]";
        }
        event.stick = true;
        event.x = static_cast<float>(*x);
        event.y = static_cast<float>(*y);
        event.name = event.id == 0 ? "stick L" : "stick R";
        duration_word = 5;
    } else {
        if (words.size() != 3) {
            return "expected '<seconds> <key> <held seconds>'";
        }
        const auto key = std::find_if(Keys.begin(), Keys.end(), [&](const KeyName& k) {
            return SameText(k.name, words[1]);
        });
        if (key == Keys.end()) {
            return "unknown key (A B X Y L R ZL ZR Plus Minus Up Down Left Right)";
        }
        event.id = key->id;
        event.name = std::string{key->name};
        duration_word = 2;
    }
    const auto duration = Number(words[duration_word]);
    if (!duration || *duration <= 0.0) {
        return "the held time is not a number of seconds > 0";
    }
    event.duration = *duration;
    if (!own_anchor) {
        file_clock.push_back(script.events.size());
    }
    script.events.push_back(std::move(event));
    return {};
}

} // Anonymous namespace

Script ParseScript(std::string_view text) {
    Script script;
    bool anchor_set = false;
    std::vector<size_t> file_clock;
    size_t line_number = 0;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = std::min(text.find('\n', begin), text.size());
        std::string_view line = text.substr(begin, end - begin);
        ++line_number;
        begin = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const auto words = Split(line);
        if (words.empty()) {
            continue;
        }
        if (std::string error = ParseLine(words, script, anchor_set, file_clock); !error.empty()) {
            std::string shown{line};
            while (!shown.empty() && (shown.back() == ' ' || shown.back() == '\t' ||
                                      shown.back() == '\r')) {
                shown.pop_back();
            }
            script.errors.push_back("line " + std::to_string(line_number) + ": " + error + ": '" +
                                    shown + "'");
        }
    }
    // The anchor line may come after the presses.
    for (const size_t i : file_clock) {
        script.events[i].anchor = script.anchor;
    }
    return script;
}

std::vector<Action> Schedule(const Script& script, Anchor anchor) {
    std::vector<Action> actions;
    for (size_t i = 0; i < script.events.size(); ++i) {
        const Event& event = script.events[i];
        if (event.anchor != anchor) {
            continue;
        }
        actions.push_back({event.start, true, i});
        actions.push_back({event.start + event.duration, false, i});
    }
    std::stable_sort(actions.begin(), actions.end(), [](const Action& a, const Action& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return !a.down && b.down;
    });
    return actions;
}

size_t DropUnstarted(std::vector<Action>& actions, size_t next) {
    next = std::min(next, actions.size());
    std::vector<bool> dropped;
    size_t count = 0;
    for (size_t i = next; i < actions.size(); ++i) {
        if (actions[i].down) {
            if (actions[i].event >= dropped.size()) {
                dropped.resize(actions[i].event + 1);
            }
            dropped[actions[i].event] = true;
            ++count;
        }
    }
    const auto first = actions.begin() + static_cast<std::ptrdiff_t>(next);
    actions.erase(std::remove_if(first, actions.end(),
                                 [&](const Action& action) {
                                     return action.event < dropped.size() && dropped[action.event];
                                 }),
                  actions.end());
    return count;
}

ExitSpec ParseExit(std::string_view text) {
    ExitSpec spec;
    const auto words = Split(text);
    if (words.empty()) {
        return spec;
    }
    if (words.size() != 1) {
        spec.error = "expected '<seconds>[@boot|@present|@album]'";
        return spec;
    }
    std::string_view seconds = words[0];
    if (const size_t at = seconds.find('@'); at != std::string_view::npos) {
        const auto anchor = AnchorNamed(seconds.substr(at + 1));
        if (!anchor) {
            spec.error = "unknown anchor after '@' (boot, present or album)";
            return spec;
        }
        spec.anchor = *anchor;
        seconds = seconds.substr(0, at);
    }
    const auto value = Number(seconds);
    if (!value || *value < 0.0) {
        spec.error = "the time is not a number of seconds >= 0";
        return spec;
    }
    spec.seconds = *value;
    spec.enabled = *value > 0.0;
    return spec;
}

const char* AnchorName(Anchor anchor) {
    switch (anchor) {
    case Anchor::Boot:
        return "boot";
    case Anchor::Album:
        return "album";
    case Anchor::Present:
        break;
    }
    return "present";
}

} // namespace Nx2AutoScript
