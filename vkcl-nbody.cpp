/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <http://unlicense.org/>
 */

// TODO: create a thread and book-keeping for each device.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <array>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <set>

#include "volk.h"

#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#include "vk_mem_alloc.h"

// glslangValidator --target-env vulkan1.0 --vn particle_attraction_code -V particle_attraction.comp -o particle_attraction.inc
#include "particle_attraction.inc"

union vec4 {
	float data[4];

	struct {
		float x, y, z, w;
	} components;
};

struct StdinMailbox {
	std::atomic<bool> input_ready;
	std::string line;
	std::mutex mtx;

	StdinMailbox();
	bool get_input(std::string &line);
};

struct Particle {
	vec4 position;
	vec4 velocity;
};

struct UBO {
	alignas(16) float delta_time;
	alignas(16) std::uint32_t particle_count;
};

StdinMailbox::StdinMailbox() {
	const auto thread_func = [](StdinMailbox *mailbox) {
		while (true) {
			std::string line;
			std::getline(std::cin, line);

			while (mailbox->input_ready);

			mailbox->mtx.lock();
			mailbox->line = std::move(line);
			mailbox->mtx.unlock();

			mailbox->input_ready = true;
		}
	};

	this->input_ready = false;
	std::thread(thread_func, this).detach();
}

bool StdinMailbox::get_input(std::string &o_line) {
	if (!this->input_ready)
		return false;

	this->mtx.lock();
	o_line = std::move(this->line);
	this->mtx.unlock();

	this->input_ready = false;
	return true;
}

static VkBool32 vulkan_debug_utils_messenger(VkDebugUtilsMessageSeverityFlagBitsEXT msg_severity, VkDebugUtilsMessageTypeFlagsEXT msg_type, const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data) {
	(void)msg_severity;
	(void)msg_type;
	(void)user_data;

	std::printf("VulkanAPI: %s\n", callback_data->pMessage);

	return VK_FALSE;
}

#if 0
// return the index of the device that the user chose if there are multiple. otherwise select the first device
static int select_device_prompt(const std::vector<VkPhysicalDevice> &physical_devs) {
	const auto print_physical_devices = [](const std::vector<std::string> &names, const std::vector<bool> &on_pci, const std::vector<VkPhysicalDevicePCIBusInfoPropertiesEXT> &pci_bus_infos, const std::vector<VkPhysicalDeviceProperties> &props) {
		std::printf("Select VkPhysicalDevice (0 to %zu):\n", names.size() - 1);

		for (std::size_t i = 0; i < names.size(); i++) {
			if (on_pci[i])
				std::printf("%zu: (PCI: %04x:%02x:%02x.%x, %04x:%04x) %s\n", i, pci_bus_infos[i].pciDomain, pci_bus_infos[i].pciBus, pci_bus_infos[i].pciDevice, pci_bus_infos[i].pciFunction, props[i].vendorID, props[i].deviceID, names[i].c_str());
			else
				std::printf("%zu: (UnknownBus, %04x:%04x) %s\n", i, props[i].vendorID, props[i].deviceID, names[i].c_str());
		}
	};

	const std::size_t num_physical_devs = physical_devs.size();

	std::vector<bool> on_pci(num_physical_devs, false);
	std::vector<VkPhysicalDevicePCIBusInfoPropertiesEXT> pci_bus_infos(num_physical_devs);
	std::vector<std::string> physical_dev_names(num_physical_devs);
	std::vector<VkPhysicalDeviceProperties> vec_props(num_physical_devs);

	for (std::size_t i = 0; i < num_physical_devs; i++) {
		std::uint32_t count;
		vkEnumerateDeviceExtensionProperties(physical_devs[i], nullptr, &count, nullptr);

		std::vector<VkExtensionProperties> extensions(count);
		vkEnumerateDeviceExtensionProperties(physical_devs[i], nullptr, &count, extensions.data());

		for (const auto &extension : extensions) {
			if (std::strcmp(extension.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0)
				on_pci[i] = true;
		}

		VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus_info = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
			.pNext = nullptr,
			.pciDomain = std::numeric_limits<std::uint32_t>::max(),
			.pciBus = std::numeric_limits<std::uint32_t>::max(),
			.pciDevice = std::numeric_limits<std::uint32_t>::max(),
			.pciFunction = std::numeric_limits<std::uint32_t>::max()
		};

		VkPhysicalDeviceProperties2KHR props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
			.pNext = on_pci[i] ? &pci_bus_info : nullptr,
			.properties = {}
		};

		vkGetPhysicalDeviceProperties2KHR(physical_devs[i], &props);

		pci_bus_infos[i] = pci_bus_info;
		physical_dev_names[i] = props.properties.deviceName;
		vec_props[i] = props.properties;
	}

	if (num_physical_devs == 1) {
		if (on_pci[0])
			std::printf("Only one physical device present: (PCI: %04x:%02x:%02x:%x, %04x:%04x) %s\n", pci_bus_infos[0].pciDomain, pci_bus_infos[0].pciBus, pci_bus_infos[0].pciDevice, pci_bus_infos[0].pciFunction, vec_props[0].vendorID, vec_props[0].deviceID, physical_dev_names[0].c_str());
		else
			std::printf("Only one physical device present: (UnknownBus, %04x:%04x) %s\n", vec_props[0].vendorID, vec_props[0].deviceID, physical_dev_names[0].c_str());

		return 0;
	}

	int idx = -1;
	while (idx < 0 || idx >= num_physical_devs) {
		print_physical_devices(physical_dev_names, on_pci, pci_bus_infos, vec_props);
		std::cin >> idx;
		std::printf("\n");
	}

	return idx;
}
#endif

