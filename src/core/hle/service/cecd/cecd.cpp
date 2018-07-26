// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/bit_field.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/file_sys/archive_systemsavedata.h"
#include "core/file_sys/directory_backend.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/file_backend.h"
#include "core/hle/ipc.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/process.h"
#include "core/hle/result.h"
#include "core/hle/service/cecd/cecd.h"
#include "core/hle/service/cecd/cecd_ndm.h"
#include "core/hle/service/cecd/cecd_s.h"
#include "core/hle/service/cecd/cecd_u.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/service.h"

namespace Service {
namespace CECD {

enum class SaveDataType {
    Invalid = 0,
    MBoxList = 1,
    MBoxInfo = 2,
    InBoxInfo = 3,
    OutBoxInfo = 4,
    OutBoxIndex = 5,
    InBoxMessage = 6,
    OutBoxMessage = 7,
    RootDir = 10,
    MBoxDir = 11,
    InBoxDir = 12,
    OutBoxDir = 13,
    MBoxDataStart = 100,
    MBoxDataProgramId = 150,
    MBoxDataEnd = 199
};

union FileOption {
    u32 raw;
    BitField<1, 1, u32> read;
    BitField<2, 1, u32> write;
    BitField<3, 1, u32> make_dir;
    BitField<4, 1, u32> skip_check;
    BitField<30, 1, u32> unk_flag;
};

static Kernel::SharedPtr<Kernel::Event> cecinfo_event;
static Kernel::SharedPtr<Kernel::Event> change_state_event;

static Service::FS::ArchiveHandle cec_system_save_data_archive;
static const std::vector<u8> cec_system_savedata_id = {0x00, 0x00, 0x00, 0x00,
                                                       0x26, 0x00, 0x01, 0x00};

static std::string EncodeBase64(const std::vector<u8>& in, const std::string& dictionary) {
    std::string out;
    out.reserve((in.size() * 4) / 3);
    int b;
    for (int i = 0; i < in.size(); i += 3) {
        b = (in[i] & 0xFC) >> 2;
        out += dictionary[b];
        b = (in[i] & 0x03) << 4;
        if (i + 1 < in.size()) {
            b |= (in[i + 1] & 0xF0) >> 4;
            out += dictionary[b];
            b = (in[i + 1] & 0x0F) << 2;
            if (i + 2 < in.size()) {
                b |= (in[i + 2] & 0xC0) >> 6;
                out += dictionary[b];
                b = in[i + 2] & 0x3F;
                out += dictionary[b];
            } else {
                out += dictionary[b];
            }
        } else {
            out += dictionary[b];
        }
    }
    return out;
}

static std::string EncodeMessageId(const std::vector<u8>& in) {
    return EncodeBase64(in, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-");
}

static std::string GetSaveDataPath(SaveDataType type, u32 title_id,
                                   const std::vector<u8>& message_id = std::vector<u8>()) {
    switch (type) {
    case SaveDataType::MBoxList:
        return "/CEC/MBoxList____";
    case SaveDataType::MBoxInfo:
        return Common::StringFromFormat("/CEC/%08x/MBoxInfo____", title_id);
    case SaveDataType::InBoxInfo:
        return Common::StringFromFormat("/CEC/%08x/InBox___/BoxInfo_____", title_id);
    case SaveDataType::OutBoxInfo:
        return Common::StringFromFormat("/CEC/%08x/OutBox__/BoxInfo_____", title_id);
    case SaveDataType::OutBoxIndex:
        return Common::StringFromFormat("/CEC/%08x/OutBox__/OBIndex_____", title_id);
    case SaveDataType::InBoxMessage:
        return Common::StringFromFormat("/CEC/%08x/InBox___/_%s", title_id,
                                        EncodeMessageId(message_id).data());
    case SaveDataType::OutBoxMessage:
        return Common::StringFromFormat("/CEC/%08x/OutBox__/_%s", title_id,
                                        EncodeMessageId(message_id).data());
    case SaveDataType::RootDir:
        return "/CEC";
    case SaveDataType::MBoxDir:
        return Common::StringFromFormat("/CEC/%08x", title_id);
    case SaveDataType::InBoxDir:
        return Common::StringFromFormat("/CEC/%08x/InBox___", title_id);
    case SaveDataType::OutBoxDir:
        return Common::StringFromFormat("/CEC/%08x/OutBox__", title_id);
    }

    int index = static_cast<int>(type) - 100;
    if (index > 0 && index < 100) {
        return Common::StringFromFormat("/CEC/%08x/MBoxData.%03d", title_id, index);
    }

    UNREACHABLE();
}

static bool IsSaveDataDir(SaveDataType type) {
    switch (type) {
    case SaveDataType::RootDir:
    case SaveDataType::MBoxDir:
    case SaveDataType::InBoxDir:
    case SaveDataType::OutBoxDir:
        return true;
    default:
        return false;
    }
}

static u32 current_title_id = 0;
static SaveDataType current_save_data_type = SaveDataType::Invalid;
static FileOption current_option{};

struct InOutBoxInfoHeader {
    u16_le magic; // bb
    u16_le padding;
    u32_le box_info_size;
    u32_le max_box_size;
    u32_le box_size;
    u32_le max_message_num;
    u32_le message_num;
    u32_le max_batch_size;
    u32_le max_message_size;
};

static_assert(sizeof(InOutBoxInfoHeader) == 0x20, "Fuck");

struct MessageHeader {
    u16_le magic; // ``
    u16_le padding;
    u32_le message_size;
    u32_le header_size;
    u32_le body_size;

    u32_le title_id;
    u32_le title_id_2;
    u32_le batch_id;
    u32_le unknown_id;

    u8 message_id[8];
    u32_le version;
    u8 message_id_2[8];
    u8 flag;
    u8 send_method;
    u8 is_unopen;
    u8 is_new;
    u64_le sender_id;
    u64_le sender_id2;
    struct Time {
        u32_le a, b, c;
    } send_time, recv_time, create_time;
    u8 send_count;
    u8 foward_count;
    u16_le user_data;
};

static_assert(sizeof(MessageHeader) == 0x70, "Fuck");

ResultCode WriteInOutBoxInfo(const std::vector<u8>& data, u32 title_id,
                             SaveDataType save_data_type) {
    bool valid_input = true;
    InOutBoxInfoHeader header{};
    if (data.size() <= sizeof(InOutBoxInfoHeader)) {
        valid_input = false;
    } else {
        std::memcpy(&header, &data[0], sizeof(InOutBoxInfoHeader));
        if (header.magic != 0x6262 || header.max_box_size == 0 || header.max_message_num == 0) {
            valid_input = false;
        }
    }

    if (!valid_input) {
        FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
        FileSys::Mode mode{};
        mode.read_flag.Assign(1);
        auto file = FS::OpenFileFromArchive(cec_system_save_data_archive, path, mode);
        if (file.Succeeded()) {
            file.Unwrap()->backend->Read(0, sizeof(InOutBoxInfoHeader), (u8*)&header);
            file.Unwrap()->backend->Close();
        }
    }

    InOutBoxInfoHeader new_header{};
    if (header.magic == 0x6262 || header.magic == 0) {
        new_header.magic = 0x6262;

        if (header.max_box_size > 0x100000) {
            return ResultCode(0xC8E1086A);
        } else {
            new_header.max_box_size = header.max_box_size;
        }
        new_header.box_size = 0;

        if (header.max_message_num > 99) {
            return ResultCode(0xC8E1086A);
        } else {
            new_header.max_message_num = header.max_message_num;
        }
        new_header.message_num = 0;
        new_header.max_message_size = 0x19000;

        if (header.max_batch_size == 0) {
            new_header.max_batch_size = header.max_message_num;
        } else {
            new_header.max_batch_size = header.max_batch_size;
        }

        new_header.box_info_size = sizeof(InOutBoxInfoHeader);
    }

    // TODO
}

ResultCode Write(const std::vector<u8>& data, u32 title_id = 0,
                 SaveDataType save_data_type = SaveDataType::Invalid, FileOption option = {}) {
    if (title_id == 0 && save_data_type == SaveDataType::Invalid && option.raw == 0) {
        if (current_title_id == 0 && current_save_data_type == SaveDataType::Invalid) {
            return ResultCode(ErrorDescription::NotInitialized, ErrorModule::CEC,
                              ErrorSummary::NotFound, ErrorLevel::Usage);
        }
        title_id = current_title_id;
        save_data_type = current_save_data_type;
    }

    switch (save_data_type) {
    case SaveDataType::MBoxList: {
        // TODO
    }
    case SaveDataType::MBoxInfo: {
        ASSERT(data.size() == 0x60);
        u16_le magic;
        u32_le in_title_id;
        memcpy(&magic, &data[0], sizeof(u16));
        memcpy(&in_title_id, &data[4], sizeof(u32));
        if (magic != 0x6363 || title_id != in_title_id) {
            return ResultCode(106, ErrorModule::CEC, ErrorSummary::InvalidArgument,
                              ErrorLevel::Status);
        }
        break;
    }
    case SaveDataType::InBoxInfo:
    case SaveDataType::OutBoxInfo: {
        ResultCode result = WriteInOutBoxInfo(data, title_id, save_data_type);
        if (result.IsSuccess()) {
            current_title_id = 0;
            current_save_data_type = SaveDataType::Invalid;
        }
        return result;
    }
    case SaveDataType::OutBoxIndex:
        break;
    default:
        if (save_data_type >= SaveDataType::MBoxDataStart &&
            save_data_type <= SaveDataType::MBoxDataEnd) {
            break;
        }
    // Fall through
    case SaveDataType::InBoxMessage:
    case SaveDataType::OutBoxMessage:
    case SaveDataType::MBoxDir:
    case SaveDataType::InBoxDir:
    case SaveDataType::OutBoxDir:
        current_title_id = 0;
        current_save_data_type = SaveDataType::Invalid;
        return RESULT_SUCCESS;
    }

    FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
    FileSys::Mode mode{};
    mode.create_flag.Assign(1);
    mode.write_flag.Assign(1);
    CASCADE_RESULT(auto file, FS::OpenFileFromArchive(cec_system_save_data_archive, path, mode));
    SCOPE_EXIT({ file->backend->Close(); });

    ResultCode result = file->backend->Write(0, data.size(), true, data.data()).Code();
    if (result.IsSuccess()) {
        current_title_id = 0;
        current_save_data_type = SaveDataType::Invalid;
    }
    return result;
}

ResultVal<u32> Open(u32 title_id, SaveDataType save_data_type, FileOption option) {
    u32 size = 0;
    if (IsSaveDataDir(save_data_type)) {
        if (option.make_dir) {
            FileSys::Path root_path(GetSaveDataPath(SaveDataType::RootDir, title_id).data());
            auto root_dir = FS::OpenDirectoryFromArchive(cec_system_save_data_archive, root_path);
            if (root_dir.Failed()) {
                CASCADE_CODE(
                    FS::CreateDirectoryFromArchive(cec_system_save_data_archive, root_path));
                // TODO: SetData5
            } else {
                root_dir->get()->backend->Close();
            }

            if (save_data_type != SaveDataType::RootDir) {
                FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
                auto dir = FS::OpenDirectoryFromArchive(cec_system_save_data_archive, path);
                if (dir.Failed()) {
                    CASCADE_CODE(
                        FS::CreateDirectoryFromArchive(cec_system_save_data_archive, path));
                } else {
                    dir->get()->backend->Close();
                }
            }
        }
        FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
        CASCADE_RESULT(auto dir, FS::OpenDirectoryFromArchive(cec_system_save_data_archive, path));
        dir->backend->Close();
    } else {
        if (!option.unk_flag && !option.skip_check) {
            FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
            FileSys::Mode mode{};
            mode.read_flag.Assign(1);
            mode.write_flag.Assign(1);
            auto file_result = FS::OpenFileFromArchive(cec_system_save_data_archive, path, mode);
            if (file_result.Failed()) {
                if (!option.write) {
                    return file_result.Code();
                }
            } else {
                auto file = std::move(file_result).Unwrap();
                size = static_cast<u32>(file->backend->GetSize());
                file->backend->Close();
            }
        }
    }

    current_title_id = title_id;
    current_save_data_type = save_data_type;
    current_option.raw = option.raw;

    return MakeResult(size);
}

ResultVal<std::vector<u8>> Read(u32 size, u32 title_id = 0,
                                SaveDataType save_data_type = SaveDataType::Invalid,
                                FileOption option = {}) {
    if (title_id == 0 && save_data_type == SaveDataType::Invalid && option.raw == 0) {
        if (current_title_id == 0 && current_save_data_type == SaveDataType::Invalid) {
            return ResultCode(ErrorDescription::NotInitialized, ErrorModule::CEC,
                              ErrorSummary::NotFound, ErrorLevel::Usage);
        }
        title_id = current_title_id;
        save_data_type = current_save_data_type;
    }
    if (IsSaveDataDir(save_data_type)) {
        return ResultCode(ErrorDescription::NotAuthorized, ErrorModule::CEC, ErrorSummary::NotFound,
                          ErrorLevel::Status);
    }

    FileSys::Path path(GetSaveDataPath(save_data_type, title_id).data());
    FileSys::Mode mode{};
    mode.read_flag.Assign(1);
    CASCADE_RESULT(auto file, FS::OpenFileFromArchive(cec_system_save_data_archive, path, mode));
    SCOPE_EXIT({ file->backend->Close(); });

    std::vector<u8> buffer(size);
    CASCADE_RESULT(auto read_size, file->backend->Read(0, size, buffer.data()));

    buffer.resize(read_size);

    if (save_data_type == SaveDataType::InBoxInfo) {
        // TODO
    } else if (save_data_type == SaveDataType::OutBoxInfo) {
        // TODO
    }

    current_title_id = 0;
    current_save_data_type = SaveDataType::Invalid;

    return MakeResult(buffer);
}

void Open(Service::Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x01, 3, 2);
    u32 title_id = rp.Pop<u32>();
    auto save_data_type = static_cast<SaveDataType>(rp.Pop<u32>());
    FileOption option{rp.Pop<u32>()};
    // rp.PopPID();

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called. title_id = 0x%08X, save_data_type = %d, option = 0x%08X",
                 title_id, save_data_type, option);

