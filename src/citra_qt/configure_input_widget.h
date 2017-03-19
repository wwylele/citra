// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <QKeyEvent>
#include <QPushButton>
#include "common/param_package.h"
#include "input_common/sdl/listener.h"

class SDLListener : public QObject {
    Q_OBJECT

signals:
    void InputDeviceParamsReceived(const QString& button_params);
};

class SDLButtonListener : public SDLListener {
    Q_OBJECT
public:
    SDLButtonListener();
    ~SDLButtonListener();

private:
    std::unique_ptr<InputCommon::SDL::Listener> listener;
};

class SDLAnalogListener : public SDLListener {
    Q_OBJECT
public:
    SDLAnalogListener();
    ~SDLAnalogListener();

private:
    std::unique_ptr<InputCommon::SDL::Listener> listener;
};

class InputDeviceSettingButton : public QPushButton {
    Q_OBJECT

public:
    explicit InputDeviceSettingButton(QWidget* parent = Q_NULLPTR);
    void SetInputDeviceParams(const std::string& params);
    std::string GetInputDeviceParams() const;

protected:
    virtual SDLListener* CreateListener() = 0;

    Common::ParamPackage param_package;
    std::unique_ptr<QTimer> timer;
    std::unique_ptr<SDLListener> sdl_listener;
    bool is_listening = false;

    void ToggleSetInputDevice();
    void BeginSetInputDevice();
    void EndSetInputDevice(const std::string& params);
    virtual void UpdateText() = 0;
    virtual QString GetWaitingText() const = 0;
};

class ButtonSetter : public InputDeviceSettingButton {
    Q_OBJECT

public:
    explicit ButtonSetter(QWidget* parent = Q_NULLPTR);

protected:
    SDLListener* CreateListener() override;
    void keyPressEvent(QKeyEvent* event) override;
    void UpdateText() override;
    QString GetWaitingText() const override;
};

class AxisAnalogSetter : public InputDeviceSettingButton {
    Q_OBJECT

public:
    explicit AxisAnalogSetter(QWidget* parent = Q_NULLPTR);

protected:
    SDLListener* CreateListener() override;
    void UpdateText() override;
    QString GetWaitingText() const override;
};

class QRadioButton;

class AnalogSetter : public QWidget {
    Q_OBJECT

public:
    explicit AnalogSetter(QWidget* parent = Q_NULLPTR);
    void SetInputDeviceParams(const std::string& params);
    std::string GetInputDeviceParams() const;

private:
    QRadioButton* radio_from_buttons;
    QRadioButton* radio_from_axes;
    QRadioButton* radio_others;

    QWidget* page_from_buttons;
    AxisAnalogSetter* page_from_axes;

    ButtonSetter* button_up;
    ButtonSetter* button_down;
    ButtonSetter* button_left;
    ButtonSetter* button_right;
    ButtonSetter* button_mod;

    Common::ParamPackage init_param_package;

    void OnPageChanged();
};