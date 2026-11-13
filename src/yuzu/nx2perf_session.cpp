// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nx2perf_session.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStringList>

#include "common/logging.h"
#include "common/settings.h"
#include "core/nx2_perf_settings.h"
#include "qt_common/config/uisettings.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Nx2PerfSession {

namespace {

namespace Perf = Core::Nx2PerfSettings;

// A restarted process waits for this process id (the old one) before it opens the log.
constexpr char RESTART_PID_VARIABLE[] = "ANKA_RESTART_WAIT_PID";
constexpr int RESTART_WAIT_MS = 30'000;

QProcessEnvironment g_start_environment;
std::vector<std::string_view> g_set_outside;
PendingGraphics g_pending;
bool g_pending_unasked = false; // the waiting settings changed since the restart question

struct Applied {
    std::vector<Perf::Var> plan;
    Perf::Game game;
};
std::optional<Applied> g_applied; // set at the first boot

struct Restart {
    QString game; ///< empty: the program's own command line again
    bool fullscreen;
};
std::optional<Restart> g_restart;

void SetVariable(const Perf::Var& var) {
    const std::string name{var.name};
    const std::string value{var.value};
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

QString Describe(const std::vector<Perf::Var>& plan) {
    QStringList lines;
    for (const auto& var : plan) {
        lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(var.name.data(), var.name.size()),
                                             QString::fromUtf8(var.value.data(), var.value.size()));
    }
    return lines.isEmpty() ? QStringLiteral("none") : lines.join(QLatin1Char(' '));
}

} // Anonymous namespace

void CaptureStartEnvironment() {
    // The log is not open yet: nothing is logged here.
    if (const char* pid_text = std::getenv(RESTART_PID_VARIABLE)) {
#ifdef _WIN32
        const unsigned long pid = std::strtoul(pid_text, nullptr, 10);
        if (pid != 0) {
            if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
                WaitForSingleObject(process, RESTART_WAIT_MS);
                CloseHandle(process);
            }
        }
        _putenv_s(RESTART_PID_VARIABLE, "");
#else
        (void)pid_text;
        unsetenv(RESTART_PID_VARIABLE);
#endif
    }
    g_start_environment = QProcessEnvironment::systemEnvironment();
    g_start_environment.remove(QString::fromLatin1(RESTART_PID_VARIABLE));
}

void CaptureOutsideSwitches() {
    g_set_outside.clear();
    Perf::ForEachName([](std::string_view name) {
        if (std::getenv(std::string{name}.c_str()) != nullptr) {
            g_set_outside.push_back(name);
        }
    });
}

const char* GameName(Perf::Game game) {
    switch (game) {
    case Perf::Game::Bananza:
        return "Donkey Kong Bananza";
    case Perf::Game::MarioKartWorld:
        return "Mario Kart World";
    case Perf::Game::Other:
        break;
    }
    return "";
}

bool SetOutside(std::string_view name) {
    for (const std::string_view outside : g_set_outside) {
        if (outside == name) {
            return true;
        }
    }
    return false;
}

