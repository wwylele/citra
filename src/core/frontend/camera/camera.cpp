// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/frontend/camera/blank_camera.h"
#include "core/frontend/camera/camera.h"
#include "core/frontend/camera/factory.h"

namespace Camera {

void Init() {
    RegisterFactory("blank", std::make_unique<BlankCameraFactory>());
}

} // namespace Camera
