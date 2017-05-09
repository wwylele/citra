// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include "common/assert.h"
#include "common/bit_field.h"
#include "common/color.h"
#include "common/math_util.h"
#include "common/vector_math.h"
#include "video_core/pica_state.h"
#include "video_core/regs_texturing.h"
#include "video_core/swrasterizer/proctex.h"
#include "video_core/utils.h"

namespace Pica {
namespace Rasterizer {

using ProcTexClamp = TexturingRegs::ProcTexClamp;
using ProcTexShift = TexturingRegs::ProcTexShift;
using ProcTexCombiner = TexturingRegs::ProcTexCombiner;
using ProcTexFilter = TexturingRegs::ProcTexFilter;

static float LookupLUT(const State::ProcTex::Lut128& lut, float index) {
    // For ProcTex LUT with 128 entries, index=0.0 is lut[0], index=127.0/128.0 is lut[127]
    // and index=1.0 is lut[127]+lut_diff[127]. For other indices, the result is interpolated using
    // value entries and difference entries.
    index *= 128;
    const int index_int = std::min(static_cast<int>(index), 127);
    const float frac = index - index_int;
    return lut[index_int].ToFloat() + frac * lut[index_int].DiffToFloat();
}

// These function are used to generate random noise for procedural texture. Their results are
// verified against real hardware.
static int UnknownPRNG(int v) {
    const int h[]{0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11};
    return ((v % 9 + 2) * 3 & 0xF) ^ h[(v / 9) & 0xF];
}

static float NoiseRand(int x, int y) {
    const int h[]{10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14};
    int u2 = UnknownPRNG(x);
    int v2 = UnknownPRNG(y);
    v2 += ((u2 & 3) == 1) ? 4 : 0;
    v2 ^= (u2 & 1) * 6;
    v2 += 10 + u2;
    v2 &= 0xF;
    v2 ^= h[u2];
    return -1.0f + v2 * 2.0f / 15.0f;
}

static float NoiseCoef(float u, float v) {
    auto& regs = g_state.regs.texturing;
    const float freq_u = float16::FromRaw(regs.proctex_noise_frequency.u).ToFloat32();
    const float freq_v = float16::FromRaw(regs.proctex_noise_frequency.v).ToFloat32();
    const float phase_u = float16::FromRaw(regs.proctex_noise_u.phase).ToFloat32();
    const float phase_v = float16::FromRaw(regs.proctex_noise_v.phase).ToFloat32();
    const float x = 9 * freq_u * std::abs(u + phase_u);
    const float y = 9 * freq_v * std::abs(v + phase_v);
    const int x_int = static_cast<int>(x);
    const int y_int = static_cast<int>(y);
    const float x_frac = x - x_int;
    const float y_frac = y - y_int;

    const float g0 = NoiseRand(x_int, y_int) * (x_frac + y_frac);
    const float g1 = NoiseRand(x_int + 1, y_int) * (x_frac + y_frac - 1);
    const float g2 = NoiseRand(x_int, y_int + 1) * (x_frac + y_frac - 1);
    const float g3 = NoiseRand(x_int + 1, y_int + 1) * (x_frac + y_frac - 2);
    const float x_noise = LookupLUT(g_state.proctex.noise_table, x_frac);
    const float y_noise = LookupLUT(g_state.proctex.noise_table, y_frac);
    const float x0 = g0 * (1 - x_noise) + g1 * x_noise;
    const float x1 = g2 * (1 - x_noise) + g3 * x_noise;
    return x0 * (1 - y_noise) + x1 * y_noise;
}

static float ShiftCoord(float u, float v, ProcTexShift mode, ProcTexClamp clamp_mode) {
    const float offset = (clamp_mode == ProcTexClamp::MirroredRepeat) ? 1 : 0.5f;
    switch (mode) {
    case ProcTexShift::None:
        return u;
    case ProcTexShift::Odd:
        return u + offset * (((int)v / 2) % 2);
    case ProcTexShift::Even:
        return u + offset * ((((int)v + 1) / 2) % 2);
    default:
        LOG_CRITICAL(HW_GPU, "Unknown shift mode %u", static_cast<u32>(mode));
        return u;
    }
};

static void ClampCoord(float& coord, ProcTexClamp mode) {
    switch (mode) {
    case ProcTexClamp::ToZero:
        if (coord > 1.0f)
            coord = 0.0f;
        break;
    case ProcTexClamp::ToEdge:
        coord = std::min(coord, 1.0f);
        break;
    case ProcTexClamp::SymmetricalRepeat:
        coord = coord - std::floor(coord);
        break;
    case ProcTexClamp::MirroredRepeat: {
        int integer = static_cast<int>(coord);
        float frac = coord - integer;
        coord = (integer % 2) == 0 ? frac : (1.0f - frac);
        break;
    }
    case ProcTexClamp::Pulse:
        if (coord < 0.5f)
            coord = 0.0f;
        else
            coord = 1.0f;
        break;
    default:
        LOG_CRITICAL(HW_GPU, "Unknown clamp mode %u", static_cast<u32>(mode));
        coord = std::min(coord, 1.0f);
        break;
    }
}

float CombineAndMap(float u, float v, ProcTexCombiner combiner,
                    const State::ProcTex::Lut128& map_table) {
    float f;
    switch (combiner) {
    case ProcTexCombiner::U:
        f = u;
        break;
    case ProcTexCombiner::U2:
        f = u * u;
        break;
    case TexturingRegs::ProcTexCombiner::V:
        f = v;
        break;
    case TexturingRegs::ProcTexCombiner::V2:
        f = v * v;
        break;
    case TexturingRegs::ProcTexCombiner::Add:
        f = (u + v) * 0.5f;
        break;
    case TexturingRegs::ProcTexCombiner::Add2:
        f = (u * u + v * v) * 0.5f;
        break;
    case TexturingRegs::ProcTexCombiner::SqrtAdd2:
        f = std::min(std::sqrt(u * u + v * v), 1.0f);
        break;
    case TexturingRegs::ProcTexCombiner::Min:
        f = std::min(u, v);
        break;
    case TexturingRegs::ProcTexCombiner::Max:
        f = std::max(u, v);
        break;
    case TexturingRegs::ProcTexCombiner::RMax:
        f = std::min(((u + v) * 0.5f + std::sqrt(u * u + v * v)) * 0.5f, 1.0f);
        break;
    default:
        LOG_CRITICAL(HW_GPU, "Unknown combiner %u", static_cast<u32>(combiner));
        f = 0.0f;
        break;
    }
    return LookupLUT(map_table, f);
}

Math::Vec4<u8> ProcTex(float u, float v) {
    u = std::abs(u);
    v = std::abs(v);

    const auto& regs = g_state.regs.texturing;

    // Generate noise
    if (regs.proctex.noise_enable) {
        float noise = NoiseCoef(u, v);
        u += noise * regs.proctex_noise_u.amplitude / 4095.0f;
        v += noise * regs.proctex_noise_v.amplitude / 4095.0f;
        u = std::abs(u);
        v = std::abs(v);
    }

    // Shift
    const float u_shift = ShiftCoord(u, v, regs.proctex.u_shift, regs.proctex.u_clamp);
    const float v_shift = ShiftCoord(v, u, regs.proctex.v_shift, regs.proctex.v_clamp);
    u = u_shift;
    v = v_shift;

    // Clamp
    ClampCoord(u, regs.proctex.u_clamp);
    ClampCoord(v, regs.proctex.v_clamp);

    // Combine and map
    const float lut_index =
        CombineAndMap(u, v, regs.proctex.color_combiner, g_state.proctex.color_map_table);

    // Look up the color
    // For the color lut, index=0.0 is lut[offset] and index=1.0 is lut[offset+width-1]
    const u32 offset = regs.proctex_lut_offset;
    const u32 width = regs.proctex_lut.width;
    const float index = offset + (lut_index * (width - 1));
    Math::Vec4<u8> final_color;
    // TODO(wwylele): implement mipmap
    switch (regs.proctex_lut.filter) {
    case ProcTexFilter::Linear:
    case ProcTexFilter::LinearMipmapLinear:
    case ProcTexFilter::LinearMipmapNearest: {
        const int index_int = static_cast<int>(index);
        const float frac = index - index_int;
        const auto color_value = g_state.proctex.color_table[index_int].ToVector().Cast<float>();
        const auto color_diff =
            g_state.proctex.color_diff_table[index_int].ToVector().Cast<float>();
        final_color = (color_value + frac * color_diff).Cast<u8>();
        break;
    }
    case ProcTexFilter::Nearest:
    case ProcTexFilter::NearestMipmapLinear:
    case ProcTexFilter::NearestMipmapNearest:
        final_color = g_state.proctex.color_table[static_cast<int>(std::round(index))].ToVector();
        break;
    }

    if (regs.proctex.separate_alpha) {
        const float final_alpha =
            CombineAndMap(u, v, regs.proctex.alpha_combiner, g_state.proctex.alpha_map_table);
        return Math::MakeVec<u8>(final_color.rgb(), static_cast<u8>(final_alpha * 255));
    } else {
        return final_color;
    }
}

} // namespace Rasterizer
} // namespace Pica
