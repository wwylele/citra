// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace Service {

class Interface;

namespace ACT {

void Initialize(Service::Interface* self);
void GetAccountDataBlock(Service::Interface* self);

/// Initializes all ACT services
void Init();

} // namespace ACT
} // namespace Service
