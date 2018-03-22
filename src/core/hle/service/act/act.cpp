// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/act/act.h"
#include "core/hle/service/act/act_a.h"
#include "core/hle/service/act/act_u.h"

namespace Service {
namespace ACT {

void Initialize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 version = cmd_buff[1];
    u32 shared_memory_size = cmd_buff[2];
    ASSERT(cmd_buff[3] == 0x20);
    ASSERT(cmd_buff[5] == 0);
    u32 shared_memory = cmd_buff[6];

    LOG_WARNING(Log,
                "(STUBBED) called, version=0x%08X, shared_memory_size=0x%X, shared_memory=0x%08X",
                version, shared_memory_size, shared_memory);
    cmd_buff[0] = 0x00010080;
    cmd_buff[1] = 0;
}

void GetAccountDataBlock(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u8 unk = static_cast<u8>(cmd_buff[1] & 0xFF);
    u32 size = cmd_buff[2];
    u32 id = cmd_buff[3];
    ASSERT(cmd_buff[4] == ((size << 4) | 0xC));
    VAddr addr = cmd_buff[5];

    LOG_WARNING(Log, "(STUBBED) called, unk=0x%02X, size=0x%X, id=0x%X", unk, size, id);

    switch (id) {
    case 0x8: {
        char network_id[0x11] = "wwylele";
        Memory::WriteBlock(addr, network_id, sizeof(network_id));
        break;
    }
    case 0x1B: {
        char16_t username[0xB] = u"wwy";
        Memory::WriteBlock(addr, username, sizeof(username));
        break;
    }
    case 0xC: {
        u32 principle_id = 0xDEADBEEF;
        Memory::WriteBlock(addr, &principle_id, sizeof(principle_id));
        break;
    }
    case 0xB: {
        char country[3] = "JP";
        Memory::WriteBlock(addr, country, sizeof(country));
        break;
    }
    case 0x17: {
        u32 a = 1;
        Memory::WriteBlock(addr, &a, sizeof(a));
        break;
    }
    }

    cmd_buff[0] = 0x00060082;
    cmd_buff[1] = 0;
}

void Init() {
    AddService(new ACT_A);
    AddService(new ACT_U);
}

} // namespace ACT
} // namespace Service
