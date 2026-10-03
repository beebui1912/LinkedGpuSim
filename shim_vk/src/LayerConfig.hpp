/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  LayerConfig
//  -----------
//  Settings of the layer (DILIGENT_SIM_* environment variables, read once) and
//  reporting of validation errors: uses of the simulated device group that a
//  real multi-device group would not accept.

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace VkSim
{

struct LayerConfig
{
    uint32_t NodeCount   = 2;     // DILIGENT_SIM_LINKED_NODE_COUNT (1..8)
    bool     Validate    = true;  // DILIGENT_SIM_VALIDATION=0 turns the checks off
    bool     HasHostLuid = false; // DILIGENT_SIM_HOST_ADAPTER_LUID ("0xHIGH_0xLOW")
    uint8_t  HostLuid[VK_LUID_SIZE] = {};
};

const LayerConfig& GetConfig();

inline uint32_t AllNodesMask(uint32_t NodeCount) { return (1u << NodeCount) - 1u; }

// A device mask the group accepts: not zero, only bits of existing devices
inline bool IsValidDeviceMask(uint32_t Mask, uint32_t NodeCount) { return Mask != 0 && (Mask & ~AllNodesMask(NodeCount)) == 0; }

// Logs "VALIDATION ERROR: ..." (also to stderr) and counts it; after 50
// messages only the count grows
void     ReportValidationError(const char* Fmt, ...);
unsigned GetValidationErrorCount();

} // namespace VkSim
