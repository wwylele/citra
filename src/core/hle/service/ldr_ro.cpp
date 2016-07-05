// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"

#include "core/arm/arm_interface.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/service/ldr_ro.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace LDR_RO

namespace LDR_RO {

static VAddr loaded_crs; ///< the virtual address of the static module

static ResultCode CROFormatError(u32 description) {
    return ResultCode(static_cast<ErrorDescription>(description), ErrorModule::RO, ErrorSummary::WrongArgument, ErrorLevel::Permanent);
}

static const ResultCode ERROR_ALREADY_INITIALIZED =
    ResultCode(ErrorDescription::AlreadyInitialized, ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Permanent);
static const ResultCode ERROR_NOT_INITIALIZED =
    ResultCode(ErrorDescription::NotInitialized,     ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Permanent);
static const ResultCode ERROR_BUFFER_TOO_SMALL =
    ResultCode(static_cast<ErrorDescription>(31),    ErrorModule::RO, ErrorSummary::InvalidArgument, ErrorLevel::Usage);
static const ResultCode ERROR_MISALIGNED_ADDRESS =
    ResultCode(ErrorDescription::MisalignedAddress,  ErrorModule::RO, ErrorSummary::WrongArgument,   ErrorLevel::Permanent);
static const ResultCode ERROR_MISALIGNED_SIZE =
    ResultCode(ErrorDescription::MisalignedSize,     ErrorModule::RO, ErrorSummary::WrongArgument,   ErrorLevel::Permanent);
static const ResultCode ERROR_ILLEGAL_ADDRESS =
    ResultCode(static_cast<ErrorDescription>(15),    ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Usage);
static const ResultCode ERROR_INVALID_CRO =
    ResultCode(static_cast<ErrorDescription>(13),    ErrorModule::RO, ErrorSummary::InvalidState,    ErrorLevel::Permanent);

static const u32 CRO_HEADER_SIZE = 0x138;
static const u32 CRO_HASH_SIZE = 0x80;

class CROHelper {
    const VAddr address; ///< the virtual address of this module

    enum class SegmentType : u32 {
        Text   = 0,
        ROData = 1,
        Data   = 2,
        BSS    = 3,
    };

    struct SegmentEntry {
        u32 offset;
        u32 size;
        SegmentType type;
    };
    static_assert(sizeof(SegmentEntry) == 12, "SegmentEntry has wrong size");

    struct ExportNamedSymbolEntry {
        u32 name_offset; // pointing to a substring in ExportStrings
        u32 symbol_segment_tag;
    };
    static_assert(sizeof(ExportNamedSymbolEntry) == 8, "ExportNamedSymbolEntry has wrong size");

    struct ExportIndexedSymbolEntry {
        u32 symbol_segment_tag;
    };
    static_assert(sizeof(ExportIndexedSymbolEntry) == 4, "ExportIndexedSymbolEntry has wrong size");

    struct ExportTreeEntry {
        u16 test_bit; // bit sddress into the name to test
        u16 left;  // the highest bit indicates whether the next entry is the last one
        u16 right; // the highest bit indicates whether the next entry is the last one
        u16 export_table_id;
    };
    static_assert(sizeof(ExportTreeEntry) == 8, "ExportTreeEntry has wrong size");

    struct ImportNamedSymbolEntry {
        u32 name_offset; // pointing to a substring in ImportStrings
        u32 patch_batch_offset; // pointing to a batch in ExternalPatchTable
    };
    static_assert(sizeof(ImportNamedSymbolEntry) == 8, "ImportNamedSymbolEntry has wrong size");

    struct ImportIndexedSymbolEntry {
        u32 index; // index in opponent's ExportIndexedSymbolEntry
        u32 patch_batch_offset; // pointing to a batch in ExternalPatchTable
    };
    static_assert(sizeof(ImportIndexedSymbolEntry) == 8, "ImportIndexedSymbolEntry has wrong size");

    struct ImportAnonymousSymbolEntry {
        u32 symbol_segment_tag; // to the opponent's segment
        u32 patch_batch_offset; // pointing to a batch in ExternalPatchTable
    };
    static_assert(sizeof(ImportAnonymousSymbolEntry) == 8, "ImportAnonymousSymbolEntry has wrong size");

    struct ImportModuleEntry {
        u32 name_offset; // pointing to a substring in ImporStrings
        u32 import_indexed_symbol_table_offset; // pointing to a subtable of ImportIndexedSymbolTable
        u32 import_indexed_symbol_num;
        u32 import_anonymous_symbol_table_offset; // pointing to a subtable of ImportAnonymousSymbolTable
        u32 import_anonymous_symbol_num;

        void GetImportIndexedSymbolEntry(u32 index, ImportIndexedSymbolEntry& entry) {
            Memory::ReadBlock(import_indexed_symbol_table_offset + index * sizeof(ImportIndexedSymbolEntry),
                &entry, sizeof(ImportIndexedSymbolEntry));
        }

        void GetImportAnonymousSymbolEntry(u32 index, ImportAnonymousSymbolEntry& entry) {
            Memory::ReadBlock(import_anonymous_symbol_table_offset + index * sizeof(ImportAnonymousSymbolEntry),
                &entry, sizeof(ImportAnonymousSymbolEntry));
        }
    };
    static_assert(sizeof(ImportModuleEntry) == 20, "ImportModuleEntry has wrong size");

    struct PatchEntry { // for ExternalPatchTable and StaticPatchTable
        u32 target_segment_tag; // to self's segment in ExternalPatchTable. to static module segment in StaticPatchTable?
        u8 type;
        u8 is_batch_end;
        u8 batch_resolved; // set at batch begin
        u8 unk3;
        u32 shift;
    };
    static_assert(sizeof(PatchEntry) == 12, "PatchEntry has wrong size");

    struct InternalPatchEntry {
        u32 target_segment_tag;
        u8 type;
        u8 value_segment_index;
        u8 unk2;
        u8 unk3;
        u32 shift;
    };
    static_assert(sizeof(InternalPatchEntry) == 12, "InternalPatchEntry has wrong size");

    struct StaticAnonymousSymbolEntry {
        u32 symbol_segment_tag;
        u32 patch_batch_offset; // pointing to a batch in StaticPatchTable
    };
    static_assert(sizeof(StaticAnonymousSymbolEntry) == 8, "StaticAnonymousSymbolEntry has wrong size");

    enum HeaderField {
        Magic = 0,
        NameOffset,
        NextCRO,
        PreviousCRO,
        FileSize,
        BssSize,
        FixedSize,
        UnknownZero,
        UnkSegmentTag,
        OnLoadSegmentTag,
        OnExitSegmentTag,
        OnUnresolvedSegmentTag,

        CodeOffset,
        CodeSize,
        DataOffset,
        DataSize,
        ModuleNameOffset,
        ModuleNameSize,
        SegmentTableOffset,
        SegmentNum,

        ExportNamedSymbolTableOffset,
        ExportNamedSymbolNum,
        ExportIndexedSymbolTableOffset,
        ExportIndexedSymbolNum,
        ExportStringsOffset,
        ExportStringsSize,
        ExportTreeTableOffset,
        ExportTreeNum,

        ImportModuleTableOffset,
        ImportModuleNum,
        ExternalPatchTableOffset,
        ExternalPatchNum,
        ImportNamedSymbolTableOffset,
        ImportNamedSymbolNum,
        ImportIndexedSymbolTableOffset,
        ImportIndexedSymbolNum,
        ImportAnonymousSymbolTableOffset,
        ImportAnonymousSymbolNum,
        ImportStringsOffset,
        ImportStringsSize,

        StaticAnonymousSymbolTableOffset,
        StaticAnonymousSymbolNum,
        InternalPatchTableOffset,
        InternalPatchNum,
        StaticPatchTableOffset,
        StaticPatchNum,
        Fix0Barrier,

        Fix3Barrier = ExportNamedSymbolTableOffset,
        Fix2Barrier = ImportModuleTableOffset,
        Fix1Barrier = StaticAnonymousSymbolTableOffset,
    };
    static_assert(Fix0Barrier == (CRO_HEADER_SIZE - CRO_HASH_SIZE) / 4, "CRO Header fields are wrong!");

    static const std::array<int, 17> ENTRY_SIZE;
    static const std::array<HeaderField, 4> FIX_BARRIERS;

    static const u32 MAGIC_CRO0;
    static const u32 MAGIC_FIXD;

    VAddr Field(HeaderField field) {
        return address + CRO_HASH_SIZE + field * 4;
    }

