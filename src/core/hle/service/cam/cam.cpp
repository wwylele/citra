// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <future>
#include <memory>
#include "common/camera/factory.h"
#include "common/logging/log.h"
#include "core/core_timing.h"
#include "core/hle/kernel/event.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/cam/cam_c.h"
#include "core/hle/service/cam/cam_q.h"
#include "core/hle/service/cam/cam_s.h"
#include "core/hle/service/cam/cam_u.h"
#include "core/hle/service/service.h"
#include "core/settings.h"

namespace Service {
namespace CAM {

struct ContextConfig {
    Flip flip;
    Effect effect;
    OutputFormat format;
    Resolution resolution;
};

struct CameraConfig {
    std::unique_ptr<Camera::CameraInterface> impl;
    std::array<ContextConfig, 2> contexts;
    int current_context;
    FrameRate frame_rate;
};

struct PortConfig {
    int camera_id;

    bool is_active;            // set when the port is activated by an Activate call.
    bool is_pending_receiving; // set if SetReceiving is called when is_busy = false. When
                               // StartCapture is called then, this will trigger an receiving
                               // process and reset itself.
    bool is_busy;      // set when StartCapture is called and reset when StopCapture is called.
    bool is_receiving; // set when there is an ongoing receiving process.

    // trimming related
    bool is_trimming;
    u16 x0;
    u16 y0;
    u16 x1;
    u16 y1;

    u32 transfer_bytes;

    Kernel::SharedPtr<Kernel::Event> completion_event;
    Kernel::SharedPtr<Kernel::Event> buffer_error_interrupt_event;
    Kernel::SharedPtr<Kernel::Event> vsync_interrupt_event;

