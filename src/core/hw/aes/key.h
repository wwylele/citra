// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "core/hw/aes/aes.h"

namespace HW {
namespace AES {

constexpr int AES_BLOCK_SIZE = 16;

using AESKey = std::array<u8, AES_BLOCK_SIZE>;

void InitKeys();

void SetGeneratorConstant(const AESKey& key);
void SetKeyX(int slot_id, const AESKey& key);
void SetKeyY(int slot_id, const AESKey& key);
void SetNormalKey(int slot_id, const AESKey& key);

bool IsNormalKeyAvailable(int slot_id);
AESKey GetNormalKey(int slot_id);

} // namspace AES
} // namespace HW