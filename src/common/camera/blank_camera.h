// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/camera/factory.h"
#include "common/camera/interface.h"

namespace Camera {

class BlankCamera final : public CameraInterface {
public:
    void StartCapture() override{};
    void StopCapture() override{};
    void SetResolution(const Service::CAM::Resolution&) override;
    void SetFlip(Service::CAM::Flip) override{};
    void SetEffect(Service::CAM::Effect) override{};
    void SetFormat(Service::CAM::OutputFormat) override;
    std::vector<u16> ReceiveFrame() const override;

private:
    int width, height;
    bool output_rgb;
};

class BlankCameraFactory final : public CameraFactory {
public:
    std::unique_ptr<CameraInterface> Create(const std::string& config) const override {
        return std::make_unique<BlankCamera>();
    }
};

} // namespace Camera