static void create_vkinstance(VkInstance &inst, VkDebugUtilsMessengerEXT &debug_msgr, const bool debug_mode) {
	static const std::array<const char *, 2> supp_layer_names = {
		"VK_LAYER_KHRONOS_validation",
		"VK_LAYER_KHRONOS_synchronization2"
	};

	static const std::array<VkValidationFeatureEnableEXT, 4> valid_enable = {
		VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
		VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
		VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
	};

	static const VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pNext = nullptr,
		.pApplicationName = "Voka",
		.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
		.pEngineName = "VokaNN",
		.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
		.apiVersion = VK_API_VERSION_1_0
	};

	static const VkDebugUtilsMessengerCreateInfoEXT dbg_msgr_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.pNext = nullptr,
		.flags = 0,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = vulkan_debug_utils_messenger,
		.pUserData = nullptr
	};

	static const VkValidationFeaturesEXT valid_features = {
		.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
		.pNext = &dbg_msgr_create_info,
		.enabledValidationFeatureCount = static_cast<std::uint32_t>(valid_enable.size()),
		.pEnabledValidationFeatures = valid_enable.data(),
		.disabledValidationFeatureCount = 0,
		.pDisabledValidationFeatures = nullptr
	};

	std::vector<VkLayerProperties> avail_layer_props;
	std::uint32_t vk_obj_count;

	std::set<const char *> layers_support, exts_support = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, "VK_KHR_portability_enumeration" };
	std::vector<const char *> layers_enable, exts_enable;

	if (volkInitialize() != VK_SUCCESS)
		throw std::runtime_error("Cannot init volk!");

	vkEnumerateInstanceLayerProperties(&vk_obj_count, nullptr);
	avail_layer_props.resize(vk_obj_count);
	vkEnumerateInstanceLayerProperties(&vk_obj_count, avail_layer_props.data());

	for (const auto& avail_layer : avail_layer_props) {
		const std::string_view avail_layer_name(avail_layer.layerName);

		for (const auto& supp_layer_name : supp_layer_names) {
			if (avail_layer_name == supp_layer_name)
				layers_support.insert(supp_layer_name);
		}
	}

	bool supp_debug_mode = false, search_valid_layer = false;

	for (const auto &layer_ptr : layers_support) {
		const std::string_view layer(layer_ptr);
		const auto is_validation_layer = search_valid_layer = layer == "VK_LAYER_KHRONOS_validation";

		if (is_validation_layer && debug_mode) {
			exts_support.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			exts_support.insert(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
			std::printf("!! Vulkan Validation Layers Enabled\n");
			supp_debug_mode = true;
			break;
		} else if (is_validation_layer && !debug_mode) {
			layers_support.erase("VK_LAYER_KHRONOS_validation");
			break;
		}
	}

	if (!search_valid_layer && debug_mode)
		std::printf("!! Vulkan Validation Layers requested, but not found\n");

	if (layers_support.size() > 0) {
		layers_enable.resize(layers_support.size());
		std::copy(layers_support.begin(), layers_support.end(), layers_enable.begin());
	}

	if (exts_support.size() > 0) {
		exts_enable.resize(exts_support.size());
		std::copy(exts_support.begin(), exts_support.end(), exts_enable.begin());
	}

	const VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = supp_debug_mode ? &valid_features : nullptr,
		.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
		.pApplicationInfo = &app_info,
		.enabledLayerCount = static_cast<std::uint32_t>(layers_enable.size()),
		.ppEnabledLayerNames = layers_enable.size() > 0 ? layers_enable.data() : nullptr,
		.enabledExtensionCount = static_cast<std::uint32_t>(exts_enable.size()),
		.ppEnabledExtensionNames = exts_enable.size() > 0 ? exts_enable.data() : nullptr
	};

	if (vkCreateInstance(&create_info, nullptr, &inst) != VK_SUCCESS)
		throw std::runtime_error("Failed to create VkInstance");

	volkLoadInstanceOnly(inst);

	if (supp_debug_mode) {
		if (vkCreateDebugUtilsMessengerEXT(inst, &dbg_msgr_create_info, nullptr, &debug_msgr) != VK_SUCCESS)
			throw std::runtime_error("Failed to create VkDebugUtilsMessengerEXT");
	}
}

static void get_physical_devs(VkInstance inst, std::vector<VkPhysicalDevice> &physical_devs) {
	std::uint32_t count;
	vkEnumeratePhysicalDevices(inst, &count, nullptr);

	if (count == 0)
		throw std::runtime_error("No VkPhysicalDevice found!");

	physical_devs.resize(count);
	vkEnumeratePhysicalDevices(inst, &count, physical_devs.data());
}