    auto result = Open(title_id, save_data_type, option);
    if (result.Succeeded()) {
        if ((option.make_dir && save_data_type == SaveDataType::MBoxDir) ||
            (option.skip_check && save_data_type == SaveDataType::MBoxDataProgramId)) {
            FileOption new_option{};
            new_option.write.Assign(1);
            if (Open(title_id, SaveDataType::MBoxDataProgramId, new_option).Succeeded()) {
                std::vector<u8> program_id_data(8);
                u64_le program_id = Kernel::g_current_process->codeset->program_id;
                std::memcpy(program_id_data.data(), &program_id, sizeof(u64));
                Write(program_id_data);
            }
        }
    } else {
        if (option.read && save_data_type == SaveDataType::MBoxInfo) {
            // TODO ???
        }
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(result.Code());
    rb.Push(result.ValueOr(0));
}

void Read(Service::Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x02, 1, 2);

    u32 size = rp.Pop<u32>();
    size_t buffer_size;
    IPC::MappedBufferPermissions perm;
    VAddr buffer_address = rp.PopMappedBuffer(&buffer_size, &perm);
    ASSERT(buffer_size == size && perm == IPC::W);

    LOG_CRITICAL(Service_CECD, "(STUBBED) called. buffer_address = 0x%08X, size = 0x%X",
                 buffer_address, size);

