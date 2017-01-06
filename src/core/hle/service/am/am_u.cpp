// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/service/am/am.h"
#include "core/hle/service/am/am_u.h"

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
    {0x04010080, nullptr, "UpdateFirmwareTo"},
    {0x04020040, nullptr, "BeginImportProgram"},
    {0x04030000, nullptr, "BeginImportProgramTemporarily"},
    {0x04040002, nullptr, "CancelImportProgram"},
    {0x04050002, nullptr, "EndImportProgram"},
    {0x04060002, nullptr, "EndImportProgramWithoutCommit"},
    {0x040700C2, nullptr, "CommitImportPrograms"},
    {0x04080042, nullptr, "GetProgramInfoFromCia"},
    {0x04090004, nullptr, "GetSystemMenuDataFromCia"},
    {0x040A0002, nullptr, "GetDependencyListFromCia"},
    {0x040B0002, nullptr, "GetTransferSizeFromCia"},
    {0x040C0002, nullptr, "GetCoreVersionFromCia"},
    {0x040D0042, nullptr, "GetRequiredSizeFromCia"},
    {0x040E00C2, nullptr, "CommitImportProgramsAndUpdateFirmwareAuto"},
    {0x040F0000, nullptr, "UpdateFirmwareAuto"},
    {0x041000C0, nullptr, "DeleteProgram"},
    {0x04110044, nullptr, "GetTwlProgramListForReboot"},
    {0x04120000, nullptr, "GetSystemUpdaterMutex"},
    {0x04130002, nullptr, "GetMetaSizeFromCia"},
    {0x04140044, nullptr, "GetMetaDataFromCia"},
    {0x04150080, nullptr, "CheckDemoLaunchRights"},
    {0x041600C0, nullptr, "GetInternalTitleLocationInfo"},
    {0x041700C0, nullptr, "PerpetuateAgbSaveData"},
    {0x04180040, nullptr, "BeginImportProgramForOverWrite"},
    {0x04190000, nullptr, "BeginImportSystemProgram"},
};

AM_U_Interface::AM_U_Interface() {
    Register(FunctionTable);
}

} // namespace AM
} // namespace Service
