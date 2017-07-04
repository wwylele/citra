// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
#include <fcntl.h>
#include <sys/stat.h>  // mkfifo
#include <sys/types.h> // mkfifo
#include <thread>
#include "common/alignment.h"
#include "common/bit_field.h"
#include "common/string_util.h"
#include "core/core_timing.h"
#include "core/hle/service/ir/other_3ds.h"
#include "core/settings.h"

namespace Service {
namespace IR {

const char* pipe_p2s = "/tmp/citra_ir_p2s";
const char* pipe_s2p = "/tmp/citra_ir_s2p";
const char* pipe_p2s_flag = "/tmp/citra_ir_p2s_flag";
const char* pipe_s2p_flag = "/tmp/citra_ir_s2p_flag";

bool CheckForAnotherInstance() {

    int fd;

    struct flock fl;

    fd = open("/tmp/citra_lock", O_RDWR | O_CREAT, 0777);

    if (fd == -1)

    {

        return false;
    }

    fl.l_type = F_WRLCK; /* F_RDLCK, F_WRLCK, F_UNLCK    */

    fl.l_whence = SEEK_SET; /* SEEK_SET, SEEK_CUR, SEEK_END */

    fl.l_start = 0; /* Offset from l_whence         */

    fl.l_len = 0; /* length, 0 = to EOF           */

    fl.l_pid = getpid(); /* our PID                      */

    // try to create a file lock

    if (fcntl(fd, F_SETLK, &fl) == -1) /* F_GETLK, F_SETLK, F_SETLKW */

    {

        // we failed to create a file lock, meaning it's already locked //

        if (errno == EACCES || errno == EAGAIN)

        {

            return true;
        }
    }

    return false;
}

void ReadPipe(int pipe, u8* buf, int size) {
    while (size > 0) {
        auto num = read(pipe, buf, size);
        buf += num;
        size -= num;
    }
}

void WritePipe(int pipe, u8* buf, int size) {
    while (size > 0) {
        auto num = write(pipe, buf, size);
        buf += num;
        size -= num;
    }
}

std::vector<u8> ReadPacket(int pipe) {
    u32 size;
    ReadPipe(pipe, (u8*)&size, 4);
    std::vector<u8> data(size);
    if (size) ReadPipe(pipe, data.data(), size);
    return data;
}

void WritePacket(int pipe, std::vector<u8> data) {
    u32 size = (u32)data.size();
    WritePipe(pipe, (u8*)&size, 4);
    if (size) WritePipe(pipe, data.data(), size);
}

bool is_primary;
int pipe_read, pipe_write, pipe_read_flag, pipe_write_flag;
int stream_beat;

Other3DS* other_3ds;
std::vector<u8> waiting_packet;

std::thread* shant;

void Shant() {
    while (1) {
        waiting_packet = ReadPacket(pipe_read);
        CoreTiming::ScheduleEvent_Threadsafe(msToCycles(10),  stream_beat, 0);
    }
}

void InitIRStream() {
    if (CheckForAnotherInstance()) {
        is_primary = false;
        if ((pipe_read = open(pipe_p2s, O_RDONLY)) < 0) throw;
        if ((pipe_read_flag = open(pipe_p2s_flag, O_RDONLY)) < 0) throw;
        if ((pipe_write = open(pipe_s2p, O_WRONLY)) < 0) throw;
        if ((pipe_write_flag = open(pipe_s2p_flag, O_WRONLY)) < 0) throw;
        LOG_INFO(Service_IR, "As Secondary");
    } else {
        is_primary = true;
        mkfifo(pipe_p2s, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
        mkfifo(pipe_p2s_flag, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
        mkfifo(pipe_s2p, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
        mkfifo(pipe_s2p_flag, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
        LOG_INFO(Service_IR, "--");
        if ((pipe_write = open(pipe_p2s, O_WRONLY)) < 0) throw;
        if ((pipe_write_flag = open(pipe_p2s_flag, O_WRONLY)) < 0) throw;
        if ((pipe_read = open(pipe_s2p, O_RDONLY)) < 0) throw;
        if ((pipe_read_flag = open(pipe_s2p_flag, O_RDONLY)) < 0) throw;
        LOG_INFO(Service_IR, "As Primary");
    }

    stream_beat =
            CoreTiming::RegisterEvent("Other3DS", [](u64, int cycles_late) {
                if (other_3ds) {
                    LOG_INFO(Service_IR, "What");
                    other_3ds->SendHaha(waiting_packet);
                }
            });
    shant = new std::thread(Shant);
}

Other3DS::Other3DS(SendFunc send_func) : IRDevice(send_func) {

}

void Other3DS::OnConnect() {
    if (is_primary) {
        WritePacket(pipe_write_flag, std::vector<u8>());
        ReadPacket(pipe_read_flag);
    } else {
        ReadPacket(pipe_read_flag);
        WritePacket(pipe_write_flag, std::vector<u8>());
    }
    other_3ds = this;
}

void Other3DS::OnDisconnect() {
    other_3ds = nullptr;
}

void Other3DS::SendHaha(const std::vector<u8>& data) {
    Send(data);
}

void Other3DS::OnReceive(const std::vector<u8>& data) {
    LOG_ERROR(Service_IR, "Received request: %s",
              Common::ArrayToString(data.data(), data.size()).c_str());
    WritePacket(pipe_write, data);
}

u8 Other3DS::GetRole() {
    return is_primary ? 1 : 2;
}

} // namespace IR
} // namespace Service