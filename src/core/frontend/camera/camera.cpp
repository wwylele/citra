// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/frontend/camera/blank_camera.h"
#include "core/frontend/camera/camera.h"
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/v4l2_camera.h"

namespace Camera {

void Init() {
    RegisterFactory("blank", std::make_unique<BlankCameraFactory>());
#ifdef __linux__
    RegisterFactory("V4L2", std::make_unique<V4L2CameraFactory>());
#endif
}

} // namespace Camera
