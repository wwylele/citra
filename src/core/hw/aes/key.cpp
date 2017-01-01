// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <sstream>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/string_util.h"
#include "core/hw/aes/aes.h"
#include "core/hw/aes/key.h"

namespace HW {
namespace AES {

namespace {

///////////////////////////////////////////////////////////////////////////////////////////////////
// 128-bit unsigned wraparound arithmetic

int WrapIndex128(int i) {
    return i < 0 ? ((i % 16) + 16) % 16 : (i > 15 ? i % 16 : i);
}

AESKey Lrot128(const AESKey& in, u32 rot) {
    AESKey out;
    u32 bit_shift, byte_shift;

    rot = rot % 128;
    byte_shift = rot / 8;
    bit_shift = rot % 8;

    for (int i = 0; i < 16; i++) {
        out[i] = (in[WrapIndex128(i + byte_shift)] << bit_shift) |
                 (in[WrapIndex128(i + byte_shift + 1)] >> (8 - bit_shift));
    }
    return out;
}

AESKey Add128(const AESKey& a, const AESKey& b) {
    AESKey out;
    unsigned carry = 0;
    unsigned sum = 0;

    for (int i = 15; i >= 0; i--) {
        sum = a[i] + b[i] + carry;
        carry = sum >> 8;
        out[i] = static_cast<u8>(sum & 0xff);
    }

    // wwylele: The part below is added in makerom, but I don't think it is correct.
    /*
    while(carry != 0){
        for(int i = 15; i >= 0; i--){
            sum = out[i] + carry;
            carry = sum >> 8;
            out[i] = static_cast<u8>(sum & 0xff);
        }
    }
    */

    return out;
}

AESKey Xor128(const AESKey& a, const AESKey& b) {
    AESKey out;
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] ^ b[i];
    }
    return out;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// key slot management

struct KeyResource {
    AESKey key;
    bool available;

    void Reset() {
        std::fill(key.begin(), key.end(), 0);
        available = false;
    }

    void Set(const AESKey& new_key) {
        key = new_key;
        available = true;
    }
};

KeyResource generator_constant;

struct KeySlot {
    KeyResource x;
    KeyResource y;
    KeyResource normal;

    void SetKeyX(const AESKey& key) {
        x.Set(key);
        if (y.available && generator_constant.available) {
            GenerateNormalKey();
        }
    }

    void SetKeyY(const AESKey& key) {
        y.Set(key);
        if (x.available && generator_constant.available) {
            GenerateNormalKey();
        }
    }

    void SetNormalKey(const AESKey& key) {
        normal.Set(key);
    }

    void GenerateNormalKey() {
        normal.Set(Lrot128(Add128(Xor128(Lrot128(x.key, 2), y.key), generator_constant.key), 87));
    }

    void Clear() {
        x.Reset();
        y.Reset();
        normal.Reset();
    }
};

std::array<KeySlot, KeySlotID::MaxKeySlotID> key_slots;

void ClearAllKeys() {
    for (KeySlot& slot : key_slots) {
        slot.Clear();
    }
    generator_constant.Reset();
}

AESKey HexToKey(const std::string& hex) {
    AESKey key{};
    if (hex.size() < 32) {
        return key;
    }
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<u8>(std::stoi(hex.substr(i * 2, 2), 0, 16));
    }
    return key;
}

void LoadPresetKeys() {
    const std::string filepath = FileUtil::GetUserPath(D_SYSDATA_IDX) + AES_KEYS;
    FileUtil::CreateFullPath(filepath); // Create path if not already created
    std::ifstream file;
    OpenFStream(file, filepath, std::ios_base::in);
    if (!file) {
        return;
    }
    while (!file.eof()) {
        std::string line;
        std::getline(file, line);
        std::vector<std::string> parts;
        Common::SplitString(line, '=', parts);
        if (parts.size() != 2) {
            continue;
        }
        const std::string& name = parts[0];
        const AESKey key = HexToKey(parts[1]);

        if (name == "generator") {
            generator_constant.Set(key);
            continue;
        }

        int slot_id;
        char key_type;
        if (std::sscanf(name.c_str(), "slot0x%XKey%c", &slot_id, &key_type) != 2) {
            continue;
        }

        if (slot_id < 0 || slot_id >= 0x40) {
            continue;
        }

        switch (key_type) {
        case 'X':
            key_slots[slot_id].SetKeyX(key);
            break;
        case 'Y':
            key_slots[slot_id].SetKeyY(key);
            break;
        case 'N':
            key_slots[slot_id].SetNormalKey(key);
            break;
        }
    }
}

} // namespace

void InitKeys() {
    ClearAllKeys();
    LoadPresetKeys();
}

void SetGeneratorConstant(const AESKey& key) {
    generator_constant.Set(key);
}

void SetKeyX(int slot_id, const AESKey& key) {
    key_slots[slot_id].SetKeyX(key);
}

void SetKeyY(int slot_id, const AESKey& key) {
    key_slots[slot_id].SetKeyY(key);
}

void SetNormalKey(int slot_id, const AESKey& key) {
    key_slots[slot_id].SetNormalKey(key);
}

bool IsNormalKeyAvailable(int slot_id) {
    return key_slots[slot_id].normal.available;
}

AESKey GetNormalKey(int slot_id) {
    return key_slots[slot_id].normal.key;
}

} // namespace AES
} // namespace HW
