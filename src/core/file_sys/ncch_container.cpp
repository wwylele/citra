// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <cstring>
#include <memory>
#include <cryptopp/aes.h>
#include <cryptopp/filters.h>
#include <cryptopp/modes.h>
#include <cryptopp/rsa.h>
#include <cryptopp/sha.h>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/file_sys/ncch_container.h"
#include "core/hw/aes/key.h"
#include "core/loader/loader.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// FileSys namespace

namespace FileSys {

using namespace Loader;

static const int kMaxSections = 8;   ///< Maximum number of sections (files) in an ExeFs
static const int kBlockSize = 0x200; ///< Size of ExeFS blocks (in bytes)

/**
 * Get the decompressed size of an LZSS compressed ExeFS file
 * @param buffer Buffer of compressed file
 * @param size Size of compressed buffer
 * @return Size of decompressed buffer
 */
static u32 LZSS_GetDecompressedSize(const u8* buffer, u32 size) {
    u32 offset_size;
    std::memcpy(&offset_size, buffer + size - sizeof(u32), sizeof(u32));
    return offset_size + size;
}

/**
 * Decompress ExeFS file (compressed with LZSS)
 * @param compressed Compressed buffer
 * @param compressed_size Size of compressed buffer
 * @param decompressed Decompressed buffer
 * @param decompressed_size Size of decompressed buffer
 * @return True on success, otherwise false
 */
static bool LZSS_Decompress(const u8* compressed, u32 compressed_size, u8* decompressed,
                            u32 decompressed_size) {
    const u8* footer = compressed + compressed_size - 8;

    u32 buffer_top_and_bottom;
    std::memcpy(&buffer_top_and_bottom, footer, sizeof(u32));

    u32 out = decompressed_size;
    u32 index = compressed_size - ((buffer_top_and_bottom >> 24) & 0xFF);
    u32 stop_index = compressed_size - (buffer_top_and_bottom & 0xFFFFFF);

    memset(decompressed, 0, decompressed_size);
    memcpy(decompressed, compressed, compressed_size);

    while (index > stop_index) {
        u8 control = compressed[--index];

        for (unsigned i = 0; i < 8; i++) {
            if (index <= stop_index)
                break;
            if (index <= 0)
                break;
            if (out <= 0)
                break;

            if (control & 0x80) {
                // Check if compression is out of bounds
                if (index < 2)
                    return false;
                index -= 2;

                u32 segment_offset = compressed[index] | (compressed[index + 1] << 8);
                u32 segment_size = ((segment_offset >> 12) & 15) + 3;
                segment_offset &= 0x0FFF;
                segment_offset += 2;

                // Check if compression is out of bounds
                if (out < segment_size)
                    return false;

                for (unsigned j = 0; j < segment_size; j++) {
                    // Check if compression is out of bounds
                    if (out + segment_offset >= decompressed_size)
                        return false;

                    u8 data = decompressed[out + segment_offset];
                    decompressed[--out] = data;
                }
            } else {
                // Check if compression is out of bounds
                if (out < 1)
                    return false;
                decompressed[--out] = compressed[--index];
            }
            control <<= 1;
        }
    }
    return true;
}

NCCHContainer::NCCHContainer(const std::string& filepath, u32 ncch_offset)
    : ncch_offset(ncch_offset), filepath(filepath) {
    file = FileUtil::IOFile(filepath, "rb");
}

Loader::ResultStatus NCCHContainer::OpenFile(const std::string& filepath, u32 ncch_offset) {
    this->filepath = filepath;
    this->ncch_offset = ncch_offset;
    file = FileUtil::IOFile(filepath, "rb");

    if (!file.IsOpen()) {
        LOG_WARNING(Service_FS, "Failed to open %s", filepath.c_str());
        return Loader::ResultStatus::Error;
    }

    LOG_DEBUG(Service_FS, "Opened %s", filepath.c_str());
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::Load() {
    if (is_loaded)
        return Loader::ResultStatus::Success;

    if (file.IsOpen()) {
        // Reset read pointer in case this file has been read before.
        file.Seek(ncch_offset, SEEK_SET);

        if (file.ReadBytes(&ncch_header, sizeof(NCCH_Header)) != sizeof(NCCH_Header))
            return Loader::ResultStatus::Error;

        // Skip NCSD header and load first NCCH (NCSD is just a container of NCCH files)...
        if (Loader::MakeMagic('N', 'C', 'S', 'D') == ncch_header.magic) {
            LOG_DEBUG(Service_FS, "Only loading the first (bootable) NCCH within the NCSD file!");
            ncch_offset += 0x4000;
            file.Seek(ncch_offset, SEEK_SET);
            file.ReadBytes(&ncch_header, sizeof(NCCH_Header));
        }

        // Verify we are loading the correct file type...
        if (Loader::MakeMagic('N', 'C', 'C', 'H') != ncch_header.magic)
            return Loader::ResultStatus::ErrorInvalidFormat;

        has_header = true;

        // System archives and DLC don't have an extended header but have RomFS
        if (ncch_header.extended_header_size) {
            if (file.ReadBytes(&exheader_header, sizeof(ExHeader_Header)) !=
                sizeof(ExHeader_Header))
                return Loader::ResultStatus::Error;

            if ((exheader_header.arm11_system_local_caps.program_id & 0xFFFFFFFF) !=
                (ncch_header.program_id & 0xFFFFFFFF)) {
                LOG_INFO(Loader, "The ROM is probably encrypted. Trying to decrypt...");

                AesContext exheader_aes{{}, {}};
                if (ncch_header.version == 0 || ncch_header.version == 2) {
                    LOG_INFO(Loader, "NCCH version 0/2");
                    std::reverse_copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                                      exheader_aes.ctr.begin());
                    exefs_aes = exerfs_code_aes = romfs_aes = exheader_aes;
                    exheader_aes.ctr[8] = 1;
                    exefs_aes->ctr[8] = exerfs_code_aes->ctr[8] = 2;
                    romfs_aes->ctr[8] = 3;
                } else if (ncch_header.version == 1) {
                    LOG_INFO(Loader, "MCCH version 1");
                    std::copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                              exheader_aes.ctr.begin());
                    exefs_aes = romfs_aes = exheader_aes;
                    auto u32ToBEArray = [](u32 value) -> std::array<u8, 4> {
                        return std::array<u8, 4>{(u8)(value >> 24), (u8)((value >> 16) & 0xFF),
                                                 (u8)((value >> 8) & 0xFF), (u8)(value & 0xFF)};
                    };
                    auto offset_exheader = u32ToBEArray(0x200);
                    auto offset_exefs = u32ToBEArray(ncch_header.exefs_offset * kBlockSize);
                    auto offset_romfs = u32ToBEArray(ncch_header.romfs_offset * kBlockSize);
                    std::copy(offset_exheader.begin(), offset_exheader.end(),
                              exheader_aes.ctr.begin() + 12);
                    std::copy(offset_exefs.begin(), offset_exefs.end(),
                              exefs_aes->ctr.begin() + 12);
                    std::copy(offset_romfs.begin(), offset_romfs.end(),
                              romfs_aes->ctr.begin() + 12);
                    exerfs_code_aes = exefs_aes;
                } else {
                    LOG_ERROR(Loader, "Unknown NCCH version %d !", ncch_header.version);
                    return Loader::ResultStatus::ErrorEncrypted;
                }

                if (ncch_header.flags[7] & 1) {
                    LOG_INFO(Loader, "FixedKey crypto");
                    std::array<u8, 16> zeros{};
                    exheader_aes.key = exefs_aes->key = exerfs_code_aes->key = romfs_aes->key =
                        zeros;
                } else {
                    HW::AES::InitKeys();
                    HW::AES::AESKey key_y;
                    if (ncch_header.flags[7] & 0x20) {
                        LOG_ERROR(Loader, "Seed crypto unsupported!");
                        return ResultStatus::ErrorEncrypted;
                    }

                    std::memcpy(key_y.data(), ncch_header.signature, sizeof(key_y));
                    HW::AES::SetKeyY(HW::AES::KeySlotID::NCCH, key_y);

                    if (!HW::AES::IsNormalKeyAvailable(HW::AES::KeySlotID::NCCH)) {
                        LOG_ERROR(Loader, "slot0x2CKeyX missing! cannot decrypt!");
                        return ResultStatus::ErrorEncrypted;
                    }

                    exheader_aes.key = exefs_aes->key =
                        HW::AES::GetNormalKey(HW::AES::KeySlotID::NCCH);

                    switch (ncch_header.flags[3]) {
                    case 0:
                        LOG_INFO(Loader, "Standard crypto");
                        exerfs_code_aes->key = romfs_aes->key = exheader_aes.key;
                        break;
                    case 1:
                        LOG_INFO(Loader, "7x crypto");
                        HW::AES::SetKeyY(HW::AES::KeySlotID::NCCH7x, key_y);
                        if (!HW::AES::IsNormalKeyAvailable(HW::AES::KeySlotID::NCCH7x)) {
                            LOG_ERROR(Loader, "slot0x25KeyX missing! Cannot decrypt!");
                            return Loader::ResultStatus::ErrorEncrypted;
                        }
                        exerfs_code_aes->key = romfs_aes->key =
                            HW::AES::GetNormalKey(HW::AES::KeySlotID::NCCH7x);
                        break;
                    case 0x0A:
                        LOG_INFO(Loader, "Secure3 crypto");
                        HW::AES::SetKeyY(HW::AES::KeySlotID::NCCHSec3, key_y);
                        if (!HW::AES::IsNormalKeyAvailable(HW::AES::KeySlotID::NCCHSec3)) {
                            LOG_ERROR(Loader, "slot0x18KeyX missing! Cannot decrypt!");
                            return Loader::ResultStatus::ErrorEncrypted;
                        }
                        exerfs_code_aes->key = romfs_aes->key =
                            HW::AES::GetNormalKey(HW::AES::KeySlotID::NCCHSec3);
                        break;
                    case 0x0B:
                        LOG_INFO(Loader, "Secure4 crypto");
                        HW::AES::SetKeyY(HW::AES::KeySlotID::NCCHSec4, key_y);
                        if (!HW::AES::IsNormalKeyAvailable(HW::AES::KeySlotID::NCCHSec4)) {
                            LOG_ERROR(Loader, "slot0x1BKeyX missing! Cannot decrypt!");
                            return Loader::ResultStatus::ErrorEncrypted;
                        }
                        exerfs_code_aes->key = romfs_aes->key =
                            HW::AES::GetNormalKey(HW::AES::KeySlotID::NCCHSec4);
                        break;
                    default:
                        LOG_ERROR(Loader, "Unknown crypto method! Cannot decrypt!");
                        return Loader::ResultStatus::ErrorEncrypted;
                    }
                }

                // Decrypt ExHeader
                CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption dec;
                dec.SetKeyWithIV(exheader_aes.key.data(), 16, exheader_aes.ctr.data(), 16);
                dec.ProcessData((CryptoPP::byte*)&exheader_header,
                                (CryptoPP::byte*)&exheader_header, sizeof(exheader_header));

                if (exheader_header.arm11_system_local_caps.program_id != ncch_header.program_id) {
                    LOG_ERROR(Loader, "Cannot decrypt!");
                    return Loader::ResultStatus::ErrorEncrypted;
                }

                is_compressed = (exheader_header.codeset_info.flags.flag & 1) == 1;
                u32 entry_point = exheader_header.codeset_info.text.address;
                u32 code_size = exheader_header.codeset_info.text.code_size;
                u32 stack_size = exheader_header.codeset_info.stack_size;
                u32 bss_size = exheader_header.codeset_info.bss_size;
                u32 core_version = exheader_header.arm11_system_local_caps.core_version;
                u8 priority = exheader_header.arm11_system_local_caps.priority;
                u8 resource_limit_category =
                    exheader_header.arm11_system_local_caps.resource_limit_category;

                LOG_DEBUG(Service_FS, "Name:                        %s",
                          exheader_header.codeset_info.name);
                LOG_DEBUG(Service_FS, "Program ID:                  %016" PRIX64,
                          ncch_header.program_id);
                LOG_DEBUG(Service_FS, "Code compressed:             %s",
                          is_compressed ? "yes" : "no");
                LOG_DEBUG(Service_FS, "Entry point:                 0x%08X", entry_point);
                LOG_DEBUG(Service_FS, "Code size:                   0x%08X", code_size);
                LOG_DEBUG(Service_FS, "Stack size:                  0x%08X", stack_size);
                LOG_DEBUG(Service_FS, "Bss size:                    0x%08X", bss_size);
                LOG_DEBUG(Service_FS, "Core version:                %d", core_version);
                LOG_DEBUG(Service_FS, "Thread priority:             0x%X", priority);
                LOG_DEBUG(Service_FS, "Resource limit category:     %d", resource_limit_category);
                LOG_DEBUG(Service_FS, "System Mode:                 %d",
                          static_cast<int>(exheader_header.arm11_system_local_caps.system_mode));
            }

            has_exheader = true;
        }

        // DLC can have an ExeFS and a RomFS but no extended header
        if (ncch_header.exefs_size) {
            exefs_offset = ncch_header.exefs_offset * kBlockSize;
            u32 exefs_size = ncch_header.exefs_size * kBlockSize;

            LOG_DEBUG(Service_FS, "ExeFS offset:                0x%08X", exefs_offset);
            LOG_DEBUG(Service_FS, "ExeFS size:                  0x%08X", exefs_size);

            file.Seek(exefs_offset + ncch_offset, SEEK_SET);
            if (file.ReadBytes(&exefs_header, sizeof(ExeFs_Header)) != sizeof(ExeFs_Header))
                return Loader::ResultStatus::Error;

            // Decrypt ExeFS
            if (exefs_aes) {
                CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption dec;
                dec.SetKeyWithIV(exefs_aes->key.data(), 16, exefs_aes->ctr.data(), 16);
                dec.ProcessData((CryptoPP::byte*)&exefs_header, (CryptoPP::byte*)&exefs_header,
                                sizeof(exefs_header));
            }

            exefs_file = FileUtil::IOFile(filepath, "rb");
            has_exefs = true;
        }

        if (ncch_header.romfs_offset != 0 && ncch_header.romfs_size != 0)
            has_romfs = true;
    }

