// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/pm_app.h"

namespace Service {
namespace PM {

const Interface::FunctionInfo FunctionTable[] = {
    // clang-format off
    {0x00010140, nullptr, "LaunchTitle"},
    {0x00020082, nullptr, "LaunchFIRM"},
    {0x00030080, nullptr, "TerminateApplication"},
    {0x00040100, nullptr, "TerminateTitle"},
    {0x000500C0, nullptr, "TerminateProcess"},
    {0x00060082, nullptr, "PrepareForReboot"},
    {0x00070042, nullptr, "GetFIRMLaunchParams"},
    {0x00080100, nullptr, "GetTitleExheaderFlags"},
    {0x00090042, nullptr, "SetFIRMLaunchParams"},
    {0x000A0140, nullptr, "SetAppResourceLimit"},
    {0x000B0140, nullptr, "GetAppResourceLimit"},
    {0x000C0080, nullptr, "UnregisterProcess"},
    {0x000D0240, nullptr, "LaunchTitleUpdate"},
    // clang-format on
};

PM_APP::PM_APP() {
    Register(FunctionTable);
}

} // namespace PM
} // namespace Service
