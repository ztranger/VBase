#pragma once

#include <cstdint>
#include <vector>

#include "VkApi.h"     // типы Vulkan + динамический загрузчик
#include "Renderer.h"

/**
 * Vulkan-бэкенд под контракт Renderer/RenderFrame. Разрабатывается на десктопе
 * (GLFW+Vulkan surface, динамическая загрузка функций), ядро переиспользуемо на
 * Android (отличается только создание surface и bootstrap загрузчика).
 *
 * ЭТАПНОСТЬ: сейчас Фаза 0 — instance/device/swapchain/present, очистка экрана.
 * create-методы и геометрия — заглушки (вернутся в следующих фазах: пайплайны,
 * буферы, текстуры, скиннинг, ImGui).
 */
class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override;

    bool init(ANativeWindow* window, void* (*glGetProc)(const char*),
              AssetSource& assets) override;
    void setSurfaceSize(int width, int height) override;
    MeshHandle createMesh(const MeshData& data) override;
    SkinnedHandle createSkinnedMesh(const SkinnedModel& model) override;
    TextureHandle createTexture(const TextureData& data) override;
    MaterialHandle createMaterial(const MaterialDesc& desc) override;
    void renderFrame(const RenderFrame& frame) override;
    float aspectRatio() const override;

private:
    struct VkTexture;  // определение ниже; нужно для сигнатуры uploadTexture

    // Этапы инициализации (каждый возвращает false при ошибке).
    bool createInstance();
    bool createSurface();        // платформозависимо (GLFW / Android)
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createDepthResources();
    bool createRenderPass();
    bool createFramebuffers();
    bool createDescriptors();
    bool createSampler();
    bool createDefaultTexture();
    bool createPipelines();
    bool createSkinnedPipeline();  // отдельный пайплайн/лейаут для скиннинга
    bool createCommandBuffers();
    bool createSyncObjects();

    bool recreateSwapchain();
    void cleanupSwapchain();
    void cleanup();

    // Хелперы Фазы 1.
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memProps, VkBuffer& buf, VkDeviceMemory& mem);
    VkShaderModule loadShaderModule(const char* path);

    // Хелперы Фазы 2.
    bool createGraphicsPipeline(VkShaderModule vs, VkShaderModule fs, VkPipeline& out);
    VkCommandBuffer beginOneTime();          // разовый командный буфер (загрузки)
    void endOneTime(VkCommandBuffer cmd);    // submit + wait + free
    VkDescriptorSet allocMaterialSet(VkImageView albedo);  // set 1 для материала
    // Создать GPU-текстуру RGBA8 из пикселей (staging + барьеры + view).
    bool uploadTexture(uint32_t w, uint32_t h, const void* rgba, VkTexture& out);

    static constexpr int kMaxFramesInFlight = 2;

    void* window_ = nullptr;      // платформенное окно (GLFWwindow* / ANativeWindow*)

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    bool validation_ = false;     // включены ли validation layers (если найдены)
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

    // Depth-буфер (Фаза 1).
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    // Раскладка и пайплайны. set 0 = кадровый UBO, set 1 = albedo-текстура.
    // Пайплайны индексируются по ShaderType (Lit/Unlit/Phong) — Фаза 2.
    VkDescriptorSetLayout setLayout0_ = VK_NULL_HANDLE;  // Frame UBO
    VkDescriptorSetLayout setLayout1_ = VK_NULL_HANDLE;  // combined image sampler (albedo)
    VkDescriptorSetLayout setLayout2_ = VK_NULL_HANDLE;  // storage buffer (кости, скиннинг)
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;   // окружение (set0,set1 + push=color)
    VkPipeline pipelines_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout skinnedPipelineLayout_ = VK_NULL_HANDLE;  // set0,1,2 + push=model/color/boneOffset
    VkPipeline skinnedPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;  // общий (LINEAR + REPEAT)

    // Дефолтная белая 1x1 — материалы/объекты без текстуры ссылаются на неё.
    VkImage whiteImage_ = VK_NULL_HANDLE;
    VkDeviceMemory whiteMem_ = VK_NULL_HANDLE;
    VkImageView whiteView_ = VK_NULL_HANDLE;
    VkDescriptorSet whiteSet_ = VK_NULL_HANDLE;  // set 1 для белой (скиннинг без текстуры)

    static constexpr uint32_t kMaxBones = 512;  // суммарно костей на кадр (все скиннинг-объекты)

    // Ресурсы на кадр «в полёте»: кадровый UBO + дескриптор + инстанс-буфер
    // (матрицы модели всех объектов кадра, host-visible, обновляется каждый кадр).
    static constexpr uint32_t kMaxInstances = 512;
    struct FrameRes {
        VkBuffer ubo = VK_NULL_HANDLE;
        VkDeviceMemory uboMem = VK_NULL_HANDLE;
        void* uboMapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkBuffer inst = VK_NULL_HANDLE;      // инстанс-буфер (матрицы iModel)
        VkDeviceMemory instMem = VK_NULL_HANDLE;
        void* instMapped = nullptr;
        VkBuffer bones = VK_NULL_HANDLE;     // SSBO костей скиннинга
        VkDeviceMemory bonesMem = VK_NULL_HANDLE;
        void* bonesMapped = nullptr;
        VkDescriptorSet bonesSet = VK_NULL_HANDLE;  // set 2
    };
    std::vector<FrameRes> frames_;

    // Меши (handle = индекс + 1). Буферы host-visible (Фаза 1: без staging).
    struct VkMesh {
        VkBuffer vbuf = VK_NULL_HANDLE;
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkBuffer ibuf = VK_NULL_HANDLE;
        VkDeviceMemory imem = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
    };
    std::vector<VkMesh> meshes_;
    std::vector<VkMesh> skinnedMeshes_;  // скиннинг-меши (handle = индекс + 1)

    // Текстуры (handle = индекс + 1).
    struct VkTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<VkTexture> textures_;
    std::vector<VkDescriptorSet> textureSets_;  // set 1 на каждую текстуру (для скиннинга)

    // Материалы (handle = индекс + 1): тип шейдера, цвет, дескриптор albedo (set 1).
    struct VkMaterial {
        uint32_t shader = 0;
        Vec3 color{1.0f, 1.0f, 1.0f};
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::vector<VkMaterial> materials_;

    AssetSource* assets_ = nullptr;  // источник SPIR-V шейдеров

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;  // на кадр «в полёте»

    std::vector<VkSemaphore> imageAvailable_;      // на кадр «в полёте»
    std::vector<VkSemaphore> renderFinished_;      // на образ swapchain
    std::vector<VkFence> inFlight_;                // на кадр «в полёте»
    std::vector<VkFence> imagesInFlight_;          // какой fence занял данный образ (не владеем)
    uint32_t currentFrame_ = 0;

    int desiredW_ = 1, desiredH_ = 1;  // запасной размер (setSurfaceSize)
    uint32_t nextHandle_ = 1;          // счётчик заглушечных handle'ов (Фаза 0)
    bool ready_ = false;
};
