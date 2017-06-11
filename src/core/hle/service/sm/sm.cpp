// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <tuple>
#include "common/assert.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/server_port.h"
#include "core/hle/result.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/sm/srv.h"

namespace Service {
namespace SM {

static ResultCode ValidateServiceName(const std::string& name) {
    if (name.size() <= 0 || name.size() > 8) {
        return ERR_INVALID_NAME_SIZE;
    }
    if (name.find('\0') != std::string::npos) {
        return ERR_NAME_CONTAINS_NUL;
    }
    return RESULT_SUCCESS;
}

void ServiceManager::InstallInterfaces(std::shared_ptr<ServiceManager> self) {
    ASSERT(self->srv_interface.expired());

    auto srv = std::make_shared<SRV>(self);
    srv->InstallAsNamedPort();
    self->srv_interface = srv;
}

ResultVal<Kernel::SharedPtr<Kernel::ServerPort>> ServiceManager::RegisterService(
    std::string name, unsigned int max_sessions) {

    CASCADE_CODE(ValidateServiceName(name));
    Kernel::SharedPtr<Kernel::ServerPort> server_port;
    Kernel::SharedPtr<Kernel::ClientPort> client_port;
    std::tie(server_port, client_port) = Kernel::ServerPort::CreatePortPair(max_sessions, name);

    registered_services.emplace(std::move(name), std::move(client_port));
    return MakeResult<Kernel::SharedPtr<Kernel::ServerPort>>(std::move(server_port));
}

ResultVal<Kernel::SharedPtr<Kernel::ClientPort>> ServiceManager::GetServicePort(
    const std::string& name) {

    CASCADE_CODE(ValidateServiceName(name));
    auto it = registered_services.find(name);
    if (it == registered_services.end()) {
        return ERR_SERVICE_NOT_REGISTERED;
    }

    return MakeResult<Kernel::SharedPtr<Kernel::ClientPort>>(it->second);
}

ResultVal<Kernel::SharedPtr<Kernel::ClientSession>> ServiceManager::ConnectToService(
    const std::string& name) {

    CASCADE_RESULT(auto client_port, GetServicePort(name));
    return client_port->Connect();
}

std::shared_ptr<ServiceManager> g_service_manager;

} // namespace SM
} // namespace Service
