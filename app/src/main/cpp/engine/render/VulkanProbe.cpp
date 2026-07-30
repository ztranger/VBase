#include "engine/render/VulkanProbe.h"

#include <dlfcn.h>

#include <vector>

#include "engine/render/VkApi.h"  // динамический загрузчик (без линковки libvulkan)
#include "engine/core/Log.h"

namespace VulkanProbe {

// Проверка через тот же загрузчик, что и VulkanRenderer: libvulkan НЕ линкуется
// (иначе имена-указатели VkApi конфликтуют с экспортами libvulkan).
bool isSupported() {
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        LOGW("Vulkan: нет libvulkan.so — GL fallback");
        return false;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkApiInitGlobal(gipa)) {
        LOGW("Vulkan: не удалось загрузить функции — GL fallback");
        dlclose(lib);
        return false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VBase";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
        LOGW("Vulkan: vkCreateInstance failed — GL fallback");
        dlclose(lib);
        return false;
    }
    vkApiLoadInstance(instance);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    bool ok = count > 0;
    if (ok) {
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[0], &props);
        LOGI("Vulkan device: %s, API %u.%u.%u", props.deviceName,
             VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
             VK_VERSION_PATCH(props.apiVersion));
    } else {
        LOGW("Vulkan: нет физических устройств — GL fallback");
    }

    vkDestroyInstance(instance, nullptr);
    dlclose(lib);
    return ok;
}

}  // namespace VulkanProbe
