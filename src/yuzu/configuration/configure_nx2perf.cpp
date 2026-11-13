// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf.h"

#include <cstdlib>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringList>

#include "qt_common/config/uisettings.h"
#include "ui_configure_nx2perf.h"
#include "yuzu/nx2perf_session.h"

namespace Perf = Core::Nx2PerfSettings;

ConfigureNx2Perf::ConfigureNx2Perf(QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureNx2Perf>()} {
    ui->setupUi(this);

    using S = Perf::Switches;
    rows = {
        {ui->read_traps, &S::read_traps, {"EDEN_NX2_READ_TRAPS"}},
        {ui->split_gpu_thread, &S::split_gpu_thread, {"EDEN_NX2_BACK_STAGES", "EDEN_NX2_BACK_CUTS"}},
        {ui->acquire_peek, &S::acquire_peek, {"EDEN_NX2_ACQUIRE_PEEK"}},
        {ui->jit_same, &S::jit_same, {"EDEN_NX2_JIT_SAME"}},
        {ui->front_macro_lean, &S::front_macro_lean, {"EDEN_NX2_FRONT_MACRO_LEAN"}},
        {ui->fg_timed, &S::fg_timed, {"EDEN_NX2_FG_TIMED"}},
        {ui->osd, &S::osd, {"EDEN_NX2_OSD"}},
        {ui->dk_dyn, &S::dk_dyn, {"EDEN_NX2_DK_DYN"}},
        {ui->dk_pacing,
         &S::dk_pacing,
         {"EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE", "EDEN_NX2_PACE_SPIN_US"}},
    };

    SetConfiguration();
    MarkSetOutside();

    // The combo's order is Perf::PresetOrder (configure_nx2perf.ui), not the stored numbers.
    connect(ui->preset, &QComboBox::activated, this, [this](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= Perf::PresetOrder.size()) {
            return;
        }
        ChoosePreset(Perf::PresetOrder[static_cast<std::size_t>(index)]);
    });
    for (const Row& row : rows) {
        connect(row.box, &QCheckBox::toggled, this, [this] { BoxChanged(); });
    }
    // The item order is Perf::FrameGen's; "Other" only shows the Graphics tab's own setup.
    if (auto* model = qobject_cast<QStandardItemModel*>(ui->frame_gen->model())) {
        model->item(static_cast<int>(Perf::FrameGen::Other))->setEnabled(false);
    }
    connect(ui->frame_gen, &QComboBox::activated, this,
            [this](int index) { ChooseFrameGen(static_cast<Perf::FrameGen>(index)); });
}

ConfigureNx2Perf::~ConfigureNx2Perf() = default;

void ConfigureNx2Perf::SetReactiveFlushing(std::function<bool(bool, bool)> func) {
    reactive_flushing = std::move(func);
}

void ConfigureNx2Perf::SetGameRunning(bool running) {
    game_running = running;
}

void ConfigureNx2Perf::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNx2Perf::RetranslateUI() {
    ui->retranslateUi(this);
    ShowPreset();
    ShowFrameGen();
    MarkSetOutside();
}

void ConfigureNx2Perf::SetConfiguration() {
    Show(UISettings::values.nx2perf);
    // A choice waiting for the game to stop (Later) is shown, not the running game's setting
    const auto waiting = Nx2PerfSession::Pending().frame_gen;
    frame_gen_shown = waiting.value_or(Nx2PerfSession::CurrentFrameGen());
    ShowFrameGen();
    if (Nx2PerfSession::HasPendingGraphics()) {
        ShowPending();
    }
}

Perf::Switches ConfigureNx2Perf::Current() const {
    Perf::Switches switches{};
    for (const Row& row : rows) {
        switches.*row.field = row.box->isChecked();
    }
    switches.preset = preset;
    return switches;
}

void ConfigureNx2Perf::Show(const Perf::Switches& switches) {
    for (const Row& row : rows) {
        const QSignalBlocker blocker{row.box};
        row.box->setChecked(switches.*row.field);
    }
    preset = switches.preset;
    ShowPreset();
}

void ConfigureNx2Perf::ShowPreset() {
    {
        const QSignalBlocker blocker{ui->preset};
        ui->preset->setCurrentIndex(Perf::IndexOf(preset));
    }
    switch (preset) {
    case Perf::Preset::Balanced:
        ui->preset_text->setText(tr("The balanced settings. Foliage is rendered at a performance "
                                    "cost. Reactive Flushing off (Graphics, Advanced). Like "
                                    "start-balanced.bat."));
        break;
    case Perf::Preset::Performance:
        ui->preset_text->setText(tr("The fastest settings, with read traps and Reactive Flushing "
                                    "off. Foliage is not rendered for a performance gain. Like "
                                    "start-performance.bat."));
        break;
    case Perf::Preset::Reactive:
        ui->preset_text->setText(
            tr("The balanced settings with Reactive Flushing instead of read traps (barrier "
               "feedback loops on, RX_RT_FIT=3). Like start-reactive-experimental.bat."));
        break;
    case Perf::Preset::Safe:
        ui->preset_text->setText(tr("All settings below off, and the GPU shortcuts that are on by "
                                    "default off too (the variables start-safe.bat sets to 0). "
                                    "Frame generation off; other graphics settings such as "
                                    "Reactive Flushing stay as they are."));
        break;
    case Perf::Preset::Custom:
        ui->preset_text->setText(tr("Your own choice of the settings below."));
        break;
    }
}

