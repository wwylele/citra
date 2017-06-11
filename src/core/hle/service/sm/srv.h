// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/kernel/kernel.h"
#include "core/hle/service/service.h"

namespace Kernel {
class HLERequestContext;
class Semaphore;
}

namespace Service {
namespace SM {

/// Interface to "srv:" service
class SRV final : public ServiceFramework<SRV> {
public:
    explicit SRV(std::shared_ptr<ServiceManager> service_manager);
    ~SRV();

private:
    void RegisterClient(Kernel::HLERequestContext& ctx);
    void EnableNotification(Kernel::HLERequestContext& ctx);
    void GetServiceHandle(Kernel::HLERequestContext& ctx);
    void Subscribe(Kernel::HLERequestContext& ctx);
    void Unsubscribe(Kernel::HLERequestContext& ctx);
    void PublishToSubscriber(Kernel::HLERequestContext& ctx);

    std::shared_ptr<ServiceManager> service_manager;
    Kernel::SharedPtr<Kernel::Semaphore> notification_semaphore;
};

} // namespace SM
} // namespace Service
