#pragma once

/**
 * Проверка доступности Vulkan на устройстве.
 * Точка расширения: когда появится VulkanRenderer, по результату
 * isSupported() в main.cpp выбирается бэкенд (Vulkan или GL).
 */
namespace VulkanProbe {

// Создаёт VkInstance, логирует все физические устройства и их версии API.
// true — есть хотя бы одно устройство с драйвером Vulkan.
bool isSupported();

} // namespace VulkanProbe
