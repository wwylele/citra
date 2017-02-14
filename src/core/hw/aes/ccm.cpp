// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cryptopp/aes.h>
#include <cryptopp/ccm.h>
#include <cryptopp/cryptlib.h>
#include <cryptopp/filters.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/hw/aes/ccm.h"
#include "core/hw/aes/key.h"

namespace HW {
namespace AES {

namespace {

// 3DS uses a non-standard AES-CCM algorithm, so we need to derive a sub class from the standard one
// and override with the non-standard part.
using namespace CryptoPP;
template <bool T_IsEncryption>
class CCM_3DSVariant_Final : public CCM_Final<CryptoPP::AES, CCM_MAC_SIZE, T_IsEncryption> {
public:
    void UncheckedSpecifyDataLengths(lword headerLength, lword messageLength,
                                     lword footerLength) override {
        // 3DS uses the aligned size to generate B0 for authentication, instead of the original size
        lword aligned_messageLength = Common::AlignUp(messageLength, AES_BLOCK_SIZE);
        CCM_Base::UncheckedSpecifyDataLengths(headerLength, aligned_messageLength, footerLength);
        CCM_Base::m_messageLength = messageLength; // restore the actuall message size
    }
};

class CCM_3DSVariant {
public:
    using Encryption = CCM_3DSVariant_Final<true>;
    using Decryption = CCM_3DSVariant_Final<false>;
};

} // namespace

std::vector<u8> EncryptSignCCM(const std::vector<u8>& pdata, const CCMNonce& nonce, int slot_id) {
    if (!IsNormalKeyAvailable(slot_id)) {
        LOG_ERROR(HW_AES, "Key slot %d not available. Will use zero key.", slot_id);
    }
    const AESKey normal = GetNormalKey(slot_id);
    std::vector<u8> cipher(pdata.size() + CCM_MAC_SIZE);

    try {
        CCM_3DSVariant::Encryption e;
        e.SetKeyWithIV(normal.data(), AES_BLOCK_SIZE, nonce.data(), CCM_NONCE_SIZE);
        e.SpecifyDataLengths(0, pdata.size(), 0);
        CryptoPP::ArraySource as(pdata.data(), pdata.size(), true,
                                 new CryptoPP::AuthenticatedEncryptionFilter(
                                     e, new CryptoPP::ArraySink(cipher.data(), cipher.size())));
    } catch (const CryptoPP::Exception& e) {
        LOG_ERROR(HW_AES, "FAILED with: %s", e.what());
    }
    return cipher;
}

std::vector<u8> DecryptVerifyCCM(const std::vector<u8>& cipher, const CCMNonce& nonce,
                                 int slot_id) {
    if (!IsNormalKeyAvailable(slot_id)) {
        LOG_ERROR(HW_AES, "Key slot %d not available. Will use zero key.", slot_id);
    }
    const AESKey normal = GetNormalKey(slot_id);
    const std::size_t pdata_size = cipher.size() - CCM_MAC_SIZE;
    std::vector<u8> pdata(pdata_size);

    try {
        CCM_3DSVariant::Decryption d;
        d.SetKeyWithIV(normal.data(), AES_BLOCK_SIZE, nonce.data(), CCM_NONCE_SIZE);
        d.SpecifyDataLengths(0, pdata_size, 0);
        CryptoPP::AuthenticatedDecryptionFilter df(
            d, new CryptoPP::ArraySink(pdata.data(), pdata_size));
        CryptoPP::ArraySource as(cipher.data(), cipher.size(), true, new CryptoPP::Redirector(df));
        if (!df.GetLastResult()) {
            LOG_ERROR(HW_AES, "FAILED");
            return {};
        }
    } catch (const CryptoPP::Exception& e) {
        LOG_ERROR(HW_AES, "FAILED with: %s", e.what());
        return {};
    }
    return pdata;
}

} // namespace AES
} // namespace HW