    LoadOverrides();

    // We need at least one of these or overrides, practically
    if (!(has_exefs || has_romfs || is_tainted))
        return Loader::ResultStatus::Error;

    is_loaded = true;
    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::LoadOverrides() {
    // Check for split-off files, mark the archive as tainted if we will use them
    std::string romfs_override = filepath + ".romfs";
    if (FileUtil::Exists(romfs_override)) {
        is_tainted = true;
    }

    // If we have a split-off exefs file/folder, it takes priority
    std::string exefs_override = filepath + ".exefs";
    std::string exefsdir_override = filepath + ".exefsdir/";
    if (FileUtil::Exists(exefs_override)) {
        exefs_file = FileUtil::IOFile(exefs_override, "rb");

        if (exefs_file.ReadBytes(&exefs_header, sizeof(ExeFs_Header)) == sizeof(ExeFs_Header)) {
            LOG_DEBUG(Service_FS, "Loading ExeFS section from %s", exefs_override.c_str());
            exefs_offset = 0;
            is_tainted = true;
            has_exefs = true;
        } else {
            exefs_file = FileUtil::IOFile(filepath, "rb");
        }
    } else if (FileUtil::Exists(exefsdir_override) && FileUtil::IsDirectory(exefsdir_override)) {
        is_tainted = true;
    }

    if (is_tainted)
        LOG_WARNING(Service_FS,
                    "Loaded NCCH %s is tainted, application behavior may not be as expected!",
                    filepath.c_str());

    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::LoadSectionExeFS(const char* name, std::vector<u8>& buffer) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    // Check if we have files that can drop-in and replace
    result = LoadOverrideExeFSSection(name, buffer);
    if (result == Loader::ResultStatus::Success || !has_exefs)
        return result;

    // As of firmware 5.0.0-11 the logo is stored between the access descriptor and the plain region
    // instead of the ExeFS.
    if (std::strcmp(name, "logo") == 0) {
        if (ncch_header.logo_region_offset && ncch_header.logo_region_size) {
            size_t logo_offset = ncch_header.logo_region_offset * kBlockSize;
            size_t logo_size = ncch_header.logo_region_size * kBlockSize;

            buffer.resize(logo_size);
            file.Seek(ncch_offset + logo_offset, SEEK_SET);

            if (file.ReadBytes(buffer.data(), logo_size) != logo_size) {
                LOG_ERROR(Service_FS, "Could not read NCCH logo");
                return Loader::ResultStatus::Error;
            }
            return Loader::ResultStatus::Success;
        } else {
            LOG_INFO(Service_FS, "Attempting to load logo from the ExeFS");
        }
    }

    // If we don't have any separate files, we'll need a full ExeFS
    if (!exefs_file.IsOpen())
        return Loader::ResultStatus::Error;

    LOG_DEBUG(Service_FS, "%d sections:", kMaxSections);
    // Iterate through the ExeFs archive until we find a section with the specified name...
    for (unsigned section_number = 0; section_number < kMaxSections; section_number++) {
        const auto& section = exefs_header.section[section_number];

        // Load the specified section...
        if (strcmp(section.name, name) == 0) {
            LOG_DEBUG(Service_FS, "%d - offset: 0x%08X, size: 0x%08X, name: %s", section_number,
                      section.offset, section.size, section.name);

            s64 section_offset =
                (section.offset + exefs_offset + sizeof(ExeFs_Header) + ncch_offset);
            exefs_file.Seek(section_offset, SEEK_SET);

            if (strcmp(section.name, ".code") == 0 && is_compressed) {
                // Section is compressed, read compressed .code section...
                std::unique_ptr<u8[]> temp_buffer;
                try {
                    temp_buffer.reset(new u8[section.size]);
                } catch (std::bad_alloc&) {
                    return Loader::ResultStatus::ErrorMemoryAllocationFailed;
                }

                if (exefs_file.ReadBytes(&temp_buffer[0], section.size) != section.size)
                    return Loader::ResultStatus::Error;

                if (exerfs_code_aes) {
                    CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption dec;
                    dec.SetKeyWithIV(exerfs_code_aes->key.data(), 16, exerfs_code_aes->ctr.data(),
                                     16);
                    dec.Seek(section.offset + sizeof(ExeFs_Header));
                    dec.ProcessData(temp_buffer.get(), temp_buffer.get(), section.size);
                }

                // Decompress .code section...
                u32 decompressed_size = LZSS_GetDecompressedSize(&temp_buffer[0], section.size);
                buffer.resize(decompressed_size);
                if (!LZSS_Decompress(&temp_buffer[0], section.size, &buffer[0], decompressed_size))
                    return Loader::ResultStatus::ErrorInvalidFormat;
            } else {
                // Section is uncompressed...
                buffer.resize(section.size);
                if (exefs_file.ReadBytes(&buffer[0], section.size) != section.size)
                    return Loader::ResultStatus::Error;

                auto aes = strcmp(section.name, ".code") == 0 ? exerfs_code_aes : exefs_aes;
                if (aes) {
                    CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption dec;
                    dec.SetKeyWithIV(aes->key.data(), 16, aes->ctr.data(), 16);
                    dec.Seek(section.offset + sizeof(ExeFs_Header));
                    dec.ProcessData(buffer.data(), buffer.data(), section.size);
                }
            }
            return Loader::ResultStatus::Success;
        }
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::LoadOverrideExeFSSection(const char* name,
                                                             std::vector<u8>& buffer) {
    std::string override_name;

    // Map our section name to the extracted equivalent
    if (!strcmp(name, ".code"))
        override_name = "code.bin";
    else if (!strcmp(name, "icon"))
        override_name = "code.bin";
    else if (!strcmp(name, "banner"))
        override_name = "banner.bnr";
    else if (!strcmp(name, "logo"))
        override_name = "logo.bcma.lz";
    else
        return Loader::ResultStatus::Error;

    std::string section_override = filepath + ".exefsdir/" + override_name;
    FileUtil::IOFile section_file(section_override, "rb");

    if (section_file.IsOpen()) {
        auto section_size = section_file.GetSize();
        buffer.resize(section_size);

        section_file.Seek(0, SEEK_SET);
        if (section_file.ReadBytes(&buffer[0], section_size) == section_size) {
            LOG_WARNING(Service_FS, "File %s overriding built-in ExeFS file",
                        section_override.c_str());
            return Loader::ResultStatus::Success;
        }
    }
    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadRomFS(std::shared_ptr<FileUtil::IOFile>& romfs_file,
                                              u64& offset, u64& size) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    if (ReadOverrideRomFS(romfs_file, offset, size) == Loader::ResultStatus::Success)
        return Loader::ResultStatus::Success;

    if (!has_romfs) {
        LOG_DEBUG(Service_FS, "RomFS requested from NCCH which has no RomFS");
        return Loader::ResultStatus::ErrorNotUsed;
    }

    if (!file.IsOpen())
        return Loader::ResultStatus::Error;

    u32 romfs_offset = ncch_offset + (ncch_header.romfs_offset * kBlockSize) + 0x1000;
    u32 romfs_size = (ncch_header.romfs_size * kBlockSize) - 0x1000;

    LOG_DEBUG(Service_FS, "RomFS offset:           0x%08X", romfs_offset);
    LOG_DEBUG(Service_FS, "RomFS size:             0x%08X", romfs_size);

    if (file.GetSize() < romfs_offset + romfs_size)
        return Loader::ResultStatus::Error;

    // We reopen the file, to allow its position to be independent from file's
    romfs_file = std::make_shared<FileUtil::IOFile>(filepath, "rb");
    if (!romfs_file->IsOpen())
        return Loader::ResultStatus::Error;

    offset = romfs_offset;
    size = romfs_size;

    return Loader::ResultStatus::Success;
}

Loader::ResultStatus NCCHContainer::ReadOverrideRomFS(std::shared_ptr<FileUtil::IOFile>& romfs_file,
                                                      u64& offset, u64& size) {
    // Check for RomFS overrides
    std::string split_filepath = filepath + ".romfs";
    if (FileUtil::Exists(split_filepath)) {
        romfs_file = std::make_shared<FileUtil::IOFile>(split_filepath, "rb");
        if (romfs_file->IsOpen()) {
            LOG_WARNING(Service_FS, "File %s overriding built-in RomFS", split_filepath.c_str());
            offset = 0;
            size = romfs_file->GetSize();
            return Loader::ResultStatus::Success;
        }
    }

    return Loader::ResultStatus::ErrorNotUsed;
}

Loader::ResultStatus NCCHContainer::ReadProgramId(u64_le& program_id) {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return result;

    if (!has_header)
        return Loader::ResultStatus::ErrorNotUsed;

    program_id = ncch_header.program_id;
    return Loader::ResultStatus::Success;
}

bool NCCHContainer::HasExeFS() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_exefs;
}

bool NCCHContainer::HasRomFS() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_romfs;
}

bool NCCHContainer::HasExHeader() {
    Loader::ResultStatus result = Load();
    if (result != Loader::ResultStatus::Success)
        return false;

    return has_exheader;
}

} // namespace FileSys
