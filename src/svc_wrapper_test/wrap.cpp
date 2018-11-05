// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/cityhash.h"
#include "common/common_types.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/result.h"
#include "core/memory.h"

u32 reg[8];

namespace HLE {

static inline u32 Param(int n) {
    return reg[n];
}

static void Return(int n, u32 value) {
    reg[n] = value;
}

/**
 * HLE a function return from the current ARM11 userland process
 * @param res Result to return
 */
static inline void FuncReturn(u32 res) {
    Return(0, res);
}

/**
 * HLE a function return (64-bit) from the current ARM11 userland process
 * @param res Result to return (64-bit)
 * @todo Verify that this function is correct
 */
static inline void FuncReturn64(u64 res) {
    Return(0, (u32)(res & 0xFFFFFFFF));
    Return(1, (u32)((res >> 32) & 0xFFFFFFFF));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Function wrappers that return type ResultCode

template <ResultCode func(u32, u32, u32, u32)>
void Wrap() {
    FuncReturn(func(Param(0), Param(1), Param(2), Param(3)).raw);
}

template <ResultCode func(u32*, u32, u32, u32, u32, u32)>
void Wrap() {
    u32 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2), Param(3), Param(0), Param(4)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32*, u32, u32, VAddr, u32, s32)>
void Wrap() {
    u32 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2), Param(3), Param(0), Param(4)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(s32*, VAddr, s32, bool, s64)>
void Wrap() {
    s32 param_1 = 0;
    s32 retval =
        func(&param_1, Param(1), (s32)Param(2), (Param(3) != 0), (((s64)Param(4) << 32) | Param(0)))
            .raw;

    Return(1, (u32)param_1);
    FuncReturn(retval);
}

template <ResultCode func(s32*, VAddr, s32, u32)>
void Wrap() {
    s32 param_1 = 0;
    u32 retval = func(&param_1, Param(1), (s32)Param(2), Param(3)).raw;

    Return(1, (u32)param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32, u32, u32, u32, s64)>
void Wrap() {
    FuncReturn(
        func(Param(0), Param(1), Param(2), Param(3), (((s64)Param(5) << 32) | Param(4))).raw);
}

template <ResultCode func(u32*)>
void Wrap() {
    u32 param_1 = 0;
    u32 retval = func(&param_1).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32, s64)>
void Wrap() {
    s32 retval = func(Param(0), (((s64)Param(3) << 32) | Param(2))).raw;

    FuncReturn(retval);
}

template <ResultCode func(Kernel::MemoryInfo*, Kernel::PageInfo*, u32)>
void Wrap() {
    Kernel::MemoryInfo memory_info = {};
    Kernel::PageInfo page_info = {};
    u32 retval = func(&memory_info, &page_info, Param(2)).raw;
    Return(1, memory_info.base_address);
    Return(2, memory_info.size);
    Return(3, memory_info.permission);
    Return(4, memory_info.state);
    Return(5, page_info.flags);
    FuncReturn(retval);
}

template <ResultCode func(Kernel::MemoryInfo*, Kernel::PageInfo*, Kernel::Handle, u32)>
void Wrap() {
    Kernel::MemoryInfo memory_info = {};
    Kernel::PageInfo page_info = {};
    u32 retval = func(&memory_info, &page_info, Param(2), Param(3)).raw;
    Return(1, memory_info.base_address);
    Return(2, memory_info.size);
    Return(3, memory_info.permission);
    Return(4, memory_info.state);
    Return(5, page_info.flags);
    FuncReturn(retval);
}

template <ResultCode func(s32*, u32)>
void Wrap() {
    s32 param_1 = 0;
    u32 retval = func(&param_1, Param(1)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32, s32)>
void Wrap() {
    FuncReturn(func(Param(0), (s32)Param(1)).raw);
}

template <ResultCode func(u32*, u32)>
void Wrap() {
    u32 param_1 = 0;
    u32 retval = func(&param_1, Param(1)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32)>
void Wrap() {
    FuncReturn(func(Param(0)).raw);
}

template <ResultCode func(u32*, s32, s32)>
void Wrap() {
    u32 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(s32*, u32, s32)>
void Wrap() {
    s32 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(s64*, u32, s32)>
void Wrap() {
    s64 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2)).raw;
    Return(1, (u32)param_1);
    Return(2, (u32)(param_1 >> 32));
    FuncReturn(retval);
}

template <ResultCode func(u32*, u32, u32, u32, u32)>
void Wrap() {
    u32 param_1 = 0;
    // The last parameter is passed in R0 instead of R4
    u32 retval = func(&param_1, Param(1), Param(2), Param(3), Param(0)).raw;
    Return(1, param_1);
    FuncReturn(retval);
}

template <ResultCode func(u32, s64, s64)>
void Wrap() {
    s64 param1 = ((u64)Param(3) << 32) | Param(2);
    s64 param2 = ((u64)Param(4) << 32) | Param(1);
    FuncReturn(func(Param(0), param1, param2).raw);
}

template <ResultCode func(s64*, Kernel::Handle, u32)>
void Wrap() {
    s64 param_1 = 0;
    u32 retval = func(&param_1, Param(1), Param(2)).raw;
    Return(1, (u32)param_1);
    Return(2, (u32)(param_1 >> 32));
    FuncReturn(retval);
}

template <ResultCode func(Kernel::Handle, u32)>
void Wrap() {
    FuncReturn(func(Param(0), Param(1)).raw);
}

template <ResultCode func(Kernel::Handle*, Kernel::Handle*, VAddr, u32)>
void Wrap() {
    Kernel::Handle param_1 = 0;
    Kernel::Handle param_2 = 0;
    u32 retval = func(&param_1, &param_2, Param(2), Param(3)).raw;
    Return(1, param_1);
    Return(2, param_2);
    FuncReturn(retval);
}

template <ResultCode func(Kernel::Handle*, Kernel::Handle*)>
void Wrap() {
    Kernel::Handle param_1 = 0;
    Kernel::Handle param_2 = 0;
    u32 retval = func(&param_1, &param_2).raw;
    Return(1, param_1);
    Return(2, param_2);
    FuncReturn(retval);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Function wrappers that return type u32

template <u32 func()>
void Wrap() {
    FuncReturn(func());
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Function wrappers that return type s64

template <s64 func()>
void Wrap() {
    FuncReturn64(func());
}

////////////////////////////////////////////////////////////////////////////////////////////////////
/// Function wrappers that return type void

template <void func(s64)>
void Wrap() {
    func(((s64)Param(1) << 32) | Param(0));
}

template <void func(VAddr, s32 len)>
void Wrap() {
    func(Param(0), Param(1));
}

template <void func(u8)>
void Wrap() {
    func((u8)Param(0));
}

} // namespace HLE

#include <cstdio>
#include <type_traits>

template <typename F>
struct Stub;

template <typename R, typename T, typename... Ts>
struct Stub<R(T, Ts...)> {
    static R Svc(T t, Ts... ts) {
        if constexpr (std::is_pointer_v<T>) {
            u8* buf = (u8*)t;
            std::size_t size = sizeof(*t);
            for (std::size_t i = 0; i < size; ++i) {
                buf[i] = (u8)rand();
            }
        } else {
            u32 hash = (u32)Common::CityHash64((char*)&t, sizeof(t));
            std::printf("Received %08X\n", hash);
        }
        return Stub<R(Ts...)>::Svc(ts...);
    }
};

template <typename R>
struct Stub<R()> {
    static R Svc() {
        if constexpr (!std::is_void_v<R>) {
            R r{};
            u8* buf = (u8*)&r;
            std::size_t size = sizeof(R);
            for (std::size_t i = 0; i < size; ++i) {
                buf[i] = (u8)rand();
            }
            return r;
        }
    }
};

static int counter = 0;

template <typename F>
void DoTest() {
    std::printf("\n[%d]\n", counter++);
    HLE::Wrap<Stub<F>::Svc>();
    u32 fin = Common::CityHash64((char*)&reg, sizeof(reg));
    std::printf("Final reg = %08X\n", fin);
}

int main(int argc, char** argv) {
    std::srand(0);
    std::printf("SVC Wrapper test\n");

    reg[0] = rand();
    reg[1] = rand();
    reg[2] = rand();
    reg[3] = rand();
    reg[4] = rand();
    reg[5] = rand();
    reg[6] = rand();
    reg[7] = rand();

    DoTest<ResultCode(u32, u32, u32, u32)>();
    DoTest<ResultCode(u32*, u32, u32, u32, u32, u32)>();
    DoTest<ResultCode(u32*, u32, u32, VAddr, u32, s32)>();
    DoTest<ResultCode(s32*, VAddr, s32, bool, s64)>();
    DoTest<ResultCode(s32*, VAddr, s32, u32)>();
    DoTest<ResultCode(u32, u32, u32, u32, s64)>();
    DoTest<ResultCode(u32*)>();
    DoTest<ResultCode(u32, s64)>();
    DoTest<ResultCode(Kernel::MemoryInfo*, Kernel::PageInfo*, u32)>();
    DoTest<ResultCode(Kernel::MemoryInfo*, Kernel::PageInfo*, Kernel::Handle, u32)>();
    DoTest<ResultCode(s32*, u32)>();
    DoTest<ResultCode(u32, s32)>();
    DoTest<ResultCode(u32*, u32)>();
    DoTest<ResultCode(u32)>();
    DoTest<ResultCode(u32*, s32, s32)>();
    DoTest<ResultCode(s32*, u32, s32)>();
    DoTest<ResultCode(s64*, u32, s32)>();
    DoTest<ResultCode(u32*, u32, u32, u32, u32)>();
    DoTest<ResultCode(u32, s64, s64)>();
    DoTest<ResultCode(s64*, Kernel::Handle, u32)>();
    DoTest<ResultCode(Kernel::Handle, u32)>();
    DoTest<ResultCode(Kernel::Handle*, Kernel::Handle*, VAddr, u32)>();
    DoTest<ResultCode(Kernel::Handle*, Kernel::Handle*)>();
    DoTest<u32()>();
    DoTest<s64()>();
    DoTest<void(s64)>();
    DoTest<void(VAddr, s32 len)>();
    DoTest<void(u8)>();

    return 0;
}
