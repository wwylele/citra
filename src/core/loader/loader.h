// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>
#include <boost/optional.hpp>
#include "common/common_types.h"
#include "common/file_util.h"

namespace Kernel {
struct AddressMapping;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Loader namespace

namespace Loader {

/// File types supported by CTR
enum class FileType {
    Error,
    Unknown,
    CCI,
    CXI,
    CIA,
    ELF,
    THREEDSX, // 3DSX
};

/**
 * Identifies the type of a bootable file based on the magic value in its header.
 * @param file open file
 * @return FileType of file
 */
FileType IdentifyFile(FileUtil::IOFile& file);

/**
 * Identifies the type of a bootable file based on the magic value in its header.
 * @param file_name path to file
 * @return FileType of file. Note: this will return FileType::Unknown if it is unable to determine
 * a filetype, and will never return FileType::Error.
 */
FileType IdentifyFile(const std::string& file_name);

/**
 * Guess the type of a bootable file from its extension
 * @param extension String extension of bootable file
 * @return FileType of file. Note: this will return FileType::Unknown if it is unable to determine
 * a filetype, and will never return FileType::Error.
 */
FileType GuessFromExtension(const std::string& extension_);

/**
 * Convert a FileType into a string which can be displayed to the user.
 */
const char* GetFileTypeString(FileType type);

/// Return type for functions in Loader namespace
enum class ResultStatus {
    Success,
    Error,
    ErrorInvalidFormat,
    ErrorNotImplemented,
    ErrorNotLoaded,
    ErrorNotUsed,
    ErrorAlreadyLoaded,
    ErrorMemoryAllocationFailed,
    ErrorEncrypted,
};

constexpr u32 MakeMagic(char a, char b, char c, char d) {
    return a | b << 8 | c << 16 | d << 24;
}

/// Interface for loading an application
class AppLoader : NonCopyable {
public:
    AppLoader(FileUtil::IOFile&& file) : file(std::move(file)) {}
    virtual ~AppLoader() {}

    /**
     * Returns the type of this file
     * @return FileType corresponding to the loaded file
     */
    virtual FileType GetFileType() = 0;

    /**
     * Load the application
     * @return ResultStatus result of function
     */
    virtual ResultStatus Load() = 0;

    /**
     * Loads the system mode that this application needs.
     * This function defaults to 2 (96MB allocated to the application) if it can't read the
     * information.
     * @returns Optional with the kernel system mode
     */
    virtual boost::optional<u32> LoadKernelSystemMode() {
        // 96MB allocated to the application.
        return 2;
    }

    /**
     * Get the code (typically .code section) of the application
     * @param buffer Reference to buffer to store data
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadCode(std::vector<u8>& buffer) {
        return ResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the icon (typically icon section) of the application
     * @param buffer Reference to buffer to store data
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadIcon(std::vector<u8>& buffer) {
        return ResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the banner (typically banner section) of the application
     * @param buffer Reference to buffer to store data
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadBanner(std::vector<u8>& buffer) {
        return ResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the logo (typically logo section) of the application
     * @param buffer Reference to buffer to store data
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadLogo(std::vector<u8>& buffer) {
        return ResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the program id of the application
     * @param out_program_id Reference to store program id into
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadProgramId(u64& out_program_id) {
        return ResultStatus::ErrorNotImplemented;
    }

    /**
     * Get the RomFS of the application
     * Since the RomFS can be huge, we return a file reference instead of copying to a buffer
     * @param romfs_file The file containing the RomFS
     * @param offset The offset the romfs begins on
     * @param size The size of the romfs
     * @return ResultStatus result of function
     */
    virtual ResultStatus ReadRomFS(std::shared_ptr<FileUtil::IOFile>& romfs_file, u64& offset,
                                   u64& size) {
        return ResultStatus::ErrorNotImplemented;
    }

protected:
    FileUtil::IOFile file;
    bool is_loaded = false;
};

/**
 * Common address mappings found in most games, used for binary formats that don't have this
 * information.
 */
extern const std::initializer_list<Kernel::AddressMapping> default_address_mappings;

/**
 * Identifies a bootable file and return a suitable loader
 * @param filename String filename of bootable file
 * @return best loader for this file
 */
std::unique_ptr<AppLoader> GetLoader(const std::string& filename);

} // namespace
