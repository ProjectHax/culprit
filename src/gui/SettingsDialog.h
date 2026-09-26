// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ProjectHax LLC

#pragma once

#include "core/Settings.h"
#include "gui/widgets/WindowChrome.h"

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace culprit {

class SettingsDialog : public ChromeDialog {
    Q_OBJECT
public:
    SettingsDialog(const Settings& s, double detectedTjmax, QWidget* parent = nullptr);
    Settings settings() const;

private:
    void load(const Settings& s);

    Settings base_;
    QComboBox* theme_;
    QComboBox* titleBar_;
    QSpinBox* interval_;
    QSpinBox* hotCount_;
    QComboBox* probeMode_;
    QSpinBox* probes_;
    QDoubleSpinBox* floatThr_;
    QDoubleSpinBox* cpuThr_;
    QDoubleSpinBox* rtThr_;
    QDoubleSpinBox* tjmax_;
    QDoubleSpinBox* highCpu_;
    QSpinBox* sustain_;
    QDoubleSpinBox* tempWarn_;
    QSpinBox* raise_;
    QSpinBox* clear_;
    QLineEdit* recDir_;
};

} // namespace culprit
