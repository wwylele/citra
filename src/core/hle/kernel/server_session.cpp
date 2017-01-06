// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>

#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/kernel/thread.h"

namespace Kernel {

ServerSession::ServerSession() = default;
ServerSession::~ServerSession() {
    // This destructor will be called automatically when the last ServerSession handle is closed by
    // the emulated application.
    // TODO(Subv): Reduce the ClientPort's connection count,
    // if the session is still open, set the connection status to 3 (Closed by server),
}

ResultVal<SharedPtr<ServerSession>> ServerSession::Create(
    std::string name, std::shared_ptr<Service::SessionRequestHandler> hle_handler) {
    SharedPtr<ServerSession> server_session(new ServerSession);

    server_session->name = std::move(name);
    server_session->signaled = false;
    server_session->hle_handler = std::move(hle_handler);

    return MakeResult<SharedPtr<ServerSession>>(std::move(server_session));
}

bool ServerSession::ShouldWait() {
    return !signaled;
}

void ServerSession::Acquire() {
    ASSERT_MSG(!ShouldWait(), "object unavailable!");
    signaled = false;
}

ResultCode ServerSession::HandleSyncRequest() {
    // The ServerSession received a sync request, this means that there's new data available
    // from its ClientSession, so wake up any threads that may be waiting on a svcReplyAndReceive or
    // similar.

    // If this ServerSession has an associated HLE handler, forward the request to it.
    if (hle_handler != nullptr) {
        // Attempt to translate the incoming request's command buffer.
        ResultCode result = TranslateHLERequest(this);
        if (result.IsError())
            return result;
        hle_handler->HandleSyncRequest(SharedPtr<ServerSession>(this));
        // TODO(Subv): Translate the response command buffer.
    }

    // If this ServerSession does not have an HLE implementation, just wake up the threads waiting
    // on it.
    signaled = true;
    WakeupAllWaitingThreads();
    return RESULT_SUCCESS;
}

ServerSession::SessionPair ServerSession::CreateSessionPair(
    const std::string& name, std::shared_ptr<Service::SessionRequestHandler> hle_handler) {
    auto server_session =
        ServerSession::Create(name + "_Server", std::move(hle_handler)).MoveFrom();
    // We keep a non-owning pointer to the ServerSession in the ClientSession because we don't want
    // to prevent the ServerSession's destructor from being called when the emulated
    // application closes the last ServerSession handle.
    auto client_session = ClientSession::Create(server_session.get(), name + "_Client").MoveFrom();

    return std::make_tuple(std::move(server_session), std::move(client_session));
}

ResultCode TranslateHLERequest(ServerSession* server_session) {
    // TODO(Subv): Implement this function once multiple concurrent processes are supported.
    return RESULT_SUCCESS;
}
}