    u32 GetField(HeaderField field) {
        return Memory::Read32(Field(field));
    }

    void SetField(HeaderField field, u32 value) {
        Memory::Write32(Field(field), value);
    }

    VAddr Next() {
        return GetField(NextCRO);
    }

    VAddr Previous() {
        return GetField(PreviousCRO);
    }

    void SetNext(VAddr next) {
        SetField(NextCRO, next);
    }

    void SetPrevious(VAddr next) {
        SetField(PreviousCRO, next);
    }

    /**
     * Interates over all registered auto-link modules, including the static module
     * and do some operation.
     * @param func a function object to operate on a module. It accepts one parameter
     *        CROHelper and returns ResultVal<bool>. It should return true if continues interation,
     *        false if stop interation, or an error code if on error.
     * @returns ResultCode indicating the result of the operation, 0 if all iteration success,
     *         otherwise error code of the last iteration.
     */
    template <typename T>
    static ResultCode ForEachAutoLinkCRO(T func) {
        VAddr current = loaded_crs;
        while (current) {
            CROHelper cro(current);
            bool next;
            CASCADE_RESULT(next, func(cro));
            if (!next)
                break;
            current = cro.Next();
        }
        return RESULT_SUCCESS;
    }

    /**
     * Read an entry in one of module tables
     * @param field one of the ***TableOffset field indicating which table to look up// TODO how to document a template param?
     * @param T the entry type. Must match the entry type in the specified table.
     * @param index the index of the entry
     * @param data where to put the read entry.
     */
    template <HeaderField field, typename T>
    void GetEntry(int index, T& data) {
        static_assert(std::is_pod<T>::value, "The entry type must be POD!");
        Memory::ReadBlock(GetField(field) + index * sizeof(T), &data, sizeof(T));
    }

    /**
     * Writes an entry in one of module tables
     * @param field one of the ***TableOffset field indicating which table to look up// TODO how to document a template param?
     * @param T the entry type. Must match the entry type in the specified table.
     * @param index the index of the entry
     * @param data the entry data to write
     */
    template <HeaderField field, typename T>
    void SetEntry(int index, const T& data) {
        static_assert(std::is_pod<T>::value, "The entry type must be POD!");
        Memory::WriteBlock(GetField(field) + index * sizeof(T), &data, sizeof(T));
    }

    /**
     * Decodes a segment tag into segment index and offset.
     * @param segment_tag the segment tag to decode
     * @returns a tuple of (index, offset)
     */
    static std::tuple<u32, u32> DecodeSegmentTag(u32 segment_tag) {
        return std::make_tuple(segment_tag & 0xF, segment_tag >> 4);
    }

    /// Convert a segment tag to virtual address in this module. returns 0 if invalid
    VAddr SegmentTagToAddress(u32 segment_tag) {
        u32 index, offset;
        std::tie(index, offset) = DecodeSegmentTag(segment_tag);
        u32 segment_num = GetField(SegmentNum);
        if (index >= segment_num)
            return 0;
        SegmentEntry entry;
        GetEntry<SegmentTableOffset>(index, entry);
        if (offset >= entry.size)
            return 0;
        return entry.offset + offset;
    }

    /**
     * Find a exported named symbol in this module.
     * @param name the name of the symbol to find
     * @return VAddr the virtual address of the symbol. 0 if not found
     */
    VAddr FindExportNamedSymbol(const std::string& name) {
        if (!GetField(ExportTreeNum))
            return 0;
        u32 len = name.size();
        ExportTreeEntry entry;
        GetEntry<ExportTreeTableOffset>(0, entry);
        u16 next = entry.left;
        u32 found_id;
        while (1) {
            GetEntry<ExportTreeTableOffset>(next & 0x7FFF, entry);
            if (next & 0x8000) {
                found_id = entry.export_table_id;
                break;
            }
            u16 test_byte = entry.test_bit >> 3;
            u16 test_bit_in_byte = entry.test_bit & 7;
            if (test_byte >= len) {
                next = entry.left;
            } else if((name[test_byte] >> test_bit_in_byte) & 1) {
                next = entry.right;
            } else {
                next = entry.left;
            }
        }

        u32 symbol_export_num = GetField(ExportNamedSymbolNum);
        if (found_id >= symbol_export_num)
            return 0;

        u32 export_strings_size = GetField(ExportStringsSize);
        ExportNamedSymbolEntry symbol_entry;
        GetEntry<ExportNamedSymbolTableOffset>(found_id, symbol_entry);
        if (Memory::GetString(symbol_entry.name_offset, export_strings_size) != name)
            return 0;
        return SegmentTagToAddress(symbol_entry.symbol_segment_tag);
    }

    /// Rebases offsets in module header according to module address
    ResultCode RebaseHeader(u32 cro_size) {
        ResultCode error = CROFormatError(0x11);

        // verifies magic
        if (GetField(Magic) != MAGIC_CRO0)
            return error;

        // verifies not registered
        if (GetField(NextCRO) || GetField(PreviousCRO))
            return error;

        // This seems to be a hard limit set by the RO module
        if (GetField(FileSize) > 0x10000000 || GetField(BssSize) > 0x10000000)
            return error;

        // verifies not fixed
        if (GetField(FixedSize))
            return error;

        if (GetField(CodeOffset) < CRO_HEADER_SIZE)
            return error;

        // verifies all offsets are in the correct order
        constexpr std::array<HeaderField, 18> OFFSET_ORDER = {{
            CodeOffset,
            ModuleNameOffset,
            SegmentTableOffset,
            ExportNamedSymbolTableOffset,
            ExportTreeTableOffset,
            ExportIndexedSymbolTableOffset,
            ExportStringsOffset,
            ImportModuleTableOffset,
            ExternalPatchTableOffset,
            ImportNamedSymbolTableOffset,
            ImportIndexedSymbolTableOffset,
            ImportAnonymousSymbolTableOffset,
            ImportStringsOffset,
            StaticAnonymousSymbolTableOffset,
            InternalPatchTableOffset,
            StaticPatchTableOffset,
            DataOffset,
            FileSize
        }};

        u32 prev_offset = GetField(OFFSET_ORDER[0]), cur_offset;
        for (int i = 1; i < OFFSET_ORDER.size(); ++i) {
            cur_offset = GetField(OFFSET_ORDER[i]);
            if (cur_offset < prev_offset)
                return error;
            prev_offset = cur_offset;
        }

        // rebases offsets
        u32 offset = GetField(NameOffset);
        if (offset)
            SetField(NameOffset, offset + address);

        for (int field = CodeOffset; field < Fix0Barrier; field += 2) {
            HeaderField header_field = static_cast<HeaderField>(field);
            offset = GetField(header_field);
            if (offset)
                SetField(header_field, offset + address);
        }

        // verifies everything is not beyond the buffer
        u32 file_end = address + cro_size;
        for (int field = CodeOffset, i = 0; field < Fix0Barrier; field += 2, ++i) {
            HeaderField offset_field = static_cast<HeaderField>(field);
            HeaderField size_field = static_cast<HeaderField>(field + 1);
            if (GetField(offset_field) + GetField(size_field) * ENTRY_SIZE[i] > file_end)
                return error;
        }

        return RESULT_SUCCESS;
    }

    /// Verify a string to be terminate by 0
    static ResultCode VerifyString(VAddr address, u32 size) {
        if (size) {
            if (Memory::Read8(address + size - 1) != 0)
                return CROFormatError(0x0B);
        }
        return RESULT_SUCCESS;
    }


    /**
     * Rebases offsets in segment table according to module address.
     * @param cro_size the size of the CRO file
     * @param data_segment_address buffer address for .data segment
     * @param data_segment_size the buffer size for .data segment
     * @param bss_segment_address the buffer address for .bss segment
     * @param bss_segment_size the buffer size for .bss segment
     * @returns ResultVal<u32> with the previous data segment offset before rebasing
     */
    ResultVal<u32> RebaseSegmentTable(u32 cro_size,
        VAddr data_segment_address, u32 data_segment_size,
        VAddr bss_segment_address, u32 bss_segment_size) {
        u32 prev_data_segment = 0;
        u32 segment_num = GetField(SegmentNum);
        for (u32 i = 0; i < segment_num; ++i) {
            SegmentEntry segment;
            GetEntry<SegmentTableOffset>(i, segment);
            if (segment.type == SegmentType::Data) {
                if (segment.size) {
                    if (segment.size > data_segment_size)
                        return ERROR_BUFFER_TOO_SMALL;
                    prev_data_segment = segment.offset;
                    segment.offset = data_segment_address;
                }
            } else if (segment.type == SegmentType::BSS) {
                if (segment.size) {
                    if (segment.size > bss_segment_size)
                        return ERROR_BUFFER_TOO_SMALL;
                    segment.offset = bss_segment_address;
                }
            } else if (segment.offset) {
                segment.offset += address;
                if (segment.offset > address + cro_size)
                    return CROFormatError(0x19);
            }
            SetEntry<SegmentTableOffset>(i, segment);
        }
        return MakeResult<u32>(prev_data_segment);
    }

