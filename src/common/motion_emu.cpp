// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/emu_window.h"
#include "common/math_util.h"
#include "common/motion_emu.h"
#include "common/thread.h"
#include "common/vector_math.h"

namespace MotionEmu {

struct Quaternion {
    Math::Vec3<float> xyz;
    float w;

    Quaternion Inverse() {
        return Quaternion { -xyz, w };
    }

    Quaternion operator- (const Quaternion& a) {
        return Quaternion { xyz - a.xyz, w - a.w };
    }
};

inline Quaternion Product(const Quaternion& a, const Quaternion& b) {
    return Quaternion {
        a.xyz * b.w + b.xyz * a.w + Cross(a.xyz, b.xyz),
        a.w * b.w + Dot(a.xyz, b.xyz)
    };
}

inline Quaternion MakeQuaternion(const Math::Vec3<float>& axis, float angle) {
    return Quaternion {
        axis * std::sin(angle / 2),
        std::cos(angle / 2)
    };
}

inline Math::Vec3<float> QuaternionRotate(const Quaternion& q, const Math::Vec3<float>& v) {
    return v + 2 * Cross(q.xyz, Cross(q.xyz, v) + v * q.w);
}

static std::unique_ptr<std::thread> motion_emu_thread;
static Common::Event shutdown_event;

static constexpr int update_millisecond = 100;
static constexpr auto update_duration =
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(update_millisecond));
static constexpr float PI = 3.14159265f;

static Math::Vec2<int> mouse_origin;

static std::mutex tilt_mutex;
static Math::Vec2<float> tilt_direction;
static float tilt_angle;

static bool is_tilting;

static void MotionEmuThread(EmuWindow& emu_window) {
    auto update_time = std::chrono::steady_clock::now();
    Quaternion q = MakeQuaternion(Math::Vec3<float>(), 0) , old_q;

    while (!shutdown_event.WaitUntil(update_time)) {
        update_time += update_duration;
        old_q = q;

        {
            std::lock_guard<std::mutex> guard(tilt_mutex);

            // Find the quaternion describing current 3DS tilting
            q = MakeQuaternion(Math::MakeVec(-tilt_direction.y, 0.0f, tilt_direction.x), tilt_angle);
        }

        auto inv_q = q.Inverse();

        // Set the gravity vector in world space
        auto gravity = Math::MakeVec(0.0f, -1.0f, 0.0f);

        // Find the angular rate vector in world space
        auto angular_rate = Product(q - old_q, inv_q).xyz * 2;
        angular_rate *= 1000 / update_millisecond / PI * 180;

        // Transform the two vectors from world space to 3DS space
        gravity = QuaternionRotate(inv_q, gravity);
        angular_rate = QuaternionRotate(inv_q, angular_rate);

        // Update the sensor state
        emu_window.AccelerometerChanged(gravity.x, gravity.y, gravity.z);
        emu_window.GyroscopeChanged(angular_rate.x, angular_rate.y, angular_rate.z);
    }
}

void Init(EmuWindow& emu_window) {
    Shutdown();
    tilt_angle = 0;
    is_tilting = false;
    motion_emu_thread = std::make_unique<std::thread>(MotionEmuThread, std::ref(emu_window));
}

void Shutdown() {
    if (motion_emu_thread) {
        shutdown_event.Set();
        motion_emu_thread->join();
        motion_emu_thread = nullptr;
    }
}

void BeginTilt(int x, int y) {
    mouse_origin = Math::MakeVec(x, y);
    is_tilting = true;
}

void Tilt(int x, int y) {
    constexpr float SENSITIVITY = 0.01f;
    auto mouse_move = Math::MakeVec(x, y) - mouse_origin;
    if (is_tilting) {
        std::lock_guard<std::mutex> guard(tilt_mutex);
        if (mouse_move.x == 0 && mouse_move.y == 0) {
            tilt_angle = 0;
        } else {
            tilt_direction = mouse_move.Cast<float>();
            tilt_angle = MathUtil::Clamp(tilt_direction.Normalize() * SENSITIVITY, 0.0f, PI * 0.5f);
        }
    }

}

void EndTilt() {
    std::lock_guard<std::mutex> guard(tilt_mutex);
    tilt_angle = 0;
    is_tilting = false;
}

}
