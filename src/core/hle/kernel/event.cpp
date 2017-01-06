// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <map>
#include <vector>
#include "common/assert.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"

namespace Kernel {

Event::Event() {}
Event::~Event() {}

SharedPtr<Event> Event::Create(ResetType reset_type, std::string name) {
    SharedPtr<Event> evt(new Event);

    evt->signaled = false;
    evt->reset_type = reset_type;
    evt->name = std::move(name);

    if (reset_type == ResetType::Pulse) {
        LOG_ERROR(Kernel, "Unimplemented event reset type Pulse");
        UNIMPLEMENTED();
    }

    return evt;
}

bool Event::ShouldWait() {
    return !signaled;
}

void Event::Acquire() {
    ASSERT_MSG(!ShouldWait(), "object unavailable!");

    // Release the event if it's not sticky...
    if (reset_type != ResetType::Sticky)
        signaled = false;
}

void Event::Signal() {
    signaled = true;
    WakeupAllWaitingThreads();
}

void Event::Clear() {
    signaled = false;
}

} // namespace
