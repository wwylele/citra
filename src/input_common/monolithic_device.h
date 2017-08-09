// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once
#include "core/frontend/input.h"
#include "core/settings.h"

namespace InputCommon {

class MonolithicDevice {
public:
    virtual bool GetButtonStatus(Settings::NativeButton::Values button) const = 0;
    virtual std::tuple<float, float> GetAnalogStatus(
        Settings::NativeAnalog::Values analog) const = 0;
    virtual std::tuple<Math::Vec3<float>, Math::Vec3<float>> GetMotionStatus() const = 0;
    virtual std::tuple<float, float, bool> GetTouchStatus() const = 0;
    virtual ~MonolithicDevice();
};

void RegisterMonolithicDevice(const std::string& name, std::shared_ptr<MonolithicDevice> device);
void UnregisterMonolithicDevice(const std::string& name);

} // namespace InputCommon