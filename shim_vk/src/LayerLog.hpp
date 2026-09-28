/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

namespace VkSim
{

void LogInit();
void LogShutdown();

void LogInfo(const char* Fmt, ...);
void LogWarn(const char* Fmt, ...);
void LogError(const char* Fmt, ...);
void LogVerbose(const char* Fmt, ...);

} // namespace VkSim
