// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <string>

namespace InputCommon {
namespace SDL {

class ListenerDetail;

using ParamsHandler = std::function<void(const std::string& params)>;

class Listener {
public:
    Listener(ParamsHandler button_params_handler, ParamsHandler analog_params_handler);
    ~Listener();

private:
    std::unique_ptr<ListenerDetail> detail;
};

} // namespace SDL
} // namespace InputCommon
