// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/am/am.h"
#include "core/hle/service/am/am_sys.h"

namespace Service {
namespace AM {

const Interface::FunctionInfo FunctionTable[] = {
    {0x00010040, GetNumPrograms, "GetNumPrograms"},
    {0x00020082, GetProgramList, "GetProgramList"},
    {0x00030084, GetProgramInfos, "GetProgramInfos"},
    {0x000400C0, nullptr, "DeleteUserProgram"},
    {0x000500C0, nullptr, "GetProductCode"},
    {0x000600C0, nullptr, "GetStorageId"},
    {0x00070080, DeleteTicket, "DeleteTicket"},
    {0x00080000, GetNumTickets, "GetNumTickets"},
    {0x00090082, GetTicketList, "GetTicketList"},
    {0x000A0000, nullptr, "GetDeviceID"},
    {0x000B0040, nullptr, "GetNumImportTitleContexts"},
    {0x000C0082, nullptr, "GetImportTitleContextList"},
    {0x000D0084, nullptr, "GetImportTitleContexts"},
    {0x000E00C0, nullptr, "DeleteImportTitleContext"},
    {0x000F00C0, nullptr, "GetNumImportContentContexts"},
    {0x00100102, nullptr, "GetImportContentContextList"},
    {0x00110104, nullptr, "GetImportContentContexts"},
    {0x00120102, nullptr, "DeleteImportContentContexts"},
    {0x00130040, nullptr, "NeedsCleanup"},
    {0x00140040, nullptr, "DoCleanup"},
    {0x00150040, nullptr, "DeleteAllImportContexts"},
    {0x00160000, nullptr, "DeleteAllTemporaryPrograms"},
    {0x00170044, nullptr, "ImportTwlBackupLegacy"},
    {0x00180080, nullptr, "InitializeTitleDatabase"},
    {0x00190040, nullptr, "QueryAvailableTitleDatabase"},
    {0x001A00C0, nullptr, "CalcTwlBackupSize"},
    {0x001B0144, nullptr, "ExportTwlBackup"},
    {0x001C0084, nullptr, "ImportTwlBackup"},
    {0x001D0000, nullptr, "DeleteAllTwlUserPrograms"},
    {0x001E00C8, nullptr, "ReadTwlBackupInfo"},
    {0x001F0040, nullptr, "DeleteAllExpiredUserPrograms"},
    {0x00200000, nullptr, "GetTwlArchiveResourceInfo"},
    {0x00210042, nullptr, "GetPersonalizedTicketInfoList"},
    {0x00220080, nullptr, "DeleteAllImportContextsFiltered"},
    {0x00230080, nullptr, "GetNumImportTitleContextsFiltered"},
    {0x002400C2, nullptr, "GetImportTitleContextListFiltered"},
    {0x002500C0, nullptr, "CheckContentRights"},
    {0x00260044, nullptr, "GetTicketLimitInfos"},
    {0x00270044, nullptr, "GetDemoLaunchInfos"},
    {0x00280108, nullptr, "ReadTwlBackupInfoEx"},
    {0x00290082, nullptr, "DeleteUserProgramsAtomically"},
    {0x002A00C0, nullptr, "GetNumExistingContentInfosSystem"},
    {0x002B0142, nullptr, "ListExistingContentInfosSystem"},
    {0x002C0084, nullptr, "GetProgramInfosIgnorePlatform"},
    {0x002D00C0, nullptr, "CheckContentRightsIgnorePlatform"},
    {0x100100C0, GetNumContentInfos, "GetNumContentInfos"},
    {0x10020104, FindContentInfos, "FindContentInfos"},
    {0x10030142, ListContentInfos, "ListContentInfos"},
    {0x10040102, DeleteContents, "DeleteContents"},
    {0x10050084, GetDataTitleInfos, "GetDataTitleInfos"},
    {0x10060080, nullptr, "GetNumDataTitleTickets"},
    {0x10070102, ListDataTitleTicketInfos, "ListDataTitleTicketInfos"},
    {0x100801C2, nullptr, "GetItemRights"},
    {0x100900C0, nullptr, "IsDataTitleInUse"},
    {0x100A0000, nullptr, "IsExternalTitleDatabaseInitialized"},
    {0x100B00C0, nullptr, "GetNumExistingContentInfos"},
    {0x100C0142, nullptr, "ListExistingContentInfos"},
    {0x100D0084, nullptr, "GetPatchTitleInfos"},
};

AM_SYS_Interface::AM_SYS_Interface() {
    Register(FunctionTable);
}

} // namespace AM
} // namespace Service
