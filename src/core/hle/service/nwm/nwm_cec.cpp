// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/nwm/nwm_cec.h"

namespace Service {
namespace NWM {

const Interface::FunctionInfo FunctionTable[] = {
    {0x000D0082, nullptr, "SendProbeRequest"},
};

NWM_CEC::NWM_CEC() {
    Register(FunctionTable);
}

} // namespace NWM
} // namespace Service
