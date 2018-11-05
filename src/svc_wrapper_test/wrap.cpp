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
/*
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
*/

// A special param index represents the return value
constexpr std::size_t INDEX_RETURN = ~(std::size_t)0;

struct ParamSlot {
    // whether the slot is used for a param
    bool used;

    // index of a param in the function signature, starting from 0, or special value INDEX_RETURN
    std::size_t param_index;

    // index of a 32-bit word within the param
    std::size_t word_index;
};

// Size in words of the given type in ARM ABI
template <typename T>
constexpr std::size_t WordSize() {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (std::is_pointer_v<T>) {
        return 1;
    } else if constexpr (std::is_same_v<T, bool>) {
        return 1;
    } else {
        return (sizeof(T) - 1) / 4 + 1;
    }
}

// Size in words of the given type in ARM ABI with pointer removed
template <typename T>
constexpr std::size_t OutputWordSize() {
    if constexpr (std::is_pointer_v<T>) {
        return WordSize<std::remove_pointer_t<T>>();
    } else {
        return 0;
    }
}

// Describes the register assignments of a SVC
struct SVCABI {
    static constexpr std::size_t RegCount = 8;

    // Register assignments for input params.
    // For example, in[0] records which input param and word r0 stores
    std::array<ParamSlot, RegCount> in{};

    // Register assignments for output params.
    // For example, out[0] records which output param (with pointer removed) and word r0 stores
    std::array<ParamSlot, RegCount> out{};

    // Search the register assignments for a word of a param
    constexpr std::size_t GetRegisterIndex(std::size_t param_index, std::size_t word_index) const {
        for (std::size_t slot = 0; slot < RegCount; ++slot) {
            if (in[slot].used && in[slot].param_index == param_index &&
                in[slot].word_index == word_index) {
                return slot;
            }
            if (out[slot].used && out[slot].param_index == param_index &&
                out[slot].word_index == word_index) {
                return slot;
            }
        }
        // should throw, but gcc bugs out here
        return 0x12345678;
    }
};

// Generates ABI info for a given SVC signature
template <typename R, typename... T>
constexpr SVCABI GetSVCABI() {
    constexpr std::size_t param_count = sizeof...(T);
    std::array<bool, param_count> param_is_output{{std::is_pointer_v<T>...}};
    std::array<std::size_t, param_count> param_size{{WordSize<T>()...}};
    std::array<std::size_t, param_count> output_param_size{{OutputWordSize<T>()...}};
    std::array<bool, param_count> param_need_align{
        {(std::is_same_v<T, u64> || std::is_same_v<T, s64>)...}};

    // derives ARM ABI, which assigns all params to r0~r3 and stack
    std::array<ParamSlot, 4> armabi_reg{};
    std::array<ParamSlot, 4> armabi_stack{};
    std::size_t reg_pos = 0;
    std::size_t stack_pos = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        if (param_need_align[i]) {
            reg_pos += reg_pos % 2;
        }
        for (std::size_t j = 0; j < param_size[i]; ++j) {
            if (reg_pos == 4) {
                armabi_stack[stack_pos++] = ParamSlot{true, i, j};
            } else {
                armabi_reg[reg_pos++] = ParamSlot{true, i, j};
            }
        }
    }

    // now derives SVC ABI which is a modified version of ARM ABI

    SVCABI mod_abi{};
    std::size_t out_pos = 0;

    // assign return value to output registers
    if constexpr (!std::is_void_v<R>) {
        for (std::size_t j = 0; j < WordSize<R>(); ++j) {
            mod_abi.out[out_pos++] = {true, INDEX_RETURN, j};
        }
    }

    // assign output params to output registers
    for (std::size_t i = 0; i < param_count; ++i) {
        if (param_is_output[i]) {
            for (std::size_t j = 0; j < output_param_size[i]; ++j) {
                mod_abi.out[out_pos++] = ParamSlot{true, i, j};
            }
        }
    }

    // assign input params to input registers
    stack_pos = 0;
    for (std::size_t k = 0; k < mod_abi.in.size(); ++k) {
        if (k < armabi_reg.size() && armabi_reg[k].used &&
            !param_is_output[armabi_reg[k].param_index]) {
            // If this is within the ARM ABI register region
            // and it is a used input param,
            // assign it directly
            mod_abi.in[k] = armabi_reg[k];
        } else {
            // Otherwise, assign it with the next available stack input
            // If all stack inputs have been allocated, this would do nothing
            // and leaves the slot unused.
            while (stack_pos < armabi_stack.size() &&
                   (!armabi_stack[stack_pos].used ||
                    param_is_output[armabi_stack[stack_pos].param_index])) {
                ++stack_pos;
            }
            if (stack_pos < armabi_stack.size()) {
                mod_abi.in[k] = armabi_stack[stack_pos++];
            }
        }
    }

    return mod_abi;
}

