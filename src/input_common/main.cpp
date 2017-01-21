// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "input_common/main.h"

namespace InputCommon {

static std::shared_ptr<Keyboard> keyboard;

void Init() {
    keyboard = std::make_shared<InputCommon::Keyboard>();
    Input::RegisterFactory<Input::ButtonDevice>("keyboard", keyboard);
}

Keyboard* GetKeyboard() {
    return keyboard.get();
}

std::string GenerateKeyboardParam(int key_code) {
    Common::ParamPackage param{
        {"engine", "keyboard"}, {"code", std::to_string(key_code)},
    };
    return param.Serialize();
}

} // namespace InputCommon
