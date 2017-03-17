// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <QWidget>
#include "citra_qt/configure_input_widget.h"
#include "core/settings.h"
#include "ui_configure_input.h"

namespace Ui {
class ConfigureInput;
}

class ConfigureInput : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInput(QWidget* parent = nullptr);

    /// Save all button configurations to settings file
    void applyConfiguration();

private:
    std::unique_ptr<Ui::ConfigureInput> ui;

    /// Each button input is represented by a QPushButton.
    std::array<ButtonSetter*, Settings::NativeButton::NumButtons> button_setters;
    std::array<AnalogSetter*, Settings::NativeAnalog::NumAnalogs> analog_setters;

    /// Load configuration settings.
    void loadConfiguration();
    /// Restore all buttons to their default values.
    void restoreDefaults();
};
