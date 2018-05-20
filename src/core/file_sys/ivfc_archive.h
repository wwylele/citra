// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include "common/common_types.h"
#include "common/file_util.h"
#include "core/file_sys/archive_backend.h"
#include "core/file_sys/directory_backend.h"
#include "core/file_sys/file_backend.h"
#include "core/hle/result.h"
#include "core/loader/loader.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// FileSys namespace

namespace FileSys {

class IVFCDelayGenerator : public DelayGenerator {
    u64 GetReadDelayNs(size_t length) override {
        // This is the delay measured for a romfs read.
        // For now we will take that as a default
        static constexpr u64 slope(94);
        static constexpr u64 offset(582778);
        static constexpr u64 minimum(663124);
        u64 IPCDelayNanoseconds = std::max<u64>(static_cast<u64>(length) * slope + offset, minimum);
        return IPCDelayNanoseconds;
    }
};

class RomFSDelayGenerator : public DelayGenerator {
public:
    u64 GetReadDelayNs(size_t length) override {
        // The delay was measured on O3DS and O2DS with
        // https://gist.github.com/B3n30/ac40eac20603f519ff106107f4ac9182
        // from the results the average of each length was taken.
        static constexpr u64 slope(94);
        static constexpr u64 offset(582778);
        static constexpr u64 minimum(663124);
        u64 IPCDelayNanoseconds = std::max<u64>(static_cast<u64>(length) * slope + offset, minimum);
        return IPCDelayNanoseconds;
    }
};

class ExeFSDelayGenerator : public DelayGenerator {
public:
    u64 GetReadDelayNs(size_t length) override {
        // The delay was measured on O3DS and O2DS with
        // https://gist.github.com/B3n30/ac40eac20603f519ff106107f4ac9182
        // from the results the average of each length was taken.
        static constexpr u64 slope(94);
        static constexpr u64 offset(582778);
        static constexpr u64 minimum(663124);
        u64 IPCDelayNanoseconds = std::max<u64>(static_cast<u64>(length) * slope + offset, minimum);
        return IPCDelayNanoseconds;
    }
};

/**
 * Helper which implements an interface to deal with IVFC images used in some archives
 * This should be subclassed by concrete archive types, which will provide the
 * input data (load the raw IVFC archive) and override any required methods
 */
class IVFCArchive : public ArchiveBackend {
public:
    IVFCArchive(std::shared_ptr<FileUtil::IOFile> file, u64 offset, u64 size);

    std::string GetName() const override;

    ResultVal<std::unique_ptr<FileBackend>> OpenFile(const Path& path,
                                                     const Mode& mode) const override;
    ResultCode DeleteFile(const Path& path) const override;
    ResultCode RenameFile(const Path& src_path, const Path& dest_path) const override;
    ResultCode DeleteDirectory(const Path& path) const override;
    ResultCode DeleteDirectoryRecursively(const Path& path) const override;
    ResultCode CreateFile(const Path& path, u64 size) const override;
    ResultCode CreateDirectory(const Path& path) const override;
    ResultCode RenameDirectory(const Path& src_path, const Path& dest_path) const override;
    ResultVal<std::unique_ptr<DirectoryBackend>> OpenDirectory(const Path& path) const override;
    u64 GetFreeBytes() const override;

protected:
    std::shared_ptr<FileUtil::IOFile> romfs_file;
    u64 data_offset;
    u64 data_size;
};

class IVFCFile : public FileBackend {
public:
    IVFCFile(std::shared_ptr<FileUtil::IOFile> file, u64 offset, u64 size,
             std::unique_ptr<DelayGenerator> delay_generator_,
             boost::optional<Loader::AesContext> aes_context_ = {});

    ResultVal<size_t> Read(u64 offset, size_t length, u8* buffer) const override;
    ResultVal<size_t> Write(u64 offset, size_t length, bool flush, const u8* buffer) override;
    u64 GetSize() const override;
    bool SetSize(u64 size) const override;
    bool Close() const override {
        return false;
    }
    void Flush() const override {}

private:
    std::shared_ptr<FileUtil::IOFile> romfs_file;
    u64 data_offset;
    u64 data_size;
    boost::optional<Loader::AesContext> aes_context;
};

class IVFCDirectory : public DirectoryBackend {
public:
    u32 Read(const u32 count, Entry* entries) override {
        return 0;
    }
    bool Close() const override {
        return false;
    }
};

} // namespace FileSys
