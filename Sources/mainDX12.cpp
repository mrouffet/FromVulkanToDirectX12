#include <array>

/**
* Sapphire Suite Debugger:
* Maxime's custom Log and Assert macros for easy debug.
*/
#include <SA/Collections/Debug>

/**
* Sapphire Suite Maths library:
* Maxime's custom Maths library.
*/
#include <SA/Collections/Maths>
#include <SA/Collections/Transform>



// ========== Windowing ==========

#include <GLFW/glfw3.h>

/**
* Required to directly access HWND handle from GLFW window type.
* See glfwGetWin32Window()
*/
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

GLFWwindow* window = nullptr;
constexpr SA::Vec2ui windowSize = { 1200, 900 };

void GLFWErrorCallback(int32_t error, const char* description)
{
	SA_LOG((L"GLFW Error [%1]: %2", error, description), Error, GLFW.API);
}



// ========== Renderer ==========

/**
* DirectX12 uses Microsoft's ComPtr (SmartPtr) to automatically decrement the object counter and call the interface destroy function on last reference destroy.
* There is no device->Destroy<object>() functions, we MUST use ComPtr.
*/
#include <wrl.h>
template <typename T>
using MComPtr = Microsoft::WRL::ComPtr<T>;

/**
* Base DirectX12 header.
* Requires `d3d12.lib`
*/
#if FORCE_AGILITY_SDK_615
/**
* Force the Agility SDK 615 for Mesh Shader ready d3d12 lib.
*/
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif
#include <d3d12.h> // vulkan.h -> d3d12.h

/**
* DX12 additionnal Debug tools:
* Report live objects after uninitialization to track leak and understroyed objects.
* Requires `dxguid.lib`
*/
#include <DXGIDebug.h>

// === Validation Layers ===

#if SA_DEBUG
DWORD VLayerCallbackCookie = 0;

void ValidationLayersDebugCallback(D3D12_MESSAGE_CATEGORY _category,
	D3D12_MESSAGE_SEVERITY _severity,
	D3D12_MESSAGE_ID _ID,
	LPCSTR _description,
	void* _context)
{
	(void)_context;

	std::wstring categoryStr;

	switch (_category)
	{
	case D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED:
		categoryStr = L"Application Defined";
		break;
	case D3D12_MESSAGE_CATEGORY_MISCELLANEOUS:
		categoryStr = L"Miscellaneous";
		break;
	case D3D12_MESSAGE_CATEGORY_INITIALIZATION:
		categoryStr = L"Initialization";
		break;
	case D3D12_MESSAGE_CATEGORY_CLEANUP:
		categoryStr = L"Cleanup";
		break;
	case D3D12_MESSAGE_CATEGORY_COMPILATION:
		categoryStr = L"Compilation";
		break;
	case D3D12_MESSAGE_CATEGORY_STATE_CREATION:
		categoryStr = L"State Creation";
		break;
	case D3D12_MESSAGE_CATEGORY_STATE_SETTING:
		categoryStr = L"State Setting";
		break;
	case D3D12_MESSAGE_CATEGORY_STATE_GETTING:
		categoryStr = L"State Getting";
		break;
	case D3D12_MESSAGE_CATEGORY_RESOURCE_MANIPULATION:
		categoryStr = L"Resource Manipulation";
		break;
	case D3D12_MESSAGE_CATEGORY_EXECUTION:
		categoryStr = L"Execution";
		break;
	case D3D12_MESSAGE_CATEGORY_SHADER:
		categoryStr = L"Shader";
		break;
	default:
		categoryStr = L"Unknown";
		break;
	}

	std::wstring dets = SA::StringFormat(L"ID [%1]\tCategory [%2]", static_cast<int>(_ID), categoryStr);

	switch (_severity)
	{
	case D3D12_MESSAGE_SEVERITY_CORRUPTION:
		SA_LOG(_description, AssertFailure, DX12.ValidationLayers, std::move(dets));
		break;
	case D3D12_MESSAGE_SEVERITY_ERROR:
		SA_LOG(_description, Error, DX12.ValidationLayers, std::move(dets));
		break;
	case D3D12_MESSAGE_SEVERITY_WARNING:
		SA_LOG(_description, Warning, DX12.ValidationLayers, std::move(dets));
		break;
	case D3D12_MESSAGE_SEVERITY_INFO:
		// Filter Info: too much logging on Resource create/destroy and Swapchain Present.
		// SA logging already tracking.
		return;
		//SA_LOG(_description, Info, DX12.ValidationLayers, std::move(dets));
		//break;
	case D3D12_MESSAGE_SEVERITY_MESSAGE:
	default:
		SA_LOG(_description, Normal, DX12.ValidationLayers, std::move(dets));
		break;
	}
}

#endif


// === Factory === /* 0001 */

/**
* Header specific to create a Factory (required to create a Device).
* Requires `dxgi.lib`
*/
#include <dxgi1_6.h>
MComPtr<IDXGIFactory6> factory; // VkInstance -> IDXGIFactory


// === Device === /* 0002 */

// Don't need to keep Adapter (physical Device) reference: only use Logical.
MComPtr<ID3D12Device> device; // VkDevice -> ID3D12Device

MComPtr<ID3D12CommandQueue> graphicsQueue; // VkQueue -> ID3D12CommandQueue
// No PresentQueue needed (already handleled by Swapchain).

/**
* Vulkan has native WaitForGPU method (vkDeviceWaitIdle).
*
* DirectX12 must implement it manually using fences.
* For more details on Fence/Event implementation, check Swapchain synchronization implementation below.
*/
HANDLE deviceFenceEvent;
MComPtr<ID3D12Fence> deviceFence;
uint32_t deviceFenceValue = 0u;

void WaitDeviceIdle()
{
	// Schedule a Signal command in the queue.
	graphicsQueue->Signal(deviceFence.Get(), deviceFenceValue);

	// Wait until the fence has been processed.
	deviceFence->SetEventOnCompletion(deviceFenceValue, deviceFenceEvent);
	WaitForSingleObjectEx(deviceFenceEvent, INFINITE, false);

	// Increment for next use.
	++deviceFenceValue;
}


// === Swapchain === /* 0003 */

constexpr uint32_t bufferingCount = 3;

MComPtr<IDXGISwapChain3> swapchain; // VkSwapchainKHR -> IDXGISwapChain
std::array<MComPtr<ID3D12Resource>, bufferingCount> swapchainImages{ nullptr }; // VkImage -> ID3D12Resource
uint32_t swapchainFrameIndex = 0u;

/**
* 0003.1
* Vulkan uses Semaphores and Fences for swapchain synchronization
* 1 Fence and 1 Semaphore are used PER FRAME.
*
*
* DirectX12 uses fence and Windows event.
* Only 1 Frame and 1 Windows Event are used FOR ALL FRAME, with different Values (one PER FRAME).
* See DirectX-Graphics-Samples/D3D12HelloFrameBuffering Sample for reference.
*/
HANDLE swapchainFenceEvent = nullptr;
MComPtr<ID3D12Fence> swapchainFence; // VkFence -> ID3D12Fence
std::array<uint32_t, bufferingCount> swapchainFenceValues{ 0u };


// === Commands === /* 0004 */
/**
* Vulkan needs only 1 CommandPool for all frames + 1 CommandBuffer per frame. Recording commands are allocated and stored in the CommandBuffer.
* DirectX12 doesn't have CommandPool, and split the CommandBuffer into "Allocator" (1 per frame) and CommandList (current list of allocated command for the frame, that can be directly reused: 1 for all frame).
* For more details, check DirectX-Graphics-Sample: HelloFrameBuffering.cpp.
*/
std::array<MComPtr<ID3D12CommandAllocator>, bufferingCount> cmdAllocs;

/**
* /!\ with DirectX12, for graphics operations, the 'CommandList' type is not enough. ID3D12GraphicsCommandList must be used.
* Like for Vulkan, we allocate 1 command buffer per frame.
*/
MComPtr<ID3D12GraphicsCommandList1> cmdList;


// === Scene Textures === /* 0005 */

// = Color =
constexpr DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // VkFormat -> DXGI_FORMAT
constexpr float sceneClearColor[] = { 0.0f, 0.1f, 0.2f, 1.0f };
// Use Swapchain backbuffer texture as color output.
MComPtr<ID3D12DescriptorHeap> sceneRTViewHeap; /* 0006-1 */

// = Depth =
constexpr DXGI_FORMAT sceneDepthFormat = DXGI_FORMAT_D16_UNORM; // VkFormat -> DXGI_FORMAT
constexpr D3D12_CLEAR_VALUE sceneDepthClearValue{ .Format = sceneDepthFormat, .DepthStencil = { 1.0f, 0 } };
MComPtr<ID3D12Resource> sceneDepthTexture; // VkImage -> ID3D12Resource
MComPtr<ID3D12DescriptorHeap> sceneDepthRTViewHeap; /* 0006-2 */


// === Pipeline === /* 0008 */

// = Viewport & Scissor =
/**
* Vulkan must specify viewport and scissor on pipeline creation by default (or use dynamic).
* DirectX12 only uses dynamic viewport and scissor (set by commandlist).
*/
D3D12_VIEWPORT viewport{}; // VkViewport -> D3D12_VIEWPORT
D3D12_RECT scissorRect{}; // VkRect2D -> D3D12_RECT

// === Shader Compiler ===
/**
* Basic native helper shader compiler header. Requires `d3dcompiler.lib`
* For more advanced features use DirectXShaderCompiler library (see below).
* /!\ WARNING /!\ Currently not used. Learning purpose only.
*/
#include <d3dcompiler.h>
MComPtr<ID3DBlob> Native_CompileShader(std::wstring _path, std::string _entry, std::string _target/*, std::vector<std::wstring> _defines = {}*/)
{
#if SA_DEBUG
	// Enable better shader debugging with the graphics debugging tools.
	constexpr UINT shaderCompileFlags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	constexpr UINT shaderCompileFlags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	MComPtr<ID3DBlob> errors;
	MComPtr<ID3DBlob> shader;

	const HRESULT hrCompileShader = D3DCompileFromFile(_path.c_str(), nullptr, nullptr, _entry.c_str(), _target.c_str(), shaderCompileFlags, 0, &shader, &errors);

	if (FAILED(hrCompileShader))
	{
		std::string errorStr(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
		SA_LOG(L"Shader {LitShader.hlsl, mainVS} compilation failed!", Error, DX12, errorStr);
		return nullptr;
	}
	else
	{
		SA_LOG(L"Shader {LitShader.hlsl, mainVS} compilation success.", Info, DX12, shader.Get());
	}

	return shader;
}

/**
* DXC (DirectXShaderCompiler) library is the advanced DirectX 12 API for shader compiling.
* This library is not native to DirectX12 library but required for compiling mesh shaders.
* Base and native DirectX12 shader compilier is d3dcompiler.lib (d3dcompiler.h)
*/
#include <dxc/dxcapi.h>
MComPtr<IDxcUtils> shaderCompilerUtils;
MComPtr<IDxcCompiler3> shaderCompiler;
MComPtr<ID3DBlob> CompileShader(std::wstring _path, std::wstring _entry, std::wstring _target, std::vector<std::wstring> _defines = {})
{
	MComPtr<IDxcBlobEncoding> blob;
	DxcBuffer dx;
	//ShaderIncluder includer; // Define your own shader includer class.

	const HRESULT hrLoadFile = shaderCompilerUtils->LoadFile(_path.c_str(), nullptr, &blob);
	if (FAILED(hrLoadFile))
	{
		SA_LOG((L"Load Shader {%1} failed!", _path), Error, DXC, (L"Error Code: %1", hrLoadFile));
		return nullptr;
	}

	dx.Ptr = blob->GetBufferPointer();
	dx.Size = blob->GetBufferSize();
	dx.Encoding = 0;

	std::vector<LPCWSTR> cArgs
	{
		L"-E",
		_entry.c_str(),
		L"-T",
		_target.c_str(),
		L"-HV",
		L"2021",
		DXC_ARG_WARNINGS_ARE_ERRORS,
		DXC_ARG_PACK_MATRIX_ROW_MAJOR,
		DXC_ARG_ALL_RESOURCES_BOUND,
		/* Add include path for shader includer (Warning: must work both from VS and standalone .exe) */
		//L"-I",
		//L"/Resources/Shaders/HLSL"
	};

#if SA_DEBUG

	cArgs.push_back(DXC_ARG_DEBUG);
	cArgs.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);

#else

	cArgs.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);

