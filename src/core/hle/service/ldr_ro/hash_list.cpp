// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cryptopp/rsa.h>
#include "common/swap.h"
#include "core/hle/service/ldr_ro/hash_list.h"
#include "core/memory.h"

namespace Service {
namespace LDR {

struct CRRHeader {
    u8 magic[4];
    INSERT_PADDING_WORDS(1);
    u32_le next_crr;
    u32_le previous_crr;

    u32_le debug_info_offset;
    u32_le debug_info_size;
    INSERT_PADDING_WORDS(2);

    u32_le unique_id_mask;
    u32_le unique_id_pattern;
    INSERT_PADDING_WORDS(6);

    u8 signature_modulus[0x100];
    u8 header_signature[0x100];
    u8 hash_list_signature[0x100];

    u32_le unique_id;
    u32_le file_size;
    INSERT_PADDING_WORDS(2);

    u32_le hash_list_offset;
    u32_le hash_list_size;
    u32_le plain_region_offset;
    u32_le plain_region_size;
};

static_assert(sizeof(CRRHeader) == 0x360, "Wrong CRRHeader size");

static std::vector<HashList> hash_lists;

static const u8 crr_public_key[256] = {
    // hidden
    };

static bool VerifyRSA(const u8* data, size_t data_len, const u8* signature, const u8* n) {
    return CryptoPP::RSASS<CryptoPP::PKCS1v15, CryptoPP::SHA256>::Verifier(
               CryptoPP::Integer(n, 256), CryptoPP::Integer(0x10001))
        .VerifyMessage(data, data_len, signature, 256);
}

HashList::HashList(VAddr address) : crr_address(address), error(RESULT_SUCCESS) {
    CRRHeader header;
    Memory::ReadBlock(crr_address, &header, sizeof(header));

    if (!VerifyRSA(reinterpret_cast<u8*>(&header.unique_id_mask), 0x120, header.header_signature,
                   crr_public_key)) {
        LOG_ERROR(Service_LDR, "Wrong header signature");
        error = ResultCode(-1);
    }

    int block_b_size = header.plain_region_offset - 0x340;
    std::vector<u8> block_b(block_b_size);
    Memory::ReadBlock(crr_address + 0x340, block_b.data(), block_b_size);

    if (!VerifyRSA(block_b.data(), block_b_size, header.hash_list_signature,
                   header.signature_modulus)) {
        LOG_ERROR(Service_LDR, "Wrong hash list signature");
        error = ResultCode(-1);
    }

    hash_list.resize(header.hash_list_size);
    Memory::ReadBlock(crr_address + header.hash_list_offset, hash_list.data(),
                      header.hash_list_size * 0x20);
}

bool HashList::HasHash(const u8* hash) const {
    return std::any_of(hash_list.begin(), hash_list.end(), [=](const std::array<u8, 0x20>& h) {
        return std::memcmp(h.data(), hash, 0x20) == 0;
    });
}

ResultCode HashList::LoadCRR(VAddr crr_address) {
    HashList hl(crr_address);
    if (hl.error.IsError()) {
        return hl.error;
    }
    hash_lists.push_back(hl);
    return RESULT_SUCCESS;
}

ResultCode HashList::UnloadCRR(VAddr crr_address) {
    auto hl = std::find_if(hash_lists.begin(), hash_lists.end(),
                           [=](const HashList& hl) { return hl.crr_address == crr_address; });

    if (hl == hash_lists.end())
        return ResultCode(0xD9012C06);

    hash_lists.erase(hl);
    return RESULT_SUCCESS;
}

ResultCode HashList::VerifyHash(VAddr crr_address, const u8* hash) {
    if (crr_address == 0) {
        if (std::any_of(hash_lists.begin(), hash_lists.end(),
                        [=](const HashList& hl) { return hl.HasHash(hash); })) {
            return RESULT_SUCCESS;
        } else {
            return ResultCode(0xD8E12C1C);
        }
    } else {
        auto hl = std::find_if(hash_lists.begin(), hash_lists.end(),
                               [=](const HashList& hl) { return hl.crr_address == crr_address; });

        if (hl == hash_lists.end())
            return ResultCode(0xD8E12C0E);

        if (hl->HasHash(hash)) {
            return RESULT_SUCCESS;
        } else {
            return ResultCode(0xD8E12C1C);
        }
    }
}

void HashList::Clear() {
    hash_lists.clear();
}
}
} // namespace
