# Custom CMake made by Maxime.
# Use built sources (compiled .dll + include files) instead of adding as git submodule and including the entire CMake build tree
# Including DirectXShaderCompiler source CMakeLists.txt will create a lot of CMake targets to build (even with options set to OFF).
# Making it simple here for learning purpose.

add_library(DirectXShaderCompiler INTERFACE)

if(WIN32)

	target_include_directories(DirectXShaderCompiler INTERFACE win/include)

	target_link_directories(DirectXShaderCompiler INTERFACE win/bin)
	target_link_libraries(DirectXShaderCompiler INTERFACE dxcompiler.dll)

else()

	# DXC cqn be used on unix to compile HLSL files into SPIRV for Vulkan

	target_include_directories(DirectXShaderCompiler INTERFACE unix/include)

	target_link_directories(DirectXShaderCompiler INTERFACE unix/libs)
	target_link_libraries(DirectXShaderCompiler INTERFACE libdxcompiler.so)

endif()