#endif

	for (auto& define : _defines)
	{
		cArgs.push_back(L"-D");
		cArgs.push_back(define.c_str());
	}


	MComPtr<IDxcResult> result;
	shaderCompiler->Compile(&dx, cArgs.data(), static_cast<uint32_t>(cArgs.size()), nullptr /*&includer*/, IID_PPV_ARGS(&result));

	HRESULT hr;
	result->GetStatus(&hr);

	if (FAILED(hr))
	{
		MComPtr<IDxcBlobUtf8> errors;
		result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

		if (errors && errors->GetStringLength() > 0)
		{
			SA_LOG((L"Shader {%1:%2} Compilation failed!", _path, _entry), Error, DXC, errors->GetStringPointer());
			return nullptr;
		}

		return nullptr;
	}

	MComPtr<ID3DBlob> code;
	result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);

	return code;
}


// = Lit =
MComPtr<ID3DBlob> litVertexShader; // VkShaderModule -> ID3DBlob
MComPtr<ID3DBlob> litPixelShader;

MComPtr<ID3D12RootSignature> litRootSign; // VkPipelineLayout -> ID3D12RootSignature /* 0008-1 */
MComPtr<ID3D12PipelineState> litPipelineState; // VkPipeline -> ID3D12PipelineState


// === Scene Objects === /* 0009 */

MComPtr<ID3D12DescriptorHeap> pbrSphereSRVHeap;

// = Camera Buffer =
struct CameraUBO
{
	SA::Mat4f view;
	SA::Mat4f invViewProj;
};
SA::TransformPRf cameraTr;
constexpr float cameraMoveSpeed = 4.0f;
constexpr float cameraRotSpeed = 16.0f;
constexpr float cameraNear = 0.1f;
constexpr float cameraFar = 1000.0f;
constexpr float cameraFOV = 90.0f;
std::array<MComPtr<ID3D12Resource>, bufferingCount> cameraBuffers;

// = Object Buffer =
struct ObjectUBO
{
	SA::Mat4f transform;
};
constexpr SA::Vec3f spherePosition(0.5f, 0.0f, 2.0f);
MComPtr<ID3D12Resource> sphereObjectBuffer;

// = PointLights Buffer =
struct PointLightUBO
{
	SA::Vec3f position;

	float intensity = 0.0f;

	SA::Vec3f color;

	float radius = 0.0f;
};
constexpr uint32_t pointLightNum = 2;
MComPtr<ID3D12Resource> pointLightBuffer;


// === Resources === /* 0010 */

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#pragma warning(disable : 4505)
#include <stb_image_resize2.h>
#pragma warning(default : 4505)

bool SubmitBufferToGPU(MComPtr<ID3D12Resource> _gpuBuffer, uint64_t _size, const void* _data, D3D12_RESOURCE_STATES _stateAfter)
{
	// Create temp upload buffer.
	MComPtr<ID3D12Resource> stagingBuffer;

	const D3D12_HEAP_PROPERTIES heap{
		.Type = D3D12_HEAP_TYPE_UPLOAD,
	};

	const D3D12_RESOURCE_DESC desc{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = _size,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = {.Count = 1, .Quality = 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = D3D12_RESOURCE_FLAG_NONE,
	};

	const HRESULT hrStagBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&stagingBuffer));
	if (FAILED(hrStagBufferCreated))
	{
		SA_LOG(L"Create Staging Buffer failed!", Error, DX12, (L"Error code: %1", hrStagBufferCreated));
		return false;
	}


	// Memory mapping and Upload (CPU to GPU transfer).
	const D3D12_RANGE range{ .Begin = 0, .End = 0 };
	void* data = nullptr;

	// vkMapMemory -> buffer->Map
	stagingBuffer->Map(0, &range, reinterpret_cast<void**>(&data));

	std::memcpy(data, _data, _size);

	// vkUnmapMemory -> buffer->Unmap
	stagingBuffer->Unmap(0, nullptr);


	// Copy GPU temp staging buffer to final GPU-only buffer.
	cmdList->CopyBufferRegion(_gpuBuffer.Get(), 0, stagingBuffer.Get(), 0, _size);


	// Resource transition to final state.
	const D3D12_RESOURCE_BARRIER barrier{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.Transition = {
			.pResource = _gpuBuffer.Get(),
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
			.StateAfter = _stateAfter,
		},
	};

	cmdList->ResourceBarrier(1, &barrier);

	cmdList->Close();

	/**
	* Instant command submit execution (easy implementation)
	* Better code would parallelize resources loading in staging buffer and submit only once at the end to execute all GPU copies.
	*/

	ID3D12CommandList* cmdListsArr[] = { cmdList.Get() };
	graphicsQueue->ExecuteCommandLists(1, cmdListsArr);

	WaitDeviceIdle();

	cmdAllocs[0]->Reset();
	cmdList->Reset(cmdAllocs[0].Get(), nullptr);

	return true;
}

bool SubmitTextureToGPU(MComPtr<ID3D12Resource> _gpuTexture, const std::vector<SA::Vec2ui>& _extents, uint64_t _totalSize, uint32_t _channelNum, const void* _data)
{
	// Create temp upload buffer.
	MComPtr<ID3D12Resource> stagingBuffer;

	const D3D12_HEAP_PROPERTIES heap{
		.Type = D3D12_HEAP_TYPE_UPLOAD,
	};

	const D3D12_RESOURCE_DESC desc{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Alignment = 0,
		.Width = _totalSize,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = {.Count = 1, .Quality = 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		.Flags = D3D12_RESOURCE_FLAG_NONE,
	};

	const HRESULT hrStagBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&stagingBuffer));
	if (FAILED(hrStagBufferCreated))
	{
		SA_LOG(L"Create Staging Buffer failed!", Error, DX12, (L"Error code: %1", hrStagBufferCreated));
		return false;
	}


	// Memory mapping and Upload (CPU to GPU transfer).
	const D3D12_RANGE range{ .Begin = 0, .End = 0 };
	void* data = nullptr;

	stagingBuffer->Map(0, &range, reinterpret_cast<void**>(&data));
	std::memcpy(data, _data, _totalSize);
	stagingBuffer->Unmap(0, nullptr);


	// Copy Buffer to texture
	const D3D12_RESOURCE_DESC resDesc = _gpuTexture->GetDesc();
	UINT64 offset = 0u;

	for (UINT16 i = 0; i < resDesc.MipLevels; ++i)
	{
		const D3D12_TEXTURE_COPY_LOCATION src{
			.pResource = stagingBuffer.Get(),
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
			.PlacedFootprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{
				.Offset = offset,
				.Footprint = D3D12_SUBRESOURCE_FOOTPRINT{
					.Format = resDesc.Format,
					.Width = _extents[i].x,
					.Height = _extents[i].y,
					.Depth = 1,
					.RowPitch = _extents[i].x * _channelNum,
				}
			}
		};

		const D3D12_TEXTURE_COPY_LOCATION dst{
			.pResource = _gpuTexture.Get(),
			.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
			.SubresourceIndex = i // currMipLevel
		};

		cmdList->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);

		offset += _extents[i].x * _extents[i].y * _channelNum;
	}


	// Resource transition to final state.
	const D3D12_RESOURCE_BARRIER barrier{
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
		.Transition = {
			.pResource = _gpuTexture.Get(),
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
			.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		},
	};

	cmdList->ResourceBarrier(1, &barrier);

	cmdList->Close();

	/**
	* Instant command submit execution (easy implementation)
	* Better code would parallelize resources loading in staging buffer and submit only once at the end to execute all GPU copies.
	*/

	ID3D12CommandList* cmdListsArr[] = { cmdList.Get() };
	graphicsQueue->ExecuteCommandLists(1, cmdListsArr);

	WaitDeviceIdle();

	cmdAllocs[0]->Reset();
	cmdList->Reset(cmdAllocs[0].Get(), nullptr);

	return true;
}

void GenerateMipMapsCPU(SA::Vec2ui _extent, std::vector<char>& _data, uint32_t& _outMipLevels, uint32_t& _outTotalSize, std::vector<SA::Vec2ui>& _outExtents, uint32_t _channelNum, uint32_t _layerNum = 1u)
{
	_outMipLevels = static_cast<uint32_t>(std::floor(std::log2(max(_extent.x, _extent.y)))) + 1;

	_outExtents.resize(_outMipLevels);

	// Compute Total Size & extents
	{
		for (uint32_t i = 0u; i < _outMipLevels; ++i)
		{
			_outExtents[i] = _extent;

			_outTotalSize += _extent.x * _extent.y * _channelNum * _layerNum * sizeof(stbi_uc);

			if (_extent.x > 1)
				_extent.x >>= 1;

			if (_extent.y > 1)
				_extent.y >>= 1;
		}
	}


	_data.resize(_outTotalSize);


	// Generate
	{
		unsigned char* src = reinterpret_cast<unsigned char*>(_data.data());

		for (uint32_t i = 1u; i < _outMipLevels; ++i)
		{
			uint64_t srcLayerOffset = _outExtents[i - 1].x * _outExtents[i - 1].y * _channelNum * sizeof(stbi_uc);
			uint64_t currLayerOffset = _outExtents[i].x * _outExtents[i].y * _channelNum * sizeof(stbi_uc);
			unsigned char* dst = src + srcLayerOffset * _layerNum;

			for (uint32_t j = 0; j < _layerNum; ++j)
			{
				bool res = stbir_resize_uint8_linear(
					src,
					static_cast<int32_t>(_outExtents[i - 1].x),
					static_cast<int32_t>(_outExtents[i - 1].y),
					0,
					dst,
					static_cast<int32_t>(_outExtents[i].x),
					static_cast<int32_t>(_outExtents[i].y),
					0,
					static_cast<stbir_pixel_layout>(_channelNum)
				);

				if (!res)
				{
					SA_LOG(L"Mip map creation failed!", Error, STB);
					return;
				}

				dst += currLayerOffset;
				src += srcLayerOffset;
			}
		}
	}
}

