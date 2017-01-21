// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include "input_common/keyboard.h"

namespace InputCommon {

class KeyButton : public Input::ButtonDevice {
public:
    KeyButton(std::shared_ptr<KeyButtonList> key_button_list_)
        : key_button_list(key_button_list_), status(false) {}

    ~KeyButton();

    bool GetStatus() const override {
        return status.load();
    }

    friend class KeyButtonList;

private:
    std::shared_ptr<KeyButtonList> key_button_list;
    std::atomic<bool> status;
};

struct KeyButtonPair {
    int key_code;
    KeyButton* key_button;
};

class KeyButtonList {
public:
    void AddKeyButton(int key_code, KeyButton* key_button) {
        std::lock_guard<std::mutex> guard(mutex);
        list.push_back(KeyButtonPair{key_code, key_button});
    }

    void RemoveKeyButton(KeyButton* key_button) {
        std::lock_guard<std::mutex> guard(mutex);
        list.remove_if([key_button](KeyButtonPair& pair) { return pair.key_button == key_button; });
    }

    void ChangeKeyStatus(int key_code, bool pressed) {
        std::lock_guard<std::mutex> guard(mutex);
        for (const KeyButtonPair& pair : list) {
            if (pair.key_code == key_code)
                pair.key_button->status.store(pressed);
        }
    }

private:
    std::mutex mutex;
    std::list<KeyButtonPair> list;
};

KeyButton::~KeyButton() {
    key_button_list->RemoveKeyButton(this);
}

Keyboard::Keyboard() {
    key_button_list = std::make_shared<KeyButtonList>();
}

std::unique_ptr<Input::ButtonDevice> Keyboard::Create(const Common::ParamPackage& params) {
    int key_code = params.Get("code", 0);
    std::unique_ptr<KeyButton> button = std::make_unique<KeyButton>(key_button_list);
    key_button_list->AddKeyButton(key_code, button.get());
    return std::move(button);
}

void Keyboard::PressKey(int key_code) {
    key_button_list->ChangeKeyStatus(key_code, true);
}

void Keyboard::ReleaseKey(int key_code) {
    key_button_list->ChangeKeyStatus(key_code, false);
}

} // namespace InputCommon
