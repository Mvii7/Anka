// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_bananza.h"

#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

#include "qt_common/config/uisettings.h"
#include "yuzu/nx2perf_session.h"

namespace Quality = Loader::Nx2DkQuality;

ConfigureBananza::ConfigureBananza(QWidget* parent) : QWidget(parent) {
    auto* const layout = new QGridLayout(this);
    intro = new QLabel(this);
    intro->setWordWrap(true);
    layout->addWidget(intro, 0, 0, 1, 3);

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Quality::Knob& knob = Quality::Knobs[i];
        Row& row = rows[i];
        row.box = new QCheckBox(this);
        row.slider = new QSlider(Qt::Horizontal, this);
        row.slider->setRange(knob.min, knob.max);
        row.slider->setValue(knob.game);
        row.value = new QLabel(this);
        row.value->setMinimumWidth(
            row.value->fontMetrics().horizontalAdvance(QStringLiteral("000 frames (game)")));
        const int line = static_cast<int>(i) + 1;
        layout->addWidget(row.box, line, 0);
        layout->addWidget(row.slider, line, 1);
        layout->addWidget(row.value, line, 2);
        connect(row.box, &QCheckBox::toggled, this, [this, i] { ShowValue(i); });
        connect(row.slider, &QSlider::valueChanged, this, [this, i] { ShowValue(i); });
    }

    const int after = static_cast<int>(rows.size()) + 1;
    auto* const reset = new QPushButton(this);
    reset->setObjectName(QStringLiteral("reset"));
    layout->addWidget(reset, after, 0);
    connect(reset, &QPushButton::clicked, this, [this] { Show(Quality::Values{}); });
    note = new QLabel(this);
    note->setWordWrap(true);
    layout->addWidget(note, after + 1, 0, 1, 3);
    layout->setRowStretch(after + 2, 1);
    layout->setColumnStretch(1, 1);

    RetranslateUI();
    Show(UISettings::values.nx2dkq);
}

ConfigureBananza::~ConfigureBananza() = default;

void ConfigureBananza::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureBananza::RetranslateUI() {
    setAccessibleName(tr("Bananza"));
    intro->setText(tr("Donkey Kong Bananza only, experimental. A ticked row replaces a value of "
                      "the game by the slider's; an unticked row leaves the game's own. Read when "
                      "the game starts: a change needs a restart of the program."));
    note->setText(tr("Lower values are faster, except the cube map interval, where higher is "
                     "faster. The terrain's own resolution is fixed by the game's data and has "
                     "no setting."));
    findChild<QPushButton*>(QStringLiteral("reset"))->setText(tr("Reset to the Game's"));

    const std::array<std::pair<QString, QString>, Quality::Count> texts{{
        {tr("Best render resolution"),
         tr("The highest resolution the game renders at before its upscaler, per axis, in "
            "percent of the output (2880x1620 docked). The game's own is 73% (2095x1179).")},
        {tr("Worst render resolution"),
         tr("The lowest resolution the game's resolution control may drop to. Set equal to the "
            "best one for a fixed resolution.")},
        {tr("Resolution control: GPU load target"),
         tr("The game lowers its resolution when its measured GPU load goes above this. Lower: "
            "it drops sooner.")},
        {tr("Frame buffer minimum scale"),
         tr("The lowest scale of the game's second resolution control (frame buffer scale).")},
        {tr("Bloom buffer scale"), tr("The bloom buffers' size relative to the frame.")},
        {tr("Cube map update interval"),
         tr("Frames between two captures of the reflection cube maps. Higher is faster; "
            "reflections follow changes later.")},
        {tr("Radial blur render scale"),
         tr("The radial blur's buffer size relative to the frame.")},
        {tr("Radial blur samples"), tr("Samples the radial blur takes per pixel.")},
        {tr("Terrain LOD distance"),
         tr("Scales the two distances at which the destructible terrain switches to its "
            "coarser meshes. Lower: coarser terrain closer to the camera, fewer triangles.")},
        {tr("Terrain LOD value"),
         tr("Scales the two values the terrain's LOD steps carry beside their distance (how "
            "coarse a step is). What higher or lower does is not known yet: for testing.")},
    }};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].box->setText(texts[i].first);
        QString tip = texts[i].second;
        const auto env = Quality::Knobs[i].env;
        const bool outside = Nx2PerfSession::SetOutside(env);
        if (outside) {
            tip += QLatin1Char(' ') +
                   tr("Set outside the program (%1), so that value is used whatever this row says.")
                       .arg(QString::fromUtf8(env.data(), static_cast<qsizetype>(env.size())));
        }
        rows[i].box->setToolTip(tip);
        rows[i].slider->setToolTip(tip);
        rows[i].box->setEnabled(!outside);
        ShowValue(i);
    }
}

void ConfigureBananza::Show(const Quality::Values& values) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const QSignalBlocker box_blocker{rows[i].box};
        const QSignalBlocker slider_blocker{rows[i].slider};
        rows[i].box->setChecked(values[i] != 0);
        rows[i].slider->setValue(values[i] != 0 ? values[i] : Quality::Knobs[i].game);
        ShowValue(i);
    }
}

void ConfigureBananza::ShowValue(std::size_t index) {
    const Row& row = rows[index];
    const bool on = row.box->isChecked();
    row.slider->setEnabled(on && row.box->isEnabled());
    const int value = on ? row.slider->value() : Quality::Knobs[index].game;
    QString text;
    switch (static_cast<Quality::Id>(index)) {
    case Quality::CubeMapInterval:
        text = tr("%n frame(s)", nullptr, value);
        break;
    case Quality::RadialBlurSamples:
        text = QString::number(value);
        break;
    default:
        text = tr("%1%").arg(value);
        break;
    }
    row.value->setText(on ? text : tr("%1 (game)").arg(text));
}

void ConfigureBananza::ApplyConfiguration() {
    Quality::Values values{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        values[i] = rows[i].box->isChecked() ? rows[i].slider->value() : 0;
    }
    UISettings::values.nx2dkq = values;
}