    /// Rebases offsets in exported named symbol table according to module address
    ResultCode RebaseExportNamedSymbolTable() {
        VAddr export_strings_offset = GetField(ExportStringsOffset);
        VAddr export_strings_end = export_strings_offset + GetField(ExportStringsSize);

        u32 symbol_export_num = GetField(ExportNamedSymbolNum);
        for (u32 i = 0; i < symbol_export_num; ++i) {
            ExportNamedSymbolEntry entry;
            GetEntry<ExportNamedSymbolTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset += address;
                if (entry.name_offset < export_strings_offset
                    || entry.name_offset >= export_strings_end) {
                    return CROFormatError(0x11);
                }
            }
            SetEntry<ExportNamedSymbolTableOffset>(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /// Verifies indeces in export tree table
    ResultCode VerifyExportTreeTable() {
        u32 tree_num = GetField(ExportTreeNum);
        for (u32 i =0; i < tree_num; ++i) {
            ExportTreeEntry entry;
            GetEntry<ExportTreeTableOffset>(i, entry);
            if ((entry.left & 0x7FFF) >= tree_num || (entry.right & 0x7FFF) >= tree_num) {
                return CROFormatError(0x11);
            }
        }
        return RESULT_SUCCESS;
    }

    /// Rebases offsets in exported module table according to module address
    ResultCode RebaseImportModuleTable() {
        VAddr import_strings_offset = GetField(ImportStringsOffset);
        VAddr import_strings_end = import_strings_offset + GetField(ImportStringsSize);
        VAddr import_indexed_symbol_table_offset = GetField(ImportIndexedSymbolTableOffset);
        VAddr index_import_table_end = import_indexed_symbol_table_offset + GetField(ImportIndexedSymbolNum) * sizeof(ImportIndexedSymbolEntry);
        VAddr import_anonymous_symbol_table_offset = GetField(ImportAnonymousSymbolTableOffset);
        VAddr offset_import_table_end = import_anonymous_symbol_table_offset + GetField(ImportAnonymousSymbolNum) * sizeof(ImportAnonymousSymbolEntry);

        u32 object_num = GetField(ImportModuleNum);
        for (u32 i = 0; i < object_num; ++i) {
            ImportModuleEntry entry;
            GetEntry<ImportModuleTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset += address;
                if (entry.name_offset < import_strings_offset
                    || entry.name_offset >= import_strings_end) {
                    return CROFormatError(0x18);
                }
            }
            if (entry.import_indexed_symbol_table_offset) {
                entry.import_indexed_symbol_table_offset += address;
                if (entry.import_indexed_symbol_table_offset < import_indexed_symbol_table_offset
                    || entry.import_indexed_symbol_table_offset > index_import_table_end) {
                    return CROFormatError(0x18);
                }
            }
            if (entry.import_anonymous_symbol_table_offset) {
                entry.import_anonymous_symbol_table_offset += address;
                if (entry.import_anonymous_symbol_table_offset < import_anonymous_symbol_table_offset
                    || entry.import_anonymous_symbol_table_offset > offset_import_table_end) {
                    return CROFormatError(0x18);
                }
            }
            SetEntry<ImportModuleTableOffset>(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /// Rebases offsets in imported named symbol table according to module address
    ResultCode RebaseImportNamedSymbolTable() {
        VAddr import_strings_offset = GetField(ImportStringsOffset);
        VAddr import_strings_end = import_strings_offset + GetField(ImportStringsSize);
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(PatchEntry);

        u32 num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset += address;
                if (entry.name_offset < import_strings_offset
                    || entry.name_offset >= import_strings_end) {
                    return CROFormatError(0x1B);
                }
            }
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset += address;
                if (entry.patch_batch_offset < external_patch_table_offset
                    || entry.patch_batch_offset > external_patch_table_end) {
                    return CROFormatError(0x1B);
                }
            }
            SetEntry<ImportNamedSymbolTableOffset>(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /// Rebases offsets in imported indexed symbol table according to module address
    ResultCode RebaseImportIndexedSymbolTable() {
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(PatchEntry);

        u32 num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry<ImportIndexedSymbolTableOffset>(i, entry);
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset += address;
                if (entry.patch_batch_offset < external_patch_table_offset
                    || entry.patch_batch_offset > external_patch_table_end) {
                    return CROFormatError(0x14);
                }
            }
            SetEntry<ImportIndexedSymbolTableOffset>(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /// Rebases offsets in imported anonymous symbol table according to module address
    ResultCode RebaseImportAnonymousSymbolTable() {
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(PatchEntry);

        u32 num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry<ImportAnonymousSymbolTableOffset>(i, entry);
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset += address;
                if (entry.patch_batch_offset < external_patch_table_offset
                    || entry.patch_batch_offset > external_patch_table_end) {
                    return CROFormatError(0x17);
                }
            }
            SetEntry<ImportAnonymousSymbolTableOffset>(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Applies a patch
     * @param target_address where to apply the patch
     * @param patch_type the type of the patch
     * @param shift address shift apply to the patched symbol
     * @param symbol_address the symbol address to be patched with
     * @param target_future_address the future address of the target.
     *        Usually equals to target_address, but will be different for a target in .data segment
     * @returns ResultCode indicating the result of the operation, 0 on success
     */
    ResultCode ApplyPatch(VAddr target_address, u8 patch_type, u32 shift, u32 symbol_address, u32 target_future_address) {
        switch (patch_type) {
            case 2:
                Memory::Write32(target_address, symbol_address + shift); // writes an obsolute address value
                break;
            case 3:
                Memory::Write32(target_address, symbol_address + shift - target_future_address); // writes an relative address value
                break;
            // TODO implement more types
            default:
                return CROFormatError(0x23);
        }
        return RESULT_SUCCESS;
    }

    /// Resets all external patches to unresolved state.
    ResultCode ResetAllExternalPatches() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));
        u32 external_patch_num = GetField(ExternalPatchNum);
        PatchEntry patch;

        // verify that the last patch is the end of a batch
        GetEntry<ExternalPatchTableOffset>(external_patch_num - 1, patch);
        if (!patch.is_batch_end) {
            return CROFormatError(0x12);
        }

        bool batch_begin = true;
        for (u32 i = 0; i < external_patch_num; ++i) {
            GetEntry<ExternalPatchTableOffset>(i, patch);
            VAddr patch_target = SegmentTagToAddress(patch.target_segment_tag);
            if (patch_target == 0) {
                return CROFormatError(0x12);
            }
            ResultCode result = ApplyPatch(patch_target, patch.type, patch.shift, unresolved_symbol, patch_target);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch %08X", result.raw);
                return result;
            }

            if (batch_begin) {
                patch.batch_resolved = 0; // reset to unresolved state
                SetEntry<ExternalPatchTableOffset>(i, patch);
            }

            batch_begin = patch.is_batch_end != 0; // current is end, next is begin
        }

        return RESULT_SUCCESS;
    }

    /**
     * Applies or resets a batch of patch
     * @param batch the virtual address of the first patch in the batch
     * @param symbol_address the symbol address to be patched with
     * @param reset false to set the batch to resolved state, true to set the batch to unresolved state
     * @returns ResultCode indicating the result of the operation, 0 on success
     */
    ResultCode ApplyPatchBatch(VAddr batch, u32 symbol_address, bool reset = false) {
        if (symbol_address == 0 && !reset)
            return CROFormatError(0x10);

        VAddr patch_address = batch;
        while (true) {
            PatchEntry patch;
            Memory::ReadBlock(patch_address, &patch, sizeof(PatchEntry));

            VAddr patch_target = SegmentTagToAddress(patch.target_segment_tag);
            if (patch_target == 0) {
                return CROFormatError(0x12);
            }
            ResultCode result = ApplyPatch(patch_target, patch.type, patch.shift, symbol_address, patch_target);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch %08X", result.raw);
                return result;
            }

            if (patch.is_batch_end)
                break;

            patch_address += sizeof(PatchEntry);
        }

        PatchEntry patch;
        Memory::ReadBlock(batch, &patch, sizeof(PatchEntry));
        patch.batch_resolved = reset ? 0 : 1;
        Memory::WriteBlock(batch, &patch, sizeof(PatchEntry));
        return RESULT_SUCCESS;
    }

    /// Applies all static anonymous symbol to the static module // TODO ???
    ResultCode ApplyStaticAnonymousSymbolToCRS() {
        VAddr static_patch_table_offset = GetField(StaticPatchTableOffset);
        VAddr static_patch_table_end = static_patch_table_offset + GetField(StaticPatchNum) * sizeof(PatchEntry);

        CROHelper crs(loaded_crs);
        u32 offset_export_num = GetField(StaticAnonymousSymbolNum);
        LOG_INFO(Service_LDR, "CRO \"%s\" exports %d static anonymous symbols", ModuleName().data(), offset_export_num);
        for (u32 i = 0; i < offset_export_num; ++i) {
            StaticAnonymousSymbolEntry entry;
            GetEntry<StaticAnonymousSymbolTableOffset>(i, entry);
            u32 batch_address = entry.patch_batch_offset + address;

            if (batch_address < static_patch_table_offset
                || batch_address > static_patch_table_end) {
                return CROFormatError(0x16);
            }

            u32 symbol_address = SegmentTagToAddress(entry.symbol_segment_tag);
            LOG_TRACE(Service_LDR, "CRO \"%s\" exports 0x%08X to the static module", ModuleName().data(), symbol_address);
            ResultCode result = crs.ApplyPatchBatch(batch_address, symbol_address);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Applies all internal patches to the module itself
    ResultCode ApplyInternalPatches(u32 old_data_segment_address) {
        u32 segment_num = GetField(SegmentNum);
        u32 internal_patch_num = GetField(InternalPatchNum);
        for (u32 i = 0; i < internal_patch_num; ++i) {
            InternalPatchEntry patch;
            GetEntry<InternalPatchTableOffset>(i, patch);
            VAddr target_addressB = SegmentTagToAddress(patch.target_segment_tag);
            if (target_addressB == 0) {
                return CROFormatError(0x15);
            }

            VAddr target_address;
            u32 target_segment_index, target_segment_offset;
            std::tie(target_segment_index, target_segment_offset) = DecodeSegmentTag(patch.target_segment_tag);
            SegmentEntry target_segment;
            GetEntry<SegmentTableOffset>(target_segment_index, target_segment);
            if (target_segment.type == SegmentType::Data) {
                // If the patch is to the .data segment, we need to patch it in the old buffer
                target_address = old_data_segment_address + target_segment_offset;
            } else {
                target_address = target_addressB;
            }

            if (patch.value_segment_index >= segment_num) {
                return CROFormatError(0x15);
            }

            SegmentEntry value_segment;
            GetEntry<SegmentTableOffset>(patch.value_segment_index, value_segment);
            LOG_TRACE(Service_LDR, "Internally patches 0x%08X with 0x%08X", target_address, value_segment.offset);
            ResultCode result = ApplyPatch(target_address, patch.type, patch.shift, value_segment.offset, target_addressB);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Unrebases offsets in imported anonymous symbol table
    void UnrebaseImportAnonymousSymbolTable() {
        u32 num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry<ImportAnonymousSymbolTableOffset>(i, entry);
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }
            SetEntry<ImportAnonymousSymbolTableOffset>(i, entry);
        }
    }

    /// Unrebases offsets in imported indexed symbol table
    void UnrebaseImportIndexedSymbolTable() {
        u32 num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry<ImportIndexedSymbolTableOffset>(i, entry);
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }
            SetEntry<ImportIndexedSymbolTableOffset>(i, entry);
        }
    }

    /// Unrebases offsets in imported named symbol table
    void UnrebaseImportNamedSymbolTable() {
        u32 num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset -= address;
            }
            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }
            SetEntry<ImportNamedSymbolTableOffset>(i, entry);
        }
    }

    /// Unrebases offsets in imported module table
    void UnrebaseImportModuleTable() {
        u32 object_num = GetField(ImportModuleNum);
        for (u32 i = 0; i < object_num; ++i) {
            ImportModuleEntry entry;
            GetEntry<ImportModuleTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset -= address;
            }
            if (entry.import_indexed_symbol_table_offset) {
                entry.import_indexed_symbol_table_offset -= address;
            }
            if (entry.import_anonymous_symbol_table_offset) {
                entry.import_anonymous_symbol_table_offset -= address;
            }
            SetEntry<ImportModuleTableOffset>(i, entry);
        }
    }

    /// Unrebases offsets in exported named symbol table
    void UnrebaseExportNamedSymbolTable() {
        u32 symbol_export_num = GetField(ExportNamedSymbolNum);
        for (u32 i = 0; i < symbol_export_num; ++i) {
            ExportNamedSymbolEntry entry;
            GetEntry<ExportNamedSymbolTableOffset>(i, entry);
            if (entry.name_offset) {
                entry.name_offset -= address;
            }
            SetEntry<ExportNamedSymbolTableOffset>(i, entry);
        }
    }

    /// Unrebases offsets in segment table
    void UnrebaseSegmentTable() {
        u32 segment_num = GetField(SegmentNum);
        for (u32 i = 0; i < segment_num; ++i) {
            SegmentEntry segment;
            GetEntry<SegmentTableOffset>(i, segment);
            if (segment.type == SegmentType::BSS) {
                segment.offset = 0;
            } else if (segment.offset) {
                segment.offset -= address;
            }
            SetEntry<SegmentTableOffset>(i, segment);
        }
    }

    /// Unrebases offsets in module header
    void UnrebaseHeader() {
        u32 offset = GetField(NameOffset);
        if (offset)
            SetField(NameOffset, offset - address);

        for (int field = CodeOffset; field < Fix0Barrier; field += 2) {
            HeaderField header_field = static_cast<HeaderField>(field);
            offset = GetField(header_field);
            if (offset)
                SetField(header_field, offset - address);
        }
    }

    /// Looks up all imported named symbols of this module in all registered auto-link modules, and resolves them if found
    ResultCode ApplyImportNamedSymbol() {
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            if (!patch_entry.batch_resolved) {
                ResultCode result = ForEachAutoLinkCRO([&](CROHelper source) -> ResultVal<bool> {
                    std::string symbol_name = Memory::GetString(entry.name_offset, import_strings_size);
                    u32 symbol_address = source.FindExportNamedSymbol(symbol_name);
                    if (symbol_address) {
                        LOG_TRACE(Service_LDR, "CRO \"%s\" imports \"%s\" from \"%s\"",
                            ModuleName().data(), symbol_name.data(), source.ModuleName().data());
                        ResultCode result = ApplyPatchBatch(patch_addr, symbol_address);
                        if (result.IsError()) {
                            LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                            return result;
                        }
                        return MakeResult<bool>(false);
                    }
                    return MakeResult<bool>(true);
                });
                if (result.IsError()) {
                    return result;
                }
            }
        }
        return RESULT_SUCCESS;
    }

    /// Resets all imported named symbols of this module to unresolved state
    ResultCode ResetImportNamedSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }

        }
        return RESULT_SUCCESS;
    }

    /// Resets all imported indexed symbols of this module to unresolved state
    ResultCode ResetImportIndexedSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));

        u32 import_num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < import_num; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry<ImportIndexedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Resets all imported anonymous symbols of this module to unresolved state
    ResultCode ResetImportAnonymousSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));

        u32 import_num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < import_num; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry<ImportAnonymousSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Finds registered auto-link modules that this module imports, and resolve indexed and anonymous symbols exported by them
    ResultCode ApplyModuleImport() {
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 import_module_num = GetField(ImportModuleNum);
        for (u32 i = 0; i < import_module_num; ++i) {
            ImportModuleEntry entry;
            GetEntry<ImportModuleTableOffset>(i, entry);
            std::string want_cro_name = Memory::GetString(entry.name_offset, import_strings_size);

            ResultCode result = ForEachAutoLinkCRO([&](CROHelper source) -> ResultVal<bool> {
                if (want_cro_name == source.ModuleName()) {
                    LOG_INFO(Service_LDR, "CRO \"%s\" imports %d indexed symbols from \"%s\"",
                        ModuleName().data(), entry.import_indexed_symbol_num, source.ModuleName().data());
                    for (u32 j = 0; j < entry.import_indexed_symbol_num; ++j) {
                        ImportIndexedSymbolEntry im;
                        entry.GetImportIndexedSymbolEntry(j, im);
                        ExportIndexedSymbolEntry ex;
                        source.GetEntry<ExportIndexedSymbolTableOffset>(im.index, ex);
                        u32 symbol_address = source.SegmentTagToAddress(ex.symbol_segment_tag);
                        LOG_TRACE(Service_LDR, "    Imports 0x%08X", symbol_address);
                        ResultCode result = ApplyPatchBatch(im.patch_batch_offset, symbol_address);
                        if (result.IsError()) {
                            LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                            return result;
                        }
                    }
                    LOG_INFO(Service_LDR, "CRO \"%s\" imports %d anonymous symbols from \"%s\"",
                        ModuleName().data(), entry.import_anonymous_symbol_num, source.ModuleName().data());
                    for (u32 j = 0; j < entry.import_anonymous_symbol_num; ++j) {
                        ImportAnonymousSymbolEntry im;
                        entry.GetImportAnonymousSymbolEntry(j, im);
                        u32 symbol_address = source.SegmentTagToAddress(im.symbol_segment_tag);
                        LOG_TRACE(Service_LDR, "    Imports 0x%08X", symbol_address);
                        ResultCode result = ApplyPatchBatch(im.patch_batch_offset, symbol_address);
                        if (result.IsError()) {
                            LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                            return result;
                        }
                    }
                    return MakeResult<bool>(false);
                }
                return MakeResult<bool>(true);
            });
            if (result.IsError()) {
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Resolves target module's imported named symbols that exported by this module
    ResultCode ApplyExportNamedSymbol(CROHelper target) {
        LOG_DEBUG(Service_LDR, "CRO \"%s\" exports named symbols to \"%s\"",
            ModuleName().data(), target.ModuleName().data());
        u32 target_import_strings_size = target.GetField(ImportStringsSize);
        u32 target_symbol_import_num = target.GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < target_symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            target.GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            if (!patch_entry.batch_resolved) {
                std::string symbol_name = Memory::GetString(entry.name_offset, target_import_strings_size);
                u32 symbol_address = FindExportNamedSymbol(symbol_name);
                if (symbol_address) {
                    LOG_TRACE(Service_LDR, "    exports symbol \"%s\"", symbol_name.data());
                    ResultCode result = target.ApplyPatchBatch(patch_addr, symbol_address);
                    if (result.IsError()) {
                        LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                        return result;
                    }
                }
            }
        }
        return RESULT_SUCCESS;
    }

    /// Reset target's named symbols imported from this module to unresolved state
    ResultCode ResetExportNamedSymbol(CROHelper target) {
        LOG_DEBUG(Service_LDR, "CRO \"%s\" unexports named symbols to \"%s\"",
            ModuleName().data(), target.ModuleName().data());
        u32 unresolved_symbol = target.SegmentTagToAddress(target.GetField(OnUnresolvedSegmentTag));
        u32 target_import_strings_size = target.GetField(ImportStringsSize);
        u32 target_symbol_import_num = target.GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < target_symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            target.GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            if (!patch_entry.batch_resolved) {
                std::string symbol_name = Memory::GetString(entry.name_offset, target_import_strings_size);
                u32 symbol_address = FindExportNamedSymbol(symbol_name);
                if (symbol_address) {
                    LOG_TRACE(Service_LDR, "    unexports symbol \"%s\"", symbol_name.data());
                    ResultCode result = target.ApplyPatchBatch(patch_addr, unresolved_symbol, true);
                    if (result.IsError()) {
                        LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                        return result;
                    }
                }
            }
        }
        return RESULT_SUCCESS;
    }

    /// Resolves imported indexed and anonymous symbols in the target module which imported this module
    ResultCode ApplyModuleExport(CROHelper target) {
        std::string module_name = ModuleName();
        u32 target_import_string_size = target.GetField(ImportStringsSize);
        u32 target_import_module_num = target.GetField(ImportModuleNum);
        for (u32 i = 0; i < target_import_module_num; ++i) {
            ImportModuleEntry entry;
            target.GetEntry<ImportModuleTableOffset>(i, entry);

            if (Memory::GetString(entry.name_offset, target_import_string_size) != module_name)
                continue;

            LOG_INFO(Service_LDR, "CRO \"%s\" exports %d indexed symbols to \"%s\"",
                module_name.data(), entry.import_indexed_symbol_num, target.ModuleName().data());
            for (u32 j = 0; j < entry.import_indexed_symbol_num; ++j) {
                ImportIndexedSymbolEntry im;
                entry.GetImportIndexedSymbolEntry(j, im);
                ExportIndexedSymbolEntry ex;
                GetEntry<ExportIndexedSymbolTableOffset>(im.index, ex);
                u32 symbol_address = SegmentTagToAddress(ex.symbol_segment_tag);
                LOG_TRACE(Service_LDR, "    exports symbol 0x%08X", symbol_address);
                ResultCode result = target.ApplyPatchBatch(im.patch_batch_offset, symbol_address);
                if (result.IsError()) {
                    LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                    return result;
                }
            }

            LOG_INFO(Service_LDR, "CRO \"%s\" exports %d anonymous symbols to \"%s\"",
                module_name.data(), entry.import_anonymous_symbol_num, target.ModuleName().data());
            for (u32 j = 0; j < entry.import_anonymous_symbol_num; ++j) {
                ImportAnonymousSymbolEntry im;
                entry.GetImportAnonymousSymbolEntry(j, im);
                u32 symbol_address = SegmentTagToAddress(im.symbol_segment_tag);
                LOG_TRACE(Service_LDR, "    exports symbol 0x%08X", symbol_address);
                ResultCode result = target.ApplyPatchBatch(im.patch_batch_offset, symbol_address);
                if (result.IsError()) {
                    LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                    return result;
                }
            }
        }

        return RESULT_SUCCESS;
    }

    /// Reset target's indexed and anonymous symbol imported from this module to unresolved state
    ResultCode ResetModuleExport(CROHelper target) {
        u32 unresolved_symbol = target.SegmentTagToAddress(target.GetField(OnUnresolvedSegmentTag));

        std::string module_name = ModuleName();
        u32 target_import_string_size = target.GetField(ImportStringsSize);
        u32 target_import_module_num = target.GetField(ImportModuleNum);
        for (u32 i = 0; i < target_import_module_num; ++i) {
            ImportModuleEntry entry;
            target.GetEntry<ImportModuleTableOffset>(i, entry);

            if (Memory::GetString(entry.name_offset, target_import_string_size) != module_name)
                continue;

            LOG_DEBUG(Service_LDR, "CRO \"%s\" unexports indexed symbols to \"%s\"",
                module_name.data(), target.ModuleName().data());
            for (u32 j = 0; j < entry.import_indexed_symbol_num; ++j) {
                ImportIndexedSymbolEntry im;
                entry.GetImportIndexedSymbolEntry(j, im);
                ResultCode result = target.ApplyPatchBatch(im.patch_batch_offset, unresolved_symbol, true);
                if (result.IsError()) {
                    LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                    return result;
                }
            }

            LOG_DEBUG(Service_LDR, "CRO \"%s\" unexports anonymous symbols to \"%s\"",
                module_name.data(), target.ModuleName().data());
            for (u32 j = 0; j < entry.import_anonymous_symbol_num; ++j) {
                ImportAnonymousSymbolEntry im;
                entry.GetImportAnonymousSymbolEntry(j, im);
                ResultCode result = target.ApplyPatchBatch(im.patch_batch_offset, unresolved_symbol, true);
                if (result.IsError()) {
                    LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                    return result;
                }
            }
        }

        return RESULT_SUCCESS;
    }

    /// Resolve the exit function in this module
    ResultCode ApplyExitPatches() {
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry<ImportNamedSymbolTableOffset>(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            PatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(PatchEntry));

            if (Memory::GetString(entry.name_offset, import_strings_size) == "__aeabi_atexit"){
                // TODO verify this code
                ResultCode result = ForEachAutoLinkCRO([&](CROHelper source) -> ResultVal<bool> {
                    u32 symbol_address = source.FindExportNamedSymbol("nnroAeabiAtexit_");
                    if (symbol_address) {
                        LOG_DEBUG(Service_LDR, "CRP \"%s\" import exit function from \"%s\"",
                            ModuleName().data(), source.ModuleName().data());
                        ResultCode result = ApplyPatchBatch(patch_addr, symbol_address);
                        if (result.IsError()) {
                            LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                            return result;
                        }
                        return MakeResult<bool>(false);
                    }
                    return MakeResult<bool>(true);
                });
                if (result.IsError()) {
                    LOG_ERROR(Service_LDR, "Error applying exit patch %08X", result.raw);
                    return result;
                }
            }
        }
        return RESULT_SUCCESS;
    }

