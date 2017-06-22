// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "core/cheat/cheat_core.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/hle/service/hid/hid.h"

namespace CheatCore {
constexpr u64 frame_ticks = 268123480ull / 60;
static int tick_event;
static std::unique_ptr<CheatEngine::CheatEngine> cheat_engine;

static void CheatTickCallback(u64, int cycles_late) {
    if (cheat_engine == nullptr)
        cheat_engine = std::make_unique<CheatEngine::CheatEngine>();
    cheat_engine->Run();
    CoreTiming::ScheduleEvent(frame_ticks - cycles_late, tick_event);
}

void Init() {
    tick_event = CoreTiming::RegisterEvent("CheatCore::tick_event", CheatTickCallback);
    CoreTiming::ScheduleEvent(frame_ticks, tick_event);
}

void Shutdown() {
    CoreTiming::UnscheduleEvent(tick_event, 0);
}

void RefreshCheats() {
    cheat_engine->RefreshCheats();
}
} // namespace CheatCore

namespace CheatEngine {
static std::string GetFilePath() {
    const auto program_id =
        Common::StringFromFormat("%016llX", Kernel::g_current_process->codeset->program_id);
    return FileUtil::GetUserPath(D_USER_IDX) + "cheats" + DIR_SEP + program_id + ".txt";
}
CheatEngine::CheatEngine() {
    const auto file_path = GetFilePath();
    if (!FileUtil::Exists(file_path))
        FileUtil::CreateEmptyFile(file_path);
    cheats_list = ReadFileContents();
}

std::vector<std::shared_ptr<CheatBase>> CheatEngine::ReadFileContents() {
    std::string file_path = GetFilePath();

    std::string contents;
    FileUtil::ReadFileToString(true, file_path.c_str(), contents);
    std::vector<std::string> lines;
    Common::SplitString(contents, '\n', lines);

    std::string code_type =
        "Gateway"; // If more cheat types added, need to specify which type in parsing.
    std::vector<std::string> notes;
    std::vector<CheatLine> cheat_lines;
    std::vector<std::shared_ptr<CheatBase>> cheats;
    std::string name;
    bool enabled = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string current_line = std::string(lines[i].c_str());
        current_line = Common::Trim(current_line);
        if (!current_line.empty()) {
            if (current_line.compare(0, 2, "+[") == 0) { // Enabled code
                if (!cheat_lines.empty()) {
                    if (code_type == "Gateway")
                        cheats.push_back(
                            std::make_shared<GatewayCheat>(cheat_lines, notes, enabled, name));
                }
                name = current_line.substr(2, current_line.length() - 3);
                cheat_lines.clear();
                notes.clear();
                enabled = true;
                continue;
            } else if (current_line.front() == '[') { // Disabled code
                if (!cheat_lines.empty()) {
                    if (code_type == "Gateway")
                        cheats.push_back(
                            std::make_shared<GatewayCheat>(cheat_lines, notes, enabled, name));
                }
                name = current_line.substr(1, current_line.length() - 2);
                cheat_lines.clear();
                notes.clear();
                enabled = false;
                continue;
            } else if (current_line.front() == '*') { // Comment
                notes.push_back(std::move(current_line));
            } else {
                cheat_lines.emplace_back(std::move(current_line));
            }
        }
        if (i == lines.size() - 1) { // End of file
            if (!cheat_lines.empty()) {
                if (code_type == "Gateway")
                    cheats.push_back(
                        std::make_shared<GatewayCheat>(cheat_lines, notes, enabled, name));
            }
        }
    }
    return cheats;
}

void CheatEngine::Save(std::vector<std::shared_ptr<CheatBase>> cheats) {
    const auto file_path = GetFilePath();
    FileUtil::IOFile file = FileUtil::IOFile(file_path, "w+");
    for (auto& cheat : cheats) {
        if (cheat->GetType() == "Gateway") {
            auto cheatString = cheat->ToString();
            file.WriteBytes(cheatString.c_str(), cheatString.length());
        }
    }
}

void CheatEngine::RefreshCheats() {
    const auto file_path = GetFilePath();
    if (!FileUtil::Exists(file_path))
        FileUtil::CreateEmptyFile(file_path);
    cheats_list = ReadFileContents();
}

void CheatEngine::Run() {
    for (auto& cheat : cheats_list) {
        cheat->Execute();
    }
}