static void create_device(VkPhysicalDevice physical_dev, VkDevice &dev, std::uint32_t &compute_queue_family_idx, std::uint32_t &transfer_queue_family_idx, std::uint32_t &transfer_queue_idx) {
	static const float priority = 1.f;

	std::uint32_t count;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_dev, &count, nullptr);

	std::vector<VkQueueFamilyProperties> queue_families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_dev, &count, queue_families.data());

	compute_queue_family_idx = std::numeric_limits<std::uint32_t>::max();
	transfer_queue_family_idx = std::numeric_limits<std::uint32_t>::max();
	transfer_queue_idx = 0;

	for (std::size_t i = 0; i < queue_families.size(); i++) {
		if (compute_queue_family_idx == std::numeric_limits<std::uint32_t>::max() && (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
			compute_queue_family_idx = static_cast<std::uint32_t>(i);
			continue;
		}

		if (transfer_queue_family_idx == std::numeric_limits<std::uint32_t>::max() && (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
			transfer_queue_family_idx = static_cast<std::uint32_t>(i);
			continue;
		}
	}

	if (compute_queue_family_idx == std::numeric_limits<std::uint32_t>::max())
		throw std::runtime_error("No compute queue found!");

	if (transfer_queue_family_idx == std::numeric_limits<std::uint32_t>::max()) {
		std::printf("! No pure transfer queue family found, falling back to extra compute queue\n");
		transfer_queue_family_idx = compute_queue_family_idx;
		transfer_queue_idx = 1;
	}

	const std::array<VkDeviceQueueCreateInfo, 2> queue_create_infos = {
		VkDeviceQueueCreateInfo {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.queueFamilyIndex = compute_queue_family_idx,
			.queueCount = 1,
			.pQueuePriorities = &priority
		},
		VkDeviceQueueCreateInfo {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.queueFamilyIndex = transfer_queue_family_idx,
			.queueCount = 1,
			.pQueuePriorities = &priority
		}
	};

	static constinit std::array<const char *, 1> device_exts = {
		"VK_KHR_portability_subset"
	};

	const VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueCreateInfoCount = static_cast<std::uint32_t>(queue_create_infos.size()),
		.pQueueCreateInfos = queue_create_infos.data(),
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = nullptr,
		.enabledExtensionCount = static_cast<std::uint32_t>(device_exts.size()),
		.ppEnabledExtensionNames = device_exts.data(),
		.pEnabledFeatures = nullptr
	};

	if (vkCreateDevice(physical_dev, &create_info, nullptr, &dev) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkDevice!");
}

static void create_allocator(const VolkDeviceTable &funcs, VkInstance inst, VkPhysicalDevice physical_dev, VkDevice dev, VmaAllocator &allocator) {
	const VmaVulkanFunctions vulkan_funcs = {
		.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
		.vkGetDeviceProcAddr = vkGetDeviceProcAddr,
		.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
		.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
		.vkAllocateMemory = funcs.vkAllocateMemory,
		.vkFreeMemory = funcs.vkFreeMemory,
		.vkMapMemory = funcs.vkMapMemory,
		.vkUnmapMemory = funcs.vkUnmapMemory,
		.vkFlushMappedMemoryRanges = funcs.vkFlushMappedMemoryRanges,
		.vkInvalidateMappedMemoryRanges = funcs.vkInvalidateMappedMemoryRanges,
		.vkBindBufferMemory = funcs.vkBindBufferMemory,
		.vkBindImageMemory = funcs.vkBindImageMemory,
		.vkGetBufferMemoryRequirements = funcs.vkGetBufferMemoryRequirements,
		.vkGetImageMemoryRequirements = funcs.vkGetImageMemoryRequirements,
		.vkCreateBuffer = funcs.vkCreateBuffer,
		.vkDestroyBuffer = funcs.vkDestroyBuffer,
		.vkCreateImage = funcs.vkCreateImage,
		.vkDestroyImage = funcs.vkDestroyImage,
		.vkCmdCopyBuffer = funcs.vkCmdCopyBuffer,
#if VMA_DEDICATED_ALLOCATION || VMA_VULKAN_VERSION >= 1001000
		.vkGetBufferMemoryRequirements2KHR = funcs.vkGetBufferMemoryRequirements2KHR,
		.vkGetImageMemoryRequirements2KHR = funcs.vkGetImageMemoryRequirements2KHR,
#endif
#if VMA_BIND_MEMORY2 || VMA_VULKAN_VERSION >= 1001000
		.vkBindBufferMemory2KHR = funcs.vkBindBufferMemory2KHR,
		.vkBindImageMemory2KHR = funcs.vkBindImageMemory2,
#endif
#if VMA_MEMORY_BUDGET || VMA_VULKAN_VERSION >= 1001000
		.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2KHR,
#endif
#if VMA_VULKAN_VERSION >= 1003000
		.vkGetDeviceBufferMemoryRequirements = funcs.vkGetDeviceBufferMemoryRequirements,
		.vkGetDeviceImageMemoryRequirements = funcs.vkGetDeviceImageMemoryRequirements,
#endif
	};

	const VmaAllocatorCreateInfo allocator_create_info = {
		.flags = 0,
		.physicalDevice = physical_dev,
		.device = dev,
		.preferredLargeHeapBlockSize = 0,
		.pAllocationCallbacks = nullptr,
		.pDeviceMemoryCallbacks = nullptr,
		.pHeapSizeLimit = nullptr,
		.pVulkanFunctions = &vulkan_funcs,
		.instance = inst,
		.vulkanApiVersion = VK_API_VERSION_1_0,
		.pTypeExternalMemoryHandleTypes = nullptr
	};

	vmaCreateAllocator(&allocator_create_info, &allocator);
}

static void create_desc_and_pipeline_layout(const VolkDeviceTable &funcs, VkDevice dev, VkDescriptorSetLayout &desc_set_layout, VkPipelineLayout &pipeline_layout) {
	const std::array<VkDescriptorSetLayoutBinding, 2> desc_set_layout_bindings = {
		VkDescriptorSetLayoutBinding {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.pImmutableSamplers = nullptr
		},
		VkDescriptorSetLayoutBinding {
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.pImmutableSamplers = nullptr
		},
	};

	const VkDescriptorSetLayoutCreateInfo desc_set_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.bindingCount = static_cast<std::uint32_t>(desc_set_layout_bindings.size()),
		.pBindings = desc_set_layout_bindings.data()
	};

	if (funcs.vkCreateDescriptorSetLayout(dev, &desc_set_layout_create_info, nullptr, &desc_set_layout) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkDescriptorSetLayout!");

	const VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.setLayoutCount = 1,
		.pSetLayouts = &desc_set_layout,
		.pushConstantRangeCount = 0,
		.pPushConstantRanges = nullptr,
	};

	if (funcs.vkCreatePipelineLayout(dev, &pipeline_layout_create_info, nullptr, &pipeline_layout) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkPipelineLayout!");
}

static void create_compute_pipeline(const VolkDeviceTable &funcs, VkDevice dev, VkPipelineLayout pipeline_layout, const std::uint32_t *code, const std::size_t code_size, VkPipeline &pipeline) {
	const VkShaderModuleCreateInfo shader_module_create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.codeSize = code_size,
		.pCode = code
	};

	VkShaderModule shader_module;
	if (funcs.vkCreateShaderModule(dev, &shader_module_create_info, nullptr, &shader_module) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkShaderModule!");

	const VkPipelineShaderStageCreateInfo pipeline_shader_stage_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = shader_module,
		.pName = "main",
		.pSpecializationInfo = nullptr
	};

	const VkComputePipelineCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.stage = pipeline_shader_stage_create_info,
		.layout = pipeline_layout,
		.basePipelineHandle = VK_NULL_HANDLE,
		.basePipelineIndex = 0
	};

	if (funcs.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &create_info, nullptr, &pipeline) != VK_SUCCESS)
		throw std::runtime_error("Cannot create compute pipeline!");

	funcs.vkDestroyShaderModule(dev, shader_module, nullptr);
}