public:
    CROHelper(VAddr cro_address) : address(cro_address) {
    }

    std::string ModuleName() {
        return Memory::GetString(GetField(ModuleNameOffset), GetField(ModuleNameSize));
    }

    u32 GetFileSize() {
        return GetField(FileSize);
    }

    u32 GetFixedSize() {
        return GetField(FixedSize);
    }

    /// Rebases the module according to its address
    ResultCode Rebase(u32 cro_size, VAddr data_segment_addresss, u32 data_segment_size, VAddr bss_segment_address, u32 bss_segment_size, bool is_crs = false) {
        ResultCode result = RebaseHeader(cro_size);
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing header %08X", result.raw);
            return result;
        }

        result = VerifyString(GetField(ModuleNameOffset), GetField(ModuleNameSize));
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error verifying module name %08X", result.raw);
            return result;
        }

        u32 prev_data_segment_address = 0;
        if (!is_crs) {
            auto result_val = RebaseSegmentTable(cro_size,
                data_segment_addresss, data_segment_size,
                bss_segment_address, bss_segment_size);
            if (result_val.Failed()) {
                LOG_ERROR(Service_LDR, "Error rebasing segment table %08X", result_val.Code().raw);
                return result_val.Code();
            }
            prev_data_segment_address = *result_val;
        }
        prev_data_segment_address += address;

        result = RebaseExportNamedSymbolTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing symbol export table %08X", result.raw);
            return result;
        }

        result = VerifyExportTreeTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error verifying export tree %08X", result.raw);
            return result;
        }

        result = VerifyString(GetField(ExportStringsOffset), GetField(ExportStringsSize));
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error verifying export strings %08X", result.raw);
            return result;
        }

        result = RebaseImportModuleTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing object table %08X", result.raw);
            return result;
        }

        result = ResetAllExternalPatches();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error resetting all external patches %08X", result.raw);
            return result;
        }

        result = RebaseImportNamedSymbolTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing symbol import table %08X", result.raw);
            return result;
        }

        result = RebaseImportIndexedSymbolTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing index import table %08X", result.raw);
            return result;
        }

        result = RebaseImportAnonymousSymbolTable();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error rebasing offset import table %08X", result.raw);
            return result;
        }

        result = VerifyString(GetField(ImportStringsOffset), GetField(ImportStringsSize));
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error verifying import strings %08X", result.raw);
            return result;
        }

        if (!is_crs) {
            result = ApplyStaticAnonymousSymbolToCRS();
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying offset export to CRS %08X", result.raw);
                return result;
            }
        }

        result = ApplyInternalPatches(prev_data_segment_address);
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error applying internal patches %08X", result.raw);
            return result;
        }

        if (!is_crs) {
            result = ApplyExitPatches();
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying exit patches %08X", result.raw);
                return result;
            }
        }

        return RESULT_SUCCESS;
    }

    /// Unrebases the module
    void Unrebase(bool is_crs = false) {
        UnrebaseImportAnonymousSymbolTable();
        UnrebaseImportIndexedSymbolTable();
        UnrebaseImportNamedSymbolTable();
        UnrebaseImportModuleTable();
        UnrebaseExportNamedSymbolTable();

        if (!is_crs)
            UnrebaseSegmentTable();

        SetNext(0);
        SetPrevious(0);

        SetField(FixedSize, 0);

        UnrebaseHeader();
    }

    /// Verifies the module by CRR
    ResultCode Verify(u32 cro_size, VAddr crr) {
        // TODO
        return RESULT_SUCCESS;
    }

    /// Links this module with all registered auto-link module
    ResultCode Link(bool link_on_load_bug_fix) {
        ResultCode result = RESULT_SUCCESS;

        {
            VAddr data_segment_address;
            if (link_on_load_bug_fix) {
                // this is a bug fix introduced by 7.2.0-17's LoadCRO_New
                // The bug itself is:
                // If a patch target is in .data segment, it will patch to the
                // user-specified buffer. But if this is linking during loading,
                // the .data segment hasn't been tranfer from CRO to the buffer,
                // thus the patch will be overwritten by data transfer.
                // To fix this bug, we need temporarily restore the old .data segment
                // offset and apply imported symbols.

                // RO service seems assuming segment_index == segment_type,
                // so we do the same
                if (GetField(SegmentNum) >= 2) { // means we have .data segment
                    SegmentEntry entry;
                    GetEntry<SegmentTableOffset>(2, entry);
                    ASSERT(entry.type == SegmentType::Data);
                    data_segment_address = entry.offset;
                    entry.offset = GetField(DataOffset);
                    SetEntry<SegmentTableOffset>(2, entry);
                }
            }
            SCOPE_EXIT({
                // restore the new .data segment address after importing
                if (link_on_load_bug_fix) {
                    if (GetField(SegmentNum) >= 2) {
                        SegmentEntry entry;
                        GetEntry<SegmentTableOffset>(2, entry);
                        entry.offset = data_segment_address;
                        SetEntry<SegmentTableOffset>(2, entry);
                    }
                }
            });

            // Imports named symbols from other modules
            result = ApplyImportNamedSymbol();
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying symbol import %08X", result.raw);
                return result;
            }

            // Imports indexed and anonymous symbols from other modules
            result =  ApplyModuleImport();
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying module import %08X", result.raw);
                return result;
            }
        }

        // Exports symbols to other modules
        result = ForEachAutoLinkCRO([this](CROHelper target) -> ResultVal<bool> {
            ResultCode result = ApplyExportNamedSymbol(target);
            if (result.IsError())
                return result;
            result = ApplyModuleExport(target);
            if (result.IsError())
                return result;
            return MakeResult<bool>(true);
        });
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error applying export %08X", result.raw);
            return result;
        }

        return RESULT_SUCCESS;
    }

    /// Unlinks this module with other modules
    ResultCode Unlink() {

        // Resets all imported named symbols
        ResultCode result = ResetImportNamedSymbol();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error resetting symbol import %08X", result.raw);
            return result;
        }

        // Resets all imported indexed symbols
        result = ResetImportIndexedSymbol();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error resetting indexed import %08X", result.raw);
            return result;
        }

        // Resets all imported anonymous symbols
        result = ResetImportAnonymousSymbol();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error resetting anonymous import %08X", result.raw);
            return result;
        }

        // Resets all symbols in other modules imported from this module
        // Note: the RO service seems only searching in auto-link modules
        result = ForEachAutoLinkCRO([this](CROHelper target) -> ResultVal<bool> {
            ResultCode result = ResetExportNamedSymbol(target);
            if (result.IsError())
                return result;
            result = ResetModuleExport(target);
            if (result.IsError())
                return result;
            return MakeResult<bool>(true);
        });
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error resetting export %08X", result.raw);
            return result;
        }

        return RESULT_SUCCESS;
    }

    void RegisterCRS() {
        SetNext(0);
        SetPrevious(0);
    }

    /// Registers this module and adds to the module list
    void Register(bool auto_link) {
        CROHelper crs(loaded_crs);

        CROHelper head(auto_link ? crs.Next() : crs.Previous());
        if (head.address) {
            // there are already CROs registered
            // register as the new tail
            CROHelper tail(head.Previous());

            // link with the old tail
            ASSERT(tail.Next() == 0);
            SetPrevious(tail.address);
            tail.SetNext(address);

            // set previous of the head pointing to the new tail
            head.SetPrevious(address);
        } else {
            // register as the first CRO
            // set previous to self as tail
            SetPrevious(address);

            // set self as head
            if (auto_link)
                crs.SetNext(address);
            else
                crs.SetPrevious(address);
        }

        // the new one is the tail
        SetNext(0);
    }

    /// Unregisters this module and removes from the module list
    void Unregister() {
        CROHelper crs(loaded_crs);
        CROHelper nhead(crs.Next()), phead(crs.Previous());
        CROHelper next(Next()), previous(Previous());
        if (address == nhead.address || address == phead.address) {
            // removing head
            if (next.address) {
                // the next is new head
                // let its previous point to the tail
                next.SetPrevious(previous.address);
            }

            // set new head
            if (address == phead.address) {
                crs.SetPrevious(next.address);
            } else {
                crs.SetNext(next.address);
            }
        } else if (next.address) {
            // link previous and next
            previous.SetNext(next.address);
            next.SetPrevious(previous.address);
        } else {
            // removing tail
            // set previous as new tail
            previous.SetNext(0);

            // let head's previous point to the new tail
            if (nhead.address && nhead.Previous() == address) {
                nhead.SetPrevious(previous.address);
            } else if (phead.address && phead.Previous() == address) {
                phead.SetPrevious(previous.address);
            } else {
                UNREACHABLE();
            }
        }

        // unlink self
        SetNext(0);
        SetPrevious(0);
    }

    u32 GetFixEnd(int fix_level) {
        u32 end = CRO_HEADER_SIZE;
        end = std::max<u32>(end, GetField(CodeOffset) + GetField(CodeSize));

        u32 entry_size_i = 2;
        int field = ModuleNameOffset;
        while (true) {
            end = std::max<u32>(end,
                GetField(static_cast<HeaderField>(field)) +
                GetField(static_cast<HeaderField>(field + 1)) * ENTRY_SIZE[entry_size_i]);

            ++entry_size_i;
            field += 2;

            if (field == FIX_BARRIERS[fix_level])
                return end;
        }
    }

    u32 Fix(int fix_level) {
        u32 fix_end = GetFixEnd(fix_level);


        if (fix_level) {
            SetField(Magic, MAGIC_FIXD);

            for (int field = FIX_BARRIERS[fix_level]; field < Fix0Barrier; field += 2) {
                SetField(static_cast<HeaderField>(field), fix_end);
                SetField(static_cast<HeaderField>(field + 1), 0);
            }
        }


        fix_end = Common::AlignUp(fix_end, 0x1000);

        u32 fixed_size = fix_end - address;
        SetField(FixedSize, fixed_size);
        return fixed_size;
    }

    bool IsLoaded() {
        u32 magic = GetField(Magic);
        if (magic != MAGIC_CRO0 && magic != MAGIC_FIXD)
            return false;

        // TODO

        return true;
    }
};

