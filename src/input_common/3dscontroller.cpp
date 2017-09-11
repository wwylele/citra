// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "3dscontroller.h"
#include "input_common/3dscontroller.h"

namespace InputCommon {

static bool TestBit(unsigned int bits, int pos) {
    return (bits & (1 << pos)) != 0;
}

bool Port3DSController::GetButtonStatus(Settings::NativeButton::Values button) const {
    std::lock_guard<std::mutex> guard(mutex);
    switch (button) {
    case Settings::NativeButton::Values::A:
        return TestBit(recv_status.keys, 0);
    case Settings::NativeButton::Values::B:
        return TestBit(recv_status.keys, 1);
    case Settings::NativeButton::Values::Select:
        return TestBit(recv_status.keys, 2);
    case Settings::NativeButton::Values::Start:
        return TestBit(recv_status.keys, 3);
    case Settings::NativeButton::Values::Right:
        return TestBit(recv_status.keys, 4);
    case Settings::NativeButton::Values::Left:
        return TestBit(recv_status.keys, 5);
    case Settings::NativeButton::Values::Up:
        return TestBit(recv_status.keys, 6);
    case Settings::NativeButton::Values::Down:
        return TestBit(recv_status.keys, 7);
    case Settings::NativeButton::Values::R:
        return TestBit(recv_status.keys, 8);
    case Settings::NativeButton::Values::L:
        return TestBit(recv_status.keys, 9);
    case Settings::NativeButton::Values::X:
        return TestBit(recv_status.keys, 10);
    case Settings::NativeButton::Values::Y:
        return TestBit(recv_status.keys, 11);
    case Settings::NativeButton::Values::ZR:
        return TestBit(recv_status.keys, 14);
    case Settings::NativeButton::Values::ZL:
        return TestBit(recv_status.keys, 15);
    }
}

std::tuple<float, float> Port3DSController::GetAnalogStatus(
    Settings::NativeAnalog::Values analog) const {
    std::lock_guard<std::mutex> guard(mutex);
    if (analog == Settings::NativeAnalog::Values::CirclePad) {
        return std::make_tuple(recv_status.circlePad.x / 156.f, recv_status.circlePad.y / 156.f);
    }
    return std::make_tuple(recv_status.cStick.x / 156.f, recv_status.cStick.y / 156.f);
}

std::tuple<Math::Vec3<float>, Math::Vec3<float>> Port3DSController::GetMotionStatus() const {
    return {};
}

std::tuple<float, float, bool> Port3DSController::GetTouchStatus() const {
    std::lock_guard<std::mutex> guard(mutex);
    return std::make_tuple(recv_status.touch.x / 320.f, recv_status.touch.y / 240.f,
                           TestBit(recv_status.keys, 20));
}

void Port3DSController::StartListening(unsigned short port) {
    listener = std::make_unique<std::thread>(&Port3DSController::Listener, this, port);
}

void Port3DSController::Listener(unsigned short port) {
    int soc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(soc, (sockaddr*)&addr, sizeof(addr)) < 0)
        perror("bind");
    Packet packet;
    // TODO: cleanup
    while (true) {
        socklen_t d;
        int k = recvfrom(soc, &packet, sizeof(packet), 0, (sockaddr*)&addr, &d);
        if (k > 0) {
            if (packet.packetHeader.command == 0) {
                LOG_CRITICAL(Input, "Connected!");
            } else {
                std::lock_guard<std::mutex> guard(mutex);
                recv_status = packet.keys_packet;
            }
        }
    }
}

Port3DSController::~Port3DSController() {
    listener->detach();
}

} // namespace InputCommon