// = Sphere =
std::array<MComPtr<ID3D12Resource>, 4> sphereVertexBuffers; // VkBuffer -> ID3D12Resource
/**
* Vulkan binds the buffer directly
* DirectX12 create 'views' (aka. how to read the memory) of buffers and use them for binding.
*/
std::array<D3D12_VERTEX_BUFFER_VIEW, 4> sphereVertexBufferViews;
uint32_t sphereIndexCount = 0u;
MComPtr<ID3D12Resource> sphereIndexBuffer;
D3D12_INDEX_BUFFER_VIEW sphereIndexBufferView;

// = RustedIron2 PBR =
MComPtr<ID3D12Resource> rustedIron2AlbedoTexture; // VkImage + VkDeviceMemory -> ID3D12Resource
MComPtr<ID3D12Resource> rustedIron2NormalTexture;
MComPtr<ID3D12Resource> rustedIron2MetallicTexture;
MComPtr<ID3D12Resource> rustedIron2RoughnessTexture;



int main()
{
	// Initialization
	if (true)
	{
		SA::Debug::InitDefaultLogger();

		// GLFW
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			glfwInit();

			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			window = glfwCreateWindow(windowSize.x, windowSize.y, "FVTDX12_DX12-Window", nullptr, nullptr);

			if (!window)
			{
				SA_LOG(L"GLFW create window failed!", Error, GLFW);
				return EXIT_FAILURE;
			}
			else
			{
				SA_LOG("GLFW create window success.", Info, GLFW, window);
			}

			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		}


		// Renderer
		{
			// Factory /* 0001-I */
			if (true)
			{
				UINT dxgiFactoryFlags = 0;

#if SA_DEBUG
				// Validation Layers (instance-level) /* 0001-I2 */
				{
					// Debug controller
					{
						MComPtr<ID3D12Debug1> debugController = nullptr;

						const HRESULT hrDebugInterface = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
						if (SUCCEEDED(hrDebugInterface))
						{
							debugController->EnableDebugLayer();
							debugController->SetEnableGPUBasedValidation(true);
						}
						else
						{
							SA_LOG(L"Validation layer DebugController initialization failed.", Error, DX12, (L"Error Code: %1", hrDebugInterface));
						}
					}

					// Report live objects
					{
						MComPtr<IDXGIInfoQueue> dxgiInfoQueue = nullptr;

						const HRESULT hrDebugInterface = DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue));
						if (SUCCEEDED(hrDebugInterface))
						{
							/**
							* Enable this to trigger breakpoints on ReportLiveObjects() and have errors in VisualStudio's output window.
							* WARNING: ReportLiveObjects() will ONLY output in VisualStudio's output window and not exe CONSOLE.
							*/
							dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
							dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
							dxgiInfoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, true);
						}
						else
						{
							SA_LOG(L"Validation layer DebugInfoQueue uninitialized failed.", Error, DX12);
						}
					}
				}

				// Enable additional debug layers.
				dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG; /* 0001-I1 */
