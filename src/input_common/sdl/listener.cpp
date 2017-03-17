// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#ifdef HAVE_SDL2
#include <SDL.h>
#endif
#include "common/logging/log.h"
#include "common/param_package.h"
#include "input_common/sdl/listener.h"

namespace InputCommon {
namespace SDL {

#ifdef HAVE_SDL2

class ListenerDetail {
public:
    ListenerDetail(ParamsHandler button_params_handler_, ParamsHandler analog_params_handler_)
        : button_params_handler(button_params_handler_),
          analog_params_handler(analog_params_handler_) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
            LOG_CRITICAL(Input, "SDL_InitSubSystem(SDL_INIT_JOYSTICK) failed with: %s",
                         SDL_GetError());
        }

        SDL_JoystickEventState(SDL_ENABLE);

        const int num_joysticks = SDL_NumJoysticks();
        for (int i = 0; i < num_joysticks; ++i) {
            SDL_Joystick* joystick = SDL_JoystickOpen(i);
            if (!joystick) {
                LOG_ERROR(Input, "SDL_JoystickOpen(%d) failed with: %s", i, SDL_GetError());
            } else {
                joysticks[joystick] = i;
            }
        }

        listen_button_stop.store(false);

        listen_button_thread =
            std::make_unique<std::thread>(&ListenerDetail::ListenButtonThread, this);
    }

    ~ListenerDetail() {
        listen_button_stop.store(true);
        listen_button_thread->join();

        for (auto& pair : joysticks) {
            SDL_JoystickClose(pair.first);
        }

        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }

private:
    ParamsHandler button_params_handler;
    ParamsHandler analog_params_handler;
    std::unordered_map<SDL_Joystick*, int> joysticks;
    std::atomic<bool> listen_button_stop;
    std::unique_ptr<std::thread> listen_button_thread;

    void ListenButtonThread() {
        LOG_INFO(Input, "called");
        while (!listen_button_stop.load()) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_JOYBUTTONDOWN: {
                    SDL_Joystick* joystick = SDL_JoystickFromInstanceID(event.jbutton.which);
                    int index = joysticks.at(joystick);
                    button_params_handler(Common::ParamPackage{
                        {"engine", "sdl"},
                        {"joystick", std::to_string(index)},
                        {"button", std::to_string(event.jbutton.button)},
                    }.Serialize());
                    break;
                }
                case SDL_JOYHATMOTION: {
                    SDL_Joystick* joystick = SDL_JoystickFromInstanceID(event.jhat.which);
                    int index = joysticks.at(joystick);
                    std::string direction;
                    if (event.jhat.value == SDL_HAT_UP) {
                        direction = "up";
                    } else if (event.jhat.value == SDL_HAT_DOWN) {
                        direction = "down";
                    } else if (event.jhat.value == SDL_HAT_LEFT) {
                        direction = "left";
                    } else if (event.jhat.value == SDL_HAT_RIGHT) {
                        direction = "right";
                    } else {
                        break;
                    }
                    button_params_handler(Common::ParamPackage{
                        {"engine", "sdl"},
                        {"joystick", std::to_string(index)},
                        {"hat", std::to_string(event.jhat.hat)},
                        {"direction", direction},
                    }.Serialize());
                    break;
                }
                case SDL_JOYAXISMOTION: {
                    if (std::abs(event.jaxis.value) > 15000) {
                        SDL_Joystick* joystick = SDL_JoystickFromInstanceID(event.jaxis.which);
                        int index = joysticks.at(joystick);
                        int axis = event.jaxis.axis / 2 * 2;
                        analog_params_handler(Common::ParamPackage{
                            {"engine", "sdl"},
                            {"joystick", std::to_string(index)},
                            {"axis_x", std::to_string(axis)},
                            {"axis_y", std::to_string(axis + 1)},
                        }.Serialize());
                    }
                } break;
                }
            }
        }
    }
};

#endif // HAVE_SDL2

Listener::Listener(ParamsHandler button_params_handler, ParamsHandler analog_params_handler) {
#ifdef HAVE_SDL2
    detail = std::make_unique<ListenerDetail>(button_params_handler, analog_params_handler);
#endif
}

Listener::~Listener() = default;

} // namespace SDL
} // namespace InputCommon