const std::array<int, 17> CROHelper::ENTRY_SIZE {{
    1, // code
    1, // data
    1, // module name
    sizeof(SegmentEntry),
    sizeof(ExportNamedSymbolEntry),
    sizeof(ExportIndexedSymbolEntry),
    1, // export strings
    sizeof(ExportTreeEntry),
    sizeof(ImportModuleEntry),
    sizeof(PatchEntry),
    sizeof(ImportNamedSymbolEntry),
    sizeof(ImportIndexedSymbolEntry),
    sizeof(ImportAnonymousSymbolEntry),
    1, // import strings
    sizeof(StaticAnonymousSymbolEntry),
    sizeof(InternalPatchEntry),
    sizeof(PatchEntry)
}};

const std::array<CROHelper::HeaderField, 4> CROHelper::FIX_BARRIERS {{
    Fix0Barrier,
    Fix1Barrier,
    Fix2Barrier,
    Fix3Barrier
}};

const u32 CROHelper::MAGIC_CRO0 = 0x304F5243;
const u32 CROHelper::MAGIC_FIXD = 0x44584946;

// This is a work-around before we implement memory aliasing.
// CRS and CRO are mapped (aliased) to another memory when loading,
// and game can read from both the original buffer or the mapped memory.
// So we use this to synchronize all original buffer with mapped memory
// after modifiying the content (rebasing, linking, etc.).
class MemorySynchronizer {
    std::map<VAddr, std::tuple<VAddr, u32>> memory_blocks;

public:
    void Clear() {
        memory_blocks.clear();
    }

