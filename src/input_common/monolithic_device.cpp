// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "core/frontend/input.h"
#include "core/settings.h"
#include "input_common/monolithic_device.h"

namespace InputCommon {

MonolithicDevice::~MonolithicDevice() = default;

class MonolithicFactoryBase {
public:
    MonolithicFactoryBase(std::shared_ptr<MonolithicDevice> device) : device(device) {}

    template <typename DeviceT>
    std::unique_ptr<DeviceT> CreateT(const Common::ParamPackage& param);

private:
    std::shared_ptr<MonolithicDevice> device;
};

template <>
std::unique_ptr<Input::ButtonDevice> MonolithicFactoryBase::CreateT(
    const Common::ParamPackage& param) {
    using namespace Settings::NativeButton;
    class Button : public Input::ButtonDevice {
        std::weak_ptr<MonolithicDevice> device;
        Values button;

    public:
        Button(std::weak_ptr<MonolithicDevice>&& device, Values button)
            : device(device), button(button) {}
        bool GetStatus() const override {
            if (auto locked = device.lock()) {
                return locked->GetButtonStatus(button);
            }
            return false;
        }
    };

    Values button = static_cast<Values>(std::distance(
        mapping.begin(), std::find(mapping.begin(), mapping.end(), param.Get("_", ""))));
    return std::make_unique<Button>(device, button);
}

template <>
std::unique_ptr<Input::AnalogDevice> MonolithicFactoryBase::CreateT(
    const Common::ParamPackage& param) {
    using namespace Settings::NativeAnalog;
    class Analog : public Input::AnalogDevice {
        std::weak_ptr<MonolithicDevice> device;
        Values analog;

    public:
        Analog(std::weak_ptr<MonolithicDevice>&& device, Values button)
            : device(device), analog(analog) {}
        std::tuple<float, float> GetStatus() const override {
            if (auto locked = device.lock()) {
                return locked->GetAnalogStatus(analog);
            }
            return {};
        }
    };

    Values analog = static_cast<Values>(std::distance(
        mapping.begin(), std::find(mapping.begin(), mapping.end(), param.Get("_", ""))));
    return std::make_unique<Analog>(device, analog);
}

template <>
std::unique_ptr<Input::MotionDevice> MonolithicFactoryBase::CreateT(const Common::ParamPackage&) {
    class Motion : public Input::MotionDevice {
        std::weak_ptr<MonolithicDevice> device;

    public:
        Motion(std::weak_ptr<MonolithicDevice>&& device) : device(device) {}
        std::tuple<Math::Vec3<float>, Math::Vec3<float>> GetStatus() const override {
            if (auto locked = device.lock()) {
                return locked->GetMotionStatus();
            }
            return {};
        }
    };
    return std::make_unique<Motion>(device);
}

template <>
std::unique_ptr<Input::TouchDevice> MonolithicFactoryBase::CreateT(const Common::ParamPackage&) {
    class Touch : public Input::TouchDevice {
        std::weak_ptr<MonolithicDevice> device;

    public:
        Touch(std::weak_ptr<MonolithicDevice>&& device) : device(device) {}
        std::tuple<float, float, bool> GetStatus() const override {
            if (auto locked = device.lock()) {
                return locked->GetTouchStatus();
            }
            return {};
        }
    };
    return std::make_unique<Touch>(device);
}

template <typename DeviceT>
class MonolithicFactoryT : public Input::Factory<DeviceT>, protected virtual MonolithicFactoryBase {
public:
    MonolithicFactoryT() : MonolithicFactoryBase(nullptr) {}
    std::unique_ptr<DeviceT> Create(const Common::ParamPackage& param) override {
        return CreateT<DeviceT>(param);
    }
};

template <typename... DeviceT>
class MonolithicFactory : public MonolithicFactoryT<DeviceT>... {
public:
    MonolithicFactory(std::shared_ptr<MonolithicDevice> device) : MonolithicFactoryBase(device) {}
};

using MonolithicFactoryFinal = MonolithicFactory<Input::ButtonDevice, Input::AnalogDevice,
                                                 Input::MotionDevice, Input::TouchDevice>;

void RegisterMonolithicDevice(const std::string& name, std::shared_ptr<MonolithicDevice> device) {
    auto factory = std::make_shared<MonolithicFactoryFinal>(device);
    Input::RegisterFactory<Input::ButtonDevice>(name, factory);
    Input::RegisterFactory<Input::AnalogDevice>(name, factory);
    Input::RegisterFactory<Input::MotionDevice>(name, factory);
    Input::RegisterFactory<Input::TouchDevice>(name, factory);
}

void UnregisterMonolithicDevice(const std::string& name) {
    Input::UnregisterFactory<Input::ButtonDevice>(name);
    Input::UnregisterFactory<Input::AnalogDevice>(name);
    Input::UnregisterFactory<Input::MotionDevice>(name);
    Input::UnregisterFactory<Input::TouchDevice>(name);
}

} // namespace InputCommon