static void create_desc_pool_and_set(const VolkDeviceTable &funcs, VkDevice dev, VkDescriptorSetLayout desc_set_layout, VkDescriptorPool &desc_pool, VkDescriptorSet &desc_set) {
	const std::array<VkDescriptorPoolSize, 2> desc_pool_sizes = {
		VkDescriptorPoolSize { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
		VkDescriptorPoolSize { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 }
	};

	const VkDescriptorPoolCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.maxSets = 1,
		.poolSizeCount = static_cast<std::uint32_t>(desc_pool_sizes.size()),
		.pPoolSizes = desc_pool_sizes.data()
	};

	if (funcs.vkCreateDescriptorPool(dev, &create_info, nullptr, &desc_pool) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkDescriptorPool!");

	const VkDescriptorSetAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.pNext = nullptr,
		.descriptorPool = desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &desc_set_layout
	};

	if (funcs.vkAllocateDescriptorSets(dev, &alloc_info, &desc_set) != VK_SUCCESS)
		throw std::runtime_error("Cannot allocate VkDescriptorSet!");
}

static void create_dev_buf(VmaAllocator allocator, VkBuffer &buf, VmaAllocation &buf_alloc, const VkDeviceSize size) {
	const VmaAllocationCreateInfo alloc_create_info = {
		.flags = 0,
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		.requiredFlags = 0,
		.preferredFlags = 0,
		.memoryTypeBits = 0,
		.pool = nullptr,
		.pUserData = nullptr,
		.priority = 0.f,
	};

	const VkBufferCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.size = size,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr
	};

	if (vmaCreateBuffer(allocator, &create_info, &alloc_create_info, &buf, &buf_alloc, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Cannot allocate device buffer memory!");
}

template<typename T>
static void create_uniform_buf(VmaAllocator allocator, VkBuffer &buf, VmaAllocation &buf_alloc, T *&pbuf, const VkDeviceSize size) {
	const VmaAllocationCreateInfo alloc_create_info = {
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
		.requiredFlags = 0,
		.preferredFlags = 0,
		.memoryTypeBits = 0,
		.pool = nullptr,
		.pUserData = nullptr,
		.priority = 0.f,
	};

	const VkBufferCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.size = size,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr
	};

	VmaAllocationInfo info;
	if (vmaCreateBuffer(allocator, &create_info, &alloc_create_info, &buf, &buf_alloc, &info) != VK_SUCCESS)
		throw std::runtime_error("Cannot allocate host buffer memory!");

	pbuf = reinterpret_cast<T *>(info.pMappedData);
}

template<typename T>
static void create_host_buf(VmaAllocator allocator, VkBuffer &buf, VmaAllocation &buf_alloc, T *&pbuf, const VkDeviceSize size) {
	const VmaAllocationCreateInfo alloc_create_info = {
		.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
		.requiredFlags = 0,
		.preferredFlags = 0,
		.memoryTypeBits = 0,
		.pool = nullptr,
		.pUserData = nullptr,
		.priority = 0.f,
	};

	const VkBufferCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr
	};

	VmaAllocationInfo info;
	if (vmaCreateBuffer(allocator, &create_info, &alloc_create_info, &buf, &buf_alloc, &info) != VK_SUCCESS)
		throw std::runtime_error("Cannot allocate host buffer memory!");

	pbuf = reinterpret_cast<T *>(info.pMappedData);
}

static void create_cmd_pool(const VolkDeviceTable &funcs, VkDevice dev, const uint32_t queue_family_idx, VkCommandPool &cmd_pool) {
	const VkCommandPoolCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueFamilyIndex = queue_family_idx
	};

	if (funcs.vkCreateCommandPool(dev, &create_info, nullptr, &cmd_pool) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkCommandPool!");
}

