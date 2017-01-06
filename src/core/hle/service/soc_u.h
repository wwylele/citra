// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include "core/hle/service/service.h"

namespace Service {
namespace SOC {

class SOC_U final : public Interface {
public:
    SOC_U();
    ~SOC_U();

    std::string GetPortName() const override {
        return "soc:U";
    }
};

} // namespace SOC
} // namespace Service