    auto result = Read(size);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(result.Code());
    rb.Push<u32>(result.ValueOr(std::vector<u8>{}).size());
    rb.PushMappedBuffer(buffer_address, buffer_size, perm);
    if (result.Succeeded()) {
        auto data = std::move(result).Unwrap();
        Memory::WriteBlock(buffer_address, data.data(), data.size());
    }
}

void ReadMessage(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void ReadMessageWithHMAC(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void Write(Service::Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x05, 1, 2);

    u32 size = rp.Pop<u32>();
    size_t buffer_size;
    IPC::MappedBufferPermissions perm;
    VAddr buffer_address = rp.PopMappedBuffer(&buffer_size, &perm);
    ASSERT(buffer_size == size && perm == IPC::R);

    LOG_CRITICAL(Service_CECD, "(STUBBED) called. buffer_address = 0x%08X, size = 0x%X",
                 buffer_address, size);

    std::vector<u8> buffer(size);
    Memory::ReadBlock(buffer_address, buffer.data(), size);
    ResultCode result = Write(buffer);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(result);
    rb.PushMappedBuffer(buffer_address, buffer_size, perm);
}

void WriteMessage(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void WriteMessageWithHMAC(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 title_id = cmd_buff[1];
    u8 box_type = cmd_buff[2] & 0xFF;
    u32 message_id_size = cmd_buff[3];
    u32 buffer_size = cmd_buff[4];
    ASSERT(IPC::MappedBufferDesc(buffer_size, IPC::R) == cmd_buff[5]);
    VAddr buffer_addr = cmd_buff[6];
    ASSERT(IPC::MappedBufferDesc(32, IPC::R) == cmd_buff[7]);
    VAddr key_addr = cmd_buff[8];
    ASSERT(IPC::MappedBufferDesc(message_id_size, IPC::RW) == cmd_buff[9]);
    VAddr message_id_addr = cmd_buff[10];

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called. title_id = 0x%08X, box_type = %d, "
                 "message_id_addr = 0x%08X, message_id_size = 0x%X, buffer_addr = "
                 "0x%08X, buffer_size = 0x%X, "
                 "key_addr = 0x%08X",
                 title_id, box_type, message_id_addr, message_id_size, buffer_addr, buffer_size,
                 key_addr);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
}

void Delete(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 title_id = cmd_buff[1];
    SaveDataType save_data_type = static_cast<SaveDataType>(cmd_buff[2]);
    u8 box_type = cmd_buff[2] & 0xFF;
    u32 message_id_size = cmd_buff[4];
    ASSERT(IPC::MappedBufferDesc(message_id_size, IPC::R) == cmd_buff[5]);
    VAddr message_id_addr = cmd_buff[6];

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called. title_id = 0x%08X, save_data_type = %d, box_type "
                 "= %d, message_id_size = 0x%X, message_id_addr = 0x%08X",
                 title_id, save_data_type, box_type, message_id_size, message_id_addr);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
}

void cecd9(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 title_id = cmd_buff[1];
    u32 size = cmd_buff[2];
    u32 option = cmd_buff[3];
    ASSERT(IPC::MappedBufferDesc(size, IPC::R) == cmd_buff[4]);
    VAddr buffer_address = cmd_buff[5];

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called, title_id = 0x%08X, option = 0x%08X, "
                 "buffer_address = 0x%08X, size = 0x%X",
                 title_id, option, buffer_address, size);
}