template<std::size_t ArraySize>
static void create_cmd_bufs(const VolkDeviceTable &funcs, VkDevice dev, VkCommandPool cmd_pool, std::array<VkCommandBuffer, ArraySize> &cmd_bufs) {
	const VkCommandBufferAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = static_cast<std::uint32_t>(cmd_bufs.size())
	};

	if (funcs.vkAllocateCommandBuffers(dev, &alloc_info, cmd_bufs.data()) != VK_SUCCESS)
		throw std::runtime_error("Cannot allocate VkCommandBuffer!");
}

static void update_desc_set(const VolkDeviceTable &funcs, VkDevice dev, VkDescriptorSet desc_set, VkBuffer dev_buf, VkBuffer uniform_buf, const VkDeviceSize dev_buf_range, const VkDeviceSize uniform_buf_range) {
	const VkDescriptorBufferInfo storage_desc_buf_info = {
		.buffer = dev_buf,
		.offset = 0,
		.range = dev_buf_range
	};

	const VkDescriptorBufferInfo uniform_desc_buf_info = {
		.buffer = uniform_buf,
		.offset = 0,
		.range = uniform_buf_range
	};

	const std::array<VkWriteDescriptorSet, 2> writes = {
		VkWriteDescriptorSet {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = desc_set,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pImageInfo = nullptr,
			.pBufferInfo = &storage_desc_buf_info,
			.pTexelBufferView = nullptr
		},
		VkWriteDescriptorSet {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = desc_set,
			.dstBinding = 1,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pImageInfo = nullptr,
			.pBufferInfo = &uniform_desc_buf_info,
			.pTexelBufferView = nullptr
		}
	};

	funcs.vkUpdateDescriptorSets(dev, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

static void create_semaphore(const VolkDeviceTable &funcs, VkDevice dev, VkSemaphore &semaphore) {
	const VkSemaphoreCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0
	};

	if (funcs.vkCreateSemaphore(dev, &create_info, nullptr, &semaphore) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkSemaphore!");
}

static void create_fence(const VolkDeviceTable &funcs, VkDevice dev, VkFence &fence) {
	const VkFenceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};

	if (funcs.vkCreateFence(dev, &create_info, nullptr, &fence) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkSemaphore!");
}

static void record_cmd_buf_work(const VolkDeviceTable& funcs, VkCommandBuffer cmd_buf, VkPipeline particle_attraction, VkPipelineLayout pipeline_layout, VkDescriptorSet desc_set, VkBuffer dev_buf, const VkDeviceSize dev_buf_size, const std::uint32_t count, const std::uint32_t compute_queue_family_idx, const std::uint32_t transfer_queue_family_idx) {
	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = 0,
		.pInheritanceInfo = nullptr
	};

	const VkBufferMemoryBarrier host_to_dev_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = transfer_queue_family_idx,
		.dstQueueFamilyIndex = compute_queue_family_idx,
		.buffer = dev_buf,
		.offset = 0,
		.size = dev_buf_size
	};

	const VkBufferMemoryBarrier dev_to_host_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.srcQueueFamilyIndex = compute_queue_family_idx,
		.dstQueueFamilyIndex = transfer_queue_family_idx,
		.buffer = dev_buf,
		.offset = 0,
		.size = dev_buf_size
	};

	funcs.vkBeginCommandBuffer(cmd_buf, &begin_info);

	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &host_to_dev_buf_mem_barrier, 0, nullptr);

	funcs.vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, particle_attraction);
	funcs.vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &desc_set, 0, nullptr);
	funcs.vkCmdDispatch(cmd_buf, count, count, 1);

	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &dev_to_host_buf_mem_barrier, 0, nullptr);

	funcs.vkEndCommandBuffer(cmd_buf);
}

static void record_cmd_buf_copy_host_to_dev(const VolkDeviceTable &funcs, VkCommandBuffer cmd_buf, VkBuffer host_buf, VkBuffer dev_buf, const VkDeviceSize size, const std::uint32_t compute_queue_family_idx, const std::uint32_t transfer_queue_family_idx) {
	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = 0,
		.pInheritanceInfo = nullptr
	};

	const VkBufferCopy region = {
		.srcOffset = 0,
		.dstOffset = 0,
		.size = size
	};

	const VkBufferMemoryBarrier host_to_dev_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = transfer_queue_family_idx,
		.dstQueueFamilyIndex = compute_queue_family_idx,
		.buffer = dev_buf,
		.offset = 0,
		.size = size
	};

	funcs.vkBeginCommandBuffer(cmd_buf, &begin_info);
	funcs.vkCmdCopyBuffer(cmd_buf, host_buf, dev_buf, 1, &region);
	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &host_to_dev_buf_mem_barrier, 0, nullptr);
	funcs.vkEndCommandBuffer(cmd_buf);
}

static void record_cmd_buf_copy_dev_to_host(const VolkDeviceTable &funcs, VkCommandBuffer cmd_buf, VkBuffer host_buf, VkBuffer dev_buf, const VkDeviceSize size, const std::uint32_t compute_queue_family_idx, const std::uint32_t transfer_queue_family_idx) {
	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = 0,
		.pInheritanceInfo = nullptr
	};

	const VkBufferCopy region = {
		.srcOffset = 0,
		.dstOffset = 0,
		.size = size
	};

	const VkBufferMemoryBarrier dev_to_host_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.srcQueueFamilyIndex = compute_queue_family_idx,
		.dstQueueFamilyIndex = transfer_queue_family_idx,
		.buffer = dev_buf,
		.offset = 0,
		.size = size
	};

	const VkBufferMemoryBarrier host_to_dev_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = transfer_queue_family_idx,
		.dstQueueFamilyIndex = compute_queue_family_idx,
		.buffer = dev_buf,
		.offset = 0,
		.size = size
	};

	funcs.vkBeginCommandBuffer(cmd_buf, &begin_info);
	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &dev_to_host_buf_mem_barrier, 0, nullptr);
	funcs.vkCmdCopyBuffer(cmd_buf, dev_buf, host_buf, 1, &region);
	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &host_to_dev_buf_mem_barrier, 0, nullptr);
	funcs.vkEndCommandBuffer(cmd_buf);
}

