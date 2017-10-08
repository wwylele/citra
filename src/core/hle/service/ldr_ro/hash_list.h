// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <vector>
#include "common/common_types.h"
#include "core/hle/result.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace LDR_RO

namespace Service {
namespace LDR {

class HashList final {
public:
    static ResultCode LoadCRR(VAddr crr_address);
    static ResultCode UnloadCRR(VAddr crr_address);
    static ResultCode VerifyHash(VAddr crr_address, const u8* hash);
    static void Clear();

private:
    explicit HashList(VAddr crr_address);
    bool HasHash(const u8* hash) const;

    ResultCode error;
    VAddr crr_address;
    std::vector<std::array<u8, 0x20>> hash_list;
};

}
} // namespace
