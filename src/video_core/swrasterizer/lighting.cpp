// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/math_util.h"
#include "video_core/swrasterizer/lighting.h"

namespace Pica {

static float LookupLightingLut(const Pica::State::Lighting& lighting, size_t lut_index, u8 index,
                               float delta) {
    ASSERT_MSG(lut_index < lighting.luts.size(), "Out of range lut");
    ASSERT_MSG(index < lighting.luts[lut_index].size(), "Out of range index");

    const auto& lut = lighting.luts[lut_index][index];

    float lut_value = lut.ToFloat();
    float lut_diff = lut.DiffToFloat();

    return lut_value + lut_diff * delta;
}

std::tuple<Math::Vec4<u8>, Math::Vec4<u8>> ComputeFragmentsColors(
    const Pica::LightingRegs& lighting, const Pica::State::Lighting& lighting_state,
    const Math::Quaternion<float>& normquat, const Math::Vec3<float>& view) {

    // TODO(Subv): Bump mapping
    Math::Vec3<float> surface_normal = {0.0f, 0.0f, 1.0f};

    if (lighting.config0.bump_mode != LightingRegs::LightingBumpMode::None) {
        LOG_CRITICAL(HW_GPU, "unimplemented bump mapping");
        UNIMPLEMENTED();
    }

    // Use the normalized the quaternion when performing the rotation
    auto normal = Math::QuaternionRotate(normquat, surface_normal);

    Math::Vec4<float> diffuse_sum = {0.0f, 0.0f, 0.0f, 1.0f};
    Math::Vec4<float> specular_sum = {0.0f, 0.0f, 0.0f, 1.0f};

    for (unsigned light_index = 0; light_index <= lighting.max_light_index; ++light_index) {
        unsigned num = lighting.light_enable.GetNum(light_index);
        const auto& light_config = lighting.light[num];

        Math::Vec3<float> refl_value = {};
        Math::Vec3<float> position = {float16::FromRaw(light_config.x).ToFloat32(),
                                      float16::FromRaw(light_config.y).ToFloat32(),
                                      float16::FromRaw(light_config.z).ToFloat32()};
        Math::Vec3<float> light_vector;

        if (light_config.config.directional)
            light_vector = position;
        else
            light_vector = position + view;

        light_vector.Normalize();

        float dist_atten = 1.0f;
        if (!lighting.IsDistAttenDisabled(num)) {
            auto distance = (-view - position).Length();
            float scale = Pica::float20::FromRaw(light_config.dist_atten_scale).ToFloat32();
            float bias = Pica::float20::FromRaw(light_config.dist_atten_bias).ToFloat32();
            size_t lut =
                static_cast<size_t>(LightingRegs::LightingSampler::DistanceAttenuation) + num;

            float sample_loc = MathUtil::Clamp(scale * distance + bias, 0.0f, 1.0f);

            u8 lutindex =
                static_cast<u8>(MathUtil::Clamp(std::floor(sample_loc * 256.0f), 0.0f, 255.0f));
            float delta = sample_loc * 256 - lutindex;
            dist_atten = LookupLightingLut(lighting_state, lut, lutindex, delta);
        }

        auto GetLutValue = [&](LightingRegs::LightingLutInput input, bool abs,
                               LightingRegs::LightingScale scale_enum,
                               LightingRegs::LightingSampler sampler) {
            Math::Vec3<float> norm_view = view.Normalized();
            Math::Vec3<float> half_angle = (norm_view + light_vector).Normalized();
            float result = 0.0f;

            switch (input) {
            case LightingRegs::LightingLutInput::NH:
                result = Math::Dot(normal, half_angle);
                break;

            case LightingRegs::LightingLutInput::VH:
                result = Math::Dot(norm_view, half_angle);
                break;

            case LightingRegs::LightingLutInput::NV:
                result = Math::Dot(normal, norm_view);
                break;

            case LightingRegs::LightingLutInput::LN:
                result = Math::Dot(light_vector, normal);
                break;

            default:
                LOG_CRITICAL(HW_GPU, "Unknown lighting LUT input %u\n", static_cast<u32>(input));
                UNIMPLEMENTED();
                result = 0.0f;
            }

            u8 index;
            float delta;

            if (abs) {
                if (light_config.config.two_sided_diffuse)
                    result = std::abs(result);
                else
                    result = std::max(result, 0.0f);

                float flr = std::floor(result * 256.0f);
                index = static_cast<u8>(MathUtil::Clamp(flr, 0.0f, 255.0f));
                delta = result * 256 - index;
            } else {
                float flr = std::floor(result * 128.0f);
                s8 signed_index = static_cast<s8>(MathUtil::Clamp(flr, -128.0f, 127.0f));
                delta = result * 128.0f - signed_index;
                index = static_cast<u8>(signed_index);
            }

            float scale = lighting.lut_scale.GetScale(scale_enum);
            return scale *
                   LookupLightingLut(lighting_state, static_cast<size_t>(sampler), index, delta);
        };

        // Specular 0 component
        float d0_lut_value = 1.0f;
        if (lighting.config1.disable_lut_d0 == 0 &&
            LightingRegs::IsLightingSamplerSupported(
                lighting.config0.config, LightingRegs::LightingSampler::Distribution0)) {
            d0_lut_value =
                GetLutValue(lighting.lut_input.d0, lighting.abs_lut_input.disable_d0 == 0,
                            lighting.lut_scale.d0, LightingRegs::LightingSampler::Distribution0);
        }

        Math::Vec3<float> specular_0 = d0_lut_value * light_config.specular_0.ToVec3f();

        // If enabled, lookup ReflectRed value, otherwise, 1.0 is used
        if (lighting.config1.disable_lut_rr == 0 &&
            LightingRegs::IsLightingSamplerSupported(lighting.config0.config,
                                                     LightingRegs::LightingSampler::ReflectRed)) {
            refl_value.x =
                GetLutValue(lighting.lut_input.rr, lighting.abs_lut_input.disable_rr == 0,
                            lighting.lut_scale.rr, LightingRegs::LightingSampler::ReflectRed);
        } else {
            refl_value.x = 1.0f;
        }

        // If enabled, lookup ReflectGreen value, otherwise, ReflectRed value is used
        if (lighting.config1.disable_lut_rg == 0 &&
            LightingRegs::IsLightingSamplerSupported(lighting.config0.config,
                                                     LightingRegs::LightingSampler::ReflectGreen)) {
            refl_value.y =
                GetLutValue(lighting.lut_input.rg, lighting.abs_lut_input.disable_rg == 0,
                            lighting.lut_scale.rg, LightingRegs::LightingSampler::ReflectGreen);
        } else {
            refl_value.y = refl_value.x;
        }

        // If enabled, lookup ReflectBlue value, otherwise, ReflectRed value is used
        if (lighting.config1.disable_lut_rb == 0 &&
            LightingRegs::IsLightingSamplerSupported(lighting.config0.config,
                                                     LightingRegs::LightingSampler::ReflectBlue)) {
            refl_value.z =
                GetLutValue(lighting.lut_input.rb, lighting.abs_lut_input.disable_rb == 0,
                            lighting.lut_scale.rb, LightingRegs::LightingSampler::ReflectBlue);
        } else {
            refl_value.z = refl_value.x;
        }

        // Specular 1 component
        float d1_lut_value = 1.0f;
        if (lighting.config1.disable_lut_d1 == 0 &&
            LightingRegs::IsLightingSamplerSupported(
                lighting.config0.config, LightingRegs::LightingSampler::Distribution1)) {
            d1_lut_value =
                GetLutValue(lighting.lut_input.d1, lighting.abs_lut_input.disable_d1 == 0,
                            lighting.lut_scale.d1, LightingRegs::LightingSampler::Distribution1);
        }

        Math::Vec3<float> specular_1 =
            d1_lut_value * refl_value * light_config.specular_1.ToVec3f();

        // Fresnel
        if (lighting.config1.disable_lut_fr == 0 &&
            LightingRegs::IsLightingSamplerSupported(lighting.config0.config,
                                                     LightingRegs::LightingSampler::Fresnel)) {

            float lut_value =
                GetLutValue(lighting.lut_input.fr, lighting.abs_lut_input.disable_fr == 0,
                            lighting.lut_scale.fr, LightingRegs::LightingSampler::Fresnel);

            // Enabled for diffuse lighting alpha component
            if (lighting.config0.fresnel_selector ==
                    LightingRegs::LightingFresnelSelector::PrimaryAlpha ||
                lighting.config0.fresnel_selector == LightingRegs::LightingFresnelSelector::Both) {
                diffuse_sum.a() *= lut_value;
            }

            // Enabled for the specular lighting alpha component
            if (lighting.config0.fresnel_selector ==
                    LightingRegs::LightingFresnelSelector::SecondaryAlpha ||
                lighting.config0.fresnel_selector == LightingRegs::LightingFresnelSelector::Both) {
                specular_sum.a() *= lut_value;
            }
        }

        auto dot_product = Math::Dot(light_vector, normal);

        // Calculate clamp highlights before applying the two-sided diffuse configuration to the dot
        // product.
        float clamp_highlights = 1.0f;
        if (lighting.config0.clamp_highlights) {
            if (dot_product <= 0.0f)
                clamp_highlights = 0.0f;
            else
                clamp_highlights = 1.0f;
        }

        if (light_config.config.two_sided_diffuse)
            dot_product = std::abs(dot_product);
        else
            dot_product = std::max(dot_product, 0.0f);

        auto diffuse =
            light_config.diffuse.ToVec3f() * dot_product + light_config.ambient.ToVec3f();
        diffuse_sum += Math::MakeVec(diffuse * dist_atten, 0.0f);

        specular_sum +=
            Math::MakeVec((specular_0 + specular_1) * clamp_highlights * dist_atten, 0.0f);
    }

    diffuse_sum += Math::MakeVec(lighting.global_ambient.ToVec3f(), 0.0f);

    auto diffuse = Math::MakeVec<float>(MathUtil::Clamp(diffuse_sum.x, 0.0f, 1.0f) * 255,
                                        MathUtil::Clamp(diffuse_sum.y, 0.0f, 1.0f) * 255,
                                        MathUtil::Clamp(diffuse_sum.z, 0.0f, 1.0f) * 255,
                                        MathUtil::Clamp(diffuse_sum.w, 0.0f, 1.0f) * 255)
                       .Cast<u8>();
    auto specular = Math::MakeVec<float>(MathUtil::Clamp(specular_sum.x, 0.0f, 1.0f) * 255,
                                         MathUtil::Clamp(specular_sum.y, 0.0f, 1.0f) * 255,
                                         MathUtil::Clamp(specular_sum.z, 0.0f, 1.0f) * 255,
                                         MathUtil::Clamp(specular_sum.w, 0.0f, 1.0f) * 255)
                        .Cast<u8>();
    return {diffuse, specular};
}

} // namespace Pica