static auto get_random_seed() {
	std::random_device source;

	std::array<std::uint64_t, 10> random_data;
	for (auto &elem : random_data)
		elem = source();

	// this creates the random seed sequence out of the random data
	return std::seed_seq(random_data.begin(), random_data.end());
}

int main(int argc, char *argv[]) {
	static const std::uint32_t particles_per_workgroup = 4096;
	static const std::size_t num_particles = particles_per_workgroup*8;

	static const VkDeviceSize storage_buf_size = sizeof(Particle)*num_particles;
	static const VkDeviceSize uniform_buf_size = sizeof(UBO);

	static const VkPipelineStageFlags wait_stage_transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
	static const VkPipelineStageFlags wait_stage_compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	static auto seed = get_random_seed();
	static std::default_random_engine rng(seed);
	std::uniform_real_distribution<float> dist(-1.f, 1.f);

	VkInstance inst;
	VkDebugUtilsMessengerEXT debug_msgr = VK_NULL_HANDLE;
	std::vector<VkPhysicalDevice> present_physical_devs, physical_devs;

	struct {
		bool debug_mode = false;
	} cli_options;

	for (int i = 1; i < argc; i++) {
		const std::string_view arg(argv[i]);

		if (arg == "-help") {
			std::printf(
				"usage: vkcl-nbody [-help] [-debug]\n"
				"-help: Display help information\n"
				"-debug: Enable Vulkan validation layers if found\n"
			);

			return 0;
		}
		else if (arg == "-debug") {
			cli_options.debug_mode = true;
		}
	}

	create_vkinstance(inst, debug_msgr, cli_options.debug_mode);
	get_physical_devs(inst, present_physical_devs);
	//physical_dev = physical_devs[select_device_prompt(physical_devs)];

	for (std::size_t i = 0; i < present_physical_devs.size(); i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(present_physical_devs[i], &props);
		std::printf("%x:%x\n", props.vendorID, props.deviceID);
		if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) // do not use CPUs
			physical_devs.push_back(present_physical_devs[i]);
	}

	std::vector<VkDevice> dev(physical_devs.size());
	std::vector<VolkDeviceTable> funcs(physical_devs.size());
	std::vector<VmaAllocator> allocator(physical_devs.size());

	std::vector<VkDescriptorSetLayout> desc_set_layout(physical_devs.size());
	std::vector<VkPipelineLayout> pipeline_layout(physical_devs.size());
	std::vector<VkPipeline> pipeline_attraction(physical_devs.size());

	std::vector<VkDescriptorPool> desc_pool(physical_devs.size());
	std::vector<VkDescriptorSet> desc_set(physical_devs.size());

	std::vector<VmaAllocation> dev_buf_alloc(physical_devs.size()), host_buf_alloc(physical_devs.size()), uniform_buf_alloc(physical_devs.size());
	std::vector<VkBuffer> dev_buf(physical_devs.size()), host_buf(physical_devs.size()), uniform_buf(physical_devs.size());
	std::vector<Particle *> particles(physical_devs.size()); // from host_buf memory
	std::vector<UBO *> ubo(physical_devs.size()); // from uniform_buf memory

	std::vector<VkCommandPool> compute_cmd_pool(physical_devs.size()), transfer_cmd_pool(physical_devs.size());
	std::vector<std::array<VkCommandBuffer, 1>> compute_cmd_bufs(physical_devs.size());

	// 0: HOST->DEV, 1: DEV->HOST
	std::vector<std::array<VkCommandBuffer, 2>> transfer_cmd_bufs(physical_devs.size());

	std::vector<VkFence> compute_fence(physical_devs.size()), dev_to_host_copy_fence(physical_devs.size());
	std::vector<VkSemaphore> copy_host_to_dev_semaphore(physical_devs.size()), copy_dev_to_host_semaphore(physical_devs.size()), compute_fin_semaphore(physical_devs.size());

	std::vector<std::uint32_t> compute_queue_family_idx(physical_devs.size()), transfer_queue_family_idx(physical_devs.size()), transfer_queue_idx(physical_devs.size());
	std::vector<VkQueue> compute_queue(physical_devs.size());
	std::vector<VkQueue> transfer_queue(physical_devs.size());

	std::vector<std::chrono::high_resolution_clock::time_point> start_time(physical_devs.size(), std::chrono::high_resolution_clock::now()), end_time(physical_devs.size());
	std::vector<float> duration(physical_devs.size(), 0.f), mean_sample(physical_devs.size(), 0.f);
	std::vector<int> num_samples(physical_devs.size(), 0);
	std::vector<bool> wait_for_copy(physical_devs.size(), true);

	StdinMailbox mailbox;
	std::string line;

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		create_device(physical_devs[i], dev[i], compute_queue_family_idx[i], transfer_queue_family_idx[i], transfer_queue_idx[i]);
		volkLoadDeviceTable(&funcs[i], dev[i]);
		funcs[i].vkGetDeviceQueue(dev[i], compute_queue_family_idx[i], 0, &compute_queue[i]);
		funcs[i].vkGetDeviceQueue(dev[i], transfer_queue_family_idx[i], transfer_queue_idx[i], &transfer_queue[i]);
		create_allocator(funcs[i], inst, physical_devs[i], dev[i], allocator[i]);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		create_desc_and_pipeline_layout(funcs[i], dev[i], desc_set_layout[i], pipeline_layout[i]);
		create_compute_pipeline(funcs[i], dev[i], pipeline_layout[i], particle_attraction_code, sizeof(particle_attraction_code), pipeline_attraction[i]);
		create_desc_pool_and_set(funcs[i], dev[i], desc_set_layout[i], desc_pool[i], desc_set[i]);

		create_dev_buf(allocator[i], dev_buf[i], dev_buf_alloc[i], storage_buf_size + uniform_buf_size);
		create_host_buf(allocator[i], host_buf[i], host_buf_alloc[i], particles[i], storage_buf_size);
		create_uniform_buf(allocator[i], uniform_buf[i], uniform_buf_alloc[i], ubo[i], uniform_buf_size);
		update_desc_set(funcs[i], dev[i], desc_set[i], dev_buf[i], uniform_buf[i], storage_buf_size, uniform_buf_size);

		create_cmd_pool(funcs[i], dev[i], compute_queue_family_idx[i], compute_cmd_pool[i]);
		create_cmd_pool(funcs[i], dev[i], transfer_queue_family_idx[i], transfer_cmd_pool[i]);
		create_cmd_bufs(funcs[i], dev[i], compute_cmd_pool[i], compute_cmd_bufs[i]);
		create_cmd_bufs(funcs[i], dev[i], transfer_cmd_pool[i], transfer_cmd_bufs[i]);

		record_cmd_buf_copy_host_to_dev(funcs[i], transfer_cmd_bufs[i][0], host_buf[i], dev_buf[i], storage_buf_size, compute_queue_family_idx[i], transfer_queue_family_idx[i]);
		record_cmd_buf_copy_dev_to_host(funcs[i], transfer_cmd_bufs[i][1], host_buf[i], dev_buf[i], storage_buf_size, compute_queue_family_idx[i], transfer_queue_family_idx[i]);
		record_cmd_buf_work(funcs[i], compute_cmd_bufs[i][0], pipeline_attraction[i], pipeline_layout[i], desc_set[i], dev_buf[i], storage_buf_size, particles_per_workgroup, compute_queue_family_idx[i], transfer_queue_family_idx[i]);

		create_fence(funcs[i], dev[i], compute_fence[i]);
		create_fence(funcs[i], dev[i], dev_to_host_copy_fence[i]);
		create_semaphore(funcs[i], dev[i], copy_host_to_dev_semaphore[i]);
		create_semaphore(funcs[i], dev[i], copy_dev_to_host_semaphore[i]);
		create_semaphore(funcs[i], dev[i], compute_fin_semaphore[i]);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		printf("GPU:%zu Creating random init data...\n", i);
		for (std::size_t j = 0; j < num_particles; j++) {
			particles[i][j].position.components.x = dist(rng);
			particles[i][j].position.components.y = dist(rng);
			particles[i][j].position.components.z = dist(rng);
			particles[i][j].position.components.w = dist(rng);

			particles[i][j].velocity.components.x = dist(rng);
			particles[i][j].velocity.components.y = dist(rng);
			particles[i][j].velocity.components.z = dist(rng);
			particles[i][j].velocity.components.w = dist(rng);
		}

		// copy init data
		{
			printf("GPU:%zu Copying init data...\n", i);

			const VkSubmitInfo submit_info = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.pNext = nullptr,
				.waitSemaphoreCount = 0,
				.pWaitSemaphores = nullptr,
				.pWaitDstStageMask = nullptr,
				.commandBufferCount = 1,
				.pCommandBuffers = &transfer_cmd_bufs[i][0],
				.signalSemaphoreCount = 1,
				.pSignalSemaphores = &copy_host_to_dev_semaphore[i]
			};

			if (funcs[i].vkQueueSubmit(transfer_queue[i], 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS)
				throw std::runtime_error("Cannot copy init data!");
		}

		ubo[i]->particle_count = num_particles;
	}

	printf("Enter quit to end the program.\n");

	while (true) {
		if (mailbox.get_input(line)) {
			if (line == "quit") {
				break;
			} else if (line == "dump") {
				for (std::size_t i = 0; i < physical_devs.size(); i++) {
					std::printf("GPU:%zu Particle:0 Position:%.2f %.2f %.2f Velocity:%.2f %.2f %.2f %.2f\n",
						i,
						particles[i][0].position.components.x,
						particles[i][0].position.components.y,
						particles[i][0].position.components.z,
						particles[i][0].velocity.components.x,
						particles[i][0].velocity.components.y,
						particles[i][0].velocity.components.z,
						particles[i][0].velocity.components.w
					);
				}
			}
		}

		for (std::size_t i = 0; i < physical_devs.size(); i++) {
			const auto compute_fence_status = funcs[i].vkGetFenceStatus(dev[i], compute_fence[i]);
			const auto dev_to_host_copy_fence_status = funcs[i].vkGetFenceStatus(dev[i], dev_to_host_copy_fence[i]);

			if (compute_fence_status == VK_SUCCESS && dev_to_host_copy_fence_status == VK_SUCCESS) {
				if (funcs[i].vkResetFences(dev[i], 1, &compute_fence[i]) != VK_SUCCESS)
					throw std::runtime_error("Failed to reset compute fence!");

				if (funcs[i].vkResetFences(dev[i], 1, &dev_to_host_copy_fence[i]) != VK_SUCCESS)
					throw std::runtime_error("Failed to reset compute fence!");

				end_time[i] = std::chrono::high_resolution_clock::now();
				const auto delta_time = std::chrono::duration_cast<std::chrono::duration<float>>(end_time[i] - start_time[i]).count();

				ubo[i]->delta_time = delta_time;
				duration[i] += delta_time;
				mean_sample[i] += delta_time;
				num_samples[i]++;

				if (duration[i] >= 10.f) {
					duration[i] = 0.f;

					const auto t = time(NULL);
					const std::tm* timest = std::localtime(&t);
					const float avg_dt = mean_sample[i] / num_samples[i];
					mean_sample[i] = 0.f;
					num_samples[i] = 0;

					std::printf("Date:%d-%02d-%02d Time:%02d:%02d:%02d GPU:%zu AverageTime:%.04f sec AverageSimulationsPerSec:%.02f\n", 1900 + timest->tm_year, 1 + timest->tm_mon, timest->tm_mday, timest->tm_hour, timest->tm_min, timest->tm_sec, i, avg_dt, 1.f/avg_dt);
				}

				const VkSubmitInfo compute_submit_info = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					.pNext = nullptr,
					.waitSemaphoreCount = 1u,
					.pWaitSemaphores = wait_for_copy[i] ? &copy_host_to_dev_semaphore[i] : &copy_dev_to_host_semaphore[i],
					.pWaitDstStageMask = &wait_stage_transfer,
					.commandBufferCount = 1u,
					.pCommandBuffers = &compute_cmd_bufs[i][0],
					.signalSemaphoreCount = 1u,
					.pSignalSemaphores = &compute_fin_semaphore[i]
				};

				const VkSubmitInfo transfer_submit_info = {
					.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					.pNext = nullptr,
					.waitSemaphoreCount = 1u,
					.pWaitSemaphores = &compute_fin_semaphore[i],
					.pWaitDstStageMask = &wait_stage_compute,
					.commandBufferCount = 1u,
					.pCommandBuffers = &transfer_cmd_bufs[i][1],
					.signalSemaphoreCount = 1u,
					.pSignalSemaphores = &copy_dev_to_host_semaphore[i]
				};

				start_time[i] = std::chrono::high_resolution_clock::now();
				if (funcs[i].vkQueueSubmit(compute_queue[i], 1, &compute_submit_info, compute_fence[i]) != VK_SUCCESS)
					throw std::runtime_error("Failed to submit work!");

				if (funcs[i].vkQueueSubmit(transfer_queue[i], 1, &transfer_submit_info, dev_to_host_copy_fence[i]) != VK_SUCCESS)
					throw std::runtime_error("Failed to submit DEV->CPU copy!");

				wait_for_copy[i] = false;
			} else if (compute_fence_status == VK_ERROR_DEVICE_LOST || dev_to_host_copy_fence_status == VK_ERROR_DEVICE_LOST) {
				throw std::runtime_error("Failed to query device fence status!");
			}
		}
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		funcs[i].vkWaitForFences(dev[i], 1, &compute_fence[i], VK_TRUE, std::numeric_limits<std::uint64_t>::max());
		funcs[i].vkWaitForFences(dev[i], 1, &dev_to_host_copy_fence[i], VK_TRUE, std::numeric_limits<std::uint64_t>::max());
		funcs[i].vkDeviceWaitIdle(dev[i]);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		funcs[i].vkDestroySemaphore(dev[i], copy_host_to_dev_semaphore[i], nullptr);
		funcs[i].vkDestroySemaphore(dev[i], copy_dev_to_host_semaphore[i], nullptr);
		funcs[i].vkDestroySemaphore(dev[i], compute_fin_semaphore[i], nullptr);
		funcs[i].vkDestroyFence(dev[i], compute_fence[i], nullptr);
		funcs[i].vkDestroyFence(dev[i], dev_to_host_copy_fence[i], nullptr);
		funcs[i].vkDestroyCommandPool(dev[i], compute_cmd_pool[i], nullptr);
		funcs[i].vkDestroyCommandPool(dev[i], transfer_cmd_pool[i], nullptr);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		vmaDestroyBuffer(allocator[i], uniform_buf[i], uniform_buf_alloc[i]);
		vmaDestroyBuffer(allocator[i], host_buf[i], host_buf_alloc[i]);
		vmaDestroyBuffer(allocator[i], dev_buf[i], dev_buf_alloc[i]);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		funcs[i].vkDestroyDescriptorPool(dev[i], desc_pool[i], nullptr);
		funcs[i].vkDestroyPipeline(dev[i], pipeline_attraction[i], nullptr);
		funcs[i].vkDestroyPipelineLayout(dev[i], pipeline_layout[i], nullptr);
		funcs[i].vkDestroyDescriptorSetLayout(dev[i], desc_set_layout[i], nullptr);
	}

	for (std::size_t i = 0; i < physical_devs.size(); i++) {
		vmaDestroyAllocator(allocator[i]);
		funcs[i].vkDestroyDevice(dev[i], nullptr);
	}

	if (debug_msgr != VK_NULL_HANDLE)
		vkDestroyDebugUtilsMessengerEXT(inst, debug_msgr, nullptr);

	vkDestroyInstance(inst, nullptr);

	return 0;
}
