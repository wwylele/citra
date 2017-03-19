// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>
#include <utility>
#include <QTimer>
#include "citra_qt/config.h"
#include "citra_qt/configure_input.h"
#include "common/param_package.h"
#include "input_common/main.h"

ConfigureInput::ConfigureInput(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInput>()) {

    ui->setupUi(this);
    setFocusPolicy(Qt::ClickFocus);

    button_setters = {
        ui->buttonA,        ui->buttonB,        ui->buttonX,         ui->buttonY,  ui->buttonDpadUp,
        ui->buttonDpadDown, ui->buttonDpadLeft, ui->buttonDpadRight, ui->buttonL,  ui->buttonR,
        ui->buttonStart,    ui->buttonSelect,   ui->buttonZL,        ui->buttonZR, ui->buttonHome,
    };

    analog_setters = {ui->analogCirclePad, ui->analogCStick};

    connect(ui->buttonRestoreDefaults, &QPushButton::released, [this]() { restoreDefaults(); });

    this->loadConfiguration();

    // TODO(wwylele): enable these when the input emulation for them is
    // implemented
    ui->buttonZL->setEnabled(false);
    ui->buttonZR->setEnabled(false);
    ui->buttonHome->setEnabled(false);
}

void ConfigureInput::applyConfiguration() {
    std::transform(button_setters.begin(), button_setters.end(), Settings::values.buttons.begin(),
                   [](const ButtonSetter* button) { return button->GetInputDeviceParams(); });
    std::transform(analog_setters.begin(), analog_setters.end(), Settings::values.analogs.begin(),
                   [](const AnalogSetter* analog) { return analog->GetInputDeviceParams(); });

    Settings::Apply();
}

void ConfigureInput::loadConfiguration() {
    for (size_t i = 0; i < button_setters.size(); ++i) {
        button_setters[i]->SetInputDeviceParams(Settings::values.buttons[i]);
    }
    for (size_t i = 0; i < analog_setters.size(); ++i) {
        analog_setters[i]->SetInputDeviceParams(Settings::values.analogs[i]);
    }
}

void ConfigureInput::restoreDefaults() {
    for (size_t i = 0; i < button_setters.size(); ++i) {
        button_setters[i]->SetInputDeviceParams(
            InputCommon::GenerateKeyboardParam(Config::default_buttons[i]));
    }

    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; i++) {
        analog_setters[i]->SetInputDeviceParams(InputCommon::GenerateAnalogParamFromKeys(
            Config::default_analogs[i][0], Config::default_analogs[i][1],
            Config::default_analogs[i][2], Config::default_analogs[i][3],
            Config::default_analogs[i][4], 0.5));
    }
    applyConfiguration();
}
