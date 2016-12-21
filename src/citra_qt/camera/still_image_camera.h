// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QImage>
#include "common/camera/factory.h"
#include "common/camera/interface.h"

namespace Camera {

class StillImageCamera final : public CameraInterface {
public:
    StillImageCamera(QImage image_) : image(image_) {}
    void StartCapture() override{};
    void StopCapture() override{};
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
    std::unique_ptr<CameraInterface> Create(const std::string& config) const override {
        QImage image(QString::fromStdString(config));
        if (image.isNull()) {
            LOG_ERROR(Service_CAM, "Couldn't load image \"%s\"", config.c_str());
        }
        return std::make_unique<StillImageCamera>(image);
    }
};

} // namespace Camera