#endif
				
				const HRESULT hrFactoryCreated = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));
				if (FAILED(hrFactoryCreated))
				{
					SA_LOG(L"Create Factory failed!", Error, DX12, (L"Error Code: %1", hrFactoryCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG(L"Create Factory success.", Info, DX12, factory.Get());
				}
			}


			// Device /* 0002-I */
			if (true)
			{
				// Don't need to keep reference after creating logical device.
				MComPtr<IDXGIAdapter3> adapter; // VkPhysicalDevice -> IDXGIAdapter.

				// Select first prefered GPU, listed by HIGH_PERFORMANCE. No need to manually sort GPU like Vulkan.
				const HRESULT hrQueryGPU = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
				if (FAILED(hrQueryGPU))
				{
					SA_LOG(L"Adapter not found!", Error, DX12, (L"Error Code: %1", hrQueryGPU));
					return EXIT_FAILURE;
				}

				const HRESULT hrDeviceCreated = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
				if (FAILED(hrDeviceCreated))
				{
					SA_LOG(L"Create Device failed!", Error, DX12, (L"Error Code: %1", hrDeviceCreated));
					return EXIT_FAILURE;
				}
				else
				{
					const LPCWSTR name = L"Main Device";
					device->SetName(name);

					SA_LOG(L"Create Device success.", Info, DX12, (L"\"%1\" [%2]", name, device.Get()));
				}

#if SA_DEBUG
				// Validation Layers (device-level) /* 0002-1 */
				{
					MComPtr<ID3D12InfoQueue1> infoQueue = nullptr;

					const HRESULT hrQueryInfoQueue = device->QueryInterface(IID_PPV_ARGS(&infoQueue));
					if (SUCCEEDED(hrQueryInfoQueue))
					{
						/**
						* Cookie must be provided to properly register message callback (and unregister later).
						* Set nullptr as cookie will not crash (and no error) but won't work.
						*/
						infoQueue->RegisterMessageCallback(ValidationLayersDebugCallback,
							D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr, &VLayerCallbackCookie);

						infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
						infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
						infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
					}
					else
					{
						SA_LOG(L"Device query info queue to enable validation layers failed.", Error, DX12, (L"Error Code: %1", hrQueryInfoQueue));
					}
				}
#endif

				// Queue /* 0002-I2 */
				{
					// This example renderer only uses 1 graphics queue.

					// GFX
					{
						const D3D12_COMMAND_QUEUE_DESC desc{
							.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
							.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
						};

						/**
						* DX12 can create queue 'on the fly' after device creation.
						* No need to specify in advance how many queues will be used by the device object.
						*/
						const HRESULT hrGFXCmdQueueCreated = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&graphicsQueue));
						if (FAILED(hrGFXCmdQueueCreated))
						{
							SA_LOG(L"Create Graphics Queue failed!", Error, DX12, (L"Error Code: %1", hrGFXCmdQueueCreated));
							return EXIT_FAILURE;
						}
						else
						{
							const LPCWSTR name = L"GraphicsQueue";
							graphicsQueue->SetName(name);

							SA_LOG(L"Create Graphics Queue success.", Info, DX12, (L"\"%1\" [%2]", name, graphicsQueue.Get()));
						}
					}
				}


				// Synchronization /* 0002-I3 */
				{
					deviceFenceEvent = CreateEvent(nullptr, false, false, nullptr);
					if (!deviceFenceEvent)
					{
						SA_LOG(L"Create Device Fence Event failed!", Error, DX12);
						return EXIT_FAILURE;
					}
					else
					{
						SA_LOG(L"Create Device Fence Event success.", Info, DX12);
					}

					const HRESULT hrDeviceFenceCreated = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&deviceFence));
					if (FAILED(hrDeviceFenceCreated))
					{
						SA_LOG(L"Create Device Fence failed!", Error, DX12, (L"Error Code: %1", hrDeviceFenceCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"DeviceFence";
						deviceFence->SetName(name);

						SA_LOG(L"Create Swapchain Fence success.", Info, DX12, (L"\"%1\" [%2]", name, deviceFence.Get()));
					}
				}
			}


			// Swapchain /* 0003-I */
			if (true)
			{
				const DXGI_SWAP_CHAIN_DESC1 desc{
					.Width = windowSize.x,
					.Height = windowSize.y,
					.Format = sceneColorFormat,
					.Stereo = false,
					.SampleDesc = {.Count = 1, .Quality = 0 },
					.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
					.BufferCount = bufferingCount,
					.Scaling = DXGI_SCALING_STRETCH,
					.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
					.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
					.Flags = 0,
				};

				MComPtr<IDXGISwapChain1> swapchain1;
				const HRESULT hrSwapChainCreated = factory->CreateSwapChainForHwnd(graphicsQueue.Get(), glfwGetWin32Window(window), &desc, nullptr, nullptr, &swapchain1);
				if (FAILED(hrSwapChainCreated))
				{
					SA_LOG(L"Create Swapchain failed!", Error, DX12, (L"Error Code: %1", hrSwapChainCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG(L"Create Swapchain success.", Info, DX12, swapchain1.Get());
				}

				const HRESULT hrSwapChainCast = swapchain1.As(&swapchain);
				if (FAILED(hrSwapChainCast))
				{
					SA_LOG(L"Swapchain cast failed!", Error, DX12, (L"Error Code: %1", hrSwapChainCast));
					return EXIT_FAILURE;
				}


				// Query back-buffers /* 0003-I1 */
				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					const HRESULT hrSwapChainGetBuffer = swapchain->GetBuffer(i, IID_PPV_ARGS(&swapchainImages[i]));
					if (FAILED(hrSwapChainGetBuffer))
					{
						SA_LOG((L"Get Swapchain Buffer [%1] failed!", i), Error, DX12, (L"Error Code: %1", hrSwapChainGetBuffer));
						return EXIT_FAILURE;
					}
					else
					{
						const std::wstring name = L"SwapchainBackBuffer [" + std::to_wstring(i) + L"]";
						swapchainImages[i]->SetName(name.data());

						SA_LOG((L"Get Swapchain Buffer [%1] success.", i), Info, DX12, (L"\"%1\" [%2]", name, swapchainImages[i].Get()));
					}
				}


				// Synchronization /* 0003.1-I */
				{
					swapchainFenceEvent = CreateEvent(nullptr, false, false, nullptr);
					if (!swapchainFenceEvent)
					{
						SA_LOG(L"Create Swapchain Fence Event failed!", Error, DX12);
						return EXIT_FAILURE;
					}

					const HRESULT hrSwapChainFenceCreated = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&swapchainFence));
					if (FAILED(hrSwapChainFenceCreated))
					{
						SA_LOG(L"Create Swapchain Fence failed!", Error, DX12, (L"Error Code: %1", hrSwapChainFenceCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"SwapchainFence";
						swapchainFence->SetName(name);

						SA_LOG(L"Create Swapchain Fence success.", Info, DX12, (L"\"%1\" [%2]", name, swapchainFence.Get()));
					}
				}
			}


			// Commands /* 0004-I */
			if (true)
			{
				// Allocators  /* 0004-I2-1 */
				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					auto& cmdAlloc = cmdAllocs[i];

					const HRESULT hrCmdAllocCreated = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
					if (FAILED(hrCmdAllocCreated))
					{
						SA_LOG((L"Create Command Allocator [%1] failed!", i), Error, DX12, (L"Error Code: %1", hrCmdAllocCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const std::wstring name = L"CommandAlloc [" + std::to_wstring(i) + L"]";
						cmdAlloc->SetName(name.c_str());

						SA_LOG((L"Create Command Allocator [%1] success", i), Info, DX12, (L"\"%1\" [%2]", name, cmdAlloc.Get()));
					}
				}

				// List /* 0004-I2-2 */
				{
					const HRESULT hrCmdListCreated = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocs[0].Get(), nullptr, IID_PPV_ARGS(&cmdList));
					if (FAILED(hrCmdListCreated))
					{
						SA_LOG("Create Command List failed!", Error, DX12, (L"Error Code: %1", hrCmdListCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"CommandList";
						cmdList->SetName(name);

						SA_LOG(L"Create Command List success.", Info, DX12, (L"\"%1\" [%2]", name, cmdList.Get()));
					}
				}

				// Command list must be closed because we will start the frame by Reset()
				cmdList->Close();
			}


			// Scene Textures /* 0005-I */
			if (true)
			{
				// Color RT View Heap /* 0006-I1 */
				{
					/**
					* Create a Render Target typed heap to allocate views.
					*/
					const D3D12_DESCRIPTOR_HEAP_DESC desc{
						.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
						.NumDescriptors = bufferingCount,
						.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
						.NodeMask = 0,
					};

					const HRESULT hrCreateHeap = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sceneRTViewHeap));
					if (FAILED(hrCreateHeap))
					{
						SA_LOG(L"Create Color RenderTarget ViewHeap failed!", Error, DX12, (L"Error Code: %1", hrCreateHeap));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"SceneRTViewHeap";
						sceneRTViewHeap->SetName(name);

						SA_LOG(L"Create Color RenderTarget ViewHeap success.", Info, DX12, (L"\"%1\" [%2]", name, sceneRTViewHeap.Get()));
					}


					// Create RT Views (for each frame)
					D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = sceneRTViewHeap->GetCPUDescriptorHandleForHeapStart();
					const UINT rtvOffset = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						device->CreateRenderTargetView(swapchainImages[i].Get(), nullptr, rtvHandle);
						rtvHandle.ptr += rtvOffset;
					}
				}


				// Depth Scene Texture /* 0005-I1 */
				{
					const D3D12_RESOURCE_DESC desc{
						.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
						.Alignment = 0u,
						.Width = windowSize.x,
						.Height = windowSize.y,
						.DepthOrArraySize = 1,
						.MipLevels = 1,
						.Format = sceneDepthFormat,
						.SampleDesc = DXGI_SAMPLE_DESC{
							.Count = 1,
							.Quality = 0,
						},
						.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
						.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
					};

					const D3D12_HEAP_PROPERTIES heap{
						.Type = D3D12_HEAP_TYPE_DEFAULT,
						.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
						.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
						.CreationNodeMask = 1,
						.VisibleNodeMask = 1,
					};

					const HRESULT hrCreateDepthTexture = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &sceneDepthClearValue, IID_PPV_ARGS(&sceneDepthTexture));
					if (FAILED(hrCreateDepthTexture))
					{
						SA_LOG(L"Create Scene Depth Texture failed!", Error, DX12, (L"Error Code: %1", hrCreateDepthTexture));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"SceneDepthTexture";
						sceneDepthTexture->SetName(name);

						SA_LOG(L"Create Scene Depth Texture success.", Info, DX12, (L"\"%1\" [%2]", name, sceneDepthTexture.Get()));
					}
				}

				// Depth Scene RT View Heap /* 0006-I2 */
				{
					/**
					* Create a 'Depth' typed heap to allocate views.
					*/
					const D3D12_DESCRIPTOR_HEAP_DESC desc{
						.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
						.NumDescriptors = 1,
						.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
						.NodeMask = 0,
					};

					const HRESULT hrCreateHeap = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sceneDepthRTViewHeap));
					if (FAILED(hrCreateHeap))
					{
						SA_LOG(L"Create Depth ViewHeap failed!", Error, DX12, (L"Error Code: %1", hrCreateHeap));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"SceneDepthViewHeap";
						sceneDepthRTViewHeap->SetName(name);

						SA_LOG(L"Create Depth ViewHeap success", Info, DX12, (L"\"%1\" [%2]", name, sceneDepthRTViewHeap.Get()));
					}

					/**
					* Create Depth View to use sceneDepthTexture as a render target.
					*/
					device->CreateDepthStencilView(sceneDepthTexture.Get(), nullptr, sceneDepthRTViewHeap->GetCPUDescriptorHandleForHeapStart());
				}
			}


			// Pipeline /* 0008-I */
			if (true)
			{
				// Viewport & Scissor
				{
					viewport = D3D12_VIEWPORT{
						.TopLeftX = 0,
						.TopLeftY = 0,
						.Width = float(windowSize.x),
						.Height = float(windowSize.y),
						.MinDepth = 0.0f,
						.MaxDepth = 1.0f,
					};

					scissorRect = D3D12_RECT{
						.left = 0,
						.top = 0,
						.right = static_cast<LONG>(windowSize.x),
						.bottom = static_cast<LONG>(windowSize.y),
					};
				}

				// Shader Compiler
				{
					const HRESULT hCreateUtils = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&shaderCompilerUtils));
					if (FAILED(hCreateUtils))
					{
						SA_LOG(L"Create DXC Shader Compiler Utils failed!", Error, DXC, (L"Error Code: %1", hCreateUtils));
						return EXIT_FAILURE;
					}

					const HRESULT hCreateInstance = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&shaderCompiler));
					if (FAILED(hCreateInstance))
					{
						SA_LOG(L"Create DXC Shader Compiler Instance failed!", Error, DXC, (L"Error Code: %1", hCreateInstance));
						return EXIT_FAILURE;
					}
				}

				// Lit
				{
					// RootSignature /* 0008-1-I */
					{
						/**
						* Root signature is the Pipeline Layout.
						* Since there is no 'DescriptorSetLayout' in DX12, we have to manually specify all the shader bindings here.
						*/

						const D3D12_DESCRIPTOR_RANGE1 pointLightSRVRange{
							.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
							.NumDescriptors = 1,
							.BaseShaderRegister = 0,
							.RegisterSpace = 0,
							.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
							.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
						};

						// Use Descriptor Table to bind all the textures at once.
						const D3D12_DESCRIPTOR_RANGE1 pbrTextureRange[]{
							// Albedo
							{
								.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
								.NumDescriptors = 1,
								.BaseShaderRegister = 1,
								.RegisterSpace = 0,
								.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
								.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
							},
							// Normal
							{
								.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
								.NumDescriptors = 1,
								.BaseShaderRegister = 2,
								.RegisterSpace = 0,
								.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
								.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
							},
							// Metallic
							{
								.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
								.NumDescriptors = 1,
								.BaseShaderRegister = 3,
								.RegisterSpace = 0,
								.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
								.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
							},
							// Roughness
							{
								.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
								.NumDescriptors = 1,
								.BaseShaderRegister = 4,
								.RegisterSpace = 0,
								.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
								.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
							},
						};

						const D3D12_ROOT_PARAMETER1 params[]{
							// Camera Constant buffer
							{
								.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
								.Descriptor = {
									.ShaderRegister = 0,
									.RegisterSpace = 0,
									.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
								},
								.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
							},
							// Object Constant buffer
							{
								.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
								.Descriptor = {
									.ShaderRegister = 1,
									.RegisterSpace = 0,
									.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
								},
								.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
							},
							// Point Lights Structured buffer
							{
								/**
								* Use DescriptorTable with SRV type instead of direct SRV binding to create BufferView in SRV Heap.
								* This allows use to correctly call pointLights.GetDimensions() in HLSL.
								*/
								.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
								.DescriptorTable {
									.NumDescriptorRanges = 1,
									.pDescriptorRanges = &pointLightSRVRange
								},
								.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
							},
							// PBR texture table
							{
								.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
								.DescriptorTable {
									.NumDescriptorRanges = _countof(pbrTextureRange),
									.pDescriptorRanges = pbrTextureRange
								},
								.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
							}
						};

						const D3D12_STATIC_SAMPLER_DESC sampler{
							.Filter = D3D12_FILTER_ANISOTROPIC,
							.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
							.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
							.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
							.MipLODBias = 0,
							.MaxAnisotropy = 16,
							.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS,
							.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
							.MinLOD = 0.0f,
							.MaxLOD = D3D12_FLOAT32_MAX,
							.ShaderRegister = 0,
							.RegisterSpace = 0,
							.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
						};

						const D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{
							.Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
							.Desc_1_1{
								.NumParameters = _countof(params),
								.pParameters = params,
								.NumStaticSamplers = 1,
								.pStaticSamplers = &sampler,
								.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
							}
						};


						MComPtr<ID3DBlob> signature;
						MComPtr<ID3DBlob> error;

						// RootSignature description must be serialized before creating object.
						const HRESULT hrSerRootSign = D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
						if (FAILED(hrSerRootSign))
						{
							std::string errorStr(static_cast<char*>(error->GetBufferPointer()), error->GetBufferSize());
							SA_LOG(L"Serialized Lit RootSignature failed!", Error, DX12, errorStr);

							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Serialized Lit RootSignature success.", Info, DX12, signature.Get());
						}


						const HRESULT hrCreateRootSign = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&litRootSign));
						if (FAILED(hrCreateRootSign))
						{
							SA_LOG(L"Create Lit RootSignature failed!", Error, DX12, (L"Error Code: %1", hrCreateRootSign));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit RootSignature success.", Info, DX12, litRootSign.Get());
						}
					}


					// Vertex Shader
					if (true)
					{
						litVertexShader = CompileShader(L"Resources/Shaders/HLSL/LitShader.hlsl", L"mainVS", L"vs_6_5");

						if (!litVertexShader)
							return EXIT_FAILURE;
					}

					// Fragment Shader
					{
						litPixelShader = CompileShader(L"Resources/Shaders/HLSL/LitShader.hlsl", L"mainPS", L"ps_6_5");

						if (!litPixelShader)
							return EXIT_FAILURE;
					}


					// PipelineState
					{
						const D3D12_RENDER_TARGET_BLEND_DESC rtBlend{
							.BlendEnable = false,
							.LogicOpEnable = false,

							.SrcBlend = D3D12_BLEND_ONE,
							.DestBlend = D3D12_BLEND_ZERO,
							.BlendOp = D3D12_BLEND_OP_ADD,

							.SrcBlendAlpha = D3D12_BLEND_ONE,
							.DestBlendAlpha = D3D12_BLEND_ZERO,
							.BlendOpAlpha = D3D12_BLEND_OP_ADD,

							.LogicOp = D3D12_LOGIC_OP_NOOP,

							.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
						};

						const D3D12_BLEND_DESC blendState{
							.AlphaToCoverageEnable = false,
							.IndependentBlendEnable = false,

							.RenderTarget
							{
								rtBlend,
								rtBlend,
								rtBlend,
								rtBlend,
								rtBlend,
								rtBlend,
								rtBlend,
								rtBlend
							}
						};

						const D3D12_RASTERIZER_DESC raster{
							.FillMode = D3D12_FILL_MODE_SOLID,
							.CullMode = D3D12_CULL_MODE_BACK,
							.FrontCounterClockwise = FALSE,
							.DepthBias = D3D12_DEFAULT_DEPTH_BIAS,
							.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
							.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
							.DepthClipEnable = TRUE,
							.MultisampleEnable = FALSE,
							.AntialiasedLineEnable = FALSE,
							.ForcedSampleCount = 0,
							.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
						};

						const D3D12_DEPTH_STENCIL_DESC depthStencilState{
							.DepthEnable = TRUE,
							.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
							.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
							.StencilEnable = FALSE,
							.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK,
							.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
							.FrontFace
							{
								.StencilFailOp = D3D12_STENCIL_OP_KEEP,
								.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
								.StencilPassOp = D3D12_STENCIL_OP_KEEP,
								.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
							},
							.BackFace
							{
								.StencilFailOp = D3D12_STENCIL_OP_KEEP,
								.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
								.StencilPassOp = D3D12_STENCIL_OP_KEEP,
								.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
							}
						};

						D3D12_INPUT_ELEMENT_DESC inputElems[]{
							{
								.SemanticName = "POSITION",
								.SemanticIndex = 0,
								.Format = DXGI_FORMAT_R32G32B32_FLOAT,
									.InputSlot = 0,
									.AlignedByteOffset = 0,
									.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
									.InstanceDataStepRate = 0
							},
							{
								.SemanticName = "NORMAL",
								.SemanticIndex = 0,
								.Format = DXGI_FORMAT_R32G32B32_FLOAT,
								.InputSlot = 1,
								.AlignedByteOffset = 0,
								.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
								.InstanceDataStepRate = 0
							},
							{
								.SemanticName = "TANGENT",
								.SemanticIndex = 0,
								.Format = DXGI_FORMAT_R32G32B32_FLOAT,
								.InputSlot = 2,
								.AlignedByteOffset = 0,
								.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
								.InstanceDataStepRate = 0
							},
							{
								.SemanticName = "TEXCOORD",
								.SemanticIndex = 0,
								.Format = DXGI_FORMAT_R32G32_FLOAT,
								.InputSlot = 3,
								.AlignedByteOffset = 0,
								.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
								.InstanceDataStepRate = 0
							}
						};
						const D3D12_INPUT_LAYOUT_DESC inputLayout{
							.pInputElementDescs = inputElems,
							.NumElements = _countof(inputElems)
						};


						const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{
							.pRootSignature = litRootSign.Get(),

							.VS{
								.pShaderBytecode = litVertexShader->GetBufferPointer(),
								.BytecodeLength = litVertexShader->GetBufferSize()
							},
							.PS{
								.pShaderBytecode = litPixelShader->GetBufferPointer(),
								.BytecodeLength = litPixelShader->GetBufferSize()
							},

							.StreamOutput = {},
							.BlendState = blendState,
							.SampleMask = UINT_MAX,

							.RasterizerState = raster,
							.InputLayout = inputLayout,

							.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,

							.NumRenderTargets = 1,
							.RTVFormats{
								sceneColorFormat
							},
							.DSVFormat = sceneDepthFormat,

							.SampleDesc
							{
								.Count = 1,
								.Quality = 0,
							},

							.NodeMask = 0,

							.CachedPSO
							{
								.pCachedBlob = nullptr,
								.CachedBlobSizeInBytes = 0,
							},

							.Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
						};

						const HRESULT hrCreatePipeline = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&litPipelineState));
						if (FAILED(hrCreatePipeline))
						{
							SA_LOG(L"Create Lit PipelineState failed!", Error, DX12, (L"Error Code: %1", hrCreatePipeline));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit PipelineState success.", Info, DX12, litPipelineState.Get());
						}
					}
				}
			}


			cmdList->Reset(cmdAllocs[0].Get(), nullptr);


			// Scene Objects /* 0009-I */
			if (true)
			{
				// PBR Sphere SRV View Heap
				{
					/**
					* Allocate Heap to emplace image/buffer views for future bindings.
					*/

					D3D12_DESCRIPTOR_HEAP_DESC desc{
						.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
						.NumDescriptors = 5,
						.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
					};

					const HRESULT hrCreateHeap = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pbrSphereSRVHeap));
					if (FAILED(hrCreateHeap))
					{
						SA_LOG(L"Create PBR Sphere SRV ViewHeap failed.", Error, DX12, (L"Error code: %1", hrCreateHeap));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"PBR Sphere SRV ViewHeap";
						pbrSphereSRVHeap->SetName(name);

						SA_LOG(L"Create PBR Sphere SRV ViewHeap success.", Info, DX12, (L"\"%1\" [%2]", name, pbrSphereSRVHeap.Get()));
					}
				}


				// Camera Buffers
				{
					const D3D12_HEAP_PROPERTIES heap{
						.Type = D3D12_HEAP_TYPE_UPLOAD, // Keep upload since we will update it each frame.
					};

					const D3D12_RESOURCE_DESC desc{
						.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
						.Alignment = 0,
						.Width = sizeof(CameraUBO),
						.Height = 1,
						.DepthOrArraySize = 1,
						.MipLevels = 1,
						.Format = DXGI_FORMAT_UNKNOWN,
						.SampleDesc = {.Count = 1, .Quality = 0 },
						.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
						.Flags = D3D12_RESOURCE_FLAG_NONE,
					};

					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cameraBuffers[i]));
						if (FAILED(hrBufferCreated))
						{
							SA_LOG((L"Create Camera Buffer [%1] failed!", i), Error, DX12, (L"Error code: %1", hrBufferCreated));
							return EXIT_FAILURE;
						}
						else
						{
							const std::wstring name = L"CameraBuffer [" + std::to_wstring(i) + L"]";
							cameraBuffers[i]->SetName(name.c_str());

							SA_LOG((L"Create Camera Buffer [%1] success", i), Info, DX12, (L"\"%1\" [%2]", name, cameraBuffers[i].Get()));
						}
					}
				}


				// Sphere Object Buffer
				{
					const D3D12_HEAP_PROPERTIES heap{
						.Type = D3D12_HEAP_TYPE_DEFAULT,
					};

					const D3D12_RESOURCE_DESC desc{
						.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
						.Alignment = 0,
						.Width = sizeof(ObjectUBO),
						.Height = 1,
						.DepthOrArraySize = 1,
						.MipLevels = 1,
						.Format = DXGI_FORMAT_UNKNOWN,
						.SampleDesc = {.Count = 1, .Quality = 0 },
						.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
						.Flags = D3D12_RESOURCE_FLAG_NONE,
					};

					const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereObjectBuffer));
					if (FAILED(hrBufferCreated))
					{
						SA_LOG(L"Create Sphere Object Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"SphereObjectBuffer";
						sphereObjectBuffer->SetName(name);

						SA_LOG(L"Create Sphere Object Buffer failed!", Info, DX12, (L"\"%1\" [%2]", name, sphereObjectBuffer.Get()));
					}

					const SA::Mat4f transform = SA::Mat4f::MakeTranslation(spherePosition);
					const bool bSubmitSuccess = SubmitBufferToGPU(sphereObjectBuffer, desc.Width, &transform, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
					if (!bSubmitSuccess)
					{
						SA_LOG(L"Sphere Object Buffer submit failed!", Error, DX12);
						return EXIT_FAILURE;
					}
				}


				// PointLights Buffer
				{
					const D3D12_HEAP_PROPERTIES heap{
						.Type = D3D12_HEAP_TYPE_DEFAULT,
					};

					const D3D12_RESOURCE_DESC desc{
						.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
						.Alignment = 0,
						.Width = pointLightNum * sizeof(PointLightUBO),
						.Height = 1,
						.DepthOrArraySize = 1,
						.MipLevels = 1,
						.Format = DXGI_FORMAT_UNKNOWN,
						.SampleDesc = {.Count = 1, .Quality = 0 },
						.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
						.Flags = D3D12_RESOURCE_FLAG_NONE,
					};

					const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pointLightBuffer));
					if (FAILED(hrBufferCreated))
					{
						SA_LOG(L"Create PointLights Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
						return EXIT_FAILURE;
					}
					else
					{
						const LPCWSTR name = L"PointLightsBuffer";
						pointLightBuffer->SetName(name);

						SA_LOG(L"Create PointLights Buffer success", Info, DX12, (L"\"%1\" [%2]", name, pointLightBuffer.Get()));
					}


					std::array<PointLightUBO, pointLightNum> pointlightsUBO{
						PointLightUBO{
							.position = SA::Vec3f(-0.25f, -1.0f, 0.0f),
							.intensity = 4.0f,
							.color = SA::Vec3f(1.0f, 1.0f, 0.0f),
							.radius = 3.0f
						},
						PointLightUBO{
							.position = SA::Vec3f(1.75f, 2.0f, 1.0f),
							.intensity = 7.0f,
							.color = SA::Vec3f(0.0f, 1.0f, 1.0f),
							.radius = 4.0f
						}
					};

					const bool bSubmitSuccess = SubmitBufferToGPU(pointLightBuffer, desc.Width, pointlightsUBO.data(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
					if (!bSubmitSuccess)
					{
						SA_LOG(L"Sphere PointLight submit failed!", Error, DX12);
						return EXIT_FAILURE;
					}

					// Create View /* 0011-I-1 */
					{
						D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
							.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
							.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
							.Buffer{
								.FirstElement = 0,
								.NumElements = static_cast<UINT>(pointlightsUBO.size()),
								.StructureByteStride = sizeof(PointLightUBO),
							},
						};
						D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = pbrSphereSRVHeap->GetCPUDescriptorHandleForHeapStart();
						device->CreateShaderResourceView(pointLightBuffer.Get(), &viewDesc, cpuHandle);
					}
				}
			}


			// Resources /* 0010-I */
			if (true)
			{
				Assimp::Importer importer;

				// Meshes
				{
					// Sphere
					{
						const char* path = "Resources/Models/Shapes/sphere.obj";
						const aiScene* scene = importer.ReadFile(path, aiProcess_CalcTangentSpace | aiProcess_ConvertToLeftHanded);
						if (!scene)
						{
							SA_LOG(L"Assimp loading failed!", Error, Assimp, path);
							return EXIT_FAILURE;
						}

						const aiMesh* inMesh = scene->mMeshes[0];

						// Position
						{
							/**
							* VkMemoryPropertyFlagBits -> D3D12_HEAP_PROPERTIES.Type
							* Defines if a buffer is GPU only, CPU-GPU, ...
							* In Vulkan, a buffer can be GPU only or CPU-GPU for data transfer (read and write).
							* In DirectX12, a buffer is either GPU only (default), 'Upload' for data transfer from CPU to GPU, or 'Readback' for data transfer from GPU to CPU.
							* 'Upload' and 'Readback' at the same time is NOT possible.
							*/
							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT, // Type Default is GPU only.
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								.Alignment = 0,
								.Width = sizeof(SA::Vec3f) * inMesh->mNumVertices,
								.Height = 1,
								.DepthOrArraySize = 1,
								.MipLevels = 1,
								.Format = DXGI_FORMAT_UNKNOWN,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereVertexBuffers[0]));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create Sphere Vertex Position Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"SphereVertexPositionBuffer";
								sphereVertexBuffers[0]->SetName(name);

								SA_LOG(L"Create Sphere Vertex Position Buffer success.", Info, DX12, (L"\"%1\" [%2]", name, sphereVertexBuffers[0].Get()));
							}

							sphereVertexBufferViews[0] = D3D12_VERTEX_BUFFER_VIEW{
								.BufferLocation = sphereVertexBuffers[0]->GetGPUVirtualAddress(),
								.SizeInBytes = static_cast<UINT>(desc.Width),
								.StrideInBytes = sizeof(SA::Vec3f),
							};

							const bool bSubmitSuccess = SubmitBufferToGPU(sphereVertexBuffers[0], desc.Width, inMesh->mVertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
							if (!bSubmitSuccess)
							{
								SA_LOG(L"Sphere Vertex Position Buffer submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}
						}

						// Normal
						{
							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								.Alignment = 0,
								.Width = sizeof(SA::Vec3f) * inMesh->mNumVertices,
								.Height = 1,
								.DepthOrArraySize = 1,
								.MipLevels = 1,
								.Format = DXGI_FORMAT_UNKNOWN,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereVertexBuffers[1]));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create Sphere Vertex Normal Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"SphereVertexNormalBuffer";
								sphereVertexBuffers[1]->SetName(name);

								SA_LOG(L"Create Sphere Vertex Normal Buffer success.", Info, DX12, (L"\"%1\" [%2]", name, sphereVertexBuffers[1].Get()));
							}

							sphereVertexBufferViews[1] = D3D12_VERTEX_BUFFER_VIEW{
								.BufferLocation = sphereVertexBuffers[1]->GetGPUVirtualAddress(),
								.SizeInBytes = static_cast<UINT>(desc.Width),
								.StrideInBytes = sizeof(SA::Vec3f),
							};

							const bool bSubmitSuccess = SubmitBufferToGPU(sphereVertexBuffers[1], desc.Width, inMesh->mNormals, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
							if (!bSubmitSuccess)
							{
								SA_LOG(L"Sphere Vertex Normal Buffer submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}
						}

						// Tangent
						{
							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								.Alignment = 0,
								.Width = sizeof(SA::Vec3f) * inMesh->mNumVertices,
								.Height = 1,
								.DepthOrArraySize = 1,
								.MipLevels = 1,
								.Format = DXGI_FORMAT_UNKNOWN,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereVertexBuffers[2]));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create Sphere Vertex Tangent Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"SphereVertexTangentBuffer";
								sphereVertexBuffers[2]->SetName(name);

								SA_LOG(L"Create Sphere Vertex Tangent Buffer success.", Info, DX12, (L"\"%1\" [%2]", name, sphereVertexBuffers[2].Get()));
							}

							sphereVertexBufferViews[2] = D3D12_VERTEX_BUFFER_VIEW{
								.BufferLocation = sphereVertexBuffers[2]->GetGPUVirtualAddress(),
								.SizeInBytes = static_cast<UINT>(desc.Width),
								.StrideInBytes = sizeof(SA::Vec3f),
							};

							const bool bSubmitSuccess = SubmitBufferToGPU(sphereVertexBuffers[2], desc.Width, inMesh->mTangents, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
							if (!bSubmitSuccess)
							{
								SA_LOG(L"Sphere Vertex Tangent Buffer submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}
						}

						// UV
						{
							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								.Alignment = 0,
								.Width = sizeof(SA::Vec2f) * inMesh->mNumVertices,
								.Height = 1,
								.DepthOrArraySize = 1,
								.MipLevels = 1,
								.Format = DXGI_FORMAT_UNKNOWN,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereVertexBuffers[3]));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create Sphere Vertex UV Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"SphereVertexUVBuffer";
								sphereVertexBuffers[3]->SetName(name);

								SA_LOG(L"Create Sphere Vertex UV Buffer success.", Info, DX12, (L"\"%1\" [%2]", name, sphereVertexBuffers[3].Get()));
							}

							sphereVertexBufferViews[3] = D3D12_VERTEX_BUFFER_VIEW{
								.BufferLocation = sphereVertexBuffers[3]->GetGPUVirtualAddress(),
								.SizeInBytes = static_cast<UINT>(desc.Width),
								.StrideInBytes = sizeof(SA::Vec2f),
							};

							std::vector<SA::Vec2f> uvs;
							uvs.reserve(inMesh->mNumVertices);

							for (uint32_t i = 0; i < inMesh->mNumVertices; ++i)
							{
								uvs.push_back(SA::Vec2f{ inMesh->mTextureCoords[0][i].x, inMesh->mTextureCoords[0][i].y });
							}

							const bool bSubmitSuccess = SubmitBufferToGPU(sphereVertexBuffers[3], desc.Width, uvs.data(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
							if (!bSubmitSuccess)
							{
								SA_LOG(L"Sphere Vertex UV Buffer submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}
						}

						// Index
						{
							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								.Alignment = 0,
								.Width = sizeof(uint16_t) * inMesh->mNumFaces * 3,
								.Height = 1,
								.DepthOrArraySize = 1,
								.MipLevels = 1,
								.Format = DXGI_FORMAT_UNKNOWN,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sphereIndexBuffer));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create Sphere Index Buffer failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"SphereIndexBuffer";
								sphereIndexBuffer->SetName(name);

								SA_LOG(L"Create Sphere Index Buffer success.", Info, DX12, (L"\"%1\" [%2]", name, sphereIndexBuffer.Get()));
							}

							sphereIndexBufferView = D3D12_INDEX_BUFFER_VIEW{
								.BufferLocation = sphereIndexBuffer->GetGPUVirtualAddress(),
								.SizeInBytes = static_cast<UINT>(desc.Width),
								.Format = DXGI_FORMAT_R16_UINT, // This model's indices are lower than 65535.
							};


							// Pack indices into uint16_t since max index < 65535.
							std::vector<uint16_t> indices;
							indices.resize(inMesh->mNumFaces * 3);
							sphereIndexCount = inMesh->mNumFaces * 3;

							for (unsigned int i = 0; i < inMesh->mNumFaces; ++i)
							{
								indices[i * 3] = static_cast<uint16_t>(inMesh->mFaces[i].mIndices[0]);
								indices[i * 3 + 1] = static_cast<uint16_t>(inMesh->mFaces[i].mIndices[1]);
								indices[i * 3 + 2] = static_cast<uint16_t>(inMesh->mFaces[i].mIndices[2]);
							}

							const bool bSubmitSuccess = SubmitBufferToGPU(sphereIndexBuffer, desc.Width, indices.data(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
							if (!bSubmitSuccess)
							{
								SA_LOG(L"Sphere Index Buffer submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}
						}
					}
				}

				// Textures
				if (true)
				{
					stbi_set_flip_vertically_on_load(true);

					// RustedIron2 PBR
					{
						const UINT srvOffset = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
						D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = pbrSphereSRVHeap->GetCPUDescriptorHandleForHeapStart();
						cpuHandle.ptr += srvOffset; // Add offset because first slot it for PointLightsBuffer.

						// Albedo
						{
							const char* path = "Resources/Textures/RustedIron2/rustediron2_basecolor.png";

							int width, height, channels;
							char* inData = reinterpret_cast<char*>(stbi_load(path, &width, &height, &channels, 4));
							if (!inData)
							{
								SA_LOG((L"STBI Texture Loading {%1} failed", path), Error, STB, stbi_failure_reason());
								return EXIT_FAILURE;
							}

							std::vector<char> data(inData, inData + width * height * channels);

							uint32_t mipLevels = 0u;
							uint32_t totalSize = 0u;
							std::vector<SA::Vec2ui> mipExtents;
							GenerateMipMapsCPU(
								SA::Vec2ui{
									static_cast<uint32_t>(width),
									static_cast<uint32_t>(height)
								},
								data,
								mipLevels,
								totalSize,
								mipExtents,
								channels);

							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
								.Alignment = 0,
								.Width = static_cast<uint32_t>(width),
								.Height = static_cast<uint32_t>(height),
								.DepthOrArraySize = 1,
								.MipLevels = static_cast<UINT16>(mipLevels),
								.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rustedIron2AlbedoTexture));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create RustedIron2 Albedo Texture failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"RustedIron2 Albedo";
								rustedIron2AlbedoTexture->SetName(name);

								SA_LOG(L"Create RustedIron2 Albedo Texture success.", Info, DX12, (L"\"%1\" [%2]", name, rustedIron2AlbedoTexture.Get()));
							}

							const bool bSubmitSuccess = SubmitTextureToGPU(rustedIron2AlbedoTexture, mipExtents, totalSize, channels, data.data());
							if (!bSubmitSuccess)
							{
								SA_LOG(L"RustedIron2 Albedo Texture submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}

							stbi_image_free(inData);


							// Create View /* 0011-I-2 */
							{
								D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
									.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
									.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
									.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
									.Texture2D{
										.MipLevels = mipLevels,
									},
								};

								device->CreateShaderResourceView(rustedIron2AlbedoTexture.Get(), &viewDesc, cpuHandle);
								cpuHandle.ptr += srvOffset;
							}
						}

						// Normal Map
						{
							const char* path = "Resources/Textures/RustedIron2/rustediron2_normal.png";

							int width, height, channels;
							char* inData = reinterpret_cast<char*>(stbi_load(path, &width, &height, &channels, 4));
							if (!inData)
							{
								SA_LOG((L"STBI Texture Loading {%1} failed", path), Error, STB, stbi_failure_reason());
								return EXIT_FAILURE;
							}
							channels = 4; // must force channels to 4 (format is RGBA).

							std::vector<char> data(inData, inData + width * height * channels);

							uint32_t mipLevels = 0u;
							uint32_t totalSize = 0u;
							std::vector<SA::Vec2ui> mipExtents;
							GenerateMipMapsCPU(
								SA::Vec2ui{
									static_cast<uint32_t>(width),
									static_cast<uint32_t>(height)
								},
								data,
								mipLevels,
								totalSize,
								mipExtents,
								channels);

							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
								.Alignment = 0,
								.Width = static_cast<uint32_t>(width),
								.Height = static_cast<uint32_t>(height),
								.DepthOrArraySize = 1,
								.MipLevels = static_cast<UINT16>(mipLevels),
								.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rustedIron2NormalTexture));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create RustedIron2 Normal Texture failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"RustedIron2 Normal";
								rustedIron2NormalTexture->SetName(name);

								SA_LOG(L"Create RustedIron2 Normal Texture success.", Info, DX12, (L"\"%1\" [%2]", name, rustedIron2NormalTexture.Get()));
							}

							const bool bSubmitSuccess = SubmitTextureToGPU(rustedIron2NormalTexture, mipExtents, totalSize, channels, data.data());
							if (!bSubmitSuccess)
							{
								SA_LOG(L"RustedIron2 Normal Texture submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}

							stbi_image_free(inData);


							// Create View /* 0011-I-3 */
							{
								D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
									.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
									.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
									.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
									.Texture2D{
										.MipLevels = mipLevels,
									},
								};

								device->CreateShaderResourceView(rustedIron2NormalTexture.Get(), &viewDesc, cpuHandle);
								cpuHandle.ptr += srvOffset;
							}
						}

						// Metallic
						{
							const char* path = "Resources/Textures/RustedIron2/rustediron2_metallic.png";

							int width, height, channels;
							char* inData = reinterpret_cast<char*>(stbi_load(path, &width, &height, &channels, 1));
							if (!inData)
							{
								SA_LOG((L"STBI Texture Loading {%1} failed", path), Error, STB, stbi_failure_reason());
								return EXIT_FAILURE;
							}

							std::vector<char> data(inData, inData + width * height * channels);

							uint32_t mipLevels = 0u;
							uint32_t totalSize = 0u;
							std::vector<SA::Vec2ui> mipExtents;
							GenerateMipMapsCPU(
								SA::Vec2ui{
									static_cast<uint32_t>(width),
									static_cast<uint32_t>(height)
								},
								data,
								mipLevels,
								totalSize,
								mipExtents,
								channels);

							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
								.Alignment = 0,
								.Width = static_cast<uint32_t>(width),
								.Height = static_cast<uint32_t>(height),
								.DepthOrArraySize = 1,
								.MipLevels = static_cast<UINT16>(mipLevels),
								.Format = DXGI_FORMAT_R8_UNORM,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rustedIron2MetallicTexture));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create RustedIron2 Metallic Texture failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"RustedIron2 Metallic";
								rustedIron2MetallicTexture->SetName(name);

								SA_LOG(L"Create RustedIron2 Metallic Texture success.", Info, DX12, (L"\"%1\" [%2]", name, rustedIron2MetallicTexture.Get()));
							}

							const bool bSubmitSuccess = SubmitTextureToGPU(rustedIron2MetallicTexture, mipExtents, totalSize, channels, data.data());
							if (!bSubmitSuccess)
							{
								SA_LOG(L"RustedIron2 Metallic Texture submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}

							stbi_image_free(inData);


							// Create View /* 0011-I-4 */
							{
								D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
									.Format = DXGI_FORMAT_R8_UNORM,
									.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
									.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
									.Texture2D{
										.MipLevels = mipLevels,
									},
								};

								device->CreateShaderResourceView(rustedIron2MetallicTexture.Get(), &viewDesc, cpuHandle);
								cpuHandle.ptr += srvOffset;
							}
						}

						// Roughness
						{
							const char* path = "Resources/Textures/RustedIron2/rustediron2_roughness.png";

							int width, height, channels;
							char* inData = reinterpret_cast<char*>(stbi_load(path, &width, &height, &channels, 1));
							if (!inData)
							{
								SA_LOG((L"STBI Texture Loading {%1} failed", path), Error, STB, stbi_failure_reason());
								return EXIT_FAILURE;
							}

							std::vector<char> data(inData, inData + width * height * channels);

							uint32_t mipLevels = 0u;
							uint32_t totalSize = 0u;
							std::vector<SA::Vec2ui> mipExtents;
							GenerateMipMapsCPU(
								SA::Vec2ui{
									static_cast<uint32_t>(width),
									static_cast<uint32_t>(height)
								},
								data,
								mipLevels,
								totalSize,
								mipExtents,
								channels);

							const D3D12_HEAP_PROPERTIES heap{
								.Type = D3D12_HEAP_TYPE_DEFAULT,
							};

							const D3D12_RESOURCE_DESC desc{
								.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
								.Alignment = 0,
								.Width = static_cast<uint32_t>(width),
								.Height = static_cast<uint32_t>(height),
								.DepthOrArraySize = 1,
								.MipLevels = static_cast<UINT16>(mipLevels),
								.Format = DXGI_FORMAT_R8_UNORM,
								.SampleDesc = {.Count = 1, .Quality = 0 },
								.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
								.Flags = D3D12_RESOURCE_FLAG_NONE,
							};

							const HRESULT hrBufferCreated = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rustedIron2RoughnessTexture));
							if (FAILED(hrBufferCreated))
							{
								SA_LOG(L"Create RustedIron2 Roughness Texture failed!", Error, DX12, (L"Error code: %1", hrBufferCreated));
								return EXIT_FAILURE;
							}
							else
							{
								const LPCWSTR name = L"RustedIron2 Roughness";
								rustedIron2RoughnessTexture->SetName(name);

								SA_LOG(L"Create RustedIron2 Roughness Texture success.", Info, DX12, (L"\"%1\" [%2]", name, rustedIron2RoughnessTexture.Get()));
							}

							const bool bSubmitSuccess = SubmitTextureToGPU(rustedIron2RoughnessTexture, mipExtents, totalSize, channels, data.data());
							if (!bSubmitSuccess)
							{
								SA_LOG(L"RustedIron2 Roughness Texture submit failed!", Error, DX12);
								return EXIT_FAILURE;
							}

							stbi_image_free(inData);


							// Create View /* 0011-I-5 */
							{
								D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{
									.Format = DXGI_FORMAT_R8_UNORM,
									.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
									.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
									.Texture2D{
										.MipLevels = mipLevels,
									},
								};

								device->CreateShaderResourceView(rustedIron2RoughnessTexture.Get(), &viewDesc, cpuHandle);
								cpuHandle.ptr += srvOffset;
							}
						}
					}
				}
			}


			cmdList->Close();
		}
	}


	// Loop
	if (true)
	{
		double oldMouseX = 0.0f;
		double oldMouseY = 0.0f;
		float dx = 0.0f;
		float dy = 0.0f;

		glfwGetCursorPos(window, &oldMouseX, &oldMouseY);

		const float fixedTime = 0.0025f;
		float accumulateTime = 0.0f;
		auto start = std::chrono::steady_clock::now();

		while (!glfwWindowShouldClose(window))
		{
			auto end = std::chrono::steady_clock::now();
			float deltaTime = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(end - start).count();
			accumulateTime += deltaTime;
			start = end;

			// Fixed Update
			if (accumulateTime >= fixedTime)
			{
				accumulateTime -= fixedTime;

				glfwPollEvents();

				// Process input
				{
					if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
						glfwSetWindowShouldClose(window, true);
					if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
						cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Right();
					if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
						cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Right();
					if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
						cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Up();
					if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
						cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Up();
					if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
						cameraTr.position += fixedTime * cameraMoveSpeed * cameraTr.Forward();
					if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
						cameraTr.position -= fixedTime * cameraMoveSpeed * cameraTr.Forward();

					double mouseX = 0.0f;
					double mouseY = 0.0f;

					glfwGetCursorPos(window, &mouseX, &mouseY);

					if (mouseX != oldMouseX || mouseY != oldMouseY)
					{
						dx += static_cast<float>(mouseX - oldMouseX) * fixedTime * cameraRotSpeed * SA::Maths::DegToRad<float>;
						dy += static_cast<float>(mouseY - oldMouseY) * fixedTime * cameraRotSpeed * SA::Maths::DegToRad<float>;

						oldMouseX = mouseX;
						oldMouseY = mouseY;

						dx = dx > SA::Maths::Pi<float> ?
							dx - SA::Maths::Pi<float> :
							dx < -SA::Maths::Pi<float> ? dx + SA::Maths::Pi<float> : dx;
						dy = dy > SA::Maths::Pi<float> ?
							dy - SA::Maths::Pi<float> :
							dy < -SA::Maths::Pi<float> ? dy + SA::Maths::Pi<float> : dy;

						cameraTr.rotation = SA::Quatf(cos(dx), 0, sin(dx), 0) * SA::Quatf(cos(dy), sin(dy), 0, 0);
					}
				}
			}


			// Render
			{
				// Swapchain Begin  /* 0003-U1 */
				{
					const UINT32 prevFenceValue = swapchainFenceValues[swapchainFrameIndex];

					// Update frame index.
					swapchainFrameIndex = swapchain->GetCurrentBackBufferIndex();

					const UINT32 currFenceValue = swapchainFenceValues[swapchainFrameIndex];


					// If the next frame is not ready to be rendered yet, wait until it is ready.
					if (swapchainFence->GetCompletedValue() < currFenceValue)
					{
						const HRESULT hrSetEvent = swapchainFence->SetEventOnCompletion(currFenceValue, swapchainFenceEvent);
						if (FAILED(hrSetEvent))
						{
							SA_LOG(L"Fence SetEventOnCompletion failed.", Error, DX12, (L"Error code: %1", hrSetEvent));
							return EXIT_FAILURE;
						}

						WaitForSingleObjectEx(swapchainFenceEvent, INFINITE, FALSE);
					}

					// Set the fence value for the next frame.
					swapchainFenceValues[swapchainFrameIndex] = prevFenceValue + 1;
				}


				// Update camera.
				auto cameraBuffer = cameraBuffers[swapchainFrameIndex];
				{

					// Fill Data with updated values.
					CameraUBO cameraUBO;
					cameraUBO.view = cameraTr.Matrix();
					const SA::Mat4f perspective = SA::Mat4f::MakePerspective(cameraFOV, float(windowSize.x) / float(windowSize.y), cameraNear, cameraFar);
					cameraUBO.invViewProj = perspective * cameraUBO.view.GetInversed();

					// Memory mapping and Upload (CPU to GPU transfer).
					const D3D12_RANGE range{ .Begin = 0, .End = 0 };
					void* data = nullptr;

					cameraBuffer->Map(0, &range, reinterpret_cast<void**>(&data));
					std::memcpy(data, &cameraUBO, sizeof(CameraUBO));
					cameraBuffer->Unmap(0, nullptr);
				}


				// Register Commands /* 0004-U */
				{
					auto cmdAlloc = cmdAllocs[swapchainFrameIndex];
					auto cmd = cmdList;

					cmdAlloc->Reset();
					cmd->Reset(cmdAlloc.Get(), nullptr);

					auto sceneColorRT = swapchainImages[swapchainFrameIndex];

					/**
					* 0006-U0
					*
					* Manage Render Targets for render:
					* Vulkan uses vkRenderPass and vkFramebuffers to describe in advance how the render targets should be managed through passes and subpasses.
					* DirectX12 doesn't have such system and must manage RenderTargets manually.
					*/
					{
						// Color Transition to RenderTarget.
						const D3D12_RESOURCE_BARRIER barrier{
							.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
							.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
							.Transition = {
								.pResource = sceneColorRT.Get(),
								.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
								.StateBefore = D3D12_RESOURCE_STATE_COMMON,
								.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET,
							},
						};

						cmd->ResourceBarrier(1, &barrier);


						// Bind & Clear /* 0006-U1 */
						{
							// Access current frame allocated view.
							D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = sceneRTViewHeap->GetCPUDescriptorHandleForHeapStart();
							const UINT rtvOffset = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
							rtvHandle.ptr += rtvOffset * swapchainFrameIndex;

							const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = sceneDepthRTViewHeap->GetCPUDescriptorHandleForHeapStart();

							cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
							cmd->ClearRenderTargetView(rtvHandle, sceneClearColor, 0, nullptr);
							cmd->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, sceneDepthClearValue.DepthStencil.Depth, sceneDepthClearValue.DepthStencil.Stencil, 0, nullptr);
						}
					}


					// Pipeline commons
					cmd->RSSetViewports(1, &viewport);
					cmd->RSSetScissorRects(1, &scissorRect);


					// Lit Pipeline
					{
						/**
						* 0011-U-1
						* Bind heaps.
						* /!\ Only one heaps of each type can be bound!
						*/
						ID3D12DescriptorHeap* descriptorHeaps[] = { pbrSphereSRVHeap.Get() };
						cmd->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);


						/**
						* 0011-U-2
						* DirectX12 doesn't have DescriptorSet: manually bind each entry of the RootSignature.
						*/
						cmd->SetGraphicsRootSignature(litRootSign.Get());
						cmd->SetGraphicsRootConstantBufferView(0, cameraBuffer->GetGPUVirtualAddress()); // Camera UBO
						cmd->SetGraphicsRootConstantBufferView(1, sphereObjectBuffer->GetGPUVirtualAddress()); // Object UBO

						const UINT srvOffset = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
						D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = pbrSphereSRVHeap->GetGPUDescriptorHandleForHeapStart();

						/**
						* Use DescriptorTable with SRV type instead of direct SRV binding to create BufferView in SRV Heap.
						* This allows use to correctly call pointLights.GetDimensions() in HLSL.
						*/
						//cmd->SetGraphicsRootShaderResourceView(2, pointLightBuffer->GetGPUVirtualAddress()); // PointLights
						cmd->SetGraphicsRootDescriptorTable(2, gpuHandle); // PointLights
						gpuHandle.ptr += srvOffset;

						cmd->SetGraphicsRootDescriptorTable(3, gpuHandle); // PBR textures
						gpuHandle.ptr += srvOffset;


						/* 0008-U */
						cmd->SetPipelineState(litPipelineState.Get());

						// Draw Sphere
						cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						cmd->IASetVertexBuffers(0, static_cast<UINT>(sphereVertexBufferViews.size()), sphereVertexBufferViews.data());
						cmd->IASetIndexBuffer(&sphereIndexBufferView);
						cmd->DrawIndexedInstanced(sphereIndexCount, 1, 0, 0, 0);
					}


					// Manage RenderTargets for present. 0006-U2
					{
						// Color Transition to Present.
						const D3D12_RESOURCE_BARRIER barrier{
							.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
							.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
							.Transition = {
								.pResource = sceneColorRT.Get(),
								.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
								.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
								.StateAfter = D3D12_RESOURCE_STATE_PRESENT,
							},
						};

						cmd->ResourceBarrier(1, &barrier);
					}


					cmd->Close();

					// Execute the command list.
					ID3D12CommandList* ppCommandLists[] = { cmd.Get() };
					graphicsQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
				}


				// Swapchain End /* 0003-U2 */
				if (true)
				{
					// Automatically present using internal present Queue if possible.
					const HRESULT hrPresent = swapchain->Present(1, 0);
					if (FAILED(hrPresent))
					{
						SA_LOG(L"Swapchain Present failed", Error, DX12, (L"Error code: %1", hrPresent));
						return EXIT_FAILURE;
					}

					// Schedule a Signal command in the queue.
					const UINT64 currFenceValue = swapchainFenceValues[swapchainFrameIndex];
					const HRESULT hrFenceSignal = graphicsQueue->Signal(swapchainFence.Get(), currFenceValue);
					if (FAILED(hrFenceSignal))
					{
						SA_LOG(L"Swapchain Fence Signal failed", Error, DX12, (L"Error code: %1", hrFenceSignal));
						return EXIT_FAILURE;
					}
				}
			}

			SA_LOG_END_OF_FRAME();
		}
	}



	// Uninitialization
	if(true)
	{
		// Renderer
		{
			WaitDeviceIdle();


			// Resources /* 0010-D */
			{
				// Textures
				{
					// RustedIron 2
					{
						SA_LOG(L"Destroying RustedIron2 Roughness Texture...", Info, DX12, rustedIron2RoughnessTexture.Get());
						rustedIron2RoughnessTexture = nullptr;

						SA_LOG(L"Destroying RustedIron2 Metallic Texture...", Info, DX12, rustedIron2MetallicTexture.Get());
						rustedIron2MetallicTexture = nullptr;


						SA_LOG(L"Destroying RustedIron2 Normal Texture...", Info, DX12, rustedIron2NormalTexture.Get());
						rustedIron2NormalTexture = nullptr;

						SA_LOG(L"Destroying RustedIron2 Albedo Texture...", Info, DX12, rustedIron2AlbedoTexture.Get());
						rustedIron2AlbedoTexture = nullptr;
					}
				}

				// Meshes
				{
					// Sphere
					{
						SA_LOG(L"Destroying Sphere Index Buffer...", Info, DX12, sphereIndexBuffer.Get());
						sphereIndexBuffer = nullptr;
						sphereIndexBufferView = D3D12_INDEX_BUFFER_VIEW{};

						SA_LOG(L"Destroying Sphere Vertex Position Buffer...", Info, DX12, sphereVertexBuffers[0].Get());
						sphereVertexBuffers[0] = nullptr;
						sphereVertexBufferViews[0] = D3D12_VERTEX_BUFFER_VIEW{};

						SA_LOG(L"Destroying Sphere Normal Position Buffer...", Info, DX12, sphereVertexBuffers[1].Get());
						sphereVertexBuffers[1] = nullptr;
						sphereVertexBufferViews[1] = D3D12_VERTEX_BUFFER_VIEW{};

						SA_LOG(L"Destroying Sphere Tangent Position Buffer...", Info, DX12, sphereVertexBuffers[2].Get());
						sphereVertexBuffers[2] = nullptr;
						sphereVertexBufferViews[2] = D3D12_VERTEX_BUFFER_VIEW{};

						SA_LOG(L"Destroying Sphere UV Position Buffer...", Info, DX12, sphereVertexBuffers[3].Get());
						sphereVertexBuffers[3] = nullptr;
						sphereVertexBufferViews[3] = D3D12_VERTEX_BUFFER_VIEW{};
					}
				}
			}


			// Scene Objects /* 0009-D */
			{
				// Camera Buffer
				{
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						SA_LOG((L"Destroying Camera Buffer [%1]...", i), Info, DX12, cameraBuffers[i].Get());
						cameraBuffers[i] = nullptr;
					}
				}

				// Sphere Object Buffer
				{
					SA_LOG(L"Destroying Sphere Object Buffer...", Info, DX12, sphereObjectBuffer.Get());
					sphereObjectBuffer = nullptr;
				}

				// PointLights Buffer
				{
					SA_LOG(L"Destroying PointLights Buffer...", Info, DX12, pointLightBuffer.Get());
					pointLightBuffer = nullptr;
				}

				// PBR Sphere ViewHeap
				{
					SA_LOG(L"Destroying PBR Sphere SRV ViewHeap...", Info, DX12, pbrSphereSRVHeap.Get());
					pbrSphereSRVHeap = nullptr;
				}
			}


			// Pipeline /* 0008-D */
			{
				// Lit
				{
					// PipelineState
					{
						SA_LOG(L"Destroying Lit PipelineState...", Info, DX12, litPipelineState.Get());
						litPipelineState = nullptr;
					}

					// Pixel Shader
					{
						SA_LOG(L"Destroying Lit Vertex Shader...", Info, DX12, litPixelShader.Get());
						litPixelShader = nullptr;
					}

					// Vertex Shader
					{
						SA_LOG(L"Destroying Lit Vertex Shader...", Info, DX12, litVertexShader.Get());
						litVertexShader = nullptr;
					}

					// RootSignature
					{
						SA_LOG(L"Destroying Lit RootSignature...", Info, DX12, litRootSign.Get());
						litRootSign = nullptr;
					}
				}

				shaderCompiler.Reset();
				shaderCompilerUtils.Reset();
			}


			// Scene Resources /* 0005-D - 0006-D */
			{
				SA_LOG(L"Destroying Scene Color RT ViewHeap...", Info, DX12, sceneRTViewHeap.Get());
				sceneRTViewHeap = nullptr;

				SA_LOG(L"Destroying Scene Depth RT ViewHeap...", Info, DX12, sceneDepthRTViewHeap.Get());
				sceneDepthRTViewHeap = nullptr;

				SA_LOG(L"Destroying Scene Depth Texture...", Info, DX12, sceneDepthTexture.Get());
				sceneDepthTexture = nullptr;
			}


			// Commands /* 0004-D */
			{
				SA_LOG(L"Destroying Command List...", Info, DX12, cmdList.Get());
				cmdList = nullptr;

				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					SA_LOG((L"Destroying Command Allocator [%1]...", i), Info, DX12, cmdAllocs[i].Get());
					cmdAllocs[i] = nullptr;
				}
			}


			// Swapchain /* 0003-D */
			{
				// Synchronization
				{
					CloseHandle(swapchainFenceEvent);
					SA_LOG(L"Destroy Swapchain Fence Event success", Info, DX12, swapchainFenceEvent);
					swapchainFenceEvent = nullptr;
					
					SA_LOG(L"Destroying Swapchain Fence...", Info, DX12, swapchainFence.Get());
					swapchainFence = nullptr;
				}

				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					SA_LOG((L"Destroying Swapchain image [%1]...", i), Info, DX12, swapchainImages[i].Get());
					swapchainImages[i] = nullptr;
				}

				SA_LOG(L"Destroying Swapchain...", Info, DX12, swapchain.Get());
				swapchain = nullptr;
			}


			// Device /* 0002-D */
			{
				// Synchronization
				{
					CloseHandle(deviceFenceEvent);
					SA_LOG(L"Destroy Device Fence Event success", Info, DX12, deviceFenceEvent);
					deviceFenceEvent = nullptr;

					SA_LOG(L"Destroying Device Fence...", Info, DX12, deviceFence.Get());
					deviceFence = nullptr;
				}

				// Queue
				{
					// GFX
					{
						SA_LOG(L"Destroying Graphics Queue...", Info, DX12, graphicsQueue.Get());
						graphicsQueue = nullptr;
					}
				}

#if SA_DEBUG
				// Validation Layers (device-level)
				if (VLayerCallbackCookie)
				{
					MComPtr<ID3D12InfoQueue1> infoQueue = nullptr;

					const HRESULT hrQueryInfoQueue = device->QueryInterface(IID_PPV_ARGS(&infoQueue));
					if (SUCCEEDED(hrQueryInfoQueue))
					{
						infoQueue->UnregisterMessageCallback(VLayerCallbackCookie);
						VLayerCallbackCookie = 0;
					}
				}
#endif

				SA_LOG(L"Destroying Device...", Info, DX12, device.Get());
				device = nullptr;
			}


			// Factory /* 0001-D */
			{
				SA_LOG(L"Destroying Factory...", Info, DX12, factory.Get());
				factory = nullptr;

#if SA_DEBUG
				// Report live objects
				MComPtr<IDXGIDebug1> dxgiDebug = nullptr;

				const HRESULT hrDebugInterface = DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug));
				if (SUCCEEDED(hrDebugInterface))
				{
					dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
				}
				else
				{
					SA_LOG(L"Validation layer uninitialized failed.", Error, DX12);
				}
#endif
			}
		}


		// GLFW
		{
			glfwDestroyWindow(window);
			SA_LOG(L"Destroy Window success", Info, GLFW, window);
			window = nullptr;

			glfwTerminate();
		}
	}

	return 0;
}
