// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <unordered_map>
#include "common/logging/log.h"
#include "core/frontend/camera/blank_camera.h"
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/v4l2_camera.h"

namespace Camera {

static std::unordered_map<std::string, std::unique_ptr<CameraFactory>> factories;

CameraFactory::~CameraFactory() = default;

void RegisterFactory(const std::string& name, std::unique_ptr<CameraFactory> factory) {
    factories[name] = std::move(factory);
}

std::unique_ptr<CameraInterface> CreateCamera(const std::string& name, const std::string& config) {
    auto pair = factories.find(name);
    if (pair == factories.end()) {
        LOG_ERROR(Service_CAM, "Unknown camera \"%s\"", name.c_str());
        return std::make_unique<BlankCamera>();
    }
    return factories[name]->Create(config);
}

} // namespace Camera