void ConfigureNx2Perf::ChoosePreset(Perf::Preset chosen) {
    ui->status->clear();
    Show(Perf::FromPreset(chosen, Current()));
    if (const auto frame_gen = Perf::FrameGenFor(chosen)) {
        ChooseFrameGen(*frame_gen);
    }
    const auto reactive = Perf::ReactiveFlushing(chosen);
    if (!reactive) {
        return;
    }
    const bool barrier_on = Perf::BarrierFeedbackLoops(chosen).value_or(false);
    if (game_running) {
        pending_reactive = *reactive;
        pending_barrier = Perf::BarrierFeedbackLoops(chosen);
        ShowPending();
        return;
    }
    if (!reactive_flushing || !reactive_flushing(*reactive, barrier_on)) {
        ui->status->setText(tr("Reactive Flushing (Graphics, Advanced) can only be changed with no "
                               "game running; set it there afterwards."));
    }
}

void ConfigureNx2Perf::BoxChanged() {
    const Perf::Switches switches = Perf::AfterBoxChange(Current());
    if (switches.preset != preset) {
        preset = switches.preset;
        ShowPreset();
    }
}

void ConfigureNx2Perf::ShowFrameGen() {
    {
        const QSignalBlocker blocker{ui->frame_gen};
        ui->frame_gen->setCurrentIndex(static_cast<int>(frame_gen_shown));
    }
    QString text;
    switch (frame_gen_shown) {
    case Perf::FrameGen::Off:
        text = tr("Off.");
        break;
    case Perf::FrameGen::Fixed2x:
        text = tr("FSR3, one generated frame per real frame. On a 60 Hz screen it holds the game "
                  "at 30 fps.");
        break;
    case Perf::FrameGen::Dynamic:
        text = tr("FSR3 aiming at 120 fps, at most the screen's refresh rate: 2x to 4x, only while "
                  "the screen can show the frames and the game's own frame rate holds.");
        break;
    case Perf::FrameGen::Other:
        text = tr("Set in the Graphics tab.");
        break;
    }
    ui->frame_gen_text->setText(
        text + QLatin1Char(' ') +
        tr("Generated frames need a screen faster than the game's frame rate; on a 60 Hz screen "
           "they only help below 30 fps."));
}

void ConfigureNx2Perf::ChooseFrameGen(Perf::FrameGen chosen) {
    if (chosen == Perf::FrameGen::Other) {
        ShowFrameGen();
        return;
    }
    frame_gen_choice = chosen;
    frame_gen_shown = chosen;
    ShowFrameGen();
    if (game_running) {
        ShowPending();
    }
}

void ConfigureNx2Perf::ShowPending() {
    ui->status->setText(tr("A game is running: Reactive Flushing and frame generation cannot change "
                           "in it. They are applied when the program restarts (Restart Now) or when the "
                           "game stops."));
}

void ConfigureNx2Perf::ApplyConfiguration() {
    UISettings::values.nx2perf = Current();
    if (game_running) {
        Nx2PerfSession::AddPendingGraphics({.reactive_flushing = pending_reactive,
                                            .barrier_feedback_loops = pending_barrier,
                                            .frame_gen = frame_gen_choice});
    } else if (frame_gen_choice) {
        Nx2PerfSession::ApplyFrameGen(*frame_gen_choice);
    }
}

void ConfigureNx2Perf::MarkSetOutside() {
    for (const Row& row : rows) {
        QStringList outside;
        for (const std::string_view name : row.names) {
            if (Nx2PerfSession::SetOutside(name)) {
                // The value now: the program never sets one that was there at its start.
                const std::string key{name};
                const char* value = std::getenv(key.c_str());
                outside << QStringLiteral("%1=%2").arg(QString::fromStdString(key),
                                                       QString::fromUtf8(value ? value : ""));
            }
        }
        // All set outside: the box changes nothing, its value is theirs.
        const bool all_outside = outside.size() == static_cast<qsizetype>(row.names.size());
        row.box->setEnabled(!all_outside);
        if (outside.isEmpty()) {
            row.box->setToolTip(QString{});
        } else if (all_outside) {
            row.box->setToolTip(tr("Set outside the program (your environment, a .bat of your own "
                                   "or nx2_switches.txt), so this is used whatever the box says: "
                                   "%1")
                                    .arg(outside.join(QStringLiteral(", "))));
        } else {
            row.box->setToolTip(tr("Partly set outside the program, and that part is used: %1. "
                                   "The box sets the rest.")
                                    .arg(outside.join(QStringLiteral(", "))));
        }
    }
}
