#include <array>
#include <fstream>
#include <sstream>

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


// Resource Loading
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#pragma warning(disable : 4505)
#include <stb_image_resize2.h>
#pragma warning(default : 4505)

#include <shaderc/shaderc.hpp>

bool CompileShaderFromFile(const std::string& _path, shaderc_shader_kind _stage, std::vector<uint32_t>& _out)
{
	// Read File
	std::string code;
	{
		std::fstream fStream(_path, std::ios_base::in);

		if (!fStream.is_open())
		{
			SA_LOG((L"Failed to open shader file {%1}", _path), Error, VK.Shader);
			return false;
		}


		std::stringstream sstream;
		sstream << fStream.rdbuf();

		fStream.close();

		code = sstream.str();
	}

	// Compile
	static shaderc::Compiler compiler;

	shaderc::CompileOptions options;

#if SA_DEBUG
	options.SetOptimizationLevel(shaderc_optimization_level_zero);
#else
	options.SetOptimizationLevel(shaderc_optimization_level_performance);
#endif

	const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(code, _stage, _path.c_str(), options);

	if (result.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		SA_LOG((L"Compile Shader {%1} failed!", _path), Error, VK.Shader, (L"Errors: %1\tWarnings: %2\n%3", result.GetNumErrors(), result.GetNumWarnings(), result.GetErrorMessage()));
		return false;
	}
	else if (result.GetNumWarnings())
	{
		SA_LOG((L"Compile Shader {%1} success with %2 warnings.", _path, result.GetNumWarnings()), Warning, VK.Shadar, result.GetErrorMessage());
	}
	else
	{
		SA_LOG((L"Compile Shader {%1} success.", _path), Info, VK.Shader);
	}

	_out = { result.cbegin(), result.cend() };

	return true;
}


// ========== Windowing ==========

#define GLFW_INCLUDE_VULKAN 1
#include <GLFW/glfw3.h>

GLFWwindow* window = nullptr;
constexpr SA::Vec2ui windowSize = { 1200, 900 };

void GLFWErrorCallback(int32_t error, const char* description)
{
	SA_LOG((L"GLFW Error [%1]: %2", error, description), Error, GLFW.API);
}



// ========== Renderer ==========

#include <vulkan/vulkan.h> // vulkan.h -> d3d12.h

// === Validation Layers ===

#if SA_DEBUG
std::vector<const char*> validationLayers{
	"VK_LAYER_KHRONOS_validation"
};

VKAPI_ATTR VkBool32 VKAPI_CALL ValidationLayersDebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	(void)pUserData;

	std::wstring msgTypeStr;

	switch (messageType)
	{
	case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
	{
		msgTypeStr = L"[General]";
		break;
	}
	case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
	{
		msgTypeStr = L"[Validation]";
		break;
	}
	case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
	{
		msgTypeStr = L"[Performance]";
		break;
	}
	default:
	{
		msgTypeStr = L"[Unknown]";
		break;
	}
	}

	switch (messageSeverity)
	{
		//case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
		//	break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
	{
		SA_LOG(pCallbackData->pMessage, Info, VK.ValidationLayers, (L"Vulkan Validation Layers %1", msgTypeStr));
		break;
	}
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
	{
		SA_LOG(pCallbackData->pMessage, Warning, VK.ValidationLayers, (L"Vulkan Validation Layers %1", msgTypeStr));
		break;
	}
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
	{
		SA_LOG(pCallbackData->pMessage, Error, VK.ValidationLayers, (L"Vulkan Validation Layers %1", msgTypeStr));
		break;
	}
	default:
	{
		SA_LOG(pCallbackData->pMessage, Normal, VK.ValidationLayers, (L"Vulkan Validation Layers %1", msgTypeStr));
		break;
	}
	}

	return VK_FALSE;
}
#endif


// === Instance ===

std::vector<const char*> vkInstanceExts{
#if SA_DEBUG
	VK_EXT_DEBUG_UTILS_EXTENSION_NAME
#endif
};

VkInstance instance = VK_NULL_HANDLE;


// === Surface ===

VkSurfaceKHR windowSurface = VK_NULL_HANDLE;


// === Device ===

struct QueueFamilyIndices
{
	uint32_t graphicsFamily = uint32_t(-1);
	//uint32_t computeFamily = uint32_t(-1);
	uint32_t presentFamily = uint32_t(-1);
};

VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
QueueFamilyIndices deviceQueueFamilyIndices;

const std::vector<const char*> vkDeviceReqExts{
	VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

VkDevice device = VK_NULL_HANDLE;

VkQueue graphicsQueue = VK_NULL_HANDLE;
//VkQueue computeQueue = VK_NULL_HANDLE;
VkQueue presentQueue = VK_NULL_HANDLE;


// === Swapchain ===

constexpr uint32_t bufferingCount = 3;

VkSwapchainKHR swapchain = VK_NULL_HANDLE;
std::array<VkImage, bufferingCount> swapchainImages{ VK_NULL_HANDLE };
std::array<VkImageView, bufferingCount> swapchainImageViews{ VK_NULL_HANDLE };

struct SwapchainSynchronisation
{
	VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
	VkSemaphore presentSemaphore = VK_NULL_HANDLE;
	VkFence		fence = VK_NULL_HANDLE;
};
std::array<SwapchainSynchronisation, bufferingCount> swapchainSyncs{};


// === Commands ===
VkCommandPool cmdPool = VK_NULL_HANDLE;
std::array<VkCommandBuffer, bufferingCount> cmdBuffers{ VK_NULL_HANDLE };


// === Scene Textures ===

// = Color =
VkFormat sceneColorFormat = VK_FORMAT_R8G8B8_SRGB;
// Use Swapchain backbuffer texture as color output.

// = Depth =
const VkFormat sceneDepthFormat = VK_FORMAT_D16_UNORM;

VkImage sceneDepthImage = VK_NULL_HANDLE;
VkDeviceMemory sceneDepthImageMemory = VK_NULL_HANDLE;
VkImageView sceneDepthImageView = VK_NULL_HANDLE;

uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}

	SA_LOG(L"Failed to find suitable memory type!", Error, VK);
	return uint32_t(-1);
}


// === RenderPass ===
VkRenderPass renderPass = VK_NULL_HANDLE;


// === Frame Buffer ===
std::array<VkFramebuffer, bufferingCount> framebuffers{ VK_NULL_HANDLE };


// === DescriptorSet ===
VkDescriptorSetLayout litDescSetLayout = VK_NULL_HANDLE;


// === Pipeline ===

// = Viewport & Scissor =
VkViewport viewport{};
VkRect2D scissorRect{};

