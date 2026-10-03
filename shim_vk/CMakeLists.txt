#
#  Copyright 2019-2026 Diligent Graphics LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  See http://www.apache.org/licenses/LICENSE-2.0
#
#  Builds VkLayer_DiligentGpuSim.dll - a Vulkan implicit/explicit layer that
#  intercepts vkEnumeratePhysicalDeviceGroups and friends so a child running
#  in Vulkan mode sees the simulated linked GPU device group.
#
#  Requires the Vulkan SDK to be installed (find_package Vulkan REQUIRED).

cmake_minimum_required(VERSION 3.15)

if(NOT WIN32)
    message(STATUS "VkLayer_DiligentGpuSim: only Windows is supported; skipping.")
    return()
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "VkLayer_DiligentGpuSim: Vulkan SDK not found; skipping layer build. "
                   "Install the LunarG Vulkan SDK and re-run CMake to enable.")
    return()
endif()

set(VKLAYER_SOURCES
    src/LayerMain.cpp
    src/LayerDevice.cpp
    src/LayerMemoryModel.cpp
    src/LayerMemoryModel.hpp
    src/LayerConfig.cpp
    src/LayerConfig.hpp
    src/LayerDispatch.hpp
    src/LayerLog.cpp
    src/LayerLog.hpp
    src/WrappedPhysicalDevice.hpp
    src/WrappedPhysicalDevice.cpp
    src/VkLayer_DiligentGpuSim.def
)

add_library(VkLayer_DiligentGpuSim SHARED ${VKLAYER_SOURCES})

target_include_directories(VkLayer_DiligentGpuSim PRIVATE
    src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(VkLayer_DiligentGpuSim PRIVATE
    ${Vulkan_LIBRARIES}
    advapi32
)

target_compile_definitions(VkLayer_DiligentGpuSim PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    VK_USE_PLATFORM_WIN32_KHR
    VK_LAYER_EXPORTS
)

set_target_properties(VkLayer_DiligentGpuSim PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    FOLDER                           "DiligentGpuSimulation"
)

if(MSVC)
    target_compile_options(VkLayer_DiligentGpuSim PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP
        /wd4100  # unreferenced formal parameter (many pass-throughs)
    )
endif()

# ---------------------------------------------------------------------------
# Layer manifest (JSON).  The Vulkan loader looks up layer implementations
# via the manifest, whose "library_path" points at our DLL.  We configure
# the file at CMake time so paths are correct, and copy it next to the DLL
# so setting VK_LAYER_PATH to the bin/ directory finds it.
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest/VkLayer_DiligentGpuSim.json.in"
    "${CMAKE_BINARY_DIR}/bin/VkLayer_DiligentGpuSim.json"
    @ONLY)
