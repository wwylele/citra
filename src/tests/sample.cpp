// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <SDL.h>
#include <catch.hpp>
#include <glad/glad.h>
#include "common/file_util.h"
#include "core/core_timing.h"

TEST_CASE("Dummy", "[test]") {
    gladLoadGLLoader(static_cast<GLADloadproc>(SDL_GL_GetProcAddress));
    REQUIRE(1 + 1 == 2);
    REQUIRE(CoreTiming::GetClockFrequencyMHz() != 0);
}