// = Lit =

VkShaderModule litVertexShader = VK_NULL_HANDLE;
VkShaderModule litFragmentShader = VK_NULL_HANDLE;

VkPipelineLayout litPipelineLayout = VK_NULL_HANDLE;
VkPipeline litPipeline = VK_NULL_HANDLE;



int main()
{
	// Initialization
	{
		SA::Debug::InitDefaultLogger();

		// GLFW
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			glfwInit();

			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			window = glfwCreateWindow(windowSize.x, windowSize.y, "FVTDX12_VK-Window", nullptr, nullptr);

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


			// Add GLFW required Extensions for present support
			{
				uint32_t glfwExtensionCount = 0;
				const char** glfwExtensions = nullptr;

				glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

				vkInstanceExts.reserve(glfwExtensionCount);
				vkInstanceExts.insert(vkInstanceExts.end(), glfwExtensions, glfwExtensions + glfwExtensionCount);
			}
		}


		// Renderer
		{
			// Instance
			{
				const VkApplicationInfo appInfo{
					.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
					.pNext = nullptr,
					.pApplicationName = "FVTDX12_VK-App",
					.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
					.pEngineName = "No Engine",
					.engineVersion = VK_MAKE_VERSION(1, 0, 0),
					.apiVersion = VK_API_VERSION_1_2,
				};

				VkInstanceCreateInfo instanceCreateInfo{
					.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
					.pNext = nullptr,
					.flags = 0u,
					.pApplicationInfo = &appInfo,
					.enabledLayerCount = 0u,
					.ppEnabledLayerNames = nullptr,
					.enabledExtensionCount = static_cast<uint32_t>(vkInstanceExts.size()),
					.ppEnabledExtensionNames = vkInstanceExts.data(),
				};

#if SA_DEBUG
				// Validation Layers
				// Check Validation Layers Support
				{
					// Query currently supported layers.
					uint32_t layerCount;
					vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

					std::vector<VkLayerProperties> availableLayers(layerCount);
					vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());


					// Check each asked supported.
					for (uint32_t i = 0; i < validationLayers.size(); ++i)
					{
						bool layerFound = false;

						for (uint32_t j = 0; j < availableLayers.size(); ++j)
						{
							// Layer found.
							if (std::strcmp(validationLayers[i], availableLayers[j].layerName) == 0)
							{
								layerFound = true;
								break;
							}
						}

						// Layer not found.
						if (!layerFound)
						{
							SA_LOG((L"Validation Layers [%1] not supported!", validationLayers[i]), Error, VK.ValidationLayers);
							return EXIT_FAILURE;
						}
					}
				}

				const VkDebugUtilsMessengerCreateInfoEXT debugUtilsInfo{
					.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
					.pNext = nullptr,
					.flags = 0u,
					.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
										VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
										VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
					.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
									VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
									VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
					.pfnUserCallback = ValidationLayersDebugCallback,
					.pUserData = nullptr,
				};

				instanceCreateInfo.pNext = &debugUtilsInfo;

				instanceCreateInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
				instanceCreateInfo.ppEnabledLayerNames = validationLayers.data();
#endif

				const VkResult vrInstanceCreated = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);
				if (vrInstanceCreated != VK_SUCCESS)
				{
					SA_LOG(L"Create VkInstance failed!", Error, VK, (L"Error Code: %1", vrInstanceCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG("Create VkInstance success.", Info, GLFW, instance);
				}
			}


			// Surface
			{
				/**
				* Create Vulkan Surface from GLFW window.
				* Required to create PresentQueue in Device.
				*/
				const VkResult vrWindowSurfaceCreated = glfwCreateWindowSurface(instance, window, nullptr, &windowSurface);
				if (vrWindowSurfaceCreated != VK_SUCCESS)
				{
					SA_LOG(L"Create Window Surafce failed!", Error, VK, (L"Error Code: %1", vrWindowSurfaceCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG("Create Window Surafce success.", Info, GLFW, windowSurface);
				}
			}


			// Device
			{
				// Query physical devices
				uint32_t deviceCount = 0;
				const VkResult vrEnumPhysDeviceCount = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
				if (vrEnumPhysDeviceCount != VK_SUCCESS)
				{
					SA_LOG(L"Enumerate Physical Devices Count failed!", Error, VK, (L"Error Code: %1", vrEnumPhysDeviceCount));
					return EXIT_FAILURE;
				}
				if (deviceCount == 0)
				{
					SA_LOG(L"No GPU with Vulkan support found!", Error, VK);
					return EXIT_FAILURE;
				}

				std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
				const VkResult vrEnumPhysDevices = vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
				if (vrEnumPhysDevices != VK_SUCCESS)
				{
					SA_LOG(L"Enumerate Physical Devices failed!", Error, VK, (L"Error Code: %1", vrEnumPhysDevices));
					return EXIT_FAILURE;
				}


				// Find first suitable device (no scoring).
				for (auto& currPhysicalDevice : physicalDevices)
				{
					// Check extensions support
					{
						// Query extensions.
						uint32_t extensionCount = 0u;
						const VkResult vrEnumDeviceExtsCount = vkEnumerateDeviceExtensionProperties(currPhysicalDevice, nullptr, &extensionCount, nullptr);
						if (vrEnumDeviceExtsCount != VK_SUCCESS)
						{
							SA_LOG(L"Enumerate Devices extensions count failed!", Error, VK, (L"Error Code: %1", vrEnumDeviceExtsCount));
							return EXIT_FAILURE;
						}

						std::vector<VkExtensionProperties> supportedExts(extensionCount);
						const VkResult vrEnumDeviceExts = vkEnumerateDeviceExtensionProperties(currPhysicalDevice, nullptr, &extensionCount, supportedExts.data());
						if (vrEnumDeviceExts != VK_SUCCESS)
						{
							SA_LOG(L"Enumerate Devices extensions failed!", Error, VK, (L"Error Code: %1", vrEnumDeviceExts));
							return EXIT_FAILURE;
						}


						// Check support
						bool bAllReqExtSupported = true;

						for (auto reqExt : vkDeviceReqExts)
						{
							bool bExtFound = false;

							for (const auto& suppExt : supportedExts)
							{
								// Extension found.
								if (std::strcmp(reqExt, suppExt.extensionName) == 0)
								{
									bExtFound = true;
									break;
								}
							}

							if (!bExtFound)
							{
								bAllReqExtSupported = false;
								break;
							}
						}

						if (!bAllReqExtSupported)
							continue; // go to next device.
					}

					// Find Queue Families
					{
						QueueFamilyIndices currPhysicalDeviceQueueFamilies;

						uint32_t queueFamilyCount = 0;
						vkGetPhysicalDeviceQueueFamilyProperties(currPhysicalDevice, &queueFamilyCount, nullptr);

						std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
						vkGetPhysicalDeviceQueueFamilyProperties(currPhysicalDevice, &queueFamilyCount, queueFamilies.data());

						for (uint32_t i = 0; i < queueFamilyCount; ++i)
						{
							const auto& currFamily = queueFamilies[i];

							// Graphics family.
							if (currPhysicalDeviceQueueFamilies.graphicsFamily == uint32_t(-1) && (currFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT))
								currPhysicalDeviceQueueFamilies.graphicsFamily = i;

							//// Compute family.
							//if (currPhysicalDeviceQueueFamilies.computeFamily == uint32_t(-1) && (currFamily.queueFlags & VK_QUEUE_COMPUTE_BIT))
							//	currPhysicalDeviceQueueFamilies.computeFamily = i;

							// Present family.
							if (currPhysicalDeviceQueueFamilies.presentFamily == uint32_t(-1))
							{
								VkBool32 presentSupport = false;
								const VkResult vrSupportKHR = vkGetPhysicalDeviceSurfaceSupportKHR(currPhysicalDevice, i, windowSurface, &presentSupport);
								if (vrSupportKHR != VK_SUCCESS)
								{
									SA_LOG(L"Physical Device Surface Support failed.", Error, VK, (L"Error Code: %1", vrSupportKHR));
									return EXIT_SUCCESS;
								}

								if (presentSupport == VK_SUCCESS)
									currPhysicalDeviceQueueFamilies.presentFamily = i;
							}
						}

						// Check all queues can be created
						if (currPhysicalDeviceQueueFamilies.graphicsFamily == uint32_t(-1) ||
							//currPhysicalDeviceQueueFamilies.computeFamily == uint32_t(-1) ||
							currPhysicalDeviceQueueFamilies.presentFamily == uint32_t(-1))
							continue;  // go to next device.

						deviceQueueFamilyIndices = currPhysicalDeviceQueueFamilies;
					}

					physicalDevice = currPhysicalDevice;
					break;
				}

				if (physicalDevice == VK_NULL_HANDLE)
				{
					SA_LOG(L"No suitable PhysicalDevice found.", Error, VK);
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG(L"Create Physical Device success", Info, VK, physicalDevice);
				}


				// Create Logical Device.
				const VkPhysicalDeviceFeatures deviceFeatures{};

				const float queuePriority = 1.0f;
				const std::array<VkDeviceQueueCreateInfo, 2> queueCreateInfo{
					VkDeviceQueueCreateInfo{
						.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
						.pNext = nullptr,
						.flags = 0,
						.queueFamilyIndex = deviceQueueFamilyIndices.graphicsFamily,
						.queueCount = 1,
						.pQueuePriorities = &queuePriority,
					},
					//VkDeviceQueueCreateInfo{
					//	.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					//	.pNext = nullptr,
					//	.flags = 0,
					//	.queueFamilyIndex = deviceQueueFamilyIndices.computeFamily,
					//	.queueCount = 1,
					//	.pQueuePriorities = &queuePriority,
					//},
					VkDeviceQueueCreateInfo{
						.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
						.pNext = nullptr,
						.flags = 0,
						.queueFamilyIndex = deviceQueueFamilyIndices.presentFamily,
						.queueCount = 1,
						.pQueuePriorities = &queuePriority,
					}
				};

				VkDeviceCreateInfo deviceCreateInfo{
					.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
					.pNext = nullptr,
					.flags = 0,
					.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfo.size()),
					.pQueueCreateInfos = queueCreateInfo.data(),
					.enabledLayerCount = 0,
					.ppEnabledLayerNames = nullptr,
					.enabledExtensionCount = static_cast<uint32_t>(vkDeviceReqExts.size()),
					.ppEnabledExtensionNames = vkDeviceReqExts.data(),
					.pEnabledFeatures = &deviceFeatures,
				};

#if SA_DEBUG

				deviceCreateInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
				deviceCreateInfo.ppEnabledLayerNames = validationLayers.data();

#endif

				const VkResult vrDeviceCreated = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device);
				if (vrDeviceCreated != VK_SUCCESS)
				{
					SA_LOG(L"Create Logical Device failed.", Error, VK, (L"Error Code: %1", vrDeviceCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG(L"Create Logical Device success.", Info, VK, device);
				}


				// Create Queues
				vkGetDeviceQueue(device, deviceQueueFamilyIndices.graphicsFamily, 0, &graphicsQueue);
				SA_LOG(L"Create Graphics Queue success.", Info, VK, graphicsQueue);

				//vkGetDeviceQueue(device, deviceQueueFamilyIndices.computeFamily, 0, &computeQueue);
				//SA_LOG(L"Create Compute Queue success.", Info, VK, computeQueue);

				vkGetDeviceQueue(device, deviceQueueFamilyIndices.presentFamily, 0, &presentQueue);
				SA_LOG(L"Create Present Queue success.", Info, VK, presentQueue);
			}


			// Swapchain
			{
				// Query Support Details
				VkSurfaceCapabilitiesKHR capabilities;
				std::vector<VkSurfaceFormatKHR> formats;
				std::vector<VkPresentModeKHR> presentModes;
				{
					// Capabilities
					const VkResult vrGetSurfaceCapabilities = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, windowSurface, &capabilities);
					if (vrGetSurfaceCapabilities != VK_SUCCESS)
					{
						SA_LOG(L"Get Physical Device Surface Capabilities failed!", Error, VK);
						return EXIT_FAILURE;
					}


					// Formats
					uint32_t formatCount = 0u;
					const VkResult vrGetSurfaceFormatsCount = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, windowSurface, &formatCount, nullptr);
					if (vrGetSurfaceFormatsCount != VK_SUCCESS)
					{
						SA_LOG(L"Get Physical Device Surface Formats Count failed!", Error, VK, (L"Error Code: %1", vrGetSurfaceFormatsCount));
						return EXIT_FAILURE;
					}
					if (formatCount == 0)
					{
						SA_LOG(L"No physical device surface formats found!", Error, VK);
						return EXIT_FAILURE;
					}

					formats.resize(formatCount);
					const VkResult vrGetSurfaceFormats = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, windowSurface, &formatCount, formats.data());
					if (vrGetSurfaceFormats != VK_SUCCESS)
					{
						SA_LOG(L"Get Physical Device Surface Formats failed!", Error, VK, (L"Error Code: %1", vrGetSurfaceFormats));
						return EXIT_FAILURE;
					}


					// Present modes
					uint32_t presentModeCount = 0u;
					const VkResult vrGetSurfacePresentCount = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, windowSurface, &presentModeCount, nullptr);
					if (vrGetSurfacePresentCount != VK_SUCCESS)
					{
						SA_LOG(L"Get Physical Device Surface PresentModes Count failed!", Error, VK, (L"Error Code: %1", vrGetSurfacePresentCount));
						return EXIT_FAILURE;
					}
					if (presentModeCount == 0)
					{
						SA_LOG(L"No physical device present modes found!", Error, VK);
						return EXIT_FAILURE;
					}

					presentModes.resize(presentModeCount);
					const VkResult vrGetSurfacePresent = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, windowSurface, &presentModeCount, presentModes.data());
					if (vrGetSurfacePresent != VK_SUCCESS)
					{
						SA_LOG(L"Get Physical Device Surface present modes failed!", Error, VK, (L"Error Code: %1", vrGetSurfacePresent));
						return EXIT_FAILURE;
					}
				}

				// ChooseSwapSurfaceFormat
				VkSurfaceFormatKHR swapchainFormat = formats[0];
				{
					// Find prefered
					for (uint32_t i = 0; i < formats.size(); ++i)
					{
						if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
						{
							swapchainFormat = formats[i];
							break;
						}
					}

					sceneColorFormat = swapchainFormat.format;
				}

				// ChooseSwapPresentMode
				VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR; // Default FIFO always supported.
				{
					// Find prefered.
					for (uint32_t i = 0; i < presentModes.size(); ++i)
					{
						if (presentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
						{
							swapchainPresentMode = presentModes[i];
							break;
						}
					}
				}

				
				VkSharingMode swapchainImageSharingMode = VK_SHARING_MODE_EXCLUSIVE;


				// Provide queue Family indices.
				std::array<uint32_t, 2> queueFamilyIndices{
					deviceQueueFamilyIndices.graphicsFamily,
					deviceQueueFamilyIndices.presentFamily,
				};

				// Graphic and present familiy are different.
				if (queueFamilyIndices[0] != queueFamilyIndices[1])
					swapchainImageSharingMode = VK_SHARING_MODE_CONCURRENT;


				const VkSwapchainCreateInfoKHR swapchainCreateInfo{
					.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
					.pNext = nullptr,
					.flags = 0u,
					.surface = windowSurface,
					.minImageCount = bufferingCount,
					.imageFormat = swapchainFormat.format,
					.imageColorSpace = swapchainFormat.colorSpace,
					.imageExtent = VkExtent2D{ windowSize.x, windowSize.y },
					.imageArrayLayers = 1u,
					.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
					.imageSharingMode = swapchainImageSharingMode,
					.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size()),
					.pQueueFamilyIndices = queueFamilyIndices.data(),
					.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
					.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
					.presentMode = swapchainPresentMode,
					.clipped = VK_TRUE,
					.oldSwapchain = VK_NULL_HANDLE,
				};

				const VkResult vrSwapchainCreated = vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain);
				if (vrSwapchainCreated != VK_SUCCESS)
				{
					SA_LOG("Create Swapchain failed!", Error, VK, (L"Error Code: %1", vrSwapchainCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG("Create Swapchain success.", Info, VK, swapchain);
				}


				// Query backbuffer images.
				uint32_t swapchainImageNum = bufferingCount;
				const VkResult vrGetSwapchainImages = vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageNum, swapchainImages.data());
				if (vrGetSwapchainImages != VK_SUCCESS || swapchainImageNum != bufferingCount)
				{
					SA_LOG(L"Get Swapchain Images failed!", Error, VK, (L"Error Code: %1", vrGetSwapchainImages));
					return EXIT_FAILURE;
				}
				else
				{
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						SA_LOG(L"Created Swapchain backbuffer images success.", Info, VK, swapchainImages[i]);
					}
				}


				// Image Views
				{
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						const VkImageViewCreateInfo imgViewCreateInfo{
							.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0,
							.image = swapchainImages[i],
							.viewType = VK_IMAGE_VIEW_TYPE_2D,
							.format = sceneColorFormat,
							.components{
								.r = VK_COMPONENT_SWIZZLE_IDENTITY,
								.g = VK_COMPONENT_SWIZZLE_IDENTITY,
								.b = VK_COMPONENT_SWIZZLE_IDENTITY,
								.a = VK_COMPONENT_SWIZZLE_IDENTITY
							},
							.subresourceRange{
								.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
								.baseMipLevel = 0,
								.levelCount = 1,
								.baseArrayLayer = 0,
								.layerCount = 1,
							}
						};

						const VkResult vrImageViewCreated = vkCreateImageView(device, &imgViewCreateInfo, nullptr, &swapchainImageViews[i]);
						if (vrImageViewCreated != VK_SUCCESS)
						{
							SA_LOG(L"Create Swapchain ImageView failed!", Error, VK, (L"Error Code: %1", vrImageViewCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Swapchain ImageView success.", Info, VK, swapchainImageViews[i]);
						}
					}
				}


				// Synchronization
				{
					VkSemaphoreCreateInfo semaphoreCreateInfo{};
					semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
					semaphoreCreateInfo.pNext = nullptr;
					semaphoreCreateInfo.flags = 0u;

					VkFenceCreateInfo fenceCreateInfo{};
					fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
					fenceCreateInfo.pNext = nullptr;
					fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						// Acquire Semaphore
						const VkResult vrAcqSemaphoreCreated = vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &swapchainSyncs[i].acquireSemaphore);
						if (vrAcqSemaphoreCreated != VK_SUCCESS)
						{
							SA_LOG((L"Create Swapchain Acquire Semaphore [%1] failed!", i), Error, VK, (L"Error Code: %1", vrAcqSemaphoreCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG((L"Create Swapchain Acquire Semaphore [%1] success", i), Info, VK, swapchainSyncs[i].acquireSemaphore);
						}


						// Present Semaphore
						const VkResult vrPresSemaphoreCreated = vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &swapchainSyncs[i].presentSemaphore);
						if (vrPresSemaphoreCreated != VK_SUCCESS)
						{
							SA_LOG((L"Create Swapchain Present Semaphore [%1] failed!", i), Error, VK, (L"Error Code: %1", vrPresSemaphoreCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG((L"Create Swapchain Present Semaphore [%1] success", i), Info, VK, swapchainSyncs[i].presentSemaphore);
						}


						// Fence
						const VkResult vrFenceCreated = vkCreateFence(device, &fenceCreateInfo, nullptr, &swapchainSyncs[i].fence);
						if (vrFenceCreated != VK_SUCCESS)
						{
							SA_LOG((L"Create Swapchain Fence [%1] failed!", i), Error, VK, (L"Error Code: %1", vrFenceCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG((L"Create Swapchain Fence [%1] success", i), Info, VK, swapchainSyncs[i].fence);
						}
					}
				}
			}


			// Commands
			{
				// Pool
				{
					const VkCommandPoolCreateInfo createInfo{
						.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
						.pNext = nullptr,
						.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
						.queueFamilyIndex = deviceQueueFamilyIndices.graphicsFamily,
					};

					const VkResult vrCmdPoolCreated = vkCreateCommandPool(device, &createInfo, nullptr, &cmdPool);
					if (vrCmdPoolCreated != VK_SUCCESS)
					{
						SA_LOG(L"Create Command Pool failed!", Error, VK, (L"Error Code: %1", vrCmdPoolCreated));
						return EXIT_FAILURE;
					}
					else
					{
						SA_LOG(L"Create Command Pool success.", Info, VK, cmdPool);
					}
				}


				// CmdBuffers
				{
					VkCommandBufferAllocateInfo allocInfo{
						.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
						.pNext = nullptr,
						.commandPool = cmdPool,
						.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
						.commandBufferCount = bufferingCount,
					};

					const VkResult vrAllocCmdBuffers = vkAllocateCommandBuffers(device, &allocInfo, cmdBuffers.data());
					if (vrAllocCmdBuffers != VK_SUCCESS)
					{
						SA_LOG(L"Allocate Command buffers failed!", Error, VK, (L"Error Code: %1", vrAllocCmdBuffers));
						return EXIT_FAILURE;
					}
					else
					{
						for (uint32_t i = 0; i < bufferingCount; ++i)
						{
							SA_LOG((L"Allocate Command buffer [%1] success.", i), Info, VK, (L"Error Code: %1", cmdBuffers[i]));
						}
					}
				}
			}


			// Scene Resources
			{
				// Depth Texture
				{
					// Image
					{
						const VkImageCreateInfo imageCreateInfo{
							.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0,
							.imageType = VK_IMAGE_TYPE_2D,
							.format = sceneDepthFormat,
							.extent = VkExtent3D{ windowSize.x, windowSize.y, 1u },
							.mipLevels = 1,
							.arrayLayers = 1,
							.samples = VK_SAMPLE_COUNT_1_BIT,
							.tiling = VK_IMAGE_TILING_OPTIMAL,
							.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
							.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
							.queueFamilyIndexCount = 0,
							.pQueueFamilyIndices = nullptr,
							.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
						};

						const VkResult vrImageCreated = vkCreateImage(device, &imageCreateInfo, nullptr, &sceneDepthImage);
						if (vrImageCreated != VK_SUCCESS)
						{
							SA_LOG(L"Create Scene Depth Image failed!", Error, Vk, (L"Error Code: %1", vrImageCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Scene Depth Image success.", Info, VK, sceneDepthImage);
						}
					}


					// Image Memory
					{
						VkMemoryRequirements memRequirements;
						vkGetImageMemoryRequirements(device, sceneDepthImage, &memRequirements);

						const VkMemoryAllocateInfo allocInfo{
							.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
							.pNext = nullptr,
							.allocationSize = memRequirements.size,
							.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
						};

						const VkResult vrImgAlloc = vkAllocateMemory(device, &allocInfo, nullptr, &sceneDepthImageMemory);
						if (vrImgAlloc != VK_SUCCESS)
						{
							SA_LOG(L"Create Scene Depth Image Memory failed!", Error, Vk, (L"Error Code: %1", vrImgAlloc));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Scene Depth Image Memory success.", Info, VK, sceneDepthImageMemory);
						}


						const VkResult vrImgMemBind = vkBindImageMemory(device, sceneDepthImage, sceneDepthImageMemory, 0);
						if (vrImgMemBind != VK_SUCCESS)
						{
							SA_LOG(L"Bind Scene Depth Image Memory failed!", Error, Vk, (L"Error Code: %1", vrImgMemBind));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Bind Scene Depth Image Memory success.", Info, VK);
						}
					}


					// Image View
					{
						const VkImageViewCreateInfo viewInfo{
							.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0,
							.image = sceneDepthImage,
							.viewType = VK_IMAGE_VIEW_TYPE_2D,
							.format = sceneDepthFormat,
							.components{
								.r = VK_COMPONENT_SWIZZLE_IDENTITY,
								.g = VK_COMPONENT_SWIZZLE_IDENTITY,
								.b = VK_COMPONENT_SWIZZLE_IDENTITY,
								.a = VK_COMPONENT_SWIZZLE_IDENTITY
							},
							.subresourceRange{
								.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
								.baseMipLevel = 0,
								.levelCount = 1,
								.baseArrayLayer = 0,
								.layerCount = 1,
							}
						};

						const VkResult vrImgViewCreated = vkCreateImageView(device, &viewInfo, nullptr, &sceneDepthImageView);
						if (vrImgViewCreated != VK_SUCCESS)
						{
							SA_LOG(L"Create Scene Depth Image View failed!", Error, Vk, (L"Error Code: %1", vrImgViewCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Scene Depth Image View success.", Info, VK, sceneDepthImageView);
						}
					}
				}
			}

			// Render Pass
			{
				const std::array<VkAttachmentDescription, 2> attachments{
					VkAttachmentDescription{
						.flags = 0,
						.format = sceneColorFormat,
						.samples = VK_SAMPLE_COUNT_1_BIT,
						.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
						.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
						.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					},
					VkAttachmentDescription{
						.flags = 0,
						.format = sceneDepthFormat,
						.samples = VK_SAMPLE_COUNT_1_BIT,
						.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
						.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
						.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
						.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
					},
				};

				const VkAttachmentReference colorAttachmentRef{
					.attachment = 0,
					.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				};

				const VkAttachmentReference depthAttachmentRef{
					.attachment = 1,
					.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				};

				const VkSubpassDescription subpass{
					.flags = 0,
					.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
					.inputAttachmentCount = 0,
					.pInputAttachments = nullptr,
					.colorAttachmentCount = 1,
					.pColorAttachments = &colorAttachmentRef,
					.pResolveAttachments = nullptr,
					.pDepthStencilAttachment = &depthAttachmentRef,
					.preserveAttachmentCount = 0,
					.pPreserveAttachments = nullptr,
				};

				const VkSubpassDependency dependency{
					.srcSubpass = VK_SUBPASS_EXTERNAL,
					.dstSubpass = 0,
					.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
					.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
					.srcAccessMask = 0,
					.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					.dependencyFlags = 0u,
				};

				const VkRenderPassCreateInfo renderPassInfo{
					.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
					.pNext = nullptr,
					.flags = 0,
					.attachmentCount = static_cast<uint32_t>(attachments.size()),
					.pAttachments = attachments.data(),
					.subpassCount = 1,
					.pSubpasses = &subpass,
					.dependencyCount = 1,
					.pDependencies = &dependency,
				};

				const VkResult vrRenderPassCreated = vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
				if (vrRenderPassCreated != VK_SUCCESS)
				{
					SA_LOG(L"Create RenderPass failed!", Error, VK, (L"Error Code: %1", vrRenderPassCreated));
					return EXIT_FAILURE;
				}
				else
				{
					SA_LOG(L"Create RenderPass success", Info, VK, renderPass);
				}
			}


			// Framebuffers
			{
				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					std::array<VkImageView, 2u> attachments{
						swapchainImageViews[i],
						sceneDepthImageView
					};

					const VkFramebufferCreateInfo framebufferInfo{
						.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
						.pNext = nullptr,
						.flags = 0,
						.renderPass = renderPass,
						.attachmentCount = static_cast<uint32_t>(attachments.size()),
						.pAttachments = attachments.data(),
						.width = windowSize.x,
						.height = windowSize.y,
						.layers = 1,
					};

					const VkResult vrFrameBuffCreated = vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]);
					if (vrFrameBuffCreated != VK_SUCCESS)
					{
						SA_LOG((L"Create FrameBuffer [%1] failed!", i), Error, VK, (L"Error Code: %1", vrFrameBuffCreated));
						return EXIT_FAILURE;
					}
					else
					{
						SA_LOG((L"Create FrameBuffer [%1] success", i), Info, VK, framebuffers[i]);
					}
				}
			}


			// DescriptorSet
			{
				// Lit
				{
					std::array<VkDescriptorSetLayoutBinding, 7> bindings{
						VkDescriptorSetLayoutBinding{ // Camera buffer
							.binding = 0,
							.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // Object buffer
							.binding = 1,
							.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // PBR Albedo
							.binding = 2,
							.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // PBR NormalMap
							.binding = 3,
							.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // PBR MetallicMap
							.binding = 4,
							.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // PBR RoughnessMap
							.binding = 5,
							.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
							.pImmutableSamplers = nullptr,
						},
						VkDescriptorSetLayoutBinding{ // PointLights buffer
							.binding = 6,
							.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
							.descriptorCount = 1,
							.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
							.pImmutableSamplers = nullptr,
						},
					};

					const VkDescriptorSetLayoutCreateInfo layoutInfo{
						.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
						.pNext = nullptr,
						.flags = 0u,
						.bindingCount = static_cast<uint32_t>(bindings.size()),
						.pBindings = bindings.data(),
					};

					const VkResult vrDescLayoutCreated = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &litDescSetLayout);
					if (vrDescLayoutCreated != VK_SUCCESS)
					{
						SA_LOG(L"Create Lit DescriptorSet Layout failed!", Error, VK, (L"Error Code: %1", vrDescLayoutCreated));
						return EXIT_FAILURE;
					}
					else
					{
						SA_LOG(L"Create Lit DescriptorSet Layout success.", Info, VK, litDescSetLayout);
					}
				}
			}


			// Pipeline
			{
				// Viewport & Scissor
				{
					viewport = VkViewport{
						.x = 0,
						.y = 0,
						.width = static_cast<float>(windowSize.x),
						.height = static_cast<float>(windowSize.y),
						.minDepth = 0.0f,
						.maxDepth = 1.0f,
					};

					scissorRect = VkRect2D{
						.offset = VkOffset2D{ 0, 0 },
						.extent = VkExtent2D{ windowSize.x, windowSize.y },
					};
				}


				// Lit
				{
					// Pipeline Layout
					{
						const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0,
							.setLayoutCount = 1u,
							.pSetLayouts = &litDescSetLayout,
							.pushConstantRangeCount = 0u,
							.pPushConstantRanges = nullptr,
						};

						const VkResult vrPipLayoutCreated = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &litPipelineLayout);
						if (vrPipLayoutCreated != VK_SUCCESS)
						{
							SA_LOG(L"Create Lit Pipeline Layout failed!", Error, VK, (L"Error Code: %1", vrPipLayoutCreated));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit Pipeline Layout success", Info, VK, litPipelineLayout);
						}
					}


					// Vertex Shader
					{
						std::vector<uint32_t> shCode;

						CompileShaderFromFile("Resources/Shaders/GLSL/LitShader.vert", shaderc_vertex_shader, shCode);

						const VkShaderModuleCreateInfo createInfo{
							.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.codeSize = static_cast<uint32_t>(shCode.size()) * sizeof(uint32_t),
							.pCode = shCode.data(),
						};

						const VkResult vrShaderCompile = vkCreateShaderModule(device, &createInfo, nullptr, &litVertexShader);
						if (vrShaderCompile != VK_SUCCESS)
						{
							SA_LOG(L"Create Lit Vertex Shader failed!", Info, VK, (L"Error code: %1", vrShaderCompile));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit Vertex Shader success", Info, VK, litVertexShader);
						}
					}

					// Fragment Shader
					{
						std::vector<uint32_t> shCode;

						CompileShaderFromFile("Resources/Shaders/GLSL/LitShader.frag", shaderc_fragment_shader, shCode);

						const VkShaderModuleCreateInfo createInfo{
							.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.codeSize = static_cast<uint32_t>(shCode.size()) * sizeof(uint32_t),
							.pCode = shCode.data(),
						};

						const VkResult vrShaderCompile = vkCreateShaderModule(device, &createInfo, nullptr, &litFragmentShader);
						if (vrShaderCompile != VK_SUCCESS)
						{
							SA_LOG(L"Create Lit Fragment Shader failed!", Info, VK, (L"Error code: %1", vrShaderCompile));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit Fragment Shader success", Info, VK, litFragmentShader);
						}
					}


					// Pipeline
					{
						const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{
							VkPipelineShaderStageCreateInfo{
								.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
								.pNext = nullptr,
								.flags = 0u,
								.stage = VK_SHADER_STAGE_VERTEX_BIT,
								.module = litVertexShader,
								.pName = "main",
								.pSpecializationInfo = nullptr,
							},
							VkPipelineShaderStageCreateInfo{
								.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
								.pNext = nullptr,
								.flags = 0u,
								.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
								.module = litFragmentShader,
								.pName = "main",
								.pSpecializationInfo = nullptr,
							},
						};

						const std::array<VkVertexInputBindingDescription, 4> vertexInputBindings{
							VkVertexInputBindingDescription{ // Position buffer
								.binding = 0,
								.stride = sizeof(SA::Vec3f),
								.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
							},
							VkVertexInputBindingDescription{ // Normal buffer
								.binding = 1,
								.stride = sizeof(SA::Vec3f),
								.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
							},
							VkVertexInputBindingDescription{ // Tangent buffer
								.binding = 2,
								.stride = sizeof(SA::Vec3f),
								.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
							},
							VkVertexInputBindingDescription{ // UV buffer
								.binding = 3,
								.stride = sizeof(SA::Vec2f),
								.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
							},
						};

						const std::array<VkVertexInputAttributeDescription, 4> vertexInputAttribs{
							VkVertexInputAttributeDescription{ // Position Input
								.location = 0u,
								.binding = 0u,
								.format = VK_FORMAT_R32G32B32_SFLOAT,
								.offset = 0u,
							},
							VkVertexInputAttributeDescription{ // Normal Input
								.location = 1u,
								.binding = 1u,
								.format = VK_FORMAT_R32G32B32_SFLOAT,
								.offset = 0u,
							},
							VkVertexInputAttributeDescription{ // Tangent Input
								.location = 2u,
								.binding = 2u,
								.format = VK_FORMAT_R32G32B32_SFLOAT,
								.offset = 0u,
							},
							VkVertexInputAttributeDescription{ // UV Input
								.location = 3u,
								.binding = 3u,
								.format = VK_FORMAT_R32G32_SFLOAT,
								.offset = 0u,
							},
						};

						const VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
							.primitiveRestartEnable = VK_FALSE,
						};

						const VkPipelineVertexInputStateCreateInfo vertexInputInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size()),
							.pVertexBindingDescriptions = vertexInputBindings.data(),
							.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttribs.size()),
							.pVertexAttributeDescriptions = vertexInputAttribs.data(),
						};

						const VkPipelineViewportStateCreateInfo viewportInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.viewportCount = 1,
							.pViewports = &viewport,
							.scissorCount = 1,
							.pScissors = &scissorRect,
						};

						const VkPipelineRasterizationStateCreateInfo rasterInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.depthClampEnable = VK_FALSE,
							.rasterizerDiscardEnable = VK_FALSE,
							.polygonMode = VK_POLYGON_MODE_FILL,
							.cullMode = VK_CULL_MODE_BACK_BIT,
							.frontFace = VK_FRONT_FACE_CLOCKWISE,
							.depthBiasEnable = VK_FALSE,
							.depthBiasConstantFactor = 0.0f,
							.depthBiasClamp = 0.0f,
							.depthBiasSlopeFactor = 0.0f,
							.lineWidth = 1.0f,
						};

						const VkPipelineMultisampleStateCreateInfo multisampleInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
							.sampleShadingEnable = VK_FALSE,
							.minSampleShading = 1.0f,
							.pSampleMask = nullptr,
							.alphaToCoverageEnable = VK_FALSE,
							.alphaToOneEnable = VK_FALSE,
						};

						const VkPipelineDepthStencilStateCreateInfo depthStencilState{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.depthTestEnable = VK_TRUE,
							.depthWriteEnable = VK_TRUE,
							.depthCompareOp = VK_COMPARE_OP_LESS,
							.depthBoundsTestEnable = VK_FALSE,
							.stencilTestEnable = VK_FALSE,
							.front = {},
							.back = {},
							.minDepthBounds = 0.0f,
							.maxDepthBounds = 1.0f,
						};

						const std::array<VkPipelineColorBlendAttachmentState, 1> colorBlendAttachs{
							VkPipelineColorBlendAttachmentState{
								.blendEnable = VK_FALSE,
								.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
								.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
								.colorBlendOp = VK_BLEND_OP_ADD,
								.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
								.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
								.alphaBlendOp = VK_BLEND_OP_ADD,
								.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
													VK_COLOR_COMPONENT_G_BIT |
													VK_COLOR_COMPONENT_B_BIT |
													VK_COLOR_COMPONENT_A_BIT,
							},
						};

						const VkPipelineColorBlendStateCreateInfo colorBlendState{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.logicOpEnable = VK_FALSE,
							.logicOp = VK_LOGIC_OP_COPY,
							.attachmentCount = static_cast<uint32_t>(colorBlendAttachs.size()),
							.pAttachments = colorBlendAttachs.data(),
							.blendConstants = { 0.0f },
						};

						const std::array<VkDynamicState, 2> dynamicStates{
							VK_DYNAMIC_STATE_VIEWPORT,
							VK_DYNAMIC_STATE_SCISSOR
						};

						const VkPipelineDynamicStateCreateInfo dynamicStateInfo{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
							.pDynamicStates = dynamicStates.data(),
						};


						const VkGraphicsPipelineCreateInfo pipelineInfo{
							.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
							.pNext = nullptr,
							.flags = 0u,
							.stageCount = static_cast<uint32_t>(shaderStages.size()),
							.pStages = shaderStages.data(),
							.pVertexInputState = &vertexInputInfo,
							.pInputAssemblyState = &inputAssemblyState,
							.pTessellationState = nullptr,
							.pViewportState = &viewportInfo,
							.pRasterizationState = &rasterInfo,
							.pMultisampleState = &multisampleInfo,
							.pDepthStencilState = &depthStencilState,
							.pColorBlendState = &colorBlendState,
							.pDynamicState = &dynamicStateInfo,
							.layout = litPipelineLayout,
							.renderPass = renderPass,
							.subpass = 0u,
							.basePipelineHandle = VK_NULL_HANDLE,
							.basePipelineIndex = -1,
						};

						const VkResult vrCreatePipeline = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &litPipeline);
						if (vrCreatePipeline != VK_SUCCESS)
						{
							SA_LOG(L"Create Lit Pipeline failed!", Error, VK, (L"Error Code: %1", vrCreatePipeline));
							return EXIT_FAILURE;
						}
						else
						{
							SA_LOG(L"Create Lit Pipeline success", Info, VK, litPipeline);
						}
					}
				}
			}
		}
	}



	// Loop
	{
	}



	// Uninitialization
	{
		// Renderer
		{
			// Pipeline
			{
				// Lit
				{
					// Pipeline
					{
						vkDestroyPipeline(device, litPipeline, nullptr);
						SA_LOG(L"Destroy Lit Pipeline success.", Info, VK, litPipeline);
						litPipeline = VK_NULL_HANDLE;
					}

					// Fragment Shader
					{
						vkDestroyShaderModule(device, litFragmentShader, nullptr);
						SA_LOG(L"Destroy Lit Fragment Shader success.", Info, VK, litFragmentShader);
						litFragmentShader = VK_NULL_HANDLE;
					}

					// Vertex Shader
					{
						vkDestroyShaderModule(device, litVertexShader, nullptr);
						SA_LOG(L"Destroy Lit Vertex Shader success.", Info, VK, litVertexShader);
						litVertexShader = VK_NULL_HANDLE;
					}

					// Pipeline Layout
					{
						vkDestroyPipelineLayout(device, litPipelineLayout, nullptr);
						SA_LOG(L"Destroy Lit PipelineLayout success.", Info, VK, litPipelineLayout);
						litPipelineLayout = VK_NULL_HANDLE;
					}
				}
			}


			// DescriptorSet
			{
				// Lit
				{
					vkDestroyDescriptorSetLayout(device, litDescSetLayout, nullptr);
					SA_LOG(L"Destroy Lit DescriptorSetLayout success.", Info, VK, litDescSetLayout);
					litDescSetLayout = VK_NULL_HANDLE;
				}
			}


			// Framebuffers
			{
				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					vkDestroyFramebuffer(device, framebuffers[i], nullptr);
					SA_LOG((L"Destroy FrameBuffer [%1] success.", i), Info, VK, framebuffers[i]);
					framebuffers[i] = VK_NULL_HANDLE;
				}
			}


			// RenderPass
			{
				vkDestroyRenderPass(device, renderPass, nullptr);
				SA_LOG(L"Destroy RenderPass success.", Info, VK, renderPass);
				renderPass = VK_NULL_HANDLE;
			}


			// Scene Resources
			{
				// Depth Texture
				{
					// Image View
					{
						vkDestroyImageView(device, sceneDepthImageView, nullptr);
						SA_LOG(L"Destroy Scene Depth ImageView success", Info, VK, sceneDepthImageView);
						sceneDepthImageView = VK_NULL_HANDLE;
					}

					// Image Memory
					{
						vkFreeMemory(device, sceneDepthImageMemory, nullptr);
						SA_LOG(L"Free Scene Depth Image Memory success", Info, VK, sceneDepthImageMemory);
						sceneDepthImageMemory = VK_NULL_HANDLE;
					}

					// Image
					{
						vkDestroyImage(device, sceneDepthImage, nullptr);
						SA_LOG(L"Destroy Scene Depth Image success", Info, VK, sceneDepthImage);
						sceneDepthImage = VK_NULL_HANDLE;
					}
				}
			}


			// Commands
			{
				// CmdBuffers
				{
					/**
					* Can be skipped: will be automatically be freed when destroying VkCommandPool.
					* Kept for logging purpose.
					*/

					vkFreeCommandBuffers(device, cmdPool, bufferingCount, cmdBuffers.data());
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						SA_LOG((L"Free Command buffer [%1] success.", i), Info, VK, cmdBuffers[i]);
						cmdBuffers[i] = VK_NULL_HANDLE;
					}
				}

				// Pool
				{
					vkDestroyCommandPool(device, cmdPool, nullptr);
					SA_LOG(L"Destroy Command Pool [%1] success.", Info, VK, cmdPool);
					cmdPool = VK_NULL_HANDLE;
				}
			}


			// Swapchain
			{
				// Synchronization
				{
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						vkDestroySemaphore(device, swapchainSyncs[i].acquireSemaphore, nullptr);
						SA_LOG((L"Destroy Swapchain Acquire Semaphore [%1] success", i), Info, VK, swapchainSyncs[i].acquireSemaphore);
						swapchainSyncs[i].acquireSemaphore = VK_NULL_HANDLE;

						vkDestroySemaphore(device, swapchainSyncs[i].presentSemaphore, nullptr);
						SA_LOG((L"Destroy Swapchain Present Semaphore [%1] success", i), Info, VK, swapchainSyncs[i].presentSemaphore);
						swapchainSyncs[i].presentSemaphore = VK_NULL_HANDLE;

						vkDestroyFence(device, swapchainSyncs[i].fence, nullptr);
						SA_LOG((L"Destroy Swapchain Fence [%1] success", i), Info, VK, swapchainSyncs[i].fence);
						swapchainSyncs[i].fence = VK_NULL_HANDLE;
					}
				}


				// ImageViews
				{
					for (uint32_t i = 0; i < bufferingCount; ++i)
					{
						vkDestroyImageView(device, swapchainImageViews[i], nullptr);
						SA_LOG(L"Destroy Swapchain ImageView success", Info, VK, swapchainImageViews[i]);
						swapchainImageViews[i] = VK_NULL_HANDLE;
					}
				}


				// Backbuffers
				for (uint32_t i = 0; i < bufferingCount; ++i)
				{
					// Do not destroy swapchain images manually, they are already attached to VkSwapchain lifetime.

					SA_LOG((L"Destroy Swapchain backbuffer image [%1] success", i), Info, VK, swapchainImages[i]);
					swapchainImages[i] = VK_NULL_HANDLE;
				}

				vkDestroySwapchainKHR(device, swapchain, nullptr);
				SA_LOG(L"Destroy Swapchain success", Info, VK, swapchain);
				swapchain = VK_NULL_HANDLE;
			}


			// Device
			{
				SA_LOG(L"Destroy Graphics Queue success", Info, VK, graphicsQueue);
				graphicsQueue = VK_NULL_HANDLE;

				//SA_LOG(L"Destroy Compute Queue success", Info, VK, computeQueue);
				//computeQueue = VK_NULL_HANDLE;

				SA_LOG(L"Destroy Present Queue success", Info, VK, presentQueue);
				presentQueue = VK_NULL_HANDLE;

				vkDestroyDevice(device, nullptr);
				SA_LOG(L"Destroy Logical Device success", Info, VK, device);
				device = VK_NULL_HANDLE;

				SA_LOG(L"Destroy Physical Device success", Info, VK, physicalDevice);
				physicalDevice = VK_NULL_HANDLE;
			}


			// Surface
			{
				vkDestroySurfaceKHR(instance, windowSurface, nullptr);
				SA_LOG(L"Destroy Window Surface success", Info, VK, windowSurface);
				windowSurface = VK_NULL_HANDLE;
			}


			// Instance
			{
				vkDestroyInstance(instance, nullptr);
				SA_LOG(L"Destroy Instance success", Info, VK, instance);
				instance = VK_NULL_HANDLE;
			}
		}


		// GLFW
		{
			glfwDestroyWindow(window);
			glfwTerminate();
		}
	}

	return 0;
}
