// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>

#include "common/assert.h"
#include "common/scm_rev.h"
#include "common/x64/cpu_detect.h"
#include "core/core.h"
#include "core/settings.h"
#include "core/telemetry_session.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/telemetry_json.h"
#endif

namespace Core {

static const char* CpuVendorToStr(Common::CPUVendor vendor) {
    switch (vendor) {
    case Common::CPUVendor::INTEL:
        return "Intel";
    case Common::CPUVendor::AMD:
        return "Amd";
    case Common::CPUVendor::OTHER:
        return "Other";
    }
    UNREACHABLE();
}

TelemetrySession::TelemetrySession() {
#ifdef ENABLE_WEB_SERVICE
    backend = std::make_unique<WebService::TelemetryJson>();
#else
    backend = std::make_unique<Telemetry::NullVisitor>();
#endif
    // Log one-time session start information
    const s64 init_time{std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count()};
    AddField(Telemetry::FieldType::Session, "Init_Time", init_time);
    std::string program_name;
    const Loader::ResultStatus res{System::GetInstance().GetAppLoader().ReadTitle(program_name)};
    if (res == Loader::ResultStatus::Success) {
        AddField(Telemetry::FieldType::Session, "ProgramName", program_name);
    }

    // Log application information
    const bool is_git_dirty{std::strstr(Common::g_scm_desc, "dirty") != nullptr};
    AddField(Telemetry::FieldType::App, "Git_IsDirty", is_git_dirty);
    AddField(Telemetry::FieldType::App, "Git_Branch", Common::g_scm_branch);
    AddField(Telemetry::FieldType::App, "Git_Revision", Common::g_scm_rev);
    AddField(Telemetry::FieldType::App, "BuildDate", Common::g_build_date);
    AddField(Telemetry::FieldType::App, "BuildName", Common::g_build_name);

    // Log user system information
    AddField(Telemetry::FieldType::UserSystem, "CPU_Model", Common::GetCPUCaps().cpu_string);
    AddField(Telemetry::FieldType::UserSystem, "CPU_BrandString",
             Common::GetCPUCaps().brand_string);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Vendor",
             CpuVendorToStr(Common::GetCPUCaps().vendor));
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_AES", Common::GetCPUCaps().aes);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_AVX", Common::GetCPUCaps().avx);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_AVX2", Common::GetCPUCaps().avx2);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_BMI1", Common::GetCPUCaps().bmi1);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_BMI2", Common::GetCPUCaps().bmi2);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_FMA", Common::GetCPUCaps().fma);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_FMA4", Common::GetCPUCaps().fma4);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSE", Common::GetCPUCaps().sse);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSE2", Common::GetCPUCaps().sse2);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSE3", Common::GetCPUCaps().sse3);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSSE3",
             Common::GetCPUCaps().ssse3);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSE41",
             Common::GetCPUCaps().sse4_1);
    AddField(Telemetry::FieldType::UserSystem, "CPU_Extension_x64_SSE42",
             Common::GetCPUCaps().sse4_2);
#ifdef __APPLE__
    AddField(Telemetry::FieldType::UserSystem, "OsPlatform", "Apple");
#elif defined(_WIN32)
    AddField(Telemetry::FieldType::UserSystem, "OsPlatform", "Windows");
#elif defined(__linux__) || defined(linux) || defined(__linux)
    AddField(Telemetry::FieldType::UserSystem, "OsPlatform", "Linux");
#else
    AddField(Telemetry::FieldType::UserSystem, "OsPlatform", "Unknown");
#endif

    // Log user configuration information
    AddField(Telemetry::FieldType::UserConfig, "Audio_EnableAudioStretching",
             Settings::values.enable_audio_stretching);
    AddField(Telemetry::FieldType::UserConfig, "Core_UseCpuJit", Settings::values.use_cpu_jit);
    AddField(Telemetry::FieldType::UserConfig, "Renderer_ResolutionFactor",
             Settings::values.resolution_factor);
    AddField(Telemetry::FieldType::UserConfig, "Renderer_ToggleFramelimit",
             Settings::values.toggle_framelimit);
    AddField(Telemetry::FieldType::UserConfig, "Renderer_UseHwRenderer",
             Settings::values.use_hw_renderer);
    AddField(Telemetry::FieldType::UserConfig, "Renderer_UseShaderJit",
             Settings::values.use_shader_jit);
    AddField(Telemetry::FieldType::UserConfig, "Renderer_UseVsync", Settings::values.use_vsync);
    AddField(Telemetry::FieldType::UserConfig, "System_IsNew3ds", Settings::values.is_new_3ds);
    AddField(Telemetry::FieldType::UserConfig, "System_RegionValue", Settings::values.region_value);
}

TelemetrySession::~TelemetrySession() {
    // Log one-time session end information
    const s64 shutdown_time{std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()};
    AddField(Telemetry::FieldType::Session, "Shutdown_Time", shutdown_time);

    // Complete the session, submitting to web service if necessary
    // This is just a placeholder to wrap up the session once the core completes and this is
    // destroyed. This will be moved elsewhere once we are actually doing real I/O with the service.
    field_collection.Accept(*backend);
    backend->Complete();
    backend = nullptr;
}

} // namespace Core
