// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <mutex>
#include <thread>
#include "input_common/monolithic_device.h"

namespace InputCommon {

// https://github.com/CTurt/3DSController/
class Port3DSController : public MonolithicDevice {
public:
    ~Port3DSController();
    void StartListening(unsigned short port);
    bool GetButtonStatus(Settings::NativeButton::Values button) const override;
    std::tuple<float, float> GetAnalogStatus(Settings::NativeAnalog::Values analog) const override;
    std::tuple<Math::Vec3<float>, Math::Vec3<float>> GetMotionStatus() const override;
    std::tuple<float, float, bool> GetTouchStatus() const override;

private:
    std::unique_ptr<std::thread> listener;
    void Listener(unsigned short port);

    struct keysPacket {
        unsigned int keys;

        struct {
            short x;
            short y;
        } circlePad;

        struct {
            unsigned short x;
            unsigned short y;
        } touch;

        struct {
            short x;
            short y;
        } cStick;
    } recv_status;

    mutable std::mutex mutex;

    struct Packet {
        struct packetHeader {
            unsigned char command;
            unsigned char keyboardActive;
        } packetHeader;

        keysPacket keys_packet;
    };
};

} // namespace InputCommon
