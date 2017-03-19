// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>
#include "citra_qt/configure_input_widget.h"
#include "common/param_package.h"
#include "input_common/main.h"

SDLButtonListener::SDLButtonListener()
    : listener(new InputCommon::SDL::Listener(
          [this](const std::string& button_params) {
              emit InputDeviceParamsReceived(QString::fromStdString(button_params));
          },
          [](const std::string& analog_params) {

          })) {}

SDLButtonListener::~SDLButtonListener() {
    // ensure the listener is destructed before the signal-slot framework get destructed
    listener = nullptr;
}

SDLAnalogListener::SDLAnalogListener()
    : listener(new InputCommon::SDL::Listener(
          [](const std::string& button_params) {

          },
          [this](const std::string& analog_params) {
              emit InputDeviceParamsReceived(QString::fromStdString(analog_params));
          })) {}

SDLAnalogListener::~SDLAnalogListener() {
    // ensure the listener is destructed before the signal-slot framework get destructed
    listener = nullptr;
}

static QString getKeyName(int key_code) {
    switch (key_code) {
    case Qt::Key_Shift:
        return QObject::tr("Shift");
    case Qt::Key_Control:
        return QObject::tr("Ctrl");
    case Qt::Key_Alt:
        return QObject::tr("Alt");
    case Qt::Key_Meta:
        return "";
    default:
        return QKeySequence(key_code).toString();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////

InputDeviceSettingButton::InputDeviceSettingButton(QWidget* parent)
    : QPushButton(parent), timer(new QTimer()) {
    connect(this, &QPushButton::released, this, &InputDeviceSettingButton::ToggleSetInputDevice);
    connect(timer.get(), &QTimer::timeout, [=]() { EndSetInputDevice(""); });
    timer->setSingleShot(true);
}

void InputDeviceSettingButton::SetInputDeviceParams(const std::string& params) {
    param_package = Common::ParamPackage(params);
    UpdateText();
}

std::string InputDeviceSettingButton::GetInputDeviceParams() const {
    return param_package.Serialize();
}

void InputDeviceSettingButton::ToggleSetInputDevice() {
    if (is_listening) {
        EndSetInputDevice("");
    } else {
        BeginSetInputDevice();
    }
}

void InputDeviceSettingButton::BeginSetInputDevice() {
    setText(GetWaitingText());
    setFocus();

    sdl_listener = std::unique_ptr<SDLListener>(CreateListener());
    connect(sdl_listener.get(), &SDLListener::InputDeviceParamsReceived, this,
            [=](const QString& params) { EndSetInputDevice(params.toStdString()); },
            Qt::QueuedConnection);

    timer->start(5000);

    grabKeyboard();
    grabMouse();
    is_listening = true;
}

void InputDeviceSettingButton::EndSetInputDevice(const std::string& params) {
    releaseKeyboard();
    releaseMouse();
    is_listening = false;

    timer->stop();

    sdl_listener = nullptr;

    if (params != "") {
        SetInputDeviceParams(params);
    } else {
        UpdateText();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////
ButtonSetter::ButtonSetter(QWidget* parent) : InputDeviceSettingButton(parent) {}

SDLListener* ButtonSetter::CreateListener() {
    return new SDLButtonListener;
}

void ButtonSetter::UpdateText() {
    if (param_package.Get("engine", "") == "keyboard") {
        setText(getKeyName(param_package.Get("code", 0)));
    } else if (param_package.Get("engine", "") == "sdl") {
        QString joystick = QString::fromStdString(param_package.Get("joystick", ""));
        if (param_package.Has("hat")) {
            setText(QString("j%1/h%2/%3")
                        .arg(joystick, QString::fromStdString(param_package.Get("hat", "")),
                             QString::fromStdString(param_package.Get("direction", ""))));
        } else {
            setText(QString("j%1/b%2").arg(
                joystick, QString::fromStdString(param_package.Get("button", ""))));
        }
    } else {
        setText(tr("[unknown]"));
    }
}

void ButtonSetter::keyPressEvent(QKeyEvent* event) {
    if (is_listening) {
        EndSetInputDevice(InputCommon::GenerateKeyboardParam(event->key()));
    }
}

QString ButtonSetter::GetWaitingText() const {
    return tr("[press key]");
}

////////////////////////////////////////////////////////////////////////////////////////////////
AxisAnalogSetter::AxisAnalogSetter(QWidget* parent) : InputDeviceSettingButton(parent) {}

SDLListener* AxisAnalogSetter::CreateListener() {
    return new SDLAnalogListener;
}

void AxisAnalogSetter::UpdateText() {
    if (param_package.Get("engine", "") == "sdl") {
        setText(QString("j%1/%2,%3")
                    .arg(QString::fromStdString(param_package.Get("joystick", "")),
                         QString::fromStdString(param_package.Get("axis_x", "")),
                         QString::fromStdString(param_package.Get("axis_y", ""))));
    } else {
        setText(tr("[unknown]"));
    }
}

QString AxisAnalogSetter::GetWaitingText() const {
    return tr("[move stick]");
}

////////////////////////////////////////////////////////////////////////////////////////////////

AnalogSetter::AnalogSetter(QWidget* parent) : QWidget(parent) {
    QVBoxLayout* main_layout = new QVBoxLayout();
    QHBoxLayout* radio_layout = new QHBoxLayout();
    main_layout->addLayout(radio_layout);
    radio_from_buttons = new QRadioButton(tr("buttons"));
    radio_from_axes = new QRadioButton(tr("gamepad"));
    radio_others = new QRadioButton(tr("others"));
    radio_layout->addWidget(radio_from_buttons);
    radio_layout->addWidget(radio_from_axes);
    radio_layout->addWidget(radio_others);

    page_from_buttons = new QWidget();
    QGridLayout* layout_from_buttons = new QGridLayout();
    button_up = new ButtonSetter();
    button_down = new ButtonSetter();
    button_left = new ButtonSetter();
    button_right = new ButtonSetter();
    button_mod = new ButtonSetter();
    layout_from_buttons->addWidget(button_up, 0, 1);
    layout_from_buttons->addWidget(button_down, 2, 1);
    layout_from_buttons->addWidget(button_left, 1, 0);
    layout_from_buttons->addWidget(button_right, 1, 2);
    layout_from_buttons->addWidget(button_mod, 1, 1);
    page_from_buttons->setLayout(layout_from_buttons);
    page_from_buttons->setVisible(false);

    page_from_axes = new AxisAnalogSetter();
    page_from_axes->setVisible(false);

    main_layout->addWidget(page_from_buttons);
    main_layout->addWidget(page_from_axes);

    setLayout(main_layout);

    connect(radio_from_buttons, &QRadioButton::toggled, this, &AnalogSetter::OnPageChanged);
    connect(radio_from_axes, &QRadioButton::toggled, this, &AnalogSetter::OnPageChanged);
    connect(radio_others, &QRadioButton::toggled, this, &AnalogSetter::OnPageChanged);
}

void AnalogSetter::SetInputDeviceParams(const std::string& params) {
    init_param_package = Common::ParamPackage(params);
    if (init_param_package.Get("engine", "") == "analog_from_button") {
        radio_from_buttons->setChecked(true);
        button_up->SetInputDeviceParams(init_param_package.Get("up", ""));
        button_down->SetInputDeviceParams(init_param_package.Get("down", ""));
        button_left->SetInputDeviceParams(init_param_package.Get("left", ""));
        button_right->SetInputDeviceParams(init_param_package.Get("right", ""));
        button_mod->SetInputDeviceParams(init_param_package.Get("modifier", ""));
    } else if (init_param_package.Get("engine", "") == "sdl") {
        radio_from_axes->setChecked(true);
        page_from_axes->SetInputDeviceParams(params);
    } else {
        radio_others->setChecked(true);
    }
}

std::string AnalogSetter::GetInputDeviceParams() const {
    if (radio_from_buttons->isChecked()) {
        return Common::ParamPackage{
            {"engine", "analog_from_button"},
            {"up", button_up->GetInputDeviceParams()},
            {"down", button_down->GetInputDeviceParams()},
            {"left", button_left->GetInputDeviceParams()},
            {"right", button_right->GetInputDeviceParams()},
            {"modifier", button_mod->GetInputDeviceParams()},
            {"modifier_scale", "0.5"},
        }.Serialize();
    } else if (radio_from_axes->isChecked()) {
        return page_from_axes->GetInputDeviceParams();
    }

    return init_param_package.Serialize();
}

void AnalogSetter::OnPageChanged() {
    page_from_buttons->setVisible(radio_from_buttons->isChecked());
    page_from_axes->setVisible(radio_from_axes->isChecked());
}