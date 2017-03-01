// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/bit_field.h"
#include "common/string_util.h"
#include "core/core_timing.h"
#include "core/hle/service/ir/extra_hid.h"
#include "core/settings.h"

namespace Service {
namespace IR {

ExtraHID::ExtraHID(SendFunc send_func) : IRDevice(send_func) {
    LoadInputDevices();

    // The data below is got from a New 3DS
    calibration_data = std::array<u8, 0x40>{{
        // 0x00
        0x00, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,
        // 0x08
        0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0xF5,
        // 0x10
        0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,
        // 0x18
        0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
        // 0x20
        0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,
        // 0x28
        0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
        // 0x30
        0xFF, 0x00, 0x08, 0x80, 0x85, 0xEB, 0x11, 0x3F,
        // 0x38
        0x85, 0xEB, 0x11, 0x3F, 0xFF, 0xFF, 0xFF, 0x65,
    }};

    send_callback = CoreTiming::RegisterEvent(
        "ExtraHID::SendHIDStatus", [this](u64, int cycles_late) { SendHIDStatus(cycles_late); });
}

ExtraHID::~ExtraHID() {
    Disconnect();
}

void ExtraHID::Connect() {}

void ExtraHID::Disconnect() {
    CoreTiming::UnscheduleEvent(send_callback, 0);
}

void ExtraHID::HandleReadHIDStatusRequest(const std::vector<u8>& data) {
    if (data.size() != 3) {
        LOG_ERROR(Service_IR, "Wrong request size (%zu): %s", data.size(),
                  Common::ArrayToString(data.data(), data.size()).c_str());
        return;
    }

    CoreTiming::UnscheduleEvent(send_callback, 0);
    hid_period = data[1];
    CoreTiming::ScheduleEvent(msToCycles(hid_period), send_callback);
}

void ExtraHID::HandleReadCalibrationDataRequest(const std::vector<u8>& data) {
    if (data.size() != 6) {
        LOG_ERROR(Service_IR, "Wrong request size (%zu): %s", data.size(),
                  Common::ArrayToString(data.data(), data.size()).c_str());
        return;
    }

    u16 offset;
    u16 size;
    std::memcpy(&offset, data.data() + 2, sizeof(u16));
    std::memcpy(&size, data.data() + 4, sizeof(u16));

    // align
    offset = Common::AlignDown(offset, 16);
    size = Common::AlignDown(size, 16);

    if (offset + size > calibration_data.size()) {
        LOG_ERROR(Service_IR, "Read beyond the end of calibration data! (offset=%u, size=%u)",
                  offset, size);
        return;
    }

    std::vector<u8> response;
    response.push_back(0x11); // Fixed header value
    // Copies offset and size from request
    response.insert(response.end(), data.begin() + 2, data.begin() + 6);
    // Copies the data
    response.insert(response.end(), calibration_data.begin() + offset,
                    calibration_data.begin() + offset + size);
    send_func(response);
}

void ExtraHID::Receive(const std::vector<u8>& data) {
    switch (data[0]) {
    case 1:
        HandleReadHIDStatusRequest(data);
        break;
    case 2:
        HandleReadCalibrationDataRequest(data);
        break;
    default:
        LOG_ERROR(Service_IR, "Unknown request: %s",
                  Common::ArrayToString(data.data(), data.size()).c_str());
        break;
    }
}

void ExtraHID::SendHIDStatus(int cycles_late) {
    if (is_device_reload_pending.exchange(false))
        LoadInputDevices();

    struct {
        union {
            BitField<0, 8, u32> header;
            BitField<8, 12, u32> c_stick_x;
            BitField<20, 12, u32> c_stick_y;
        } c_stick;
        union {
            BitField<0, 5, u8> battery;
            BitField<5, 1, u8> zl;
            BitField<6, 1, u8> zr;
            BitField<7, 1, u8> r;
        } buttons;
        u8 unknown;
    } response;
    static_assert(sizeof(response) == 6, "HID status response has wrong size!");

    response.c_stick.header.Assign(0x10); // Fixed header value
    float x, y;
    std::tie(x, y) = c_stick->GetStatus();
    response.c_stick.c_stick_x.Assign(0x800 + 0x7FF * x);
    response.c_stick.c_stick_y.Assign(0x800 + 0x7FF * y);
    response.buttons.battery.Assign(0x1F);
    // Note: for buttons, the bit is set when the button is NOT pressed
    response.buttons.zl.Assign(!zl->GetStatus());
    response.buttons.zr.Assign(!zr->GetStatus());
    response.buttons.r.Assign(1);
    response.unknown = 0;

    std::vector<u8> response_buffer(sizeof(response));
    memcpy(response_buffer.data(), &response, sizeof(response));
    send_func(response_buffer);

    CoreTiming::ScheduleEvent(msToCycles(hid_period) - cycles_late, send_callback);
}

void ExtraHID::ReloadInputDevices() {
    is_device_reload_pending.store(true);
}

void ExtraHID::LoadInputDevices() {
    zl = Input::CreateDevice<Input::ButtonDevice>(
        Settings::values.buttons[Settings::NativeButton::ZL]);
    zr = Input::CreateDevice<Input::ButtonDevice>(
        Settings::values.buttons[Settings::NativeButton::ZR]);
    c_stick = Input::CreateDevice<Input::AnalogDevice>(
        Settings::values.analogs[Settings::NativeAnalog::CStick]);
}

} // namespace IR
} // namespace Service
