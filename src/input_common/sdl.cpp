// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <unordered_map>
#include <SDL.h>
#include "common/math_util.h"
#include "input_common/sdl.h"

namespace InputCommon {

namespace SDL {

class SDLJoystick;
class SDLButtonFactory;
class SDLAnalogFactory;
static std::unordered_map<int, std::weak_ptr<SDLJoystick>> joystick_list;
static std::shared_ptr<SDLButtonFactory> button_factory;
static std::shared_ptr<SDLAnalogFactory> analog_factory;

class SDLJoystick {
public:
    SDLJoystick(int joystick_index) {
        joystick = SDL_JoystickOpen(joystick_index);
        if (!joystick) {
            LOG_ERROR(Input, "failed to open joystick %d", joystick_index);
        }
    }

    ~SDLJoystick() {
        if (joystick) {
            SDL_JoystickClose(joystick);
        }
    }

    bool GetButton(int button) const {
        if (!joystick)
            return {};
        SDL_JoystickUpdate();
        return SDL_JoystickGetButton(joystick, button) == 1;
    }

    std::tuple<float, float> GetAnalog(int axis_x, int axis_y) const {
        if (!joystick)
            return {};
        SDL_JoystickUpdate();
        float x = SDL_JoystickGetAxis(joystick, axis_x) / 32767.0f;
        float y = SDL_JoystickGetAxis(joystick, axis_y) / 32767.0f;
        y = -y; // 3DS uses an y-axis inverse from SDL

        // Make sure the coordinates are in the unit circle,
        // otherwise normalize it.
        float r = x * x + y * y;
        if (r > 1) {
            r = std::sqrt(r);
            x /= r;
            y /= r;
        }

        return std::make_tuple(x, y);
    }

private:
    SDL_Joystick* joystick;
};

class SDLButton : public Input::ButtonDevice {
public:
    SDLButton(std::shared_ptr<SDLJoystick> joystick, int button)
        : joystick(joystick), button(button) {}

    bool GetStatus() const {
        return joystick->GetButton(button);
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    int button;
};

class SDLAnalog : public Input::AnalogDevice {
public:
    SDLAnalog(std::shared_ptr<SDLJoystick> joystick, int axis_x, int axis_y)
        : joystick(joystick), axis_x(axis_x), axis_y(axis_y) {}

    std::tuple<float, float> GetStatus() const {
        return joystick->GetAnalog(axis_x, axis_y);
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    int axis_x;
    int axis_y;
};

static std::shared_ptr<SDLJoystick> GetJoystick(int joystick_index) {
    std::shared_ptr<SDLJoystick> joystick = joystick_list[joystick_index].lock();
    if (!joystick) {
        joystick = std::make_shared<SDLJoystick>(joystick_index);
        joystick_list[joystick_index] = joystick;
    }
    return joystick;
}

/// A button device factory that creates button devices from SDL joystick
class SDLButtonFactory : public Input::Factory<Input::ButtonDevice> {
public:
    /**
     * Creates a button device from a joystick button
     * @param params contains parameters for creating the device:
     *     "joystick": the index of the joystick to bind
     *     "button": the index of the button to bind
     */
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const int joystick_index = params.Get("joystick", 0);
        const int button = params.Get("button", 0);
        return std::make_unique<SDLButton>(GetJoystick(joystick_index), button);
    }
};

/// An analog device factory that creates analog devices from SDL joystick
class SDLAnalogFactory : public Input::Factory<Input::AnalogDevice> {
public:
    /**
     * Creates analog device from joystick axes
     * @param params contains parameters for creating the device:
     *     "joystick": the index of the joystick to bind
     *     "axis_x": the index of the axis to be bind as x-axis
     *     "axis_y": the index of the axis to be bind as y-axis
     */
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const int joystick_index = params.Get("joystick", 0);
        const int axis_x = params.Get("axis_x", 0);
        const int axis_y = params.Get("axis_y", 1);
        return std::make_unique<SDLAnalog>(GetJoystick(joystick_index), axis_x, axis_y);
    }
};

void Init() {
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        LOG_CRITICAL(Input, "SDL_Init(SDL_INIT_JOYSTICK) failed with: %s", SDL_GetError());
    } else {
        using namespace Input;
        RegisterFactory<ButtonDevice>("sdl", std::make_shared<SDLButtonFactory>());
        RegisterFactory<AnalogDevice>("sdl", std::make_shared<SDLAnalogFactory>());
    }
}

} // namespace SDL
} // namespace InputCommon
