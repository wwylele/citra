// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef __linux__

#include <linux/videodev2.h>
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/interface.h"

namespace Camera {

class V4L2Camera final : public CameraInterface {
public:
    V4L2Camera(int device_id);
    ~V4L2Camera();
    void StartCapture() override;
    void StopCapture() override;
    void SetResolution(const Service::CAM::Resolution&) override;
    void SetFlip(Service::CAM::Flip) override;
    void SetEffect(Service::CAM::Effect) override;
    void SetFormat(Service::CAM::OutputFormat) override;
    std::vector<u16> ReceiveFrame() const override;

private:
    bool is_valid = false;
    bool is_capturing = false;
    int width, height;
    int fd;
    v4l2_buf_type format_type;
    v4l2_format format;
    static constexpr unsigned NUM_BUFFERS = 2;
    struct {
        char* memory;
        size_t length;
    } mapped_buffers[NUM_BUFFERS];
    bool crop_image_horizontally;
};

class V4L2CameraFactory final : public CameraFactory {
public:
    std::unique_ptr<CameraInterface> Create(const std::string& config) const override {
        return std::make_unique<V4L2Camera>(std::stoi(config));
    }
};

} // namespace Camera

#endif
