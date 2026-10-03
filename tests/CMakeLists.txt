#
#  Copyright 2019-2026 Diligent Graphics LLC
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  See http://www.apache.org/licenses/LICENSE-2.0
#
#  Self-tests of the shims on the real driver (need a GPU): ctest -C Release

if(TARGET D3D12Sim)
    add_executable(D3D12SimTest D3D12SimTest.cpp)
    target_link_libraries(D3D12SimTest PRIVATE d3d12 dxgi d3dcompiler dxguid)
    target_compile_definitions(D3D12SimTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    set_target_properties(D3D12SimTest PROPERTIES
        CXX_STANDARD 17
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        FOLDER                           "DiligentGpuSimulation/tests")
    add_dependencies(D3D12SimTest D3D12Sim)
    add_test(NAME D3D12SimTest COMMAND D3D12SimTest)
    add_test(NAME D3D12SimTest_Tier2 COMMAND D3D12SimTest --tier 2)
endif()

if(TARGET VkLayer_DiligentGpuSim AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VkSimTest.cpp")
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        add_executable(VkSimTest VkSimTest.cpp)
        target_include_directories(VkSimTest PRIVATE ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(VkSimTest PRIVATE ${Vulkan_LIBRARIES} dxgi)
        target_compile_definitions(VkSimTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        set_target_properties(VkSimTest PROPERTIES
            CXX_STANDARD 17
            RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
            FOLDER                           "DiligentGpuSimulation/tests")
        add_dependencies(VkSimTest VkLayer_DiligentGpuSim)
        add_test(NAME VkSimTest COMMAND VkSimTest)
    endif()
endif()
