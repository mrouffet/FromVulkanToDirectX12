cmake_minimum_required(VERSION 3.24)

project(FromVulkanToDirectX12)


set(MY_BIN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/Build/bin/${CMAKE_CXX_COMPILER_ID}_$<CONFIG>")

# Log directory at configuration-time
message("[${CMAKE_PROJECT_NAME}]: Output directory: ${MY_BIN_OUTPUT_DIR}")

# Set output directory.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${MY_BIN_OUTPUT_DIR}/libs")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MY_BIN_OUTPUT_DIR}/libs")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MY_BIN_OUTPUT_DIR}")

set(MY_PACKAGE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/packages")


# ===== Target mainDX12 =====
add_executable(FVTDX12_mainDX12 "Sources/mainDX12.cpp")

# Install Agility SDK 1.615.1 to nake sure everyone has Mesh-shader ready D3D12 dll
# See Agility SDK documentation: https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
execute_process(
    COMMAND ./Cmd/nuget install Microsoft.Direct3D.D3D12 -Version 1.615.1 -OutputDirectory "${CMAKE_SOURCE_DIR}/ThirdParty/packages"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set(MY_AGILITY_SDK_INCLUDE_DIR "${MY_PACKAGE_OUTPUT_DIR}/Microsoft.Direct3D.D3D12.1.615.1/build/native/include")
set(MY_AGILITY_SDK_BIN_OUTPUT_DIR "${MY_PACKAGE_OUTPUT_DIR}/Microsoft.Direct3D.D3D12.1.615.1/build/native/bin/x64")

target_compile_features(FVTDX12_mainDX12 PRIVATE c_std_11 cxx_std_20)
target_compile_options(FVTDX12_mainDX12 PRIVATE /W4 /WX)

# Force Agility SDK 615
target_include_directories(FVTDX12_mainDX12 PRIVATE ${MY_AGILITY_SDK_INCLUDE_DIR})
target_compile_definitions(FVTDX12_mainDX12 PRIVATE FORCE_AGILITY_SDK_615=1)

target_link_libraries(FVTDX12_mainDX12 PUBLIC d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib)
target_link_libraries(FVTDX12_mainDX12 PUBLIC glfw assimp stb DirectXShaderCompiler SA_Logger SA_Maths)

add_custom_command(
    TARGET FVTDX12_mainDX12
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Resources
    $<TARGET_FILE_DIR:FVTDX12_mainDX12>/Resources
)

# Copy Agility SDK bin to output directory
add_custom_command(
    TARGET FVTDX12_mainDX12
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${MY_AGILITY_SDK_BIN_OUTPUT_DIR}
    $<TARGET_FILE_DIR:FVTDX12_mainDX12>/D3D12
)

### Fix dxcompiler.dll mUtils->CreateReflection.
add_custom_command(
	TARGET FVTDX12_mainDX12 POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy
	${CMAKE_SOURCE_DIR}/ThirdParty/DirectXShaderCompiler/win/bin/dxcompiler.dll
	$<TARGET_FILE_DIR:FVTDX12_mainDX12>
)

### Fix dxil.dll lookup.
add_custom_command(
	TARGET FVTDX12_mainDX12 POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy
	${CMAKE_SOURCE_DIR}/ThirdParty/DirectXShaderCompiler/win/bin/dxil.dll
	$<TARGET_FILE_DIR:FVTDX12_mainDX12>
)


# ===== Target mainVK =====
find_package(Vulkan COMPONENTS shaderc_combined) # https://cmake.org/cmake/help/latest/module/FindVulkan.html

if(${Vulkan_FOUND})
    add_executable(FVTDX12_mainVK "Sources/mainVK.cpp")

    target_compile_features(FVTDX12_mainVK PRIVATE c_std_11 cxx_std_20)
    target_compile_options(FVTDX12_mainVK PRIVATE /W4 /WX)


    target_link_libraries(FVTDX12_mainVK PUBLIC Vulkan::Vulkan Vulkan::shaderc_combined)
    target_link_libraries(FVTDX12_mainVK PUBLIC glfw assimp stb SA_Logger SA_Maths)
    target_link_options(FVTDX12_mainVK PUBLIC "/ignore:4099") # shaderc_combined doesn't provide .pdb files in debug: remove linker warning.

    add_custom_command(
        TARGET FVTDX12_mainVK
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Resources
        $<TARGET_FILE_DIR:FVTDX12_mainVK>/Resources
    )
else()
    message(WARNING "WARNING: Vulkan SDK not found: can't compile mainVK.cpp.")
endif()



# ===== ThirdParty =====
add_subdirectory(ThirdParty/glfw)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable assimp built as shared libraries")
set(ASSIMP_INSTALL OFF CACHE BOOL "Disable assimp install")
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "Disable assimp build tests")
add_subdirectory(ThirdParty/assimp)

add_subdirectory(ThirdParty/stb)

add_subdirectory(ThirdParty/DirectXShaderCompiler)


## SA libraries
add_subdirectory(ThirdParty/SA/Logger)
add_subdirectory(ThirdParty/SA/Maths)
