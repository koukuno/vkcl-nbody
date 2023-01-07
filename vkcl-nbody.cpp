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

#include "volk.h"

#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#include "vk_mem_alloc.h"

static const std::uint32_t particle_calculate_code[] =
#include "particle_calculate.inc"
;

static const std::uint32_t particle_integrate_code[] =
#include "particle_integrate.inc"
;

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
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
			.pNext = on_pci[i] ? &pci_bus_info : nullptr,
			.properties = {}
		};

		props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR;
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

static void create_vkinstance(VkInstance &inst) {
	static const std::array<const char *, 1> extensions = {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
	};

	const VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pNext = nullptr,
		.pApplicationName = "vkcl-nbody",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "vkcl-nbody",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_0
	};

	const VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.pApplicationInfo = &app_info,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = nullptr,
		.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
		.ppEnabledExtensionNames = extensions.data()
	};

	if (vkCreateInstance(&create_info, nullptr, &inst) != VK_SUCCESS)
		throw std::runtime_error("Failed to create VkInstance!");

	volkLoadInstanceOnly(inst);
}

static void get_physical_devs(VkInstance inst, std::vector<VkPhysicalDevice> &physical_devs) {
	std::uint32_t count;
	vkEnumeratePhysicalDevices(inst, &count, nullptr);

	if (count == 0)
		throw std::runtime_error("No VkPhysicalDevice found!");

	physical_devs.resize(count);
	vkEnumeratePhysicalDevices(inst, &count, physical_devs.data());
}

static void create_device(VkPhysicalDevice physical_dev, VkDevice &dev, std::uint32_t &compute_queue_family_idx) {
	static const float priority = 1.f;

	std::uint32_t count;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_dev, &count, nullptr);

	std::vector<VkQueueFamilyProperties> queue_families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_dev, &count, queue_families.data());

	compute_queue_family_idx = std::numeric_limits<std::uint32_t>::max();

	for (std::size_t i = 0; i < queue_families.size(); i++) {
		if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			compute_queue_family_idx = static_cast<std::uint32_t>(i);
			break;
		}
	}

	if (compute_queue_family_idx == std::numeric_limits<std::uint32_t>::max())
		throw std::runtime_error("No compute queue found!");

	const VkDeviceQueueCreateInfo queue = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueFamilyIndex = compute_queue_family_idx,
		.queueCount = 1,
		.pQueuePriorities = &priority
	};

	const VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = nullptr,
		.enabledExtensionCount = 0,
		.ppEnabledExtensionNames = nullptr,
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

static void create_cmd_pool(const VolkDeviceTable &funcs, VkDevice dev, const uint32_t compute_queue_family_idx, VkCommandPool &cmd_pool) {
	const VkCommandPoolCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.queueFamilyIndex = compute_queue_family_idx
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

#if 0
static void create_semaphore(const VolkDeviceTable &funcs, VkDevice dev, VkSemaphore &semaphore) {
	const VkSemaphoreCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0
	};

	if (funcs.vkCreateSemaphore(dev, &create_info, nullptr, &semaphore) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkSemaphore!");
}
#endif

static void create_fence(const VolkDeviceTable &funcs, VkDevice dev, VkFence &fence) {
	const VkFenceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0
	};

	if (funcs.vkCreateFence(dev, &create_info, nullptr, &fence) != VK_SUCCESS)
		throw std::runtime_error("Cannot create VkSemaphore!");
}

static void record_cmd_buf_work(const VolkDeviceTable &funcs, VkCommandBuffer cmd_buf, VkPipeline pipeline_calculate, VkPipeline pipeline_integrate, VkPipelineLayout pipeline_layout, VkDescriptorSet desc_set, VkBuffer dev_buf, const VkDeviceSize dev_buf_storage_size, const std::uint32_t count) {
	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pNext = nullptr,
		.flags = 0,
		.pInheritanceInfo = nullptr
	};

	const VkBufferMemoryBarrier storage_buf_mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = dev_buf,
		.offset = 0,
		.size = dev_buf_storage_size
	};

	funcs.vkBeginCommandBuffer(cmd_buf, &begin_info);
	funcs.vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_calculate);
	funcs.vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &desc_set, 0, nullptr);
	funcs.vkCmdDispatch(cmd_buf, count, 1, 1);
	funcs.vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &storage_buf_mem_barrier, 0, nullptr);
	funcs.vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_integrate);
	funcs.vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &desc_set, 0, nullptr);
	funcs.vkCmdDispatch(cmd_buf, count, 1, 1);
	funcs.vkEndCommandBuffer(cmd_buf);
}