enum class SystemInfoType { EulaVersion = 1, Eula = 2, ParentControl = 3 };

void GetSystemInfo(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 info_size = cmd_buff[1];
    SystemInfoType type = static_cast<SystemInfoType>(cmd_buff[2]);
    u32 param_size = cmd_buff[3];
    ASSERT(IPC::MappedBufferDesc(param_size, IPC::R) == cmd_buff[4]);
    VAddr param_addr = cmd_buff[5];
    ASSERT(IPC::MappedBufferDesc(info_size, IPC::W) == cmd_buff[6]);
    VAddr info_addr = cmd_buff[7];

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called, info_addr = 0x%08X, info_size = 0x%X, type = %d, "
                 "param_addr = 0x%08X, param_size = 0x%X",
                 info_addr, info_size, type, param_addr, param_size);

    switch (type) {
    case SystemInfoType::EulaVersion:
        if (info_size != 2) {
            cmd_buff[1] = 0xC8810BEF;
        } else {
            // TODO read from cfg
            Memory::Write16(info_addr, 0xFFFF);
            cmd_buff[1] = RESULT_SUCCESS.raw;
        }
        break;
    case SystemInfoType::Eula:
        if (info_size != 1) {
            cmd_buff[1] = 0xC8810BEF;
        } else {
            // TODO read from cfg
            Memory::Write8(info_addr, 1);
            cmd_buff[1] = RESULT_SUCCESS.raw;
        }
        break;
    case SystemInfoType::ParentControl:
        if (info_size != 1) {
            cmd_buff[1] = 0xC8810BEF;
        } else {
            // TODO read from cfg
            Memory::Write8(info_addr, 0);
            cmd_buff[1] = RESULT_SUCCESS.raw;
        }
        break;
    default:
        LOG_ERROR(Service_CECD, "Unknown system info type %d", type);
        cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    }
}

