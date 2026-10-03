#
#  Copyright 2019-2026 Diligent Graphics LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  See http://www.apache.org/licenses/LICENSE-2.0
#
#  Builds D3D12Sim.dll - the injected shim that hooks D3D12CreateDevice
#  and virtualizes ID3D12Device's linked-node behaviour on a single-node GPU.

cmake_minimum_required(VERSION 3.15)

if(NOT WIN32)
    message(STATUS "D3D12Sim shim: only Windows is supported; skipping.")
    return()
endif()

set(D3D12SIM_SOURCES
    src/DllMain.cpp
    src/D3D12Hook.cpp
    src/D3D12Hook.hpp
    src/D3D12Vtable.hpp
    src/D3D12DeviceWrapper.cpp
    src/D3D12DeviceWrapper.hpp
    src/D3D12CommandWrappers.cpp
    src/D3D12CommandWrappers.hpp
    src/D3D12VtableCheck.cpp
    src/NodeMasks.cpp
    src/NodeMasks.hpp
    src/VtableShadow.cpp
    src/VtableShadow.hpp
    src/DXGIHook.cpp
    src/DXGIHook.hpp
    src/DXGIFactoryWrapper.cpp
    src/DXGIFactoryWrapper.hpp
    src/VirtualDXGIAdapter.cpp
    src/VirtualDXGIAdapter.hpp
    src/ShimConfig.cpp
    src/ShimConfig.hpp
    src/ShimLog.cpp
    src/ShimLog.hpp
)

add_library(D3D12Sim SHARED ${D3D12SIM_SOURCES})

target_include_directories(D3D12Sim PRIVATE src)

target_link_libraries(D3D12Sim PRIVATE
    minhook
    d3d12
    dxgi
    advapi32
)

target_compile_definitions(D3D12Sim PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    D3D12SIM_EXPORTS
)

set_target_properties(D3D12Sim PROPERTIES
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
    target_compile_options(D3D12Sim PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP
        /wd4100    # unreferenced formal parameter (many pass-through wrappers)
    )
endif()
