// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QImage>
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/interface.h"

namespace Camera {

class StillImageCamera final : public CameraInterface {
public:
    StillImageCamera(QImage image);
    void StartCapture() override;
    void StopCapture() override;
    void SetResolution(const Service::CAM::Resolution&) override;
    void SetFlip(Service::CAM::Flip) override;
    void SetEffect(Service::CAM::Effect) override;
    void SetFormat(Service::CAM::OutputFormat) override;
    std::vector<u16> ReceiveFrame() const override;

private:
    QImage image;
    int width, height;
    bool output_rgb;
    bool flip_horizontal, flip_vertical;
};

class StillImageCameraFactory final : public CameraFactory {
public:
    std::unique_ptr<CameraInterface> Create(const std::string& config) const override;
};

} // namespace Camera
