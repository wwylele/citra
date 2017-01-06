// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/server_port.h"
#include "core/hle/kernel/server_session.h"

namespace Kernel {

ClientPort::ClientPort() {}
ClientPort::~ClientPort() {}

ResultVal<SharedPtr<ClientSession>> ClientPort::Connect() {
    // Note: Threads do not wait for the server endpoint to call
    // AcceptSession before returning from this call.

    if (active_sessions >= max_sessions) {
        // TODO(Subv): Return an error code in this situation after session disconnection is
        // implemented.
        /*return ResultCode(ErrorDescription::MaxConnectionsReached,
                          ErrorModule::OS, ErrorSummary::WouldBlock,
                          ErrorLevel::Temporary);*/
    }
    active_sessions++;

    // Create a new session pair, let the created sessions inherit the parent port's HLE handler.
    auto sessions =
        ServerSession::CreateSessionPair(server_port->GetName(), server_port->hle_handler);
    auto client_session = std::get<SharedPtr<ClientSession>>(sessions);
    auto server_session = std::get<SharedPtr<ServerSession>>(sessions);

    if (server_port->hle_handler)
        server_port->hle_handler->ClientConnected(server_session);

    server_port->pending_sessions.push_back(std::move(server_session));

    // Wake the threads waiting on the ServerPort
    server_port->WakeupAllWaitingThreads();

    return MakeResult<SharedPtr<ClientSession>>(std::move(client_session));
}

} // namespace