template <std::size_t param_index, std::size_t word_size, std::size_t... indices>
constexpr std::array<std::size_t, word_size> GetRegIndicesImpl(SVCABI abi,
                                                               std::index_sequence<indices...>) {
    return {{abi.GetRegisterIndex(param_index, indices)...}};
}

/// Gets assigned register index for the param_index-th param of word_size in a function with
/// signature R(Ts...)
template <std::size_t param_index, std::size_t word_size, typename R, typename... Ts>
constexpr std::array<std::size_t, word_size> GetRegIndices() {
    constexpr SVCABI abi = GetSVCABI<R, Ts...>();
    return GetRegIndicesImpl<param_index, word_size>(abi, std::make_index_sequence<word_size>{});
}

// Gets the value for the param_index-th param of word_size in a function with signature R(Ts...)
template <std::size_t param_index, typename T, typename R, typename... Ts>
void GetParam(T& value) {
    constexpr auto regi = GetRegIndices<param_index, WordSize<T>(), R, Ts...>();
    if constexpr (std::is_class_v<T> || std::is_union_v<T>) {
        std::array<u32, WordSize<T>()> buf;
        for (std::size_t i = 0; i < WordSize<T>(); ++i) {
            buf[i] = Param(regi[i]);
        }
        std::memcpy(&value, &buf, sizeof(T));
    } else if constexpr (WordSize<T>() == 2) {
        u64 l = Param(regi[0]);
        u64 h = Param(regi[1]);
        value = static_cast<T>(l | (h << 32));
    } else if constexpr (std::is_same_v<T, bool>) {
        value = Param(regi[0]) != 0; // Is this correct or should only test the lowest byte?
    } else {
        value = static_cast<T>(Param(regi[0]));
    }
}

// Sets the value for the param_index-th param of word_size in a function with signature R(Ts...)
template <std::size_t param_index, typename T, typename R, typename... Ts>
void SetParam(const T& value) {
    constexpr auto regi = GetRegIndices<param_index, WordSize<T>(), R, Ts...>();
    if constexpr (std::is_class_v<T> || std::is_union_v<T>) {
        std::array<u32, WordSize<T>()> buf;
        std::memcpy(&buf, &value, sizeof(T));
        for (std::size_t i = 0; i < WordSize<T>(); ++i) {
            Return(regi[i], buf[i]);
        }
    } else if constexpr (WordSize<T>() == 2) {
        u64 u = static_cast<u64>(value);
        Return(regi[0], static_cast<u32>(u & 0xFFFFFFFF));
        Return(regi[1], static_cast<u32>(u >> 32));
    } else {
        u32 u = static_cast<u32>(value);
        Return(regi[0], u);
    }
}

template <typename SVCT, typename R, typename... Ts>
struct WrapPass;

template <typename SVCT, typename R, typename T, typename... Ts>
struct WrapPass<SVCT, R, T, Ts...> {
    // Do I/O for the param T in function (R svc(Us..., T, Ts...)) and then move on to the next
    // param Us are params whose I/O is already handled T is the current param to do I/O Ts are
    // params whose I/O is not handled yet
    template <typename... Us>
    static void Call(SVCT svc, Us... u) {
        constexpr std::size_t current_param_index = sizeof...(Us);
        if constexpr (std::is_pointer_v<T>) {
            using OutputT = std::remove_pointer_t<T>;
            OutputT output;
            WrapPass<SVCT, R, Ts...>::Call(svc, u..., &output);
            SetParam<current_param_index, OutputT, R, Us..., T, Ts...>(output);
        } else {
            T input;
            GetParam<current_param_index, T, R, Us..., T, Ts...>(input);
            WrapPass<SVCT, R, Ts...>::Call(svc, u..., input);
        }
    }
};

template <typename SVCT, typename R>
struct WrapPass<SVCT, R /*empty list for T, Ts*/> {
    // Call function (R svc(Us...)) and transfer the return value to registers
    template <typename... Us>
    static void Call(SVCT svc, Us... u) {
        if constexpr (std::is_void_v<R>) {
            svc(u...);
        } else {
            R r = svc(u...);
            SetParam<INDEX_RETURN, R, R, Us...>(r);
        }
    }
};

template <typename T>
struct WrapHelper;

template <typename R, typename... T>
struct WrapHelper<R (*)(T...)> {
    static void Call(R (*svc)(T...)) {
        WrapPass<decltype(svc), R, T...>::Call(svc /*Empty list for Us*/);
    }
};

template <auto F>
void Wrap() {
    WrapHelper<decltype(F)>::Call(F);
};

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
