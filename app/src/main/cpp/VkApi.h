#pragma once

// Минимальный динамический загрузчик Vulkan (в духе GlApi.h для GL).
// На десктопе нет vulkan-1.lib, поэтому все функции грузим через
// vkGetInstanceProcAddr / vkGetDeviceProcAddr. VK_NO_PROTOTYPES отключает
// статические прототипы — вместо них указатели ниже. Работает и на Android
// (bootstrap-функцию платформа передаёт снаружи: GLFW на десктопе, dlsym на Android).

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Списки функций (X-макрос): глобальные, инстанс-уровня, девайс-уровня.
#define VK_GLOBAL_FUNCS(X)               \
    X(vkCreateInstance)                  \
    X(vkEnumerateInstanceLayerProperties)\
    X(vkEnumerateInstanceExtensionProperties)

#define VK_INSTANCE_FUNCS(X)               \
    X(vkDestroyInstance)                   \
    X(vkDestroySurfaceKHR)                 \
    X(vkEnumeratePhysicalDevices)          \
    X(vkGetPhysicalDeviceProperties)       \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)     \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)\
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)     \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)\
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice)                      \
    X(vkGetDeviceProcAddr)                 \
    X(vkCreateDebugUtilsMessengerEXT)      \
    X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FUNCS(X)         \
    X(vkDestroyDevice)             \
    X(vkDeviceWaitIdle)            \
    X(vkGetDeviceQueue)            \
    X(vkCreateSwapchainKHR)        \
    X(vkDestroySwapchainKHR)       \
    X(vkGetSwapchainImagesKHR)     \
    X(vkAcquireNextImageKHR)       \
    X(vkQueuePresentKHR)           \
    X(vkQueueSubmit)               \
    X(vkQueueWaitIdle)             \
    X(vkCreateImageView)           \
    X(vkDestroyImageView)          \
    X(vkCreateRenderPass)          \
    X(vkDestroyRenderPass)         \
    X(vkCreateFramebuffer)         \
    X(vkDestroyFramebuffer)        \
    X(vkCreateCommandPool)         \
    X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers)    \
    X(vkResetCommandBuffer)        \
    X(vkBeginCommandBuffer)        \
    X(vkEndCommandBuffer)          \
    X(vkCmdBeginRenderPass)        \
    X(vkCmdEndRenderPass)          \
    X(vkCreateSemaphore)           \
    X(vkDestroySemaphore)          \
    X(vkCreateFence)               \
    X(vkDestroyFence)              \
    X(vkWaitForFences)             \
    X(vkResetFences)               \
    X(vkFreeCommandBuffers)        \
    X(vkCreateShaderModule)        \
    X(vkDestroyShaderModule)       \
    X(vkCreatePipelineLayout)      \
    X(vkDestroyPipelineLayout)     \
    X(vkCreateGraphicsPipelines)   \
    X(vkDestroyPipeline)           \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout)\
    X(vkCreateDescriptorPool)      \
    X(vkDestroyDescriptorPool)     \
    X(vkAllocateDescriptorSets)    \
    X(vkUpdateDescriptorSets)      \
    X(vkCreateBuffer)              \
    X(vkDestroyBuffer)             \
    X(vkGetBufferMemoryRequirements)\
    X(vkBindBufferMemory)          \
    X(vkCreateImage)               \
    X(vkDestroyImage)              \
    X(vkGetImageMemoryRequirements)\
    X(vkBindImageMemory)           \
    X(vkAllocateMemory)            \
    X(vkFreeMemory)                \
    X(vkMapMemory)                 \
    X(vkUnmapMemory)               \
    X(vkCmdBindPipeline)           \
    X(vkCmdBindDescriptorSets)     \
    X(vkCmdBindVertexBuffers)      \
    X(vkCmdBindIndexBuffer)        \
    X(vkCmdPushConstants)          \
    X(vkCmdDraw)                   \
    X(vkCmdDrawIndexed)            \
    X(vkCmdSetViewport)            \
    X(vkCmdSetScissor)             \
    X(vkCmdCopyBufferToImage)      \
    X(vkCmdPipelineBarrier)        \
    X(vkCreateSampler)             \
    X(vkDestroySampler)

// Объявления указателей на функции.
#define VK_DECLARE(n) extern PFN_##n n;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE

// Инициализация. gipa — bootstrap vkGetInstanceProcAddr, полученный платформой
// (десктоп: glfwGetInstanceProcAddress; Android: dlsym("vkGetInstanceProcAddr")).
bool vkApiInitGlobal(PFN_vkGetInstanceProcAddr gipa);  // грузит глобальные функции
void vkApiLoadInstance(VkInstance instance);           // после vkCreateInstance
void vkApiLoadDevice(VkDevice device);                 // после vkCreateDevice
PFN_vkGetInstanceProcAddr vkApiLoader();               // bootstrap-загрузчик (для ImGui)
