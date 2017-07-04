// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include "core/frontend/input.h"
#include "core/hle/service/ir/ir_user.h"

namespace Service {
namespace IR {

class Other3DS final : public IRDevice {
public:
    explicit Other3DS(SendFunc send_func);
    // ~Other3DS();

    void OnConnect() override;
    void OnDisconnect() override;
    void OnReceive(const std::vector<u8>& data) override;

    u8 GetRole();
    friend void StreamOut();
private:
};

void InitIRStream();

} // namespace IR
} // namespace Service