void GatewayCheat::Execute() {
    if (enabled == false)
        return;
    u32 addr = 0;
    u32 reg = 0;
    u32 offset = 0;
    u32 val = 0;
    int if_flag = 0;
    int loop_count = 0;
    s32 loopbackline = 0;
    u32 counter = 0;
    bool loop_flag = false;
    for (size_t i = 0; i < cheat_lines.size(); i++) {
        auto line = cheat_lines[i];
        if (line.type == CheatType::Null)
            continue;
        addr = line.address;
        val = line.value;
        if (if_flag > 0) {
            if (line.type == CheatType::Patch)
                i += (line.value + 7) / 8;
            if (line.type == CheatType::Terminator)
                if_flag--;                              // ENDIF
            if (line.type == CheatType::FullTerminator) // NEXT & Flush
            {
                if (loop_flag)
                    i = loopbackline - 1;
                else {
                    offset = 0;
                    reg = 0;
                    loop_count = 0;
                    counter = 0;
                    if_flag = 0;
                    loop_flag = 0;
                }
            }
            continue;
        }

        switch (line.type) {
        case CheatType::Write32: { // 0XXXXXXX YYYYYYYY   word[XXXXXXX+offset] = YYYYYYYY
            addr = line.address + offset;
            Memory::Write32(addr, val);
            break;
        }
        case CheatType::Write16: { // 1XXXXXXX 0000YYYY   half[XXXXXXX+offset] = YYYY
            addr = line.address + offset;
            Memory::Write16(addr, static_cast<u16>(val));
            break;
        }
        case CheatType::Write8: { // 2XXXXXXX 000000YY   byte[XXXXXXX+offset] = YY
            addr = line.address + offset;
            Memory::Write8(addr, static_cast<u8>(val));
            break;
        }
        case CheatType::GreaterThan32: { // 3XXXXXXX YYYYYYYY   IF YYYYYYYY > word[XXXXXXX]
                                         // ;unsigned
            if (line.address == 0)
                line.address = offset;
            val = Memory::Read32(line.address);
            if (line.value > val) {
                if (if_flag > 0)
                    if_flag--;
            } else {
                if_flag++;
            }
            break;
        }
        case CheatType::LessThan32: { // 4XXXXXXX YYYYYYYY   IF YYYYYYYY < word[XXXXXXX]   ;unsigned
            if (line.address == 0)
                line.address = offset;
            val = Memory::Read32(line.address);
            if (line.value < val) {
                if (if_flag > 0)
                    if_flag--;
            } else {
                if_flag++;
            }
            break;
        }
        case CheatType::EqualTo32: { // 5XXXXXXX YYYYYYYY   IF YYYYYYYY = word[XXXXXXX]
            if (line.address == 0)
                line.address = offset;
            val = Memory::Read32(line.address);
            if (line.value == val) {
                if (if_flag > 0)
                    if_flag--;
            } else {
                if_flag++;
            }
            break;
        }
        case CheatType::NotEqualTo32: { // 6XXXXXXX YYYYYYYY   IF YYYYYYYY <> word[XXXXXXX]
            if (line.address == 0)
                line.address = offset;
            val = Memory::Read32(line.address);
            if (line.value != val) {
                if (if_flag > 0)
                    if_flag--;
            } else {
                if_flag++;
            }
            break;
        }
        case CheatType::GreaterThan16: { // 7XXXXXXX ZZZZYYYY   IF YYYY > ((not ZZZZ) AND
                                         // half[XXXXXXX])
            if (line.address == 0)
                line.address = offset;
            int x = line.address & 0x0FFFFFFF;
            int y = line.value & 0xFFFF;
            int z = line.value >> 16;
            val = Memory::Read16(x);
            if (y > (u16)((~z) & val))
            {
                if (if_flag > 0)
                    if_flag--;
            }
            else {
                if_flag++;
            }
            break;
        }
        case CheatType::LessThan16: { // 8XXXXXXX ZZZZYYYY   IF YYYY < ((not ZZZZ) AND
                                      // half[XXXXXXX])
            if (line.address == 0)
                line.address = offset;
            int x = line.address & 0x0FFFFFFF;
            int y = line.value & 0xFFFF;
            int z = line.value >> 16;
            val = Memory::Read16(x);
            if (y < (u16)((~z) & val))
            {
                if (if_flag > 0)
                    if_flag--;
            }
            else {
                if_flag++;
            }
            break;
        }
        case CheatType::EqualTo16: { // 9XXXXXXX ZZZZYYYY   IF YYYY = ((not ZZZZ) AND half[XXXXXXX])
            if (line.address == 0)
                line.address = offset;
            int x = line.address & 0x0FFFFFFF;
            int y = line.value & 0xFFFF;
            int z = line.value >> 16;
            val = Memory::Read16(x);
            if (y == (u16)((~z) & val))
            {
                if (if_flag > 0)
                    if_flag--;
            }
            else {
                if_flag++;
            }
            break;
        }
        case CheatType::NotEqualTo16: { // AXXXXXXX ZZZZYYYY   IF YYYY <> ((not ZZZZ) AND
                                        // half[XXXXXXX])
            if (line.address == 0)
                line.address = offset;
            int x = line.address & 0x0FFFFFFF;
            int y = line.value & 0xFFFF;
            int z = line.value >> 16;
            val = Memory::Read16(x);
            if (y != (u16)((~z) & val))
            {
                if (if_flag > 0)
                    if_flag--;
            }
            else {
                if_flag++;
            }
            break;
        }
        case CheatType::LoadOffset: { // BXXXXXXX 00000000   offset = word[XXXXXXX+offset]
            addr = line.address + offset;
            offset = Memory::Read32(addr);
            break;
        }
        case CheatType::Loop: {
            if (loop_count < (line.value + 1))
                loop_flag = 1;
            else
                loop_flag = 0;
            loop_count++;
            loopbackline = i;
            break;
        }
        case CheatType::Terminator: {
            break;
        }
        case CheatType::LoopExecuteVariant: {
            if (loop_flag)
                i = loopbackline - 1;
            break;
        }
        case CheatType::FullTerminator: {
            if (loop_flag)
                i = loopbackline - 1;
            else {
                offset = 0;
                reg = 0;
                loop_count = 0;
                counter = 0;
                if_flag = 0;
                loop_flag = 0;
            }
            break;
        }
        case CheatType::SetOffset: {
            offset = line.value;
            break;
        }
        case CheatType::AddValue: {
            reg += line.value;
            break;
        }
        case CheatType::SetValue: {
            reg = line.value;
            break;
        }
        case CheatType::IncrementiveWrite32: {
            addr = line.value + offset;
            Memory::Write32(addr, reg);
            offset += 4;
            break;
        }
        case CheatType::IncrementiveWrite16: {
            addr = line.value + offset;
            Memory::Write16(addr, static_cast<u16>(reg));
            offset += 2;
            break;
        }
        case CheatType::IncrementiveWrite8: {
            addr = line.value + offset;
            Memory::Write8(addr, static_cast<u8>(reg));
            offset += 1;
            break;
        }
        case CheatType::Load32: {
            addr = line.value + offset;
            reg = Memory::Read32(addr);
            break;
        }
        case CheatType::Load16: {
            addr = line.value + offset;
            reg = Memory::Read16(addr);
            break;
        }
        case CheatType::Load8: {
            addr = line.value + offset;
            reg = Memory::Read8(addr);
            break;
        }
        case CheatType::AddOffset: {
            offset += line.value;
            break;
        }
        case CheatType::Joker: {
            auto state = Service::HID::GetInputsThisFrame();
            auto result = (state.hex & line.value) == line.value;
            if (result) {
                if (if_flag > 0)
                    if_flag--;
            }
            else {
                if_flag++;
            }
            break;
        }
        case CheatType::Patch: {
            // Patch Code (Miscellaneous Memory Manipulation Codes)
            // EXXXXXXX YYYYYYYY
            // Copies YYYYYYYY bytes from (current code location + 8) to [XXXXXXXX + offset].
            auto x = line.address & 0x0FFFFFFF;
            auto y = line.value;
            addr = x + offset;
            {
                u32 j = 0, t = 0, b = 0;
                if (y > 0)
                    i++; // skip over the current code
                while (y >= 4) {
                    u32 tmp = (t == 0) ? cheat_lines[i].address : cheat_lines[i].value;
                    if (t == 1)
                        i++;
                    t ^= 1;
                    Memory::Write32(addr, tmp);
                    addr += 4;
                    y -= 4;
                }
                while (y > 0) {
                    u32 tmp = ((t == 0) ? cheat_lines[i].address : cheat_lines[i].value) >> b;
                    Memory::Write8(addr, tmp);
                    addr += 1;
                    y -= 1;
                    b += 4;
                }
            }
            break;
        }
        }
    }
}

std::string GatewayCheat::ToString() {
    std::string result;
    if (cheat_lines.empty())
        return result;
    if (enabled)
        result += '+';
    result += '[' + name + "]\n";
    for (auto& str : notes) {
        if (str.front() == '*')
            str.insert(0, 1, '*');
    }
    result += Common::Join(notes, "\n");
    if (!notes.empty())
        result += '\n';
    for (const auto& line : cheat_lines)
        result += line.cheat_line + '\n';
    result += '\n';
    return result;
}
}