void cecdB(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void cecdC(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void GetCecStateAbbreviated(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    cmd_buff[2] = static_cast<u32>(CecStateAbbreviated::CEC_STATE_ABBREV_IDLE);

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void GetCecInfoEventHandle(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw;                                    // No error
    cmd_buff[3] = Kernel::g_handle_table.Create(cecinfo_event).Unwrap(); // Event handle

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void GetChangeStateEventHandle(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw;                                         // No error
    cmd_buff[3] = Kernel::g_handle_table.Create(change_state_event).Unwrap(); // Event handle

    LOG_CRITICAL(Service_CECD, "(STUBBED) called");
}

void OpenAndWrite(Service::Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x11, 4, 4);

    u32 size = rp.Pop<u32>();
    u32 title_id = rp.Pop<u32>();
    auto save_data_type = static_cast<SaveDataType>(rp.Pop<u32>());
    FileOption option{rp.Pop<u32>()};
    rp.Skip(2, false); // PID
    size_t buffer_size;
    IPC::MappedBufferPermissions perm;
    VAddr buffer_address = rp.PopMappedBuffer(&buffer_size, &perm);
    ASSERT(buffer_size == size && perm == IPC::R);

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called. title_id = 0x%08X, save_data_type = %d, option = "
                 "0x%08X, buffer_address = 0x%08X, size = 0x%X",
                 title_id, save_data_type, option, buffer_address, size);

    // TODO activity check

    ResultCode result = Open(title_id, save_data_type, option).Code();
    if (result.IsSuccess()) {
        std::vector<u8> buffer(size);
        Memory::ReadBlock(buffer_address, buffer.data(), size);
        result = Write(buffer);
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(result);
    rb.PushMappedBuffer(buffer_address, buffer_size, perm);
}

void OpenAndRead(Service::Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x12, 4, 4);

    u32 size = rp.Pop<u32>();
    u32 title_id = rp.Pop<u32>();
    auto save_data_type = static_cast<SaveDataType>(rp.Pop<u32>());
    FileOption option{rp.Pop<u32>()};
    rp.Skip(2, false); // PID
    size_t buffer_size;
    IPC::MappedBufferPermissions perm;
    VAddr buffer_address = rp.PopMappedBuffer(&buffer_size, &perm);
    ASSERT(buffer_size == size && perm == IPC::W);

    LOG_CRITICAL(Service_CECD,
                 "(STUBBED) called. title_id = 0x%08X, save_data_type = %d, option = "
                 "0x%08X, buffer_address = 0x%08X, size = 0x%X",
                 title_id, save_data_type, option, buffer_address, size);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);

    bool failed = false;
    auto open_result = Open(title_id, save_data_type, option);
    if (open_result.ValueOr(0) == 0 && !option.skip_check) {
        rb.Push(ResultCode(ErrorDescription::NoData, ErrorModule::CEC, ErrorSummary::NotFound,
                           ErrorLevel::Status));
        rb.Push<u32>(0);
        failed = true;
    } else {
        auto read_result = Read(size, title_id, save_data_type, option);
        rb.Push(read_result.Code());
        rb.Push<u32>(read_result.ValueOr(std::vector<u8>{}).size());
        if (read_result.Succeeded()) {
            auto data = std::move(read_result).Unwrap();
            Memory::WriteBlock(buffer_address, data.data(), data.size());
        } else {
            failed = true;
        }
    }

    if (failed) {
        if (option.read && save_data_type == SaveDataType::MBoxInfo) {
            // TODO ???
        }
    }

    rb.PushMappedBuffer(buffer_address, buffer_size, perm);
}

void Init() {
    AddService(new CECD_NDM);
    AddService(new CECD_S);
    AddService(new CECD_U);

    cecinfo_event = Kernel::Event::Create(Kernel::ResetType::OneShot, "CECD::cecinfo_event");
    change_state_event =
        Kernel::Event::Create(Kernel::ResetType::OneShot, "CECD::change_state_event");

    // Open the SystemSaveData archive 0x00010026
    FileSys::Path archive_path(cec_system_savedata_id);
    auto archive_result =
        Service::FS::OpenArchive(Service::FS::ArchiveIdCode::SystemSaveData, archive_path);

    // If the archive didn't exist, create the files inside
    if (archive_result.Code().description == FileSys::ErrCodes::NotFormatted) {
        // Format the archive to create the directories
        Service::FS::FormatArchive(Service::FS::ArchiveIdCode::SystemSaveData,
                                   FileSys::ArchiveFormatInfo(), archive_path);

        // Open it again to get a valid archive now that the folder exists
        archive_result =
            Service::FS::OpenArchive(Service::FS::ArchiveIdCode::SystemSaveData, archive_path);
    }

    ASSERT_MSG(archive_result.Succeeded(), "Could not open the CEC SystemSaveData archive!");

    cec_system_save_data_archive = *archive_result;
}

void Shutdown() {
    cecinfo_event = nullptr;
    change_state_event = nullptr;
}

} // namespace CECD

} // namespace Service