Boot Prepare(QWidget* parent, u64 program_id, const QString& game, bool fullscreen,
             bool applet) {
    const Perf::Game kind = Perf::GameOf(program_id);
    auto plan = Perf::Plan(UISettings::values.nx2perf, kind, SetOutside);
    if (!g_applied) {
        for (const auto& var : plan) {
            SetVariable(var);
        }
        LOG_INFO(Frontend, "Dozer settings for {:016X}{}: {}", program_id,
                 kind == Perf::Game::Other ? "" : fmt::format(" ({})", GameName(kind)),
                 Describe(plan).toStdString());
        for (const std::string_view name : g_set_outside) {
            LOG_INFO(Frontend, "Dozer: {} was set outside the program, left as it is", name);
        }
        g_applied = Applied{std::move(plan), kind};
        return Boot::Go;
    }
    if (plan == g_applied->plan) {
        return Boot::Go;
    }
    if (applet) {
        LOG_INFO(Frontend, "Dozer: applet {:016X} runs with this session's settings: {}",
                 program_id, Describe(g_applied->plan).toStdString());
        return Boot::Go;
    }

    QString why;
    if (kind != Perf::Game::Other && kind != g_applied->game) {
        why = QObject::tr("%1's own settings are off in this session, which started with "
                          "another game.")
                  .arg(QString::fromUtf8(GameName(kind)));
    } else if (kind == Perf::Game::Other && g_applied->game != Perf::Game::Other) {
        why = QObject::tr("%1's own settings are still on in this session; they are only for "
                          "that game.")
                  .arg(QString::fromUtf8(GameName(g_applied->game)));
    } else {
        why = QObject::tr("The tuning settings changed since this session's first game.");
    }
    QMessageBox box{parent};
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QObject::tr("Tuning"));
    box.setText(QObject::tr("Restart to start this game with the right tuning settings?"));
    box.setInformativeText(why + QStringLiteral("\n\n") +
                           QObject::tr("These settings are read once per session, so the "
                                       "program restarts and starts the game."));
    QPushButton* const restart = box.addButton(QObject::tr("Restart"), QMessageBox::AcceptRole);
    QPushButton* const anyway = box.addButton(QObject::tr("Start Anyway"), QMessageBox::NoRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(restart);
    box.exec();
    if (box.clickedButton() == restart) {
        LOG_INFO(Frontend, "Dozer: restarting for {:016X}: {}", program_id,
                 Describe(plan).toStdString());
        g_restart = Restart{game, fullscreen};
        return Boot::Restart;
    }
    if (box.clickedButton() == anyway) {
        LOG_INFO(Frontend, "Dozer: {:016X} started with this session's settings: {}",
                 program_id, Describe(g_applied->plan).toStdString());
        return Boot::Go;
    }
    return Boot::Cancel;
}

bool OfferRestart(QWidget* parent, const QString& running_game, bool fullscreen) {
    std::optional<std::vector<Perf::Var>> applied;
    Perf::Game game = Perf::Game::Other;
    if (g_applied) {
        applied = g_applied->plan;
        game = g_applied->game;
    }
    const auto plan = Perf::Plan(UISettings::values.nx2perf, game, SetOutside);
    const bool pending = HasPendingGraphics();
    const bool pending_new = std::exchange(g_pending_unasked, false);
    if (!Perf::NeedsRestart(applied, plan) && !(applied && pending_new)) {
        return false;
    }
    QString text = QObject::tr("This session's game already started with the earlier "
                               "settings; the new ones take effect after a restart.");
    if (pending) {
        text += QStringLiteral("\n\n") +
                QObject::tr("Reactive Flushing and frame generation cannot change in a running "
                            "game: they are applied before the restart, or when the game stops "
                            "if you choose Later.");
    }
    if (!running_game.isEmpty()) {
        text += QStringLiteral("\n\n") +
                QObject::tr("The program opens again with the running game: unsaved game progress is "
                            "lost.");
    }
    QMessageBox box{parent};
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Tuning"));
    box.setText(QObject::tr("Restart now to apply?"));
    box.setInformativeText(text);
    QPushButton* const restart = box.addButton(QObject::tr("Restart Now"), QMessageBox::AcceptRole);
    QPushButton* const later = box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(later);
    box.exec();
    if (box.clickedButton() != restart) {
        LOG_INFO(Frontend, "Dozer: settings changed, restart later: {}", Describe(plan).toStdString());
        return false;
    }
    LOG_INFO(Frontend, "Dozer: restarting to apply: {}", Describe(plan).toStdString());
    g_restart = Restart{running_game, fullscreen};
    return true;
}

void AddPendingGraphics(const PendingGraphics& pending) {
    const PendingGraphics before = g_pending;
    if (pending.reactive_flushing) {
        g_pending.reactive_flushing = pending.reactive_flushing;
    }
    if (pending.barrier_feedback_loops) {
        g_pending.barrier_feedback_loops = pending.barrier_feedback_loops;
    }
    if (pending.frame_gen) {
        g_pending.frame_gen = pending.frame_gen;
    }
    g_pending_unasked |= before.reactive_flushing != g_pending.reactive_flushing ||
                         before.barrier_feedback_loops != g_pending.barrier_feedback_loops ||
                         before.frame_gen != g_pending.frame_gen;
}

