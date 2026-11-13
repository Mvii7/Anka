// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QWidget>

#include "core/loader/nx2_dk_quality.h"

class QCheckBox;
class QLabel;
class QSlider;

// The Bananza tab (Tuning): Donkey Kong Bananza's quality settings
// (core/loader/nx2_dk_quality.h), one row each: a box to override the game's value and a slider
// for the value. A row whose variable was set outside the program is greyed out: theirs is used.
class ConfigureBananza : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureBananza(QWidget* parent = nullptr);
    ~ConfigureBananza() override;

    void ApplyConfiguration();

private:
    struct Row {
        QCheckBox* box;
        QSlider* slider;
        QLabel* value;
    };

    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void Show(const Loader::Nx2DkQuality::Values& values);
    void ShowValue(std::size_t index);

    QLabel* intro;
    QLabel* note;
    std::array<Row, Loader::Nx2DkQuality::Count> rows{};
};