    std::future<std::vector<u16>> capture_result; // will hold the received frame.
    VAddr dest;                                   // the destination address of a receiving process
    u32 dest_size;                                // the destination size of a receiving process
};

// built-in resolution parameters
static constexpr std::array<Resolution, 8> RESOLUTION_LIBRARY{{
    {640, 480, 0, 0, 639, 479},  // VGA
    {320, 240, 0, 0, 639, 479},  // QVGA
    {160, 120, 0, 0, 639, 479},  // QQVGA
    {352, 288, 26, 0, 613, 479}, // CIF
    {176, 144, 26, 0, 613, 479}, // QCIF
    {256, 192, 0, 0, 639, 479},  // DS_LCD
    {512, 384, 0, 0, 639, 479},  // DS_LCDx4
    {400, 240, 0, 48, 639, 431}, // CTR_TOP_LCD
}};

// latency in ms for each frame rate option
static constexpr std::array<int, 13> LATENCY_BY_FRAME_RATE{{
    67,  // Rate_15
    67,  // Rate_15_To_5
    67,  // Rate_15_To_2
    100, // Rate_10
    118, // Rate_8_5
    200, // Rate_5
    50,  // Rate_20
    50,  // Rate_20_To_5
    33,  // Rate_30
    33,  // Rate_30_To_5
    67,  // Rate_15_To_10
    50,  // Rate_20_To_10
    33,  // Rate_30_To_10
}};

static std::array<CameraConfig, 3> cameras;
static std::array<PortConfig, 2> ports;
static int completion_event_callback;

static const ResultCode ERROR_INVALID_ENUM_VALUE(ErrorDescription::InvalidEnumValue,
                                                 ErrorModule::CAM, ErrorSummary::InvalidArgument,
                                                 ErrorLevel::Usage);
static const ResultCode ERROR_OUT_OF_RANGE(ErrorDescription::OutOfRange, ErrorModule::CAM,
                                           ErrorSummary::InvalidArgument, ErrorLevel::Usage);

// transforms a "*_select" parameter to an ID list
static std::vector<int> BitSetToIDs(u32 bit_set) {
    static const std::array<std::vector<int>, 8> SET_TO_ID{{
        {}, {0}, {1}, {0, 1}, {2}, {0, 2}, {1, 2}, {0, 1, 2},
    }};
    return SET_TO_ID[bit_set];
}

// verifies a "*_select" parameter is in a valid range
static bool VerifyBitSet(u32 bit_set, unsigned count) {
    return bit_set < (1 << count);
}

// verifies a "*_select" parameter represents an unique ID
static bool VerifyBitSetUnique(u32 bit_set, unsigned count) {
    return bit_set < (1 << count) && bit_set != 0 && !(bit_set & (bit_set - 1));
}

static void CompletionEventCallBack(u64 port_id, int) {
    auto& port = ports[port_id];
    auto& camera = cameras[port.camera_id];
    auto buffer = port.capture_result.get();

    if (port.is_trimming) {
        u32 trim_size = (port.x1 - port.x0) * (port.y1 - port.y0) * 2;
        if (port.dest_size != trim_size) {
            LOG_ERROR(Service_CAM, "The destination size (%d) doesn't match the source (%d)!",
                      port.dest_size, trim_size);
        }

        VAddr dest_ptr = port.dest;
        int dest_size = port.dest_size;
        int trim_width = port.x1 - port.x0;
        int line_bytes = trim_width * sizeof(u16);
        int trim_height = port.y1 - port.y0;
        int original_width = camera.contexts[camera.current_context].resolution.width;
        int src_offset = port.y0 * original_width + port.x0;
        const u16* src_ptr = buffer.data() + src_offset;
        int src_size = (buffer.size() - src_offset) * sizeof(u16);

        for (int y = 0; y < trim_height; ++y) {
            int copy_length = std::min({line_bytes, dest_size, src_size});
            if (copy_length <= 0) {
                break;
            }
            Memory::WriteBlock(dest_ptr, src_ptr, copy_length);
            dest_ptr += copy_length;
            dest_size -= copy_length;
            src_ptr += original_width;
            src_size -= original_width * sizeof(u16);
        }
    } else {
        u32 buffer_size = buffer.size() * sizeof(u16);
        if (port.dest_size != buffer_size) {
            LOG_ERROR(Service_CAM, "The destination size (%d) doesn't match the source (%d)!",
                      port.dest_size, buffer_size);
        }
        Memory::WriteBlock(port.dest, buffer.data(), std::min<u32>(port.dest_size, buffer_size));
    }

    port.is_receiving = false;
    port.completion_event->Signal();
}

// Starts a receiving process on the specified port. This can only be called when is_busy = true and
// is_receiving = false.
static void StartReceiving(int port_id) {
    auto& port = ports[port_id];
    port.is_receiving = true;

    // launches a capture task asynchronously
    auto& camera = cameras[port.camera_id];
    port.capture_result =
        std::async(std::launch::async, &Camera::CameraInterface::ReceiveFrame, camera.impl.get());

    // schedules an completion event according to the frame rate. The event will block on the
    // capture task if it is not finished in the expected time.
    CoreTiming::ScheduleEvent(
        msToCycles(LATENCY_BY_FRAME_RATE[static_cast<int>(camera.frame_rate)]),
        completion_event_callback, port_id);
}

// Force to cancel any ongoing receiving process on the specified port. This is used by functions
// that stops the capture.
// TODO: what is the exact behaviour on real 3DS when stopping capture during an ongoing process?
//       Will the completion event still be signaled?
static void CancelReceiving(int port_id) {
    if (!ports[port_id].is_receiving)
        return;
    LOG_WARNING(Service_CAM, "tries to cancel an ongoing receiving process.");
    CoreTiming::UnscheduleEvent(completion_event_callback, port_id);
    ports[port_id].capture_result.wait();
    ports[port_id].is_receiving = false;
}

void StartCapture(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            if (!ports[i].is_busy) {
                if (!ports[i].is_active) {
                    // This doesn't return an error, but seems to put the camera in an undefined
                    // state
                    LOG_ERROR(Service_CAM, "port %d hasn't been activated", i);
                } else {
                    cameras[ports[i].camera_id].impl->StartCapture();
                    ports[i].is_busy = true;
                    if (ports[i].is_pending_receiving) {
                        ports[i].is_pending_receiving = false;
                        StartReceiving(i);
                    }
                }
            } else {
                LOG_WARNING(Service_CAM, "port %d already started", i);
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x1, 1, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void StopCapture(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            if (ports[i].is_busy) {
                CancelReceiving(i);
                cameras[ports[i].camera_id].impl->StopCapture();
                ports[i].is_busy = false;
            } else {
                LOG_WARNING(Service_CAM, "port %d already stopped", i);
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x2, 1, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void IsBusy(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSet(port_select, 2)) {
        bool is_busy = true;
        // Note: the behaviour on no or both ports selected are verified against real 3DS.
        for (int i : BitSetToIDs(port_select)) {
            is_busy &= ports[i].is_busy;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = is_busy ? 1 : 0;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x3, 2, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void ClearBuffer(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    cmd_buff[0] = IPC::MakeHeader(0x4, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_WARNING(Service_CAM, "(STUBBED) called, port_select=%d", port_select);
}

void GetVsyncInterruptEvent(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = IPC::CopyHandleDesc();
        cmd_buff[3] = Kernel::g_handle_table.Create(ports[port].vsync_interrupt_event).MoveFrom();
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
        cmd_buff[2] = IPC::CopyHandleDesc();
        cmd_buff[2] = 0;
    }

    cmd_buff[0] = IPC::MakeHeader(0x5, 1, 2);

    LOG_WARNING(Service_CAM, "(STUBBED) called, port_select=%d", port_select);
}

void GetBufferErrorInterruptEvent(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = IPC::CopyHandleDesc();
        cmd_buff[3] =
            Kernel::g_handle_table.Create(ports[port].buffer_error_interrupt_event).MoveFrom();
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
        cmd_buff[2] = IPC::CopyHandleDesc();
        cmd_buff[2] = 0;
    }

    LOG_WARNING(Service_CAM, "(STUBBED) called, port_select=%d", port_select);
}

void SetReceiving(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    VAddr dest = cmd_buff[1];
    u8 port_select = cmd_buff[2] & 0xFF;
    u32 image_size = cmd_buff[3];
    u16 trans_unit = cmd_buff[4] & 0xFFFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port_id = BitSetToIDs(port_select).back();
        auto& port = ports[port_id];
        CancelReceiving(port_id);
        port.completion_event->Clear();
        port.dest = dest;
        port.dest_size = image_size;

        if (port.is_busy) {
            StartReceiving(port_id);
        } else {
            port.is_pending_receiving = true;
        }

        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = IPC::CopyHandleDesc();
        cmd_buff[3] = Kernel::g_handle_table.Create(port.completion_event).MoveFrom();
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x7, 1, 2);

    LOG_DEBUG(Service_CAM, "called, addr=0x%X, port_select=%d, image_size=%d, trans_unit=%d", dest,
              port_select, image_size, trans_unit);
}

void IsFinishedReceiving(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = (ports[port].is_receiving || ports[port].is_pending_receiving) ? 0 : 1;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x8, 2, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void SetTransferLines(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;
    u16 transfer_lines = cmd_buff[2] & 0xFFFF;
    u16 width = cmd_buff[3] & 0xFFFF;
    u16 height = cmd_buff[4] & 0xFFFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            ports[i].transfer_bytes = transfer_lines * width * 2;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x9, 1, 0);

    LOG_WARNING(Service_CAM, "(STUBBED) called, port_select=%d, lines=%d, width=%d, height=%d",
                port_select, transfer_lines, width, height);
}

void GetMaxLines(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u16 width = cmd_buff[1] & 0xFFFF;
    u16 height = cmd_buff[2] & 0xFFFF;

    // Note: the result of the algorithm below are hwtested with width < 640 and with height < 480
    if (width * height * 2 % 256 != 0) {
        cmd_buff[1] = ERROR_OUT_OF_RANGE.raw;
    } else {
        u32 lines = 2560 / width;
        if (lines > height) {
            lines = height;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
        while (height % lines != 0 || (lines * width * 2 % 256 != 0)) {
            --lines;
            if (lines == 0) {
                cmd_buff[1] = ERROR_OUT_OF_RANGE.raw;
                break;
            }
        }
        cmd_buff[2] = lines;
    }

    cmd_buff[0] = IPC::MakeHeader(0xA, 2, 0);

    LOG_DEBUG(Service_CAM, "called, width=%d, height=%d", width, height);
}

void SetTransferBytes(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;
    u16 transfer_bytes = cmd_buff[2] & 0xFFFF;
    u16 width = cmd_buff[3] & 0xFFFF;
    u16 height = cmd_buff[4] & 0xFFFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            ports[i].transfer_bytes = transfer_bytes;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0xB, 1, 0);

    LOG_WARNING(Service_CAM, "(STUBBED)called, port_select=%d, bytes=%d, width=%d, height=%d",
                port_select, transfer_bytes, width, height);
}

void GetTransferBytes(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = ports[port].transfer_bytes;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0xC, 2, 0);

    LOG_WARNING(Service_CAM, "(STUBBED)called, port_select=%d", port_select);
}

void GetMaxBytes(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u16 width = cmd_buff[1] & 0xFFFF;
    u16 height = cmd_buff[2] & 0xFFFF;

    // Note: the result of the algorithm below are hwtested with width < 640 and with height < 480
    if (width * height * 2 % 256 != 0) {
        cmd_buff[1] = ERROR_OUT_OF_RANGE.raw;
    } else {
        u32 bytes = 5120;

        while (width * height * 2 % bytes != 0) {
            bytes -= 256;
        }

        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = bytes;
    }
    cmd_buff[0] = IPC::MakeHeader(0xD, 2, 0);

    LOG_DEBUG(Service_CAM, "called, width=%d, height=%d", width, height);
}

void SetTrimming(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;
    bool trim = (cmd_buff[2] & 0xFF) != 0;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            ports[i].is_trimming = trim;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0xE, 1, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d, trim=%d", port_select, trim);
}

void IsTrimming(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = ports[port].is_trimming;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0xF, 2, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void SetTrimmingParams(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;
    u16 x0 = cmd_buff[2] & 0xFFFF;
    u16 y0 = cmd_buff[3] & 0xFFFF;
    u16 x1 = cmd_buff[4] & 0xFFFF;
    u16 y1 = cmd_buff[5] & 0xFFFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            ports[i].x0 = x0;
            ports[i].y0 = y0;
            ports[i].x1 = x1;
            ports[i].y1 = y1;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x10, 1, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d, x0=%d, y0=%d, x1=%d, y1=%d", port_select, x0,
              y0, x1, y1);
}

void GetTrimmingParams(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;

    if (VerifyBitSetUnique(port_select, 2)) {
        int port = BitSetToIDs(port_select).back();
        cmd_buff[1] = RESULT_SUCCESS.raw;
        cmd_buff[2] = ports[port].x0;
        cmd_buff[3] = ports[port].y0;
        cmd_buff[4] = ports[port].x1;
        cmd_buff[5] = ports[port].y1;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x11, 5, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d", port_select);
}

void SetTrimmingParamsCenter(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 port_select = cmd_buff[1] & 0xFF;
    u16 trim_w = cmd_buff[2] & 0xFFFF;
    u16 trim_h = cmd_buff[3] & 0xFFFF;
    u16 cam_w = cmd_buff[4] & 0xFFFF;
    u16 cam_h = cmd_buff[5] & 0xFFFF;

    if (VerifyBitSet(port_select, 2)) {
        for (int i : BitSetToIDs(port_select)) {
            ports[i].x0 = (cam_w - trim_w) / 2;
            ports[i].y0 = (cam_h - trim_h) / 2;
            ports[i].x1 = ports[i].x0 + trim_w;
            ports[i].y1 = ports[i].y0 + trim_h;
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid port_select=%d", port_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x12, 1, 0);

    LOG_DEBUG(Service_CAM, "called, port_select=%d, trim_w=%d, trim_h=%d, cam_w=%d, cam_h=%d",
              port_select, trim_w, trim_h, cam_w, cam_h);
}

static void ActivatePort(int port_id, int camera_id) {
    if (ports[port_id].is_busy && ports[port_id].camera_id != camera_id) {
        CancelReceiving(port_id);
        cameras[ports[port_id].camera_id].impl->StopCapture();
        ports[port_id].is_busy = false;
    }
    ports[port_id].is_active = true;
    ports[port_id].camera_id = camera_id;
}

void Activate(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;

    if (camera_select < 8) {
        if (camera_select == 0) { // deactive all
            for (int i = 0; i < 2; ++i) {
                if (ports[i].is_busy) {
                    CancelReceiving(i);
                    cameras[ports[i].camera_id].impl->StopCapture();
                    ports[i].is_busy = false;
                }
                ports[i].is_active = false;
            }
            cmd_buff[1] = RESULT_SUCCESS.raw;
        } else if ((camera_select & 3) == 3) {
            LOG_ERROR(Service_CAM, "camera 0 and 1 can't be both activated");
            cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
        } else {
            if (camera_select & 1) {
                ActivatePort(0, 0);
            } else if ((camera_select >> 1) & 1) {
                ActivatePort(0, 1);
            }

            if ((camera_select >> 2) & 1) {
                ActivatePort(1, 2);
            }
            cmd_buff[1] = RESULT_SUCCESS.raw;
        }
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d", camera_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x13, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d", camera_select);
}

void SwitchContext(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    u8 context_select = cmd_buff[2] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSetUnique(context_select, 2)) {
        int context = BitSetToIDs(context_select).back();
        for (int camera : BitSetToIDs(camera_select)) {
            cameras[camera].current_context = context;
            const auto& context_config = cameras[camera].contexts[context];
            cameras[camera].impl->SetFlip(context_config.flip);
            cameras[camera].impl->SetEffect(context_config.effect);
            cameras[camera].impl->SetFormat(context_config.format);
            cameras[camera].impl->SetResolution(context_config.resolution);
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x14, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, context_select=%d", camera_select,
              context_select);
}

void FlipImage(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    auto flip = static_cast<Flip>(cmd_buff[2] & 0xFF);
    u8 context_select = cmd_buff[3] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSet(context_select, 2)) {
        for (int camera : BitSetToIDs(camera_select)) {
            for (int context : BitSetToIDs(context_select)) {
                cameras[camera].contexts[context].flip = flip;
                if (cameras[camera].current_context == context) {
                    cameras[camera].impl->SetFlip(flip);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x1D, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, flip=%d, context_select=%d", camera_select,
              static_cast<int>(flip), context_select);
}

void SetDetailSize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    Resolution resolution;
    resolution.width = cmd_buff[2] & 0xFFFF;
    resolution.height = cmd_buff[3] & 0xFFFF;
    resolution.crop_x0 = cmd_buff[4] & 0xFFFF;
    resolution.crop_y0 = cmd_buff[5] & 0xFFFF;
    resolution.crop_x1 = cmd_buff[6] & 0xFFFF;
    resolution.crop_y1 = cmd_buff[7] & 0xFFFF;
    u8 context_select = cmd_buff[8] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSet(context_select, 2)) {
        for (int camera : BitSetToIDs(camera_select)) {
            for (int context : BitSetToIDs(context_select)) {
                cameras[camera].contexts[context].resolution = resolution;
                if (cameras[camera].current_context == context) {
                    cameras[camera].impl->SetResolution(resolution);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x1E, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, width=%d, height=%d, crop_x0=%d, crop_y0=%d, "
                           "crop_x1=%d, crop_y1=%d,  context_select=%d",
              camera_select, resolution.width, resolution.height, resolution.crop_x0,
              resolution.crop_y0, resolution.crop_x1, resolution.crop_y1, context_select);
}

void SetSize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    u8 size = cmd_buff[2] & 0xFF;
    u8 context_select = cmd_buff[3] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSet(context_select, 2)) {
        for (int camera : BitSetToIDs(camera_select)) {
            for (int context : BitSetToIDs(context_select)) {
                cameras[camera].contexts[context].resolution = RESOLUTION_LIBRARY[size];
                if (cameras[camera].current_context == context) {
                    cameras[camera].impl->SetResolution(RESOLUTION_LIBRARY[size]);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x1F, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, size=%d, context_select=%d", camera_select,
              size, context_select);
}

void SetFrameRate(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    auto frame_rate = static_cast<FrameRate>(cmd_buff[2] & 0xFF);

    if (VerifyBitSet(camera_select, 3)) {
        for (int camera : BitSetToIDs(camera_select)) {
            cameras[camera].frame_rate = frame_rate;
            // TODO(wwylele): consider hinting the actual camera with the expected frame rate
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d", camera_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x20, 1, 0);

    LOG_WARNING(Service_CAM, "(STUBBED) called, camera_select=%d, frame_rate=%d", camera_select,
                static_cast<int>(frame_rate));
}

void SetEffect(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    auto effect = static_cast<Effect>(cmd_buff[2] & 0xFF);
    u8 context_select = cmd_buff[3] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSet(context_select, 2)) {
        for (int camera : BitSetToIDs(camera_select)) {
            for (int context : BitSetToIDs(context_select)) {
                cameras[camera].contexts[context].effect = effect;
                if (cameras[camera].current_context == context) {
                    cameras[camera].impl->SetEffect(effect);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x22, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, effect=%d, context_select=%d", camera_select,
              static_cast<int>(effect), context_select);
}

void SetOutputFormat(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select = cmd_buff[1] & 0xFF;
    auto format = static_cast<OutputFormat>(cmd_buff[2] & 0xFF);
    u8 context_select = cmd_buff[3] & 0xFF;

    if (VerifyBitSet(camera_select, 3) && VerifyBitSet(context_select, 2)) {
        for (int camera : BitSetToIDs(camera_select)) {
            for (int context : BitSetToIDs(context_select)) {
                cameras[camera].contexts[context].format = format;
                if (cameras[camera].current_context == context) {
                    cameras[camera].impl->SetFormat(format);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", camera_select,
                  context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x25, 1, 0);

    LOG_DEBUG(Service_CAM, "called, camera_select=%d, format=%d, context_select=%d", camera_select,
              static_cast<int>(format), context_select);
}

void SynchronizeVsyncTiming(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 camera_select1 = cmd_buff[1] & 0xFF;
    u8 camera_select2 = cmd_buff[2] & 0xFF;

    cmd_buff[0] = IPC::MakeHeader(0x29, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_WARNING(Service_CAM, "(STUBBED) called, camera_select1=%d, camera_select2=%d",
                camera_select1, camera_select2);
}

void GetStereoCameraCalibrationData(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    // Default values taken from yuriks' 3DS. Valid data is required here or games using the
    // calibration get stuck in an infinite CPU loop.
    StereoCameraCalibrationData data = {};
    data.isValidRotationXY = 0;
    data.scale = 1.001776f;
    data.rotationZ = 0.008322907f;
    data.translationX = -87.70484f;
    data.translationY = -7.640977f;
    data.rotationX = 0.0f;
    data.rotationY = 0.0f;
    data.angleOfViewRight = 64.66875f;
    data.angleOfViewLeft = 64.76067f;
    data.distanceToChart = 250.0f;
    data.distanceCameras = 35.0f;
    data.imageWidth = 640;
    data.imageHeight = 480;

    cmd_buff[0] = IPC::MakeHeader(0x2B, 17, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;
    memcpy(&cmd_buff[2], &data, sizeof(data));

    LOG_TRACE(Service_CAM, "called");
}

void SetPackageParameterWithoutContext(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    PackageParameterWithoutContext package;
    std::memcpy(&package, cmd_buff + 1, sizeof(package));

    cmd_buff[0] = IPC::MakeHeader(0x33, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_DEBUG(Service_CAM, "(STUBBED) called");
}

void SetPackageParameterWithContext(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    PackageParameterWithContext package;
    std::memcpy(&package, cmd_buff + 1, sizeof(package));

    if (VerifyBitSet(package.camera_select, 3) && VerifyBitSet(package.context_select, 2)) {
        for (int camera_id : BitSetToIDs(package.camera_select)) {
            auto& camera = cameras[camera_id];
            for (int context_id : BitSetToIDs(package.context_select)) {
                auto& context = camera.contexts[context_id];
                context.effect = package.effect;
                context.flip = package.flip;
                context.resolution = RESOLUTION_LIBRARY[static_cast<int>(package.size)];
                if (context_id == camera.current_context) {
                    camera.impl->SetEffect(context.effect);
                    camera.impl->SetFlip(context.flip);
                    camera.impl->SetResolution(context.resolution);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", package.camera_select,
                  package.context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x34, 1, 0);

    LOG_DEBUG(Service_CAM, "called");
}

void SetPackageParameterWithContextDetail(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    PackageParameterWithContextDetail package;
    std::memcpy(&package, cmd_buff + 1, sizeof(package));

    if (VerifyBitSet(package.camera_select, 3) && VerifyBitSet(package.context_select, 2)) {
        for (int camera_id : BitSetToIDs(package.camera_select)) {
            auto& camera = cameras[camera_id];
            for (int context_id : BitSetToIDs(package.context_select)) {
                auto& context = camera.contexts[context_id];
                context.effect = package.effect;
                context.flip = package.flip;
                context.resolution = package.resolution;
                if (context_id == camera.current_context) {
                    camera.impl->SetEffect(context.effect);
                    camera.impl->SetFlip(context.flip);
                    camera.impl->SetResolution(context.resolution);
                }
            }
        }
        cmd_buff[1] = RESULT_SUCCESS.raw;
    } else {
        LOG_ERROR(Service_CAM, "invalid camera_select=%d, context_select=%d", package.camera_select,
                  package.context_select);
        cmd_buff[1] = ERROR_INVALID_ENUM_VALUE.raw;
    }

    cmd_buff[0] = IPC::MakeHeader(0x35, 1, 0);

    LOG_DEBUG(Service_CAM, "called");
}

void GetSuitableY2rStandardCoefficient(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[0] = IPC::MakeHeader(0x36, 2, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = 0;

    LOG_WARNING(Service_CAM, "(STUBBED) called");
}

void PlayShutterSound(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 sound_id = cmd_buff[1] & 0xFF;

    cmd_buff[0] = IPC::MakeHeader(0x38, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_WARNING(Service_CAM, "(STUBBED) called, sound_id=%d", sound_id);
}

void DriverInitialize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    for (int camera_id = 0; camera_id < 3; ++camera_id) {
        auto& camera = cameras[camera_id];
        camera.current_context = 0;
        for (int context_id = 0; context_id < 2; ++context_id) {
            // Note: the following default values are verified against real 3DS
            auto& context = camera.contexts[context_id];
            context.flip = camera_id == 1 ? Flip::Horizontal : Flip::None;
            context.effect = Effect::None;
            context.format = OutputFormat::YUV422;
            context.resolution =
                context_id == 0 ? RESOLUTION_LIBRARY[5 /*DS_LCD*/] : RESOLUTION_LIBRARY[0 /*VGA*/];
        }
        camera.impl = Camera::CreateCamera(Settings::values.camera_name[camera_id],
                                           Settings::values.camera_config[camera_id]);
        camera.impl->SetFlip(camera.contexts[0].flip);
        camera.impl->SetEffect(camera.contexts[0].effect);
        camera.impl->SetFormat(camera.contexts[0].format);
        camera.impl->SetResolution(camera.contexts[0].resolution);
    }

    for (auto& port : ports) {
        port.completion_event->Clear();
        port.buffer_error_interrupt_event->Clear();
        port.vsync_interrupt_event->Clear();
        port.is_receiving = false;
        port.is_active = false;
        port.is_pending_receiving = false;
        port.is_busy = false;
        port.is_trimming = false;
        port.x0 = 0;
        port.y0 = 0;
        port.x1 = 0;
        port.y1 = 0;
        port.transfer_bytes = 256;
    }

    cmd_buff[0] = IPC::MakeHeader(0x39, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_DEBUG(Service_CAM, "called");
}

void DriverFinalize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    CancelReceiving(0);
    CancelReceiving(1);

    for (auto& camera : cameras) {
        camera.impl = nullptr;
    }

    cmd_buff[0] = IPC::MakeHeader(0x3A, 1, 0);
    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_DEBUG(Service_CAM, "called");
}

void Init() {
    using namespace Kernel;

    AddService(new CAM_C_Interface);
    AddService(new CAM_Q_Interface);
    AddService(new CAM_S_Interface);
    AddService(new CAM_U_Interface);

    for (auto& port : ports) {
        port.completion_event = Event::Create(ResetType::Sticky, "CAM_U::completion_event");
        port.buffer_error_interrupt_event =
            Event::Create(ResetType::OneShot, "CAM_U::buffer_error_interrupt_event");
        port.vsync_interrupt_event =
            Event::Create(ResetType::OneShot, "CAM_U::vsync_interrupt_event");
    }
    completion_event_callback =
        CoreTiming::RegisterEvent("CAM_U::CompletionEventCallBack", CompletionEventCallBack);
}

void Shutdown() {
    CancelReceiving(0);
    CancelReceiving(1);
    for (auto& port : ports) {
        port.completion_event = nullptr;
        port.buffer_error_interrupt_event = nullptr;
        port.vsync_interrupt_event = nullptr;
    }
    for (auto& camera : cameras) {
        camera.impl = nullptr;
    }
}

} // namespace CAM

} // namespace Service
