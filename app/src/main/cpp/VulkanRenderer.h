#pragma once

#include <vulkan/vulkan.h>
#include <android/native_window.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "Renderer.h"

/**
 * Минимальный Vulkan-бэкенд: рисует тот же треугольник, что и GlRenderer,
 * с теми же касанием и подсветкой. Параметры кадра идут через push-константы.
 * Ресайз/поворот обрабатываются пересозданием swapchain по VK_ERROR_OUT_OF_DATE_KHR.
 */
class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override;

    bool init(ANativeWindow* window) override;
    void render() override;
    void onPointer(float x, float y, bool pressed) override;

private:
    // Этапы инициализации (каждый возвращает false при ошибке -> init() откатывается).
    bool createInstance();
    bool createSurface(ANativeWindow* window);
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createRenderPass();
    bool createFramebuffers();
    bool createPipeline();
    bool createVertexBuffer();
    bool createCommandBuffers();
    bool createSyncObjects();

    bool recreateSwapchain();
    void cleanupSwapchain();
    void cleanup();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    VkShaderModule createShaderModule(const uint32_t* code, size_t sizeBytes) const;
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    static constexpr int kMaxFramesInFlight = 2;

    ANativeWindow* window_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_ = {};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkFramebuffer> framebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;   // по одному на кадр «в полёте»

    std::vector<VkSemaphore> imageAvailable_;       // на кадр «в полёте»
    std::vector<VkSemaphore> renderFinished_;       // на образ swapchain
    std::vector<VkFence> inFlight_;                 // на кадр «в полёте»
    uint32_t currentFrame_ = 0;

    // Ввод и анимация — пишутся/читаются в потоке android_main.
    float touchX_ = 0.0f;
    float touchY_ = 0.0f;
    bool pressed_ = false;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
