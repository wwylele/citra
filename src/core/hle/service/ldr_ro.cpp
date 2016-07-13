// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/swap.h"

#include "core/arm/arm_interface.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/service/ldr_ro.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace LDR_RO

namespace LDR_RO {

// GCC versions < 5.0 do not implement std::is_trivially_copyable.
// Excluding MSVC because it has weird behaviour for std::is_trivially_copyable.
#if (__GNUC__ >= 5) || defined(__clang__)
    #define ASSERT_CRO_STRUCT(name, size) \
        static_assert(std::is_standard_layout<name>::value, "CRO structure " #name " doesn't use standard layout"); \
        static_assert(std::is_trivially_copyable<name>::value, "CRO structure " #name " isn't trivially copyable"); \
        static_assert(sizeof(name) == (size), "Unexpected struct size for CRO structure " #name)
#else
    #define ASSERT_CRO_STRUCT(name, size) \
        static_assert(std::is_standard_layout<name>::value, "CRO structure " #name " doesn't use standard layout"); \
        static_assert(sizeof(name) == (size), "Unexpected struct size for CRO structure " #name)
#endif

static constexpr u32 CRO_HEADER_SIZE = 0x138;
static constexpr u32 CRO_HASH_SIZE = 0x80;

static const ResultCode ERROR_ALREADY_INITIALIZED =   // 0xD9612FF9
    ResultCode(ErrorDescription::AlreadyInitialized,         ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Permanent);
static const ResultCode ERROR_NOT_INITIALIZED =       // 0xD9612FF8
    ResultCode(ErrorDescription::NotInitialized,             ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Permanent);
static const ResultCode ERROR_BUFFER_TOO_SMALL =      // 0xE0E12C1F
    ResultCode(static_cast<ErrorDescription>(31),            ErrorModule::RO, ErrorSummary::InvalidArgument, ErrorLevel::Usage);
static const ResultCode ERROR_MISALIGNED_ADDRESS =    // 0xD9012FF1
    ResultCode(ErrorDescription::MisalignedAddress,          ErrorModule::RO, ErrorSummary::WrongArgument,   ErrorLevel::Permanent);
static const ResultCode ERROR_MISALIGNED_SIZE =       // 0xD9012FF2
    ResultCode(ErrorDescription::MisalignedSize,             ErrorModule::RO, ErrorSummary::WrongArgument,   ErrorLevel::Permanent);
static const ResultCode ERROR_ILLEGAL_ADDRESS =       // 0xE1612C0F
    ResultCode(static_cast<ErrorDescription>(15),            ErrorModule::RO, ErrorSummary::Internal,        ErrorLevel::Usage);
static const ResultCode ERROR_INVALID_MEMORY_STATE =  // 0xD8A12C08
    ResultCode(static_cast<ErrorDescription>(8),             ErrorModule::RO, ErrorSummary::InvalidState,    ErrorLevel::Permanent);
static const ResultCode ERROR_NOT_LOADED =            // 0xD8A12C0D
    ResultCode(static_cast<ErrorDescription>(13),            ErrorModule::RO, ErrorSummary::InvalidState,    ErrorLevel::Permanent);
static const ResultCode ERROR_INVALID_DESCRIPTOR =    // 0xD9001830
    ResultCode(ErrorDescription::OS_InvalidBufferDescriptor, ErrorModule::OS, ErrorSummary::WrongArgument,   ErrorLevel::Permanent);

static ResultCode CROFormatError(u32 description) {
    return ResultCode(static_cast<ErrorDescription>(description), ErrorModule::RO, ErrorSummary::WrongArgument, ErrorLevel::Permanent);
}

class CROHelper final {
    const VAddr address; ///< the virtual address of this module

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

    enum class SegmentType : u32 {
        Code   = 0,
        ROData = 1,
        Data   = 2,
        BSS    = 3,
    };

    union SegmentTag {
        u32_le raw;
        BitField<0, 4, u32_le> segment_index;
        BitField<4, 28, u32_le> offset_into_segment;
        SegmentTag() = default;
        SegmentTag(u32 raw_) : raw(raw_) {}
    };

    struct SegmentEntry {
        u32_le offset;
        u32_le size;
        SegmentType type;
        static constexpr HeaderField TABLE_OFFSET_FIELD = SegmentTableOffset;
    };
    ASSERT_CRO_STRUCT(SegmentEntry, 12);

    struct ExportNamedSymbolEntry {
        u32_le name_offset;         // pointing to a substring in ExportStrings
        SegmentTag symbol_position; // to self's segment
        static constexpr HeaderField TABLE_OFFSET_FIELD = ExportNamedSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(ExportNamedSymbolEntry, 8);

    struct ExportIndexedSymbolEntry {
        SegmentTag symbol_position; // to self's segment
        static constexpr HeaderField TABLE_OFFSET_FIELD = ExportIndexedSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(ExportIndexedSymbolEntry, 4);

    struct ExportTreeEntry {
        u16_le test_bit; // bit sddress into the name to test
        union Child{
            u16_le raw;
            BitField<0, 15, u16_le> next_index;
            BitField<15, 1, u16_le> is_end;
        } left, right;
        u16_le export_table_index; // index of an ExportNamedSymbolEntry
        static constexpr HeaderField TABLE_OFFSET_FIELD = ExportTreeTableOffset;
    };
    ASSERT_CRO_STRUCT(ExportTreeEntry, 8);

    struct ImportNamedSymbolEntry {
        u32_le name_offset;        // pointing to a substring in ImportStrings
        u32_le patch_batch_offset; // pointing to a patch batch in ExternalPatchTable
        static constexpr HeaderField TABLE_OFFSET_FIELD = ImportNamedSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(ImportNamedSymbolEntry, 8);

    struct ImportIndexedSymbolEntry {
        u32_le index;              // index of an opponent's ExportIndexedSymbolEntry
        u32_le patch_batch_offset; // pointing to a patch batch in ExternalPatchTable
        static constexpr HeaderField TABLE_OFFSET_FIELD = ImportIndexedSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(ImportIndexedSymbolEntry, 8);

    struct ImportAnonymousSymbolEntry {
        SegmentTag symbol_position; // to the opponent's segment
        u32_le patch_batch_offset;  // pointing to a patch batch in ExternalPatchTable
        static constexpr HeaderField TABLE_OFFSET_FIELD = ImportAnonymousSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(ImportAnonymousSymbolEntry, 8);

    struct ImportModuleEntry {
        u32_le name_offset;                          // pointing to a substring in ImporStrings
        u32_le import_indexed_symbol_table_offset;   // pointing to a subtable in ImportIndexedSymbolTable
        u32_le import_indexed_symbol_num;
        u32_le import_anonymous_symbol_table_offset; // pointing to a subtable in ImportAnonymousSymbolTable
        u32_le import_anonymous_symbol_num;
        static constexpr HeaderField TABLE_OFFSET_FIELD = ImportModuleTableOffset;

        void GetImportIndexedSymbolEntry(u32 index, ImportIndexedSymbolEntry& entry) {
            Memory::ReadBlock(import_indexed_symbol_table_offset + index * sizeof(ImportIndexedSymbolEntry),
                &entry, sizeof(ImportIndexedSymbolEntry));
        }

        void GetImportAnonymousSymbolEntry(u32 index, ImportAnonymousSymbolEntry& entry) {
            Memory::ReadBlock(import_anonymous_symbol_table_offset + index * sizeof(ImportAnonymousSymbolEntry),
                &entry, sizeof(ImportAnonymousSymbolEntry));
        }
    };
    ASSERT_CRO_STRUCT(ImportModuleEntry, 20);

    enum class PatchType : u8 {
        Nothing                = 0,
        AbsoluteAddress        = 2,
        RelativeAddress        = 3,
        ThumbBranch            = 10,
        ArmBranch              = 28,
        ModifyArmBranch        = 29,
        AbsoluteAddress2       = 38,
        AlignedRelativeAddress = 42,
    };

    struct PatchEntry {
        SegmentTag target_position; // to self's segment as an ExternalPatchEntry; to static module segment as a StaticPatchEntry
        PatchType type;
        u8 is_batch_end;
        u8 is_batch_resolved;       // set at a batch beginning if the batch is resolved
        INSERT_PADDING_BYTES(1);
        u32_le shift;
    };

    struct ExternalPatchEntry : PatchEntry {
        static constexpr HeaderField TABLE_OFFSET_FIELD = ExternalPatchTableOffset;
    };
    ASSERT_CRO_STRUCT(ExternalPatchEntry, 12);

    struct StaticPatchEntry : PatchEntry {
        static constexpr HeaderField TABLE_OFFSET_FIELD = StaticPatchTableOffset;
    };
    ASSERT_CRO_STRUCT(StaticPatchEntry, 12);

    struct InternalPatchEntry {
        SegmentTag target_position; // to self's segment
        PatchType type;
        u8 symbol_segment;
        INSERT_PADDING_BYTES(2);
        u32_le shift;
        static constexpr HeaderField TABLE_OFFSET_FIELD = InternalPatchTableOffset;
    };
    ASSERT_CRO_STRUCT(InternalPatchEntry, 12);

    struct StaticAnonymousSymbolEntry {
        SegmentTag symbol_position; // to self's segment
        u32_le patch_batch_offset;  // pointing to a patch batch in StaticPatchTable
        static constexpr HeaderField TABLE_OFFSET_FIELD = StaticAnonymousSymbolTableOffset;
    };
    ASSERT_CRO_STRUCT(StaticAnonymousSymbolEntry, 8);

    static std::array<int, 17> ENTRY_SIZE;
    static std::array<HeaderField, 4> FIX_BARRIERS;

    static constexpr u32 MAGIC_CRO0 = 0x304F5243;
    static constexpr u32 MAGIC_FIXD = 0x44584946;

    VAddr Field(HeaderField field) const {
        return address + CRO_HASH_SIZE + field * 4;
    }

    u32 GetField(HeaderField field) const {
        return Memory::Read32(Field(field));
    }

    void SetField(HeaderField field, u32 value) {
        Memory::Write32(Field(field), value);
    }

    VAddr Next() const {
        return GetField(NextCRO);
    }

    VAddr Previous() const {
        return GetField(PreviousCRO);
    }

    void SetNext(VAddr next) {
        SetField(NextCRO, next);
    }

    void SetPrevious(VAddr next) {
        SetField(PreviousCRO, next);
    }

    /**
     * Iterates over all registered auto-link modules, including the static module.
     * @param crs_address the virtual address of the static module
     * @param func a function object to operate on a module. It accepts one parameter
     *        CROHelper and returns ResultVal<bool>. It should return true to continue the iteration,
     *        false to stop the iteration, or an error code (which will also stop the iteration).
     * @returns ResultCode indicating the result of the operation, RESULT_SUCCESS if all iteration success,
     *         otherwise error code of the last iteration.
     */
    template <typename FunctionObject>
    static ResultCode ForEachAutoLinkCRO(VAddr crs_address, FunctionObject func) {
        VAddr current = crs_address;
        while (current) {
            CROHelper cro(current);
            CASCADE_RESULT(bool next, func(cro));
            if (!next)
                break;
            current = cro.Next();
        }
        return RESULT_SUCCESS;
    }

    /**
     * Reads an entry in one of module tables.
     * @param index index of the entry
     * @param data where to put the read entry
     * @note the entry type must have the static member TABLE_OFFSET_FIELD
     *       indicating which table the entry is in.
     */
    template <typename T>
    void GetEntry(std::size_t index, T& data) const {
        Memory::ReadBlock(GetField(T::TABLE_OFFSET_FIELD) + index * sizeof(T), &data, sizeof(T));
    }

    /**
     * Writes an entry to one of module tables.
     * @param index index of the entry
     * @param data the entry data to write
     * @note the entry type must have the static member TABLE_OFFSET_FIELD
     *       indicating which table the entry is in.
     */
    template <typename T>
    void SetEntry(std::size_t index, const T& data) {
        Memory::WriteBlock(GetField(T::TABLE_OFFSET_FIELD) + index * sizeof(T), &data, sizeof(T));
    }

    /**
     * Converts a segment tag to virtual address in this module.
     * @param segment_tag the segment tag to convert
     * @returns VAddr the virtual address the segment tag points to; 0 if invalid.
     */
    VAddr SegmentTagToAddress(SegmentTag segment_tag) const {
        u32 segment_num = GetField(SegmentNum);

        if (segment_tag.segment_index >= segment_num)
            return 0;

        SegmentEntry entry;
        GetEntry(segment_tag.segment_index, entry);

        if (segment_tag.offset_into_segment >= entry.size)
            return 0;

        return entry.offset + segment_tag.offset_into_segment;
    }

    /**
     * Finds a exported named symbol in this module.
     * @param name the name of the symbol to find
     * @return VAddr the virtual address of the symbol. 0 if not found.
     */
    VAddr FindExportNamedSymbol(const std::string& name) const {
        if (!GetField(ExportTreeNum))
            return 0;

        std::size_t len = name.size();
        ExportTreeEntry entry;
        GetEntry(0, entry);
        ExportTreeEntry::Child next;
        next.raw = entry.left.raw;
        u32 found_id;

        while (true) {
            GetEntry(next.next_index, entry);

            if (next.is_end) {
                found_id = entry.export_table_index;
                break;
            }

            u16 test_byte = entry.test_bit >> 3;
            u16 test_bit_in_byte = entry.test_bit & 7;

            if (test_byte >= len) {
                next.raw = entry.left.raw;
            } else if((name[test_byte] >> test_bit_in_byte) & 1) {
                next.raw = entry.right.raw;
            } else {
                next.raw = entry.left.raw;
            }
        }

        u32 symbol_export_num = GetField(ExportNamedSymbolNum);

        if (found_id >= symbol_export_num)
            return 0;

        u32 export_strings_size = GetField(ExportStringsSize);
        ExportNamedSymbolEntry symbol_entry;
        GetEntry(found_id, symbol_entry);

        if (Memory::GetString(symbol_entry.name_offset, export_strings_size) != name)
            return 0;

        return SegmentTagToAddress(symbol_entry.symbol_position);
    }

    /**
     * Rebases offsets in module header according to module address.
     * @param cro_size the size of the CRO file
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
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

        // verifies that all offsets are in the correct order
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

        u32 prev_offset = GetField(OFFSET_ORDER[0]);
        u32 cur_offset;
        for (std::size_t i = 1; i < OFFSET_ORDER.size(); ++i) {
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

    /**
     * Verify a string to be terminated by 0
     * @param address the virtual address of the string
     * @param size the size of the string, including the terminating 0
     * @returns ResultCode RESULT_SUCCESS if the string is terminated by 0, otherwise error code.
     */
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
     * @returns ResultVal<u32> with the previous data segment offset before rebasing.
     */
    ResultVal<u32> RebaseSegmentTable(u32 cro_size,
        VAddr data_segment_address, u32 data_segment_size,
        VAddr bss_segment_address, u32 bss_segment_size) {
        u32 prev_data_segment = 0;
        u32 segment_num = GetField(SegmentNum);
        for (u32 i = 0; i < segment_num; ++i) {
            SegmentEntry segment;
            GetEntry(i, segment);
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
            SetEntry(i, segment);
        }
        return MakeResult<u32>(prev_data_segment);
    }

    /**
     * Rebases offsets in exported named symbol table according to module address.
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
    ResultCode RebaseExportNamedSymbolTable() {
        VAddr export_strings_offset = GetField(ExportStringsOffset);
        VAddr export_strings_end = export_strings_offset + GetField(ExportStringsSize);

        u32 symbol_export_num = GetField(ExportNamedSymbolNum);
        for (u32 i = 0; i < symbol_export_num; ++i) {
            ExportNamedSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.name_offset) {
                entry.name_offset += address;
                if (entry.name_offset < export_strings_offset
                    || entry.name_offset >= export_strings_end) {
                    return CROFormatError(0x11);
                }
            }

            SetEntry(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Verifies indeces in export tree table.
     * @returns ResultCode RESULT_SUCCESS if all indeces are verified to be valid, otherwise error code.
     */
    ResultCode VerifyExportTreeTable() const {
        u32 tree_num = GetField(ExportTreeNum);
        for (u32 i = 0; i < tree_num; ++i) {
            ExportTreeEntry entry;
            GetEntry(i, entry);

            if (entry.left.next_index >= tree_num || entry.right.next_index >= tree_num) {
                return CROFormatError(0x11);
            }
        }
        return RESULT_SUCCESS;
    }

    /**
     * Rebases offsets in exported module table according to module address.
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
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
            GetEntry(i, entry);

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

            SetEntry(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Rebases offsets in imported named symbol table according to module address.
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
    ResultCode RebaseImportNamedSymbolTable() {
        VAddr import_strings_offset = GetField(ImportStringsOffset);
        VAddr import_strings_end = import_strings_offset + GetField(ImportStringsSize);
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(ExternalPatchEntry);

        u32 num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry(i, entry);

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

            SetEntry(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Rebases offsets in imported indexed symbol table according to module address.
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
    ResultCode RebaseImportIndexedSymbolTable() {
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(ExternalPatchEntry);

        u32 num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.patch_batch_offset) {
                entry.patch_batch_offset += address;
                if (entry.patch_batch_offset < external_patch_table_offset
                    || entry.patch_batch_offset > external_patch_table_end) {
                    return CROFormatError(0x14);
                }
            }

            SetEntry(i, entry);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Rebases offsets in imported anonymous symbol table according to module address.
     * @returns ResultCode RESULT_SUCCESS if all offsets are verified to be valid, otherwise error code.
     */
    ResultCode RebaseImportAnonymousSymbolTable() {
        VAddr external_patch_table_offset = GetField(ExternalPatchTableOffset);
        VAddr external_patch_table_end = external_patch_table_offset + GetField(ExternalPatchNum) * sizeof(ExternalPatchEntry);

        u32 num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < num ; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.patch_batch_offset) {
                entry.patch_batch_offset += address;
                if (entry.patch_batch_offset < external_patch_table_offset
                    || entry.patch_batch_offset > external_patch_table_end) {
                    return CROFormatError(0x17);
                }
            }

            SetEntry(i, entry);
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
    ResultCode ApplyPatch(VAddr target_address, PatchType patch_type, u32 shift, u32 symbol_address, u32 target_future_address) {
        switch (patch_type) {
            case PatchType::Nothing:
                break;
            case PatchType::AbsoluteAddress:
            case PatchType::AbsoluteAddress2:
                Memory::Write32(target_address, symbol_address + shift);
                break;
            case PatchType::RelativeAddress:
                Memory::Write32(target_address, symbol_address + shift - target_future_address);
                break;
            case PatchType::ThumbBranch:
            case PatchType::ArmBranch:
            case PatchType::ModifyArmBranch:
            case PatchType::AlignedRelativeAddress:
                // TODO(wwylele): implement other types
                UNIMPLEMENTED();
                break;
            default:
                return CROFormatError(0x22);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Clears a patch to zero
     * @param target_address where to apply the patch
     * @param patch_type the type of the patch
     * @returns ResultCode indicating the result of the operation, 0 on success
     */
    ResultCode ClearPatch(VAddr target_address, PatchType patch_type) {
        switch (patch_type) {
            case PatchType::Nothing:
                break;
            case PatchType::AbsoluteAddress:
            case PatchType::AbsoluteAddress2:
            case PatchType::RelativeAddress:
                Memory::Write32(target_address, 0);
                break;
            case PatchType::ThumbBranch:
            case PatchType::ArmBranch:
            case PatchType::ModifyArmBranch:
            case PatchType::AlignedRelativeAddress:
                // TODO(wwylele): implement other types
                UNIMPLEMENTED();
                break;
            default:
                return CROFormatError(0x22);
        }
        return RESULT_SUCCESS;
    }

    /**
     * Resets all external patches to unresolved state.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetExternalPatches() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));
        u32 external_patch_num = GetField(ExternalPatchNum);
        ExternalPatchEntry patch;

        // Verifies that the last patch is the end of a batch
        GetEntry(external_patch_num - 1, patch);
        if (!patch.is_batch_end) {
            return CROFormatError(0x12);
        }

        bool batch_begin = true;
        for (u32 i = 0; i < external_patch_num; ++i) {
            GetEntry(i, patch);
            VAddr patch_target = SegmentTagToAddress(patch.target_position);

            if (patch_target == 0) {
                return CROFormatError(0x12);
            }

            ResultCode result = ApplyPatch(patch_target, patch.type, patch.shift, unresolved_symbol, patch_target);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch %08X", result.raw);
                return result;
            }

            if (batch_begin) {
                // resets to unresolved state
                patch.is_batch_resolved = 0;
                SetEntry(i, patch);
            }

            // if current is an end, then the next is a beginning
            batch_begin = patch.is_batch_end != 0;
        }

        return RESULT_SUCCESS;
    }

    /**
     * Clears all external patches to zero.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ClearExternalPatches() {
        u32 external_patch_num = GetField(ExternalPatchNum);
        ExternalPatchEntry patch;

        bool batch_begin = true;
        for (u32 i = 0; i < external_patch_num; ++i) {
            GetEntry(i, patch);
            VAddr patch_target = SegmentTagToAddress(patch.target_position);

            if (patch_target == 0) {
                return CROFormatError(0x12);
            }

            ResultCode result = ClearPatch(patch_target, patch.type);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error clearing patch %08X", result.raw);
                return result;
            }

            if (batch_begin) {
                // resets to unresolved state
                patch.is_batch_resolved = 0;
                SetEntry(i, patch);
            }

            // if current is an end, then the next is a beginning
            batch_begin = patch.is_batch_end != 0;
        }

        return RESULT_SUCCESS;
    }

    /**
     * Applies or resets a batch of patch
     * @param batch the virtual address of the first patch in the batch
     * @param symbol_address the symbol address to be patched with
     * @param reset false to set the batch to resolved state, true to reset the batch to unresolved state
     * @returns ResultCode indicating the result of the operation, 0 on success
     */
    ResultCode ApplyPatchBatch(VAddr batch, u32 symbol_address, bool reset = false) {
        if (symbol_address == 0 && !reset)
            return CROFormatError(0x10);

        VAddr patch_address = batch;
        while (true) {
            PatchEntry patch;
            Memory::ReadBlock(patch_address, &patch, sizeof(PatchEntry));

            VAddr patch_target = SegmentTagToAddress(patch.target_position);
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
        patch.is_batch_resolved = reset ? 0 : 1;
        Memory::WriteBlock(batch, &patch, sizeof(PatchEntry));
        return RESULT_SUCCESS;
    }

    /**
     * Applies all static anonymous symbol to the static module.
     * @param crs_address the virtual address of the static module
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyStaticAnonymousSymbolToCRS(VAddr crs_address) {
        VAddr static_patch_table_offset = GetField(StaticPatchTableOffset);
        VAddr static_patch_table_end = static_patch_table_offset + GetField(StaticPatchNum) * sizeof(StaticPatchEntry);

        CROHelper crs(crs_address);
        u32 offset_export_num = GetField(StaticAnonymousSymbolNum);
        LOG_INFO(Service_LDR, "CRO \"%s\" exports %d static anonymous symbols", ModuleName().data(), offset_export_num);
        for (u32 i = 0; i < offset_export_num; ++i) {
            StaticAnonymousSymbolEntry entry;
            GetEntry(i, entry);
            u32 batch_address = entry.patch_batch_offset + address;

            if (batch_address < static_patch_table_offset
                || batch_address > static_patch_table_end) {
                return CROFormatError(0x16);
            }

            u32 symbol_address = SegmentTagToAddress(entry.symbol_position);
            LOG_TRACE(Service_LDR, "CRO \"%s\" exports 0x%08X to the static module", ModuleName().data(), symbol_address);
            ResultCode result = crs.ApplyPatchBatch(batch_address, symbol_address);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /**
     * Applies all internal patches to the module itself.
     * @param old_data_segment_address the virtual address of data segment in CRO buffer
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyInternalPatches(u32 old_data_segment_address) {
        u32 segment_num = GetField(SegmentNum);
        u32 internal_patch_num = GetField(InternalPatchNum);
        for (u32 i = 0; i < internal_patch_num; ++i) {
            InternalPatchEntry patch;
            GetEntry(i, patch);
            VAddr target_addressB = SegmentTagToAddress(patch.target_position);
            if (target_addressB == 0) {
                return CROFormatError(0x15);
            }

            VAddr target_address;
            SegmentEntry target_segment;
            GetEntry(patch.target_position.segment_index, target_segment);

            if (target_segment.type == SegmentType::Data) {
                // If the patch is to the .data segment, we need to patch it in the old buffer
                target_address = old_data_segment_address + patch.target_position.offset_into_segment;
            } else {
                target_address = target_addressB;
            }

            if (patch.symbol_segment >= segment_num) {
                return CROFormatError(0x15);
            }

            SegmentEntry symbol_segment;
            GetEntry(patch.symbol_segment, symbol_segment);
            LOG_TRACE(Service_LDR, "Internally patches 0x%08X with 0x%08X", target_address, symbol_segment.offset);
            ResultCode result = ApplyPatch(target_address, patch.type, patch.shift, symbol_segment.offset, target_addressB);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying patch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /**
     * Clears all internal patches to zero.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ClearInternalPatches() {
        u32 internal_patch_num = GetField(InternalPatchNum);
        for (u32 i = 0; i < internal_patch_num; ++i) {
            InternalPatchEntry patch;
            GetEntry(i, patch);
            VAddr target_address = SegmentTagToAddress(patch.target_position);

            if (target_address == 0) {
                return CROFormatError(0x15);
            }

            ResultCode result = ClearPatch(target_address, patch.type);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error clearing patch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /// Unrebases offsets in imported anonymous symbol table
    void UnrebaseImportAnonymousSymbolTable() {
        u32 num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < num; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }

            SetEntry(i, entry);
        }
    }

    /// Unrebases offsets in imported indexed symbol table
    void UnrebaseImportIndexedSymbolTable() {
        u32 num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < num; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }

            SetEntry(i, entry);
        }
    }

    /// Unrebases offsets in imported named symbol table
    void UnrebaseImportNamedSymbolTable() {
        u32 num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.name_offset) {
                entry.name_offset -= address;
            }

            if (entry.patch_batch_offset) {
                entry.patch_batch_offset -= address;
            }

            SetEntry(i, entry);
        }
    }

    /// Unrebases offsets in imported module table
    void UnrebaseImportModuleTable() {
        u32 object_num = GetField(ImportModuleNum);
        for (u32 i = 0; i < object_num; ++i) {
            ImportModuleEntry entry;
            GetEntry(i, entry);

            if (entry.name_offset) {
                entry.name_offset -= address;
            }

            if (entry.import_indexed_symbol_table_offset) {
                entry.import_indexed_symbol_table_offset -= address;
            }

            if (entry.import_anonymous_symbol_table_offset) {
                entry.import_anonymous_symbol_table_offset -= address;
            }

            SetEntry(i, entry);
        }
    }

    /// Unrebases offsets in exported named symbol table
    void UnrebaseExportNamedSymbolTable() {
        u32 symbol_export_num = GetField(ExportNamedSymbolNum);
        for (u32 i = 0; i < symbol_export_num; ++i) {
            ExportNamedSymbolEntry entry;
            GetEntry(i, entry);

            if (entry.name_offset) {
                entry.name_offset -= address;
            }

            SetEntry(i, entry);
        }
    }

    /// Unrebases offsets in segment table
    void UnrebaseSegmentTable() {
        u32 segment_num = GetField(SegmentNum);
        for (u32 i = 0; i < segment_num; ++i) {
            SegmentEntry segment;
            GetEntry(i, segment);

            if (segment.type == SegmentType::BSS) {
                segment.offset = 0;
            } else if (segment.offset) {
                segment.offset -= address;
            }

            SetEntry(i, segment);
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

    /**
     * Looks up all imported named symbols of this module in all registered auto-link modules, and resolves them if found.
     * @param crs_address the virtual address of the static module
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyImportNamedSymbol(VAddr crs_address) {
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            if (!patch_entry.is_batch_resolved) {
                ResultCode result = ForEachAutoLinkCRO(crs_address, [&](CROHelper source) -> ResultVal<bool> {
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

    /**
     * Resets all imported named symbols of this module to unresolved state.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetImportNamedSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));

        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }

        }
        return RESULT_SUCCESS;
    }

    /**
     * Resets all imported indexed symbols of this module to unresolved state.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetImportIndexedSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));

        u32 import_num = GetField(ImportIndexedSymbolNum);
        for (u32 i = 0; i < import_num; ++i) {
            ImportIndexedSymbolEntry entry;
            GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /**
     * Resets all imported anonymous symbols of this module to unresolved state.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetImportAnonymousSymbol() {
        u32 unresolved_symbol = SegmentTagToAddress(GetField(OnUnresolvedSegmentTag));

        u32 import_num = GetField(ImportAnonymousSymbolNum);
        for (u32 i = 0; i < import_num; ++i) {
            ImportAnonymousSymbolEntry entry;
            GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            ResultCode result = ApplyPatchBatch(patch_addr, unresolved_symbol, true);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error reseting patch batch %08X", result.raw);
                return result;
            }
        }
        return RESULT_SUCCESS;
    }

    /**
     * Finds registered auto-link modules that this module imports, and resolves indexed and anonymous symbols exported by them.
     * @param crs_address the virtual address of the static module
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyModuleImport(VAddr crs_address) {
        u32 import_strings_size = GetField(ImportStringsSize);

        u32 import_module_num = GetField(ImportModuleNum);
        for (u32 i = 0; i < import_module_num; ++i) {
            ImportModuleEntry entry;
            GetEntry(i, entry);
            std::string want_cro_name = Memory::GetString(entry.name_offset, import_strings_size);

            ResultCode result = ForEachAutoLinkCRO(crs_address, [&](CROHelper source) -> ResultVal<bool> {
                if (want_cro_name == source.ModuleName()) {
                    LOG_INFO(Service_LDR, "CRO \"%s\" imports %d indexed symbols from \"%s\"",
                        ModuleName().data(), entry.import_indexed_symbol_num, source.ModuleName().data());
                    for (u32 j = 0; j < entry.import_indexed_symbol_num; ++j) {
                        ImportIndexedSymbolEntry im;
                        entry.GetImportIndexedSymbolEntry(j, im);
                        ExportIndexedSymbolEntry ex;
                        source.GetEntry(im.index, ex);
                        u32 symbol_address = source.SegmentTagToAddress(ex.symbol_position);
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
                        u32 symbol_address = source.SegmentTagToAddress(im.symbol_position);
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

    /**
     * Resolves target module's imported named symbols that exported by this module.
     * @param target the module to resolve.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyExportNamedSymbol(CROHelper target) {
        LOG_DEBUG(Service_LDR, "CRO \"%s\" exports named symbols to \"%s\"",
            ModuleName().data(), target.ModuleName().data());
        u32 target_import_strings_size = target.GetField(ImportStringsSize);
        u32 target_symbol_import_num = target.GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < target_symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            target.GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            if (!patch_entry.is_batch_resolved) {
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

    /**
     * Resets target's named symbols imported from this module to unresolved state.
     * @param target the module to reset.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetExportNamedSymbol(CROHelper target) {
        LOG_DEBUG(Service_LDR, "CRO \"%s\" unexports named symbols to \"%s\"",
            ModuleName().data(), target.ModuleName().data());
        u32 unresolved_symbol = target.SegmentTagToAddress(target.GetField(OnUnresolvedSegmentTag));
        u32 target_import_strings_size = target.GetField(ImportStringsSize);
        u32 target_symbol_import_num = target.GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < target_symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            target.GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            if (!patch_entry.is_batch_resolved) {
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

    /**
     * Resolves imported indexed and anonymous symbols in the target module which imports this module.
     * @param target the module to resolve.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyModuleExport(CROHelper target) {
        std::string module_name = ModuleName();
        u32 target_import_string_size = target.GetField(ImportStringsSize);
        u32 target_import_module_num = target.GetField(ImportModuleNum);
        for (u32 i = 0; i < target_import_module_num; ++i) {
            ImportModuleEntry entry;
            target.GetEntry(i, entry);

            if (Memory::GetString(entry.name_offset, target_import_string_size) != module_name)
                continue;

            LOG_INFO(Service_LDR, "CRO \"%s\" exports %d indexed symbols to \"%s\"",
                module_name.data(), entry.import_indexed_symbol_num, target.ModuleName().data());
            for (u32 j = 0; j < entry.import_indexed_symbol_num; ++j) {
                ImportIndexedSymbolEntry im;
                entry.GetImportIndexedSymbolEntry(j, im);
                ExportIndexedSymbolEntry ex;
                GetEntry(im.index, ex);
                u32 symbol_address = SegmentTagToAddress(ex.symbol_position);
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
                u32 symbol_address = SegmentTagToAddress(im.symbol_position);
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

    /**
     * Resets target's indexed and anonymous symbol imported from this module to unresolved state.
     * @param target the module to reset.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ResetModuleExport(CROHelper target) {
        u32 unresolved_symbol = target.SegmentTagToAddress(target.GetField(OnUnresolvedSegmentTag));

        std::string module_name = ModuleName();
        u32 target_import_string_size = target.GetField(ImportStringsSize);
        u32 target_import_module_num = target.GetField(ImportModuleNum);
        for (u32 i = 0; i < target_import_module_num; ++i) {
            ImportModuleEntry entry;
            target.GetEntry(i, entry);

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

    /**
     * Resolves the exit function in this module
     * @param crs_address the virtual address of the static module.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ApplyExitPatches(VAddr crs_address) {
        u32 import_strings_size = GetField(ImportStringsSize);
        u32 symbol_import_num = GetField(ImportNamedSymbolNum);
        for (u32 i = 0; i < symbol_import_num; ++i) {
            ImportNamedSymbolEntry entry;
            GetEntry(i, entry);
            VAddr patch_addr = entry.patch_batch_offset;
            ExternalPatchEntry patch_entry;
            Memory::ReadBlock(patch_addr, &patch_entry, sizeof(ExternalPatchEntry));

            if (Memory::GetString(entry.name_offset, import_strings_size) == "__aeabi_atexit"){
                ResultCode result = ForEachAutoLinkCRO(crs_address, [&](CROHelper source) -> ResultVal<bool> {
                    u32 symbol_address = source.FindExportNamedSymbol("nnroAeabiAtexit_");

                    if (symbol_address) {
                        LOG_DEBUG(Service_LDR, "CRO \"%s\" import exit function from \"%s\"",
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
    explicit CROHelper(VAddr cro_address) : address(cro_address) {
    }

    std::string ModuleName() const {
        return Memory::GetString(GetField(ModuleNameOffset), GetField(ModuleNameSize));
    }

    u32 GetFileSize() const {
        return GetField(FileSize);
    }

    u32 GetFixedSize() const {
        return GetField(FixedSize);
    }

    /**
     * Rebases the module according to its address.
     * @param crs_address the virtual address of the static module
     * @param cro_size the size of the CRO file
     * @param data_segment_address buffer address for .data segment
     * @param data_segment_size the buffer size for .data segment
     * @param bss_segment_address the buffer address for .bss segment
     * @param bss_segment_size the buffer size for .bss segment
     * @param is_crs true if the module itself is the static module
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode Rebase(VAddr crs_address, u32 cro_size,
        VAddr data_segment_addresss, u32 data_segment_size,
        VAddr bss_segment_address, u32 bss_segment_size, bool is_crs) {
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

        result = ResetExternalPatches();
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
            result = ApplyStaticAnonymousSymbolToCRS(crs_address);
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
            result = ApplyExitPatches(crs_address);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying exit patches %08X", result.raw);
                return result;
            }
        }

        return RESULT_SUCCESS;
    }

    /**
     * Unrebases the module.
     * @param is_crs true if the module itself is the static module
     */
    void Unrebase(bool is_crs) {
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

    /**
     * Verifies module hash by CRR.
     * @param cro_size the size of the CRO
     * @param crr the virtual address of the CRR
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode VerifyHash(u32 cro_size, VAddr crr) const {
        // TODO(wwylele): actually verify the hash
        return RESULT_SUCCESS;
    }

    /**
     * Links this module with all registered auto-link module.
     * @param crs_address the virtual address of the static module
     * @param link_on_load_bug_fix true if link when loading and fix the bug
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode Link(VAddr crs_address, bool link_on_load_bug_fix) {
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
                    GetEntry(2, entry);
                    ASSERT(entry.type == SegmentType::Data);
                    data_segment_address = entry.offset;
                    entry.offset = GetField(DataOffset);
                    SetEntry(2, entry);
                }
            }
            SCOPE_EXIT({
                // Restore the new .data segment address after importing
                if (link_on_load_bug_fix) {
                    if (GetField(SegmentNum) >= 2) {
                        SegmentEntry entry;
                        GetEntry(2, entry);
                        entry.offset = data_segment_address;
                        SetEntry(2, entry);
                    }
                }
            });

            // Imports named symbols from other modules
            result = ApplyImportNamedSymbol(crs_address);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying symbol import %08X", result.raw);
                return result;
            }

            // Imports indexed and anonymous symbols from other modules
            result = ApplyModuleImport(crs_address);
            if (result.IsError()) {
                LOG_ERROR(Service_LDR, "Error applying module import %08X", result.raw);
                return result;
            }
        }

        // Exports symbols to other modules
        result = ForEachAutoLinkCRO(crs_address, [this](CROHelper target) -> ResultVal<bool> {
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

    /**
     * Unlinks this module with other modules.
     * @param crs_address the virtual address of the static module
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode Unlink(VAddr crs_address) {

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
        result = ForEachAutoLinkCRO(crs_address, [this](CROHelper target) -> ResultVal<bool> {
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

    /**
     * Clears all patches to zero.
     * @returns ResultCode RESULT_SUCCESS if success, otherwise error code.
     */
    ResultCode ClearPatches() {
        ResultCode result = ClearExternalPatches();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error clearing external patches %08X", result.raw);
            return result;
        }

        result = ClearInternalPatches();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error clearing internal patches %08X", result.raw);
            return result;
        }
        return RESULT_SUCCESS;
    }

    void InitCRS() {
        SetNext(0);
        SetPrevious(0);
    }

    /**
     * Registers this module and adds to the module list.
     * @param crs_address the virtual address of the static module
     * @auto_link whether to register as an auto link module
     */
    void Register(VAddr crs_address, bool auto_link) {
        CROHelper crs(crs_address);
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

    /**
     * Unregisters this module and removes from the module list.
     * @param crs_address the virtual address of the static module
     */
    void Unregister(VAddr crs_address) {
        CROHelper crs(crs_address);
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

    /**
     * Gets the end of reserved data according to the fix level.
     * @param fix_level fix level from 0 to 3
     * @returns the end of reserved data.
     */
    u32 GetFixEnd(u32 fix_level) const {
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

    /**
     * Zeros offsets to cropped data according to the fix level and marks as fixed.
     * @param fix_level fix level from 0 to 3
     * @returns page-aligned size of the module after fixing.
     */
    u32 Fix(u32 fix_level) {
        u32 fix_end = GetFixEnd(fix_level);

        if (fix_level != 0) {
            SetField(Magic, MAGIC_FIXD);

            for (int field = FIX_BARRIERS[fix_level]; field < Fix0Barrier; field += 2) {
                SetField(static_cast<HeaderField>(field), fix_end);
                SetField(static_cast<HeaderField>(field + 1), 0);
            }
        }

        fix_end = Common::AlignUp(fix_end, Memory::PAGE_SIZE);

        u32 fixed_size = fix_end - address;
        SetField(FixedSize, fixed_size);
        return fixed_size;
    }

    bool IsLoaded() const {
        u32 magic = GetField(Magic);
        if (magic != MAGIC_CRO0 && magic != MAGIC_FIXD)
            return false;

        // TODO(wwylele): verify memory state here after memory aliasing is implemented

        return true;
    }

    bool IsFixed() const {
        return GetField(Magic) == MAGIC_FIXD;
    }

    /**
     * Gets the page address of the code segment.
     * @returns a tuple of (address, size); (0, 0) if the code segment doesn't exist.
     */
    std::tuple<VAddr, u32> GetExecutablePages() const {
        u32 segment_num = GetField(SegmentNum);
        for (u32 i = 0; i < segment_num; ++i) {
            SegmentEntry entry;
            GetEntry(i, entry);
            if (entry.type == SegmentType::Code && entry.size != 0) {
                VAddr begin = Common::AlignDown(entry.offset, Memory::PAGE_SIZE);
                VAddr end = Common::AlignUp(entry.offset + entry.size, Memory::PAGE_SIZE);
                return std::make_tuple(begin, end - begin);
            }
        }
        return std::make_tuple(0, 0);
    }
};

std::array<int, 17> CROHelper::ENTRY_SIZE {{
    1, // code
    1, // data
    1, // module name
    sizeof(SegmentEntry),
    sizeof(ExportNamedSymbolEntry),
    sizeof(ExportIndexedSymbolEntry),
    1, // export strings
    sizeof(ExportTreeEntry),
    sizeof(ImportModuleEntry),
    sizeof(ExternalPatchEntry),
    sizeof(ImportNamedSymbolEntry),
    sizeof(ImportIndexedSymbolEntry),
    sizeof(ImportAnonymousSymbolEntry),
    1, // import strings
    sizeof(StaticAnonymousSymbolEntry),
    sizeof(InternalPatchEntry),
    sizeof(StaticPatchEntry)
}};

std::array<CROHelper::HeaderField, 4> CROHelper::FIX_BARRIERS {{
    Fix0Barrier,
    Fix1Barrier,
    Fix2Barrier,
    Fix3Barrier
}};

// This is a work-around before we implement memory aliasing.
// CRS and CRO are mapped (aliased) to another memory when loading.
// Games can read from both the original buffer or the mapped memory,
// and even write to the original buffer after CRO loading.
// So we use this to synchronize all original buffer with mapped memory
// after modifiying the content (rebasing, linking, etc.).
class MemorySynchronizer {
    std::map<VAddr, std::tuple<VAddr, u32>> memory_blocks;

public:
    void Clear() {
        memory_blocks.clear();
    }

    void AddMemoryBlock(VAddr mapping, VAddr original, u32 size) {
        memory_blocks[mapping] = std::make_tuple(original, size);
    }

    void RemoveMemoryBlock(VAddr source) {
        memory_blocks.erase(source);
    }

    void SynchronizeOriginalMemory() {
        for (auto block : memory_blocks) {
            VAddr mapping = block.first;
            VAddr original;
            u32 size;
            std::tie(original, size) = block.second;
            Memory::CopyBlock(original, mapping, size);
        }
    }

    void SynchronizeMappingMemory() {
        for (auto block : memory_blocks) {
            VAddr mapping = block.first;
            VAddr original;
            u32 size;
            std::tie(original, size) = block.second;
            Memory::CopyBlock(mapping, original, size);
        }
    }
};

static MemorySynchronizer memory_synchronizer;

// TODO(wwylele): this should be in the per-client storage when we implement multi-process
static VAddr loaded_crs; ///< the virtual address of the static module

static bool VerifyBufferState(VAddr buffer_ptr, u32 size) {
    auto vma = Kernel::g_current_process->vm_manager.FindVMA(buffer_ptr);
    return vma != Kernel::g_current_process->vm_manager.vma_map.end()
        && vma->second.base + vma->second.size >= buffer_ptr + size
        && vma->second.permissions == Kernel::VMAPermission::ReadWrite
        && vma->second.meminfo_state == Kernel::MemoryState::Private;
}

/**
 * LDR_RO::Initialize service function
 *  Inputs:
 *      1 : CRS buffer pointer
 *      2 : CRS Size
 *      3 : Process memory address where the CRS will be mapped
 *      4 : handle translation descriptor (zero)
 *      5 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Initialize(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr crs_buffer_ptr = cmd_buff[1];
    u32 crs_size         = cmd_buff[2];
    VAddr crs_address    = cmd_buff[3];
    u32 descriptor       = cmd_buff[4];
    u32 process          = cmd_buff[5];

    LOG_WARNING(Service_LDR, "called, crs_buffer_ptr=0x%08X, crs_address=0x%08X, size=0x%X, descriptor=0x%08X, process=0x%08X",
                crs_buffer_ptr, crs_address, crs_size, descriptor, process);

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

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

    if (crs_buffer_ptr & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRS original address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (crs_address & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRS mapping address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (crs_size & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRS size is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_SIZE.raw;
        return;
    }

    if (!VerifyBufferState(crs_buffer_ptr, crs_size)) {
        LOG_ERROR(Service_LDR, "CRS original buffer is in invalid state");
        cmd_buff[1] = ERROR_INVALID_MEMORY_STATE.raw;
        return;
    }

    if (crs_address < 0x00100000 || crs_address + crs_size > 0x04000000) {
        LOG_ERROR(Service_LDR, "CRS mapping address is illegal");
        cmd_buff[1] = ERROR_ILLEGAL_ADDRESS.raw;
        return;
    }

    ResultCode result(RESULT_SUCCESS.raw);

    // TODO(wwylele): should be memory aliasing
    std::shared_ptr<std::vector<u8>> crs_mem = std::make_shared<std::vector<u8>>(crs_size);
    Memory::ReadBlock(crs_buffer_ptr, crs_mem->data(), crs_size);
    result = Kernel::g_current_process->vm_manager.MapMemoryBlock(crs_address, crs_mem, 0, crs_size, Kernel::MemoryState::Code).Code();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error mapping memory block %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }

    result = Kernel::g_current_process->vm_manager.ReprotectRange(crs_address, crs_size, Kernel::VMAPermission::Read);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error reprotecting memory block %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }

    memory_synchronizer.AddMemoryBlock(crs_address, crs_buffer_ptr, crs_size);

    CROHelper crs(crs_address);
    crs.InitCRS();

    result = crs.Rebase(0, crs_size, 0, 0, 0, 0, true);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error rebasing CRS %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }

    memory_synchronizer.SynchronizeOriginalMemory();

    loaded_crs = crs_address;

    cmd_buff[1] = RESULT_SUCCESS.raw;
}

/**
 * LDR_RO::LoadCRR service function
 *  Inputs:
 *      1 : CRR buffer pointer
 *      2 : CRR Size
 *      3 : handle translation descriptor (zero)
 *      4 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void LoadCRR(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 crr_buffer_ptr = cmd_buff[1];
    u32 crr_size       = cmd_buff[2];
    u32 descriptor     = cmd_buff[3];
    u32 process        = cmd_buff[4];

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    cmd_buff[0] = IPC::MakeHeader(2, 1, 0);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_LDR, "(STUBBED) called, crr_buffer_ptr=0x%08X, crr_size=0x%08X, descriptor=0x%08X, process=0x%08X",
                crr_buffer_ptr, crr_size, descriptor, process);
}

/**
 * LDR_RO::UnloadCRR service function
 *  Inputs:
 *      1 : CRR buffer pointer
 *      2 : handle translation descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnloadCRR(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 crr_buffer_ptr = cmd_buff[1];
    u32 descriptor     = cmd_buff[2];
    u32 process        = cmd_buff[3];

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    cmd_buff[0] = IPC::MakeHeader(3, 1, 0);

    cmd_buff[1] = RESULT_SUCCESS.raw; // No error

    LOG_WARNING(Service_LDR, "(STUBBED) called, crr_buffer_ptr=0x%08X, descriptor=0x%08X, process=0x%08X",
                crr_buffer_ptr, descriptor, process);
}

/**
 * LDR_RO::LoadCRO service function
 *  Inputs:
 *      1 : CRO buffer pointer
 *      2 : memory address where the CRO will be mapped
 *      3 : CRO Size
 *      4 : .data segment buffer pointer
 *      5 : must be zero
 *      6 : .data segment buffer size
 *      7 : .bss segment buffer pointer
 *      8 : .bss segment buffer size
 *      9 : (bool) register CRO as auto-link module
 *     10 : fix level
 *     11 : CRR address (zero if use loaded CRR)
 *     12 : handle translation descriptor (zero)
 *     13 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : CRO fixed size
 */
template <bool link_on_load_bug_fix>
static void LoadCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr cro_buffer_ptr       = cmd_buff[1];
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
    u32 descriptor             = cmd_buff[12];
    u32 process                = cmd_buff[13];

    LOG_DEBUG(Service_LDR, "called (%s), cro_buffer_ptr=0x%08X, cro_address=0x%08X, size=0x%X, "
        "data_segment_address=0x%08X, zero=%d, data_segment_size=0x%X, bss_segment_address=0x%08X, bss_segment_size=0x%X, "
        "auto_link=%s, fix_level=%d, crr_address=0x%08X, descriptor=0x%08X, process=0x%08X",
        link_on_load_bug_fix ? "new" : "old", cro_buffer_ptr, cro_address, cro_size,
        data_segment_address, zero, data_segment_size, bss_segment_address, bss_segment_size,
        auto_link ? "true" : "false", fix_level, crr_address, descriptor, process
        );

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    memory_synchronizer.SynchronizeMappingMemory();

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

    if (cro_buffer_ptr & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO original address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (cro_address & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO mapping address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (cro_size & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO size is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_SIZE.raw;
        return;
    }

    if (!VerifyBufferState(cro_buffer_ptr, cro_size)) {
        LOG_ERROR(Service_LDR, "CRO original buffer is in invalid state");
        cmd_buff[1] = ERROR_INVALID_MEMORY_STATE.raw;
        return;
    }

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

    // TODO(wwylele): should be memory aliasing
    std::shared_ptr<std::vector<u8>> cro_mem = std::make_shared<std::vector<u8>>(cro_size);
    Memory::ReadBlock(cro_buffer_ptr, cro_mem->data(), cro_size);
    ResultCode result = Kernel::g_current_process->vm_manager.MapMemoryBlock(cro_address, cro_mem, 0, cro_size, Kernel::MemoryState::Code).Code();
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error mapping memory block %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }

    result = Kernel::g_current_process->vm_manager.ReprotectRange(cro_address, cro_size, Kernel::VMAPermission::Read);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error reprotecting memory block %08X", result.raw);
        Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
        cmd_buff[1] = result.raw;
        return;
    }

    memory_synchronizer.AddMemoryBlock(cro_address, cro_buffer_ptr, cro_size);

    CROHelper cro(cro_address);

    result = cro.VerifyHash(cro_size, crr_address);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error verifying CRO in CRR %08X", result.raw);
        Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
        cmd_buff[1] = result.raw;
        return;
    }

    result = cro.Rebase(loaded_crs, cro_size, data_segment_address, data_segment_size, bss_segment_address, bss_segment_size, false);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error rebasing CRO %08X", result.raw);
        Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
        cmd_buff[1] = result.raw;
        return;
    }

    result = cro.Link(loaded_crs, link_on_load_bug_fix);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error linking CRO %08X", result.raw);
        Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
        cmd_buff[1] = result.raw;
        return;
    }

    cro.Register(loaded_crs, auto_link);

    u32 fix_size = cro.Fix(fix_level);

    memory_synchronizer.SynchronizeOriginalMemory();

    if (fix_size != cro_size) {
        result = Kernel::g_current_process->vm_manager.UnmapRange(cro_address + fix_size, cro_size - fix_size);
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error unmapping memory block %08X", result.raw);
            Kernel::g_current_process->vm_manager.UnmapRange(cro_address, cro_size);
            cmd_buff[1] = result.raw;
            return;
        }
    }

    // Changes the block size
    memory_synchronizer.AddMemoryBlock(cro_address, cro_buffer_ptr, fix_size);

    VAddr exe_begin;
    u32 exe_size;
    std::tie(exe_begin, exe_size) = cro.GetExecutablePages();
    if (exe_begin) {
        result = Kernel::g_current_process->vm_manager.ReprotectRange(exe_begin,exe_size, Kernel::VMAPermission::ReadExecute);
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error reprotecting memory block %08X", result.raw);
            Kernel::g_current_process->vm_manager.UnmapRange(cro_address, fix_size);
            cmd_buff[1] = result.raw;
            return;
        }
    }

    Core::g_app_core->ClearInstructionCache();

    LOG_INFO(Service_LDR, "CRO \"%s\" loaded at 0x%08X, fixed_end=0x%08X",
        cro.ModuleName().data(), cro_address, cro_address+fix_size);

    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = fix_size;
}

/**
 * LDR_RO::UnloadCRO service function
 *  Inputs:
 *      1 : mapped CRO pointer
 *      2 : zero? (RO service doesn't care)
 *      3 : original CRO pointer
 *      4 : handle translation descriptor (zero)
 *      5 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnloadCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr cro_address      = cmd_buff[1];
    u32 zero               = cmd_buff[2];
    VAddr original_buffer  = cmd_buff[3];
    u32 descriptor         = cmd_buff[4];
    u32 process            = cmd_buff[5];

    LOG_DEBUG(Service_LDR, "called, cro_address=0x%08X, zero=%d, original_buffer=0x%08X, descriptor=0x%08X, process=0x%08X",
        cro_address, zero, original_buffer, descriptor, process);

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    CROHelper cro(cro_address);

    memory_synchronizer.SynchronizeMappingMemory();

    cmd_buff[0] = IPC::MakeHeader(5, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_NOT_LOADED.raw;
        return;
    }

    LOG_INFO(Service_LDR, "Unloading CRO \"%s\"", cro.ModuleName().data());

    // Note that if the CRO is not fixed (loaded with fix_level = 0),
    // games will modify the .data section entry, making it pointing to the orignal data in CRO buffer
    // instead of the .data buffer, before calling UnloadCRO. In this case,
    // any modification to the .data section (Unlink and ClearPatches) below.
    // will actually do in CRO buffer.

    u32 fixed_size = cro.GetFixedSize();

    cro.Unregister(loaded_crs);

    ResultCode result = cro.Unlink(loaded_crs);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unlinking CRO %08X", result.raw);
        cmd_buff[1] = result.raw;
        return;
    }

    // If the module is not fixed, clears all external/internal patches
    // to restore the state before loading, so that it can be loaded again(?)
    if (!cro.IsFixed()) {
        result = cro.ClearPatches();
        if (result.IsError()) {
            LOG_ERROR(Service_LDR, "Error clearing patches %08X", result.raw);
            cmd_buff[1] = result.raw;
            return;
        }
    }

    cro.Unrebase(false);

    memory_synchronizer.SynchronizeOriginalMemory();

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
 *      2 : handle translation descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void LinkCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr cro_address = cmd_buff[1];
    u32 descriptor    = cmd_buff[2];
    u32 process       = cmd_buff[3];

    LOG_DEBUG(Service_LDR, "called, cro_address=0x%08X, descriptor=0x%08X, process=0x%08X",
        cro_address, descriptor, process);

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    CROHelper cro(cro_address);

    memory_synchronizer.SynchronizeMappingMemory();

    cmd_buff[0] = IPC::MakeHeader(6, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_NOT_LOADED.raw;
        return;
    }

    LOG_INFO(Service_LDR, "Linking CRO \"%s\"", cro.ModuleName().data());

    ResultCode result = cro.Link(loaded_crs, false);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error linking CRO %08X", result.raw);
    }

    memory_synchronizer.SynchronizeOriginalMemory();
    Core::g_app_core->ClearInstructionCache();

    cmd_buff[1] = result.raw;
}

/**
 * LDR_RO::UnlinkCRO service function
 *  Inputs:
 *      1 : mapped CRO pointer
 *      2 : handle translation descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnlinkCRO(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr cro_address = cmd_buff[1];
    u32 descriptor    = cmd_buff[2];
    u32 process       = cmd_buff[3];

    LOG_DEBUG(Service_LDR, "called, cro_address=0x%08X, descriptor=0x%08X, process=0x%08X",
        cro_address, descriptor, process);

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    CROHelper cro(cro_address);

    memory_synchronizer.SynchronizeMappingMemory();

    cmd_buff[0] = IPC::MakeHeader(7, 1, 0);

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    if (cro_address & Memory::PAGE_MASK) {
        LOG_ERROR(Service_LDR, "CRO address is not aligned");
        cmd_buff[1] = ERROR_MISALIGNED_ADDRESS.raw;
        return;
    }

    if (!cro.IsLoaded()) {
        LOG_ERROR(Service_LDR, "Invalid or not loaded CRO");
        cmd_buff[1] = ERROR_NOT_LOADED.raw;
        return;
    }

    LOG_INFO(Service_LDR, "Unlinking CRO \"%s\"", cro.ModuleName().data());

    ResultCode result = cro.Unlink(loaded_crs);
    if (result.IsError()) {
        LOG_ERROR(Service_LDR, "Error unlinking CRO %08X", result.raw);
    }

    memory_synchronizer.SynchronizeOriginalMemory();
    Core::g_app_core->ClearInstructionCache();

    cmd_buff[1] = result.raw;
}

/**
 * LDR_RO::Shutdown service function
 *  Inputs:
 *      1 : original CRS buffer pointer
 *      2 : handle translation descriptor (zero)
 *      3 : KProcess handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Shutdown(Service::Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    VAddr crs_buffer_ptr = cmd_buff[1];
    u32 descriptor       = cmd_buff[2];
    u32 process          = cmd_buff[3];

    LOG_DEBUG(Service_LDR, "called, crs_buffer_ptr=0x%08X, descriptor=0x%08X, process=0x%08X",
        crs_buffer_ptr, descriptor, process);

    if (descriptor != 0) {
        LOG_ERROR(Service_LDR, "IPC handle descriptor failed validation (0x%X).", descriptor);
        cmd_buff[0] = IPC::MakeHeader(0, 1, 0);
        cmd_buff[1] = ERROR_INVALID_DESCRIPTOR.raw;
        return;
    }

    memory_synchronizer.SynchronizeMappingMemory();

    if (!loaded_crs) {
        LOG_ERROR(Service_LDR, "Not initialized");
        cmd_buff[1] = ERROR_NOT_INITIALIZED.raw;
        return;
    }

    cmd_buff[0] = IPC::MakeHeader(8, 1, 0);

    CROHelper crs(loaded_crs);
    crs.Unrebase(true);

    memory_synchronizer.SynchronizeOriginalMemory();

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