const PendingGraphics& Pending() {
    return g_pending;
}

bool HasPendingGraphics() {
    return g_pending.reactive_flushing || g_pending.barrier_feedback_loops || g_pending.frame_gen;
}

bool ApplyPendingGraphics() {
    if (!HasPendingGraphics()) {
        return false;
    }
    if (g_pending.reactive_flushing) {
        Settings::values.use_reactive_flushing.SetValue(*g_pending.reactive_flushing);
    }
    if (g_pending.barrier_feedback_loops) {
        Settings::values.barrier_feedback_loops.SetValue(*g_pending.barrier_feedback_loops);
    }
    if (g_pending.frame_gen) {
        ApplyFrameGen(*g_pending.frame_gen);
    }
    LOG_INFO(Frontend, "Dozer: graphics settings chosen during the game applied: reactive flushing "
                       "{}, barrier feedback loops {}, frame generation {}",
             g_pending.reactive_flushing ? (*g_pending.reactive_flushing ? "on" : "off") : "-",
             g_pending.barrier_feedback_loops ? (*g_pending.barrier_feedback_loops ? "on" : "off")
                                              : "-",
             g_pending.frame_gen ? static_cast<int>(*g_pending.frame_gen) : -1);
    g_pending = {};
    g_pending_unasked = false;
    return true;
}

Perf::FrameGen CurrentFrameGen() {
    const auto& v = Settings::values;
    return Perf::FrameGenFrom({.on = v.frame_gen.GetValue(true),
                               .fsr3 = v.frame_gen_type.GetValue(true) ==
                                       Settings::FrameGenType::Fsr3,
                               .multiplier = v.frame_gen_multiplier.GetValue(true),
                               .target_rate = v.frame_gen_target_rate.GetValue(true)});
}

void ApplyFrameGen(Perf::FrameGen choice) {
    auto& v = Settings::values;
    const Perf::FrameGenSettings now =
        Perf::FrameGenApply(choice, {.on = v.frame_gen.GetValue(true),
                                     .fsr3 = v.frame_gen_type.GetValue(true) ==
                                             Settings::FrameGenType::Fsr3,
                                     .multiplier = v.frame_gen_multiplier.GetValue(true),
                                     .target_rate = v.frame_gen_target_rate.GetValue(true)});
    v.frame_gen.SetValue(now.on);
    if (now.on) {
        if (now.fsr3) {
            v.frame_gen_type.SetValue(Settings::FrameGenType::Fsr3);
        }
        v.frame_gen_multiplier.SetValue(now.multiplier);
        v.frame_gen_target_rate.SetValue(now.target_rate);
    }
}

void CancelRestart() {
    if (g_restart) {
        LOG_INFO(Frontend, "Dozer: the window stayed open, no restart");
    }
    g_restart.reset();
}

void RestartIfRequested() {
    if (!g_restart) {
        return;
    }
    QStringList arguments;
    if (g_restart->game.isEmpty()) {
        arguments = QCoreApplication::arguments().mid(1); // the same command line
    } else {
        if (g_restart->fullscreen) {
            arguments << QStringLiteral("-f");
        }
        arguments << QStringLiteral("-g") << g_restart->game;
    }
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    QProcessEnvironment environment = g_start_environment;
    // The new process waits for this one to exit before it opens the log (this one still holds
    // it, and saves the settings on its way out).
    environment.insert(QString::fromLatin1(RESTART_PID_VARIABLE),
                       QString::number(QCoreApplication::applicationPid()));
    process.setProcessEnvironment(environment);
#ifdef _WIN32
    // Qt passes bInheritHandles=TRUE: the new process would keep this one's inheritable handles
    // open (the log file, saves, game files) after this one exits.
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->inheritHandles = false; });
#endif
    if (!process.startDetached()) {
        LOG_ERROR(Frontend, "Dozer: the restart failed: {}",
                  process.errorString().toStdString());
    }
}

} // namespace Nx2PerfSession