    void AddMemoryBlock(VAddr source, VAddr dest, u32 size) {
        memory_blocks[source] = std::make_tuple(dest, size);
    }

    void RemoveMemoryBlock(VAddr source) {
        memory_blocks.erase(source);
    }

    void SynchronizeMemory() {
        for (auto block : memory_blocks) {
            VAddr source, dest;
            u32 size;
            std::tie(dest, size) = block.second;
            Memory::CopyBlock(dest, block.first, size);
        }
    }
};

static MemorySynchronizer memory_synchronizer;

/**
 * LDR_RO::Initialize service function
 *  Inputs:
 *      1 : CRS buffer pointer
 *      2 : CRS Size
 *      3 : Process memory address where the CRS will be mapped
 *      4 : Copy handle descriptor (zero)
 *      5 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Initialize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr crs_buffer  = cmd_buff[1];
    u32 crs_size      = cmd_buff[2];
    VAddr crs_address = cmd_buff[3];

    LOG_WARNING(Service_LDR, "called. loading CRS from 0x%08X to 0x%08X, size = 0x%X",
                crs_buffer, crs_address, crs_size);

    cmd_buff[0] = IPC::MakeHeader(1, 1, 0);

    if (loaded_crs) {
        LOG_ERROR(Service_LDR, "Already initialized");
        cmd_buff[1] = ERROR_ALREADY_INITIALIZED.raw;
        return;
    }

    if (crs_size < CRO_HEADER_SIZE) {
        LOG_ERROR(Service_LDR, "CRS is too small");
        cmd_buff[1] = ERROR_BUFFER_TOO_SMALL.raw;
        return;
    }

    if (crs_buffer & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRS original address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (crs_address & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRS mapping address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (crs_size & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRS size is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_SIZE.raw;
        return;
    }

    // TODO check memory access

    if (crs_address < 0x00100000 || crs_address + crs_size > 0x04000000) {
        LOG_ERROR(Service_LDR, "CRS mapping address is illegal");
        cmd_buff[1] = ERROR_ILLEGAL_ADDRESS.raw;
        return;
    }

    ResultCode result(RESULT_SUCCESS.raw);

    // TODO should be memory aliasing
    std::shared_ptr<std::vector<u8>> crs_mem = std::make_shared<std::vector<u8>>(crs_size);
    Memory::ReadBlock(crs_buffer, crs_mem->data(), crs_size);
    result = Kernel::g_current_process->vm_manager.MapMemoryBlock(crs_address, crs_mem, 0, crs_size, Kernel::MemoryState::Code).Code();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error mapping memory block %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }
    memory_synchronizer.AddMemoryBlock(crs_address, crs_buffer, crs_size);

    CROHelper crs(crs_address);
    crs.RegisterCRS();

    result = crs.Rebase(crs_size, 0, 0, 0, 0, true);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error Loading CRS %08X", result.raw);
        cmd_buff[1] = result.raw;
        UNREACHABLE();//Debug
        return;
    }

    memory_synchronizer.SynchronizeMemory();

    loaded_crs = crs_address;

    cmd_buff[1] = RESULT_SUCCESS.raw;
}

/**
 * LDR_RO::LoadCRR service function
 *  Inputs:
 *      1 : CRR buffer pointer
 *      2 : CRR Size
 *      3 : Copy handle descriptor (zero)
 *      4 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void LoadCRR(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 crr_buffer_ptr = cmd_buff[1];
    u32 crr_size       = cmd_buff[2];
    u32 process        = cmd_buff[4];

    cmd_buff[0] = IPC::MakeHeader(2, 1, 0);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_LDR, "(STUBBED) called. crr_buffer_ptr=0x%08X, crr_size=0x%08X, process=0x%08X",
                crr_buffer_ptr, crr_size, process);
}

/**
 * LDR_RO::UnloadCRR service function
 *  Inputs:
 *      1 : CRR buffer pointer
 *      2 : Copy handle descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnloadCRR(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 crr_buffer_ptr = cmd_buff[1];
    u32 process        = cmd_buff[3];

    cmd_buff[0] = IPC::MakeHeader(3, 1, 0);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_LDR, "(STUBBED) called. crr_buffer_ptr=0x%08X, process=0x%08X",
                crr_buffer_ptr, process);
}

/**
 * LDR_RO::LoadCRO service function
 *  Inputs:
 *      1 : CRO buffer pointer
 *      2 : CRO Size
 *      3 : Process memory address where the CRO will be mapped
 *      4 : .data segment buffer pointer
 *      5 : must be zero
 *      6 : .data segment buffer size
 *      7 : .bss segment buffer pointer
 *      8 : .bss segment buffer size
 *      9 : (bool) register CRO as auto-link module
 *     10 : fix level
 *     11 : CRR address (zero if use loaded CRR)
 *     12 : Copy handle descriptor (zero)
 *     13 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : CRO fixed size
 */
