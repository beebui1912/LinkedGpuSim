#
#  Copyright 2019-2026 Diligent Graphics LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  See http://www.apache.org/licenses/LICENSE-2.0
#
#  Builds SimulationLauncher.exe - a small Win32 dialog that picks the target
#  GPU + child executable and spawns SimulationApp.exe.

cmake_minimum_required(VERSION 3.15)

if(NOT WIN32)
    message(STATUS "SimulationLauncher: only Windows is supported; skipping.")
    return()
endif()

set(LAUNCHER_SOURCES
    src/WinMain.cpp
    src/LauncherDialog.cpp
    src/LauncherDialog.hpp
    src/resource.h
    src/LauncherDialog.rc
)

add_executable(SimulationLauncher WIN32 ${LAUNCHER_SOURCES})

target_include_directories(SimulationLauncher PRIVATE src)

target_compile_definitions(SimulationLauncher PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
)

target_link_libraries(SimulationLauncher PRIVATE
    dxgi
    comctl32
    shell32
    ole32
)

set_target_properties(SimulationLauncher PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    FOLDER                           "DiligentGpuSimulation"
)

if(MSVC)
    target_compile_options(SimulationLauncher PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /MP
    )
endif()

# Ensure SimulationLauncher.exe is built after SimulationApp.exe so users can
# just double-click it.
if(TARGET SimulationApp)
    add_dependencies(SimulationLauncher SimulationApp)
endif()