static void record_cmd_buf_copy(const VolkDeviceTable &funcs, VkCommandBuffer cmd_buf, VkBuffer host_buf, VkBuffer dev_buf, const VkDeviceSize size) {
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

	funcs.vkBeginCommandBuffer(cmd_buf, &begin_info);
	funcs.vkCmdCopyBuffer(cmd_buf, host_buf, dev_buf, 1, &region);
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

int main() {
	static const std::uint32_t count = 4096;
	static const std::size_t num_particles = count*1024;

	static const VkDeviceSize storage_buf_size = sizeof(Particle)*num_particles;
	static const VkDeviceSize uniform_buf_size = sizeof(UBO);

	static auto seed = get_random_seed();
	static std::default_random_engine rng(seed);
	std::uniform_real_distribution<float> dist(0.0, 1000.0);

	VkInstance inst;
	std::vector<VkPhysicalDevice> physical_devs;
	VkPhysicalDevice physical_dev;

	VkDevice dev;
	VolkDeviceTable funcs;
	VmaAllocator allocator;

	VkDescriptorSetLayout desc_set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline_calculate, pipeline_integrate;

	VkDescriptorPool desc_pool;
	VkDescriptorSet desc_set;

	VmaAllocation dev_buf_alloc, host_buf_alloc, uniform_buf_alloc;
	VkBuffer dev_buf, host_buf, uniform_buf;
	Particle *particles; // from host_buf memory
	UBO *ubo; // from uniform_buf memory

	VkCommandPool cmd_pool;
	std::array<VkCommandBuffer, 2> cmd_bufs;

	VkFence fence;

	std::uint32_t compute_queue_family_idx;
	VkQueue compute_queue;

	if (volkInitialize() != VK_SUCCESS)
		throw std::runtime_error("Cannot load Vulkan runtime library!");

	create_vkinstance(inst);
	get_physical_devs(inst, physical_devs);
	physical_dev = physical_devs[select_device_prompt(physical_devs)];

	create_device(physical_dev, dev, compute_queue_family_idx);
	volkLoadDeviceTable(&funcs, dev);
	funcs.vkGetDeviceQueue(dev, compute_queue_family_idx, 0, &compute_queue);
	create_allocator(funcs, inst, physical_dev, dev, allocator);

	create_desc_and_pipeline_layout(funcs, dev, desc_set_layout, pipeline_layout);
	create_compute_pipeline(funcs, dev, pipeline_layout, particle_calculate_code, sizeof(particle_calculate_code), pipeline_calculate);
	create_compute_pipeline(funcs, dev, pipeline_layout, particle_integrate_code, sizeof(particle_integrate_code), pipeline_integrate);
	create_desc_pool_and_set(funcs, dev, desc_set_layout, desc_pool, desc_set);

	create_dev_buf(allocator, dev_buf, dev_buf_alloc, storage_buf_size + uniform_buf_size);
	create_host_buf(allocator, host_buf, host_buf_alloc, particles, storage_buf_size);
	create_uniform_buf(allocator, uniform_buf, uniform_buf_alloc, ubo, uniform_buf_size);
	update_desc_set(funcs, dev, desc_set, dev_buf, uniform_buf, storage_buf_size, uniform_buf_size);

	create_cmd_pool(funcs, dev, compute_queue_family_idx, cmd_pool);
	create_cmd_bufs(funcs, dev, cmd_pool, cmd_bufs);
	record_cmd_buf_work(funcs, cmd_bufs[0], pipeline_calculate, pipeline_integrate, pipeline_layout, desc_set, dev_buf, storage_buf_size, count);
	record_cmd_buf_copy(funcs, cmd_bufs[1], host_buf, dev_buf, storage_buf_size);

	create_fence(funcs, dev, fence);

	printf("Creating random init data...\n");
	for (std::size_t i = 0; i < num_particles; i++) {
		particles[i].position.components.x = dist(rng);
		particles[i].position.components.y = dist(rng);
		particles[i].position.components.z = dist(rng);
		particles[i].position.components.w = dist(rng);

		particles[i].velocity.components.x = dist(rng);
		particles[i].velocity.components.y = dist(rng);
		particles[i].velocity.components.z = dist(rng);
		particles[i].velocity.components.w = dist(rng);
	}

	// copy init data
	{
		printf("Copying init data...\n");

		const VkSubmitInfo submit_info = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreCount = 0,
			.pWaitSemaphores = nullptr,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd_bufs[1],
			.signalSemaphoreCount = 0,
			.pSignalSemaphores = nullptr
		};

		if (funcs.vkQueueSubmit(compute_queue, 1, &submit_info, fence) != VK_SUCCESS)
			throw std::runtime_error("Cannot copy init data!");

		if (funcs.vkWaitForFences(dev, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS)
			throw std::runtime_error("Failed to wait for fence! (copy completion)");

		if (funcs.vkResetFences(dev, 1, &fence) != VK_SUCCESS)
			throw std::runtime_error("Failed to reset fence!");
	}

	ubo->particle_count = num_particles;

	printf("Enter quit to end the program.\n");

	std::chrono::high_resolution_clock::time_point start_time, end_time;
	StdinMailbox mailbox;
	std::string line;
	float duration = 0.f, mean_sample = 0.f;
	int num_samples = 0;

	while (true) {
		if (mailbox.get_input(line)) {
			if (line == "quit")
				break;
		}

		const VkSubmitInfo submit_info = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreCount = 0,
			.pWaitSemaphores = nullptr,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd_bufs[0],
			.signalSemaphoreCount = 0,
			.pSignalSemaphores = nullptr
		};

		funcs.vkQueueSubmit(compute_queue, 1, &submit_info, fence);

		start_time = std::chrono::high_resolution_clock::now();

		if (funcs.vkWaitForFences(dev, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS)
			throw std::runtime_error("Failed to wait for fence! (copy completion)");

		end_time = std::chrono::high_resolution_clock::now();
		ubo->delta_time = std::chrono::duration_cast<std::chrono::duration<float>>(end_time - start_time).count();

		duration += ubo->delta_time;
		mean_sample += ubo->delta_time;
		num_samples++;

		if (duration >= 10.f) {
			duration = 0.f;

			const auto t = time(NULL);
			const std::tm *timest = std::localtime(&t);
			const float avg_dt = mean_sample/num_samples, avg_bps = 1.f/avg_dt;
			mean_sample = 0.f;
			num_samples = 0;

			std::printf("DateTime:%d-%02d-%02d %02d:%02d:%02d AverageBodiesPerSecond (AverageBPS):%.02f\n", 1900 + timest->tm_year, 1 + timest->tm_mon, timest->tm_mday, timest->tm_hour, timest->tm_min, timest->tm_sec, avg_bps);
		}

		if (funcs.vkResetFences(dev, 1, &fence) != VK_SUCCESS)
			throw std::runtime_error("Failed to reset fence!");
	}

	funcs.vkDeviceWaitIdle(dev);
	funcs.vkDestroyFence(dev, fence, nullptr);
	funcs.vkDestroyCommandPool(dev, cmd_pool, nullptr);

	vmaDestroyBuffer(allocator, uniform_buf, uniform_buf_alloc);
	vmaDestroyBuffer(allocator, host_buf, host_buf_alloc);
	vmaDestroyBuffer(allocator, dev_buf, dev_buf_alloc);

	funcs.vkDestroyDescriptorPool(dev, desc_pool, nullptr);
	funcs.vkDestroyPipeline(dev, pipeline_calculate, nullptr);
	funcs.vkDestroyPipeline(dev, pipeline_integrate, nullptr);
	funcs.vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
	funcs.vkDestroyDescriptorSetLayout(dev, desc_set_layout, nullptr);

	vmaDestroyAllocator(allocator);
	funcs.vkDestroyDevice(dev, nullptr);

	vkDestroyInstance(inst, nullptr);

	return 0;
}
