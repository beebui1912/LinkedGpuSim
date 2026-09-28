#
#  Copyright 2019-2026 Diligent Graphics LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Standalone build for the SimulationApp launcher.  This module does NOT
#  depend on the surrounding DiligentEngine CMake tree - it can be configured
#  and built on its own:
#
#      cmake -S DiligentGpuSimulation -B DiligentGpuSimulation/build
#      cmake --build DiligentGpuSimulation/build --config Release
#
#  It can also be included from a parent CMakeLists via add_subdirectory().

cmake_minimum_required(VERSION 3.15)

# Support both a standalone configure and being pulled in from a parent
# project (e.g. via add_subdirectory).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(DiligentGpuSimulation CXX)
endif()

if(NOT WIN32)
    message(STATUS "DiligentGpuSimulation: only Windows is supported at the moment; skipping.")
    return()
endif()

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

set(SIMAPP_SOURCES
    src/main.cpp
    src/SimulationApp.cpp
    src/SimulationApp.hpp
    src/ProcessLauncher.cpp
    src/ProcessLauncher.hpp
    src/LogCapture.cpp
    src/LogCapture.hpp
    src/LinkedGpuSimulator.cpp
    src/LinkedGpuSimulator.hpp
    src/GpuInfoConsole.cpp
    src/GpuInfoConsole.hpp
    src/ShimInjector.cpp
    src/ShimInjector.hpp
)

add_executable(SimulationApp ${SIMAPP_SOURCES})

target_include_directories(SimulationApp PRIVATE src)

target_compile_definitions(SimulationApp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00   # Windows 10+ (for DXGI 1.4 + modern PDH)
)

# System libraries we actually use.  Explicit even though the .cpp files also
# carry `#pragma comment(lib, ...)` — that helps MinGW / clang-cl / ninja.
target_link_libraries(SimulationApp PRIVATE
    dxgi
    pdh
    shell32
    advapi32
)

if(MSVC)
    target_compile_options(SimulationApp PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP
    )
    # Force a console subsystem so stdout/stderr are always attached.
    set_property(TARGET SimulationApp PROPERTY
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    target_compile_options(SimulationApp PRIVATE
        -Wall -Wextra -Wpedantic
        -municode
    )
    target_link_options(SimulationApp PRIVATE -municode)
endif()

set_target_properties(SimulationApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    FOLDER                           "DiligentGpuSimulation"
)

# Convenience: on multi-config generators, tag the debug binary.
set_target_properties(SimulationApp PROPERTIES DEBUG_POSTFIX "d")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(STATUS "SimulationApp will be built into: ${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Driver-level shim (D3D12Sim.dll)
#
# The shim is an injected DLL that hooks d3d12!D3D12CreateDevice in a child
# process and patches the returned ID3D12Device vtable so it reports N linked
# nodes and silently rewrites NodeMask arguments in Create* calls back down
# to the single physical node.
#
# Requires MinHook (BSD-2-Clause) for the inline hook.  Fetched at configure
# time; can be disabled with -DDILIGENT_GPUSIM_BUILD_SHIM=OFF.
# ---------------------------------------------------------------------------
option(DILIGENT_GPUSIM_BUILD_SHIM "Build the D3D12 driver-level simulation shim" ON)

if(DILIGENT_GPUSIM_BUILD_SHIM)
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)

    # MinHook's CMakeLists uses cmake_minimum_required < 3.5 which CMake 4+
    # refuses by default.  Loosen the policy just for that dependency.
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)

    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        423d1e45af2ed2719a5c31e990e935ef301ed9c3  # master @ 2023-06-15, BSD-2-Clause
    )
    FetchContent_MakeAvailable(minhook)

    if(NOT TARGET minhook)
        message(WARNING "MinHook was not registered as a target after FetchContent - the driver shim will be skipped.")
        set(DILIGENT_GPUSIM_BUILD_SHIM OFF)
    else()
        set_target_properties(minhook PROPERTIES FOLDER "DiligentGpuSimulation/thirdparty")
    endif()
endif()

if(DILIGENT_GPUSIM_BUILD_SHIM)
    add_subdirectory(shim)
endif()

# ---------------------------------------------------------------------------
# Vulkan layer skeleton (VkLayer_DiligentGpuSim.dll) - Phase 4 counterpart to
# the D3D12 shim.  Load-time plumbing is complete; the linked-device-group
# synthesis logic is stubbed with TODO markers (see shim_vk/src/LayerMain.cpp).
# Requires the Vulkan SDK to be installed on the build machine.
# ---------------------------------------------------------------------------
option(DILIGENT_GPUSIM_BUILD_VK_LAYER "Build the Vulkan layer skeleton" ON)
if(DILIGENT_GPUSIM_BUILD_VK_LAYER)
    add_subdirectory(shim_vk)
endif()

# ---------------------------------------------------------------------------
# GUI launcher (SimulationLauncher.exe) - small Win32 dialog that lists host
# GPUs, offers a file picker for the child .exe, and spawns SimulationApp
# with the chosen args in a new console.
# ---------------------------------------------------------------------------
option(DILIGENT_GPUSIM_BUILD_LAUNCHER "Build the GUI launcher (SimulationLauncher.exe)" ON)
if(DILIGENT_GPUSIM_BUILD_LAUNCHER)
    add_subdirectory(launcher)
endif()
