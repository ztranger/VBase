#include "VkApi.h"

// Хранилище указателей + bootstrap.
static PFN_vkGetInstanceProcAddr g_gipa = nullptr;

#define VK_DEFINE(n) PFN_##n n = nullptr;
VK_GLOBAL_FUNCS(VK_DEFINE)
VK_INSTANCE_FUNCS(VK_DEFINE)
VK_DEVICE_FUNCS(VK_DEFINE)
#undef VK_DEFINE

bool vkApiInitGlobal(PFN_vkGetInstanceProcAddr gipa) {
    if (gipa == nullptr) return false;
    g_gipa = gipa;
    // Глобальные функции грузятся с instance = VK_NULL_HANDLE (по спецификации).
    #define VK_LOAD(n) n = (PFN_##n)g_gipa(VK_NULL_HANDLE, #n);
    VK_GLOBAL_FUNCS(VK_LOAD)
    #undef VK_LOAD
    return vkCreateInstance != nullptr;
}

void vkApiLoadInstance(VkInstance instance) {
    #define VK_LOAD(n) n = (PFN_##n)g_gipa(instance, #n);
    VK_INSTANCE_FUNCS(VK_LOAD)
    #undef VK_LOAD
}

void vkApiLoadDevice(VkDevice device) {
    // Девайс-уровневые — через vkGetDeviceProcAddr (прямой диспатч, без слоёв).
    #define VK_LOAD(n) n = (PFN_##n)vkGetDeviceProcAddr(device, #n);
    VK_DEVICE_FUNCS(VK_LOAD)
    #undef VK_LOAD
}

PFN_vkGetInstanceProcAddr vkApiLoader() { return g_gipa; }
