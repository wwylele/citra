// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <vector>
#include "common/common_types.h"

namespace HW {
namespace AES {

constexpr int CCM_NONCE_SIZE = 12;
constexpr int CCM_MAC_SIZE = 16;

using CCMNonce = std::array<u8, CCM_NONCE_SIZE>;

/**
 * Encrypts and adds a MAC to the given data using AES-CCM algorithm.
 * @param pdata The plain text data to encrypt
 * @param nonce The nonce data to use for encryption
 * @param slot_id The slot ID of the key to use for encryption
 * @returns a vector of u8 containing the encrypted data with MAC at the end
 */
std::vector<u8> EncryptSignCCM(const std::vector<u8>& pdata, const CCMNonce& nonce, int slot_id);

/**
 * Decrypts and verify the MAC of the given data using AES-CCM algorithm.
 * @param cipher The cipher text data to decrypt, with MAC at the end to verify
 * @param nonce The nonce data to use for decryption
 * @param slot_id The slot ID of the key to use for decryption
 * @returns a vector of u8 containing the decrypted data; an empty vector if the verification fails
 */
std::vector<u8> DecryptVerifyCCM(const std::vector<u8>& cipher, const CCMNonce& nonce, int slot_id);

} // namespace AES
} // namespace HW