template <bool link_on_load_bug_fix>
static void LoadCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr cro_buffer           = cmd_buff[1];
    VAddr cro_address          = cmd_buff[2];
    u32 cro_size               = cmd_buff[3];
    VAddr data_segment_address = cmd_buff[4];
    u32 zero                   = cmd_buff[5];
    u32 data_segment_size      = cmd_buff[6];
    u32 bss_segment_address    = cmd_buff[7];
    u32 bss_segment_size       = cmd_buff[8];
    bool auto_link             = (cmd_buff[9] & 0xFF) != 0;
    u32 fix_level              = cmd_buff[10];
    VAddr crr_address          = cmd_buff[11];

    LOG_WARNING(Service_LDR, "called (%s), loading CRO from 0x%08X to 0x%08X, size = 0x%X, "
        "data_segment = 0x%08X, data_size = 0x%X, bss_segment = 0x%08X, bss_size = 0x%X, "
        "auto_link = %s, fix_level = %d, crr = 0x%08X",
        link_on_load_bug_fix ? "new" : "old",
        cro_buffer, cro_address, cro_size,
        data_segment_address, data_segment_size, bss_segment_address, bss_segment_size,
        auto_link ? "true" : "false", fix_level, crr_address
        );

    cmd_buff[0] = IPC::MakeHeader(link_on_load_bug_fix ? 9 : 4, 2, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_size < CRO_HEADER_SIZE) {
        LOG_ERROR(Service_LDR, "CRO too small");
        cmd_buff[1] = ERROR_BUFFER_TOO_SMALL.raw;
        return;
    }

    if (cro_buffer & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO original address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (cro_address & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO mapping address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (cro_size & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO size is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_SIZE.raw;
        return;
    }

    // TODO check memory access

    if (cro_address < 0x00100000 || cro_address + cro_size > 0x04000000) {
        LOG_ERROR(Service_LDR, "CRO mapping address is illegal");
        cmd_buff[1] = ERROR_ILLEGAL_ADDRESS.raw;
        return;
    }

    if (zero) {
        LOG_ERROR(Service_LDR, "Zero is not zero %d", zero);
        cmd_buff[1] = ERROR_ILLEGAL_ADDRESS.raw;
        return;
    }

    // TODO should be memory aliasing
    std::shared_ptr<std::vector<u8>> cro_mem = std::make_shared<std::vector<u8>>(cro_size);
    Memory::ReadBlock(cro_buffer, cro_mem->data(), cro_size);
    ResultCode result = Kernel::g_current_process->vm_manager.MapMemoryBlock(cro_address, cro_mem, 0, cro_size, Kernel::MemoryState::Code).Code();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error mapping memory block %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }
    memory_synchronizer.AddMemoryBlock(cro_address, cro_buffer, cro_size);

    CROHelper cro(cro_address);

    result = cro.Verify(cro_size, crr_address);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error verifying CRO in CRR %08X", result.raw);
        // TODO Unmap memory
        cmd_buff[1] = result.raw;
        return;
    }

    result = cro.Rebase(cro_size, data_segment_address, data_segment_size, bss_segment_address, bss_segment_size);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error rebasing CRO %08X", result.raw);
        // TODO Unmap memory
        cmd_buff[1] = result.raw;
        UNREACHABLE();//Debug
        return;
    }

    result = cro.Link(link_on_load_bug_fix);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error linking CRO %08X", result.raw);
        // TODO Unmap memory
        cmd_buff[1] = result.raw;
        UNREACHABLE();//Debug
        return;
    }

    cro.Register(auto_link);

    u32 fix_size = cro.Fix(fix_level);

    memory_synchronizer.SynchronizeMemory();

    if (fix_size != cro_size) {
        std::shared_ptr<std::vector<u8>> fixed_cro_mem = std::make_shared<std::vector<u8>>(
            cro_mem->data(), cro_mem->data() + fix_size);
        Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
        ResultCode result = Kernel::g_current_process->vm_manager.MapMemoryBlock(cro_address, fixed_cro_mem, 0, fix_size, Kernel::MemoryState::Code).Code();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error remapping memory block %08X", result.raw);
            cmd_buff[1] = result.raw;
            return;
        }
    }
    memory_synchronizer.AddMemoryBlock(cro_address, cro_buffer, fix_size);

    // TODO reprotect .text page

    Core::g_app_core->ClearInstructionCache();

    LOG_INFO(Service_LDR, "CRO \"%s\" loaded at 0x%08X, fixed_end = 0x%08X",
        cro.ModuleName().data(), cro_address, cro_address+fix_size
        );

    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = fix_size;
}

