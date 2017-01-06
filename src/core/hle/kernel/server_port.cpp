// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>
#include "common/assert.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/server_port.h"
#include "core/hle/kernel/thread.h"

namespace Kernel {

ServerPort::ServerPort() {}
ServerPort::~ServerPort() {}

bool ServerPort::ShouldWait() {
    // If there are no pending sessions, we wait until a new one is added.
    return pending_sessions.size() == 0;
}

void ServerPort::Acquire() {
    ASSERT_MSG(!ShouldWait(), "object unavailable!");
}

std::tuple<SharedPtr<ServerPort>, SharedPtr<ClientPort>> ServerPort::CreatePortPair(
    u32 max_sessions, std::string name,
    std::shared_ptr<Service::SessionRequestHandler> hle_handler) {

    SharedPtr<ServerPort> server_port(new ServerPort);
    SharedPtr<ClientPort> client_port(new ClientPort);

    server_port->name = name + "_Server";
    server_port->hle_handler = std::move(hle_handler);
    client_port->name = name + "_Client";
    client_port->server_port = server_port;
    client_port->max_sessions = max_sessions;
    client_port->active_sessions = 0;

    return std::make_tuple(std::move(server_port), std::move(client_port));
}

} // namespace
