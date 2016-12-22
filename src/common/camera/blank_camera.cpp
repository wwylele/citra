// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/camera/blank_camera.h"

namespace Camera {

void BlankCamera::SetFormat(Service::CAM::OutputFormat output_format) {
    output_rgb = output_format == Service::CAM::OutputFormat::RGB565;
}

void BlankCamera::SetResolution(const Service::CAM::Resolution& resolution) {
    width = resolution.width;
    height = resolution.height;
};

std::vector<u16> BlankCamera::ReceiveFrame() const {
    // Note: 0x80008000 stands for two black pixels in YUV422
    std::vector<u16> buffer(width * height, output_rgb ? 0 : 0x8000);
    return buffer;
}

} // namespace Camera