/**
 * LDR_RO::UnloadCRO service function
 *  Inputs:
 *      1 : mapped CRO pointer
 *      2 : zero?
 *      3 : Original CRO pointer
 *      4 : Copy handle descriptor (zero)
 *      5 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnloadCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 cro_address = cmd_buff[1];

    CROHelper cro(cro_address);

    LOG_WARNING(Service_LDR, "Unloading CRO \"%s\" at 0x%08X", cro.ModuleName().data(), cro_address);

    cmd_buff[0] = IPC::MakeHeader(5, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_INVALID_CRO.raw;
        return;
    }

    // TODO unprotect .text page

    u32 fixed_size = cro.GetFixedSize();

    cro.Unregister();

    ResultCode result = cro.Unlink();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unlinking CRO %08X", result.raw);
        cmd_buff[1] = result.raw;
        UNREACHABLE();//Debug
        return;
    }

    // TODO if not fixed, clear all external/internal patches to restore the state before loading

    cro.Unrebase();

    memory_synchronizer.SynchronizeMemory();

    result = Kernel::g_current_process->vm_manager.UnmapRange(cro_address, fixed_size);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unmapping CRO %08X", result.raw);
    }
    memory_synchronizer.RemoveMemoryBlock(cro_address);

    Core::g_app_core->ClearInstructionCache();

    cmd_buff[1] = result.raw;
}

/**
 * LDR_RO::LinkCRO service function
 *  Inputs:
 *      1 : mapped CRO pointer
 *      2 : Copy handle descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void LinkCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 cro_address = cmd_buff[1];

    CROHelper cro(cro_address);
    LOG_WARNING(Service_LDR, "Linking CRO \"%s\"", cro.ModuleName().data());

    cmd_buff[0] = IPC::MakeHeader(6, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_INVALID_CRO.raw;
        return;
    }

    ResultCode result = cro.Link(false);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error linking CRO %08X", result.raw);
    }

    memory_synchronizer.SynchronizeMemory();
    Core::g_app_core->ClearInstructionCache();

    cmd_buff[1] = result.raw;
}

/**
 * LDR_RO::UnlinkCRO service function
 *  Inputs:
 *      1 : mapped CRO pointer
 *      2 : Copy handle descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnlinkCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 cro_address = cmd_buff[1];

    CROHelper cro(cro_address);
    LOG_WARNING(Service_LDR, "Unlinking CRO \"%s\"", cro.ModuleName().data());

    cmd_buff[0] = IPC::MakeHeader(7, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & 0xFFF) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_INVALID_CRO.raw;
        return;
    }

    ResultCode result = cro.Unlink();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unlinking CRO %08X", result.raw);
    }

    memory_synchronizer.SynchronizeMemory();
    Core::g_app_core->ClearInstructionCache();

    cmd_buff[1] = result.raw;
}

/**
 * LDR_RO::Shutdown service function
 *  Inputs:
 *      1 : CRS buffer pointer
 *      2 : Copy handle descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Shutdown(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    LOG_WARNING(Service_LDR, "called, CRS buffer = 0x%08X", cmd_buff[1]);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    cmd_buff[0] = IPC::MakeHeader(8, 1, 0);

    CROHelper crs(loaded_crs);
    crs.Unrebase(true);

    memory_synchronizer.SynchronizeMemory();

    ResultCode result = Kernel::g_current_process->vm_manager.UnmapRange(loaded_crs, crs.GetFileSize());
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unmapping CRS %08X", result.raw);
    }
    memory_synchronizer.RemoveMemoryBlock(loaded_crs);

    loaded_crs = 0;
    cmd_buff[1] = result.raw;
}

const Interface::FunctionInfo FunctionTable[] = {
    {0x000100C2, Initialize,            "Initialize"},
    {0x00020082, LoadCRR,               "LoadCRR"},
    {0x00030042, UnloadCRR,             "UnloadCRR"},
    {0x000402C2, LoadCRO<false>,        "LoadCRO"},
    {0x000500C2, UnloadCRO,             "UnloadCRO"},
    {0x00060042, LinkCRO,               "LinkCRO"},
    {0x00070042, UnlinkCRO,             "UnlinkCRO"},
    {0x00080042, Shutdown,              "Shutdown"},
    {0x000902C2, LoadCRO<true>,         "LoadCRO_New"},
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Interface class

Interface::Interface() {
    Register(FunctionTable);

    loaded_crs = 0;
    memory_synchronizer.Clear();
}

} // namespace
