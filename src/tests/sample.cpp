// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <catch.hpp>
#include <glad/glad.h>
#include "common/file_util.h"
#include "core/core_timing.h"

TEST_CASE("Dummy", "[test]") {
    gladLoadGL();
    REQUIRE(1 + 1 == 2);
    REQUIRE(CoreTiming::GetClockFrequencyMHz() != 0);
}
