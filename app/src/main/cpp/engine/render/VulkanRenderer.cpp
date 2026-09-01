#include "engine/render/VulkanRenderer.h"

// Платформенные куски (surface, bootstrap загрузчика) под #ifdef: десктоп — GLFW,
// Android — vkCreateAndroidSurfaceKHR + dlopen libvulkan. Ядро (пайплайны/дескрипторы/
// отрисовка) общее.
#ifdef __ANDROID__
#include <dlfcn.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <cmath>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "engine/assets/AssetSource.h"
#include "engine/render/GameUi.h"
#include "engine/assets/Font.h"
#include "engine/core/Log.h"
#include "engine/core/RenderFrame.h"

namespace {
// Проверка результата: залогировать и вернуть false из вызывающей функции.
#define VK_CHECK(expr, msg)                                   \
    do {                                                      \
        VkResult _r = (expr);                                 \
        if (_r != VK_SUCCESS) {                               \
            LOGE("Vulkan: %s (VkResult=%d)", msg, (int)_r);   \
            return false;                                     \
        }                                                     \
    } while (0)

// Колбэк validation layers -> в лог (ошибки/предупреждения/инфо).
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOGE("Vulkan validation: %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOGW("Vulkan validation: %s", data->pMessage);
    } else {
        LOGI("Vulkan validation: %s", data->pMessage);
    }
    return VK_FALSE;  // не прерывать вызов, вызвавший сообщение
}

// Загрузчик Vulkan-функций для imgui_impl_vulkan (user = VkInstance).
PFN_vkVoidFunction imguiVkLoader(const char* name, void* user) {
    return vkApiLoader()((VkInstance)user, name);
}

// Настройки messenger'а — общие для pNext инстанса и постоянного messenger'а.
VkDebugUtilsMessengerCreateInfoEXT makeDebugInfo() {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
    return ci;
}

// Кадровые данные (descriptor set 0, binding 0). std140: mat4+mat4+vec4+vec4+vec4.
// Раскладка ОБЯЗАНА совпадать с блоком Frame в vk-шейдерах. Размер 176 байт.
struct FrameUBOData {
    float viewProj[16];
    float lightVP[16];   // проекция глазами света (тени)
    float lightDir[4];   // xyz = направление НА свет, w = shadow bias
    float viewPos[4];    // xyz = позиция камеры
    float fogColor[4];   // xyz = цвет тумана (линейный), w = плотность
};

// Push-константы окружения: только цвет материала (модельная матрица — инстансный
// атрибут). 16 байт, фрагментный шейдер.
struct PushData {
    float color[4];
};

// Push-константы HUD: размер экрана (xy) + цвет текста (rgb). 32 байта.
struct HudPushData {
    float screen[4];
    float color[4];
};

// Push-константы скиннинга: модель + цвет + смещение костей в SSBO. 84 байта.
// Раскладка совпадает со std430 push_constant в skin.vert/frag (mat4@0, vec4@64, int@80).
struct SkinnedPushData {
    float model[16];
    float color[4];
    int boneOffset;
};

// Extent swapchain: currentExtent, если драйвер его диктует; иначе — заданный извне.
VkExtent2D chooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& caps, int desiredW, int desiredH) {
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        return caps.currentExtent;
    }
    VkExtent2D e;
    e.width = (uint32_t)(desiredW > 0 ? desiredW : 1);
    e.height = (uint32_t)(desiredH > 0 ? desiredH : 1);
    if (e.width < caps.minImageExtent.width) e.width = caps.minImageExtent.width;
    if (e.height < caps.minImageExtent.height) e.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width > 0 && e.width > caps.maxImageExtent.width) {
        e.width = caps.maxImageExtent.width;
    }
    if (caps.maxImageExtent.height > 0 && e.height > caps.maxImageExtent.height) {
        e.height = caps.maxImageExtent.height;
    }
    return e;
}

// IDENTITY, если поддерживается: композитор сам поворачивает буфер под ориентацию
// окна (как EGL для GLES). preTransform = currentTransform (ROTATE_90 на Android
// landscape) без поворота MVP/HUD/ImGui = картинка «стоя» растянута на экран.
VkSurfaceTransformFlagBitsKHR choosePreTransform(const VkSurfaceCapabilitiesKHR& caps) {
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    return caps.currentTransform;
}

// Коррекция клип-пространства GL -> Vulkan: Y вниз + глубина [-1,1] -> [0,1].
// column-major (m[col*4+row]); умножается слева: viewProj_vk = C * proj * view.
Mat4 vulkanClipFix() {
    Mat4 c;
    for (int i = 0; i < 16; ++i) c.m[i] = 0.0f;
    c.m[0] = 1.0f;    // x' = x
    c.m[5] = -1.0f;   // y' = -y  (Vulkan: Y вниз)
    c.m[10] = 0.5f;   // z' = 0.5*z + 0.5*w
    c.m[14] = 0.5f;
    c.m[15] = 1.0f;   // w' = w
    return c;
}
}  // namespace

VulkanRenderer::~VulkanRenderer() { cleanup(); }

bool VulkanRenderer::init(void* nativeWindow, AssetSource& assets) {
    assets_ = &assets;  // источник SPIR-V шейдеров
    window_ = nativeWindow;  // непрозрачный хэндл: десктоп — GLFWwindow*, Android — ANativeWindow*

    // Bootstrap загрузчика: получаем vkGetInstanceProcAddr. Десктоп — у GLFW,
    // Android — dlsym из libvulkan.so.
#ifdef __ANDROID__
    void* vklib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (vklib == nullptr) {
        LOGE("Vulkan: не удалось dlopen libvulkan.so");
        return false;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(vklib, "vkGetInstanceProcAddr");
#else
    auto gipa = (PFN_vkGetInstanceProcAddr)glfwGetInstanceProcAddress(
        VK_NULL_HANDLE, "vkGetInstanceProcAddr");
#endif
    if (!vkApiInitGlobal(gipa)) {
        LOGE("Vulkan: не удалось загрузить глобальные функции");
        return false;
    }

    if (!createInstance()) return false;
    vkApiLoadInstance(instance_);
    // Постоянный debug messenger (если validation layers включены и функция есть).
    if (validation_ && vkCreateDebugUtilsMessengerEXT != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT dbg = makeDebugInfo();
        if (vkCreateDebugUtilsMessengerEXT(instance_, &dbg, nullptr, &debugMessenger_) != VK_SUCCESS) {
            LOGW("Vulkan: не удалось создать debug messenger");
        }
    }
    if (!createSurface() || !pickPhysicalDevice() || !createDevice()) return false;
    vkApiLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    if (!createSwapchain() || !createImageViews() || !createDepthResources() ||
        !createRenderPass() || !createFramebuffers() || !createShadowResources() ||
        !createDescriptors() || !createSampler() || !createCommandBuffers() ||
        !createDefaultTexture() || !createPipelines() || !createSkinnedPipeline() ||
        !createShadowPipelines() || !createHud() || !createSyncObjects()) {
        return false;
    }

    // ImGui: контекст + Vulkan renderer-бэкенд. Функции грузятся через наш
    // загрузчик (VK_NO_PROTOTYPES). Пул дескрипторов бэкенд создаёт сам
    // (DescriptorPoolSize > 0). Platform-бэкенд (glfw) поднимает main.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
    GameUi::loadFont(assets);  // кириллический шрифт ДО сборки атласа бэкендом
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, imguiVkLoader, instance_);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = instance_;
    ii.PhysicalDevice = physicalDevice_;
    ii.Device = device_;
    ii.QueueFamily = queueFamily_;
    ii.Queue = queue_;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = 2;
    ii.ImageCount = (uint32_t)swapchainImages_.size();
    ii.PipelineInfoMain.RenderPass = renderPass_;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (ImGui_ImplVulkan_Init(&ii)) {
        imguiReady_ = true;
        GameUi::loadSkin(*this, assets);
    } else {
        LOGW("Vulkan: ImGui_ImplVulkan_Init failed (панель не будет рисоваться)");
        ImGui::DestroyContext();  // осиротевший контекст: cleanup его не тронет (imguiReady_=false)
    }

    ready_ = true;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    LOGI("Vulkan renderer initialized: %s (%ux%u)", props.deviceName,
         swapchainExtent_.width, swapchainExtent_.height);
    return true;
}

bool VulkanRenderer::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VBase";
    app.apiVersion = VK_API_VERSION_1_0;

    // Расширения инстанса: surface + платформенное. Десктоп — список от GLFW,
    // Android — VK_KHR_surface + VK_KHR_android_surface.
#ifdef __ANDROID__
    std::vector<const char*> exts = {"VK_KHR_surface", "VK_KHR_android_surface"};
#else
    uint32_t glfwExtCount = 0;
    const char** glfwExt = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (glfwExt == nullptr) {
        LOGE("Vulkan: GLFW не вернул список расширений (нет поддержки Vulkan?)");
        return false;
    }
    std::vector<const char*> exts(glfwExt, glfwExt + glfwExtCount);
#endif

    // Validation layers — если установлены (Vulkan SDK). На машинах без них
    // (или на Android) просто не включаем, рендер работает как есть.
    std::vector<const char*> layers;
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> avail(layerCount);
    if (layerCount > 0) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
    for (const VkLayerProperties& l : avail) {
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            validation_ = true;
            break;
        }
    }
    if (validation_) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        LOGI("Vulkan: validation layers включены");
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    // Messenger в pNext — чтобы ловить сообщения и при create/destroy инстанса.
    // Плюс включаем synchronization validation (ловит гонки семафоров/барьеров).
    VkDebugUtilsMessengerCreateInfoEXT dbg = makeDebugInfo();
    VkValidationFeatureEnableEXT vfEnable[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vf{};
    vf.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = vfEnable;
    vf.pNext = &dbg;
    if (validation_) ci.pNext = &vf;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
    return true;
}

bool VulkanRenderer::createSurface() {
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = (ANativeWindow*)window_;
    VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_),
             "vkCreateAndroidSurfaceKHR");
#else
    VK_CHECK(glfwCreateWindowSurface(instance_, (GLFWwindow*)window_, nullptr, &surface_),
             "glfwCreateWindowSurface");
#endif
    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) { LOGE("Vulkan: нет физических устройств"); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (VkPhysicalDevice dev : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physicalDevice_ = dev;
                queueFamily_ = i;
                return true;
            }
        }
    }
    LOGE("Vulkan: не найдено устройство с графической+present очередью");
    return false;
}

bool VulkanRenderer::createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = devExts;

    VK_CHECK(vkCreateDevice(physicalDevice_, &ci, nullptr, &device_), "vkCreateDevice");
    return true;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    // Формат: предпочитаем B8G8R8A8_UNORM/SRGB, иначе первый доступный.
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    if (fmtCount == 0) { LOGE("Vulkan: нет форматов surface"); return false; }
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    swapchainFormat_ = chosen.format;

    swapchainExtent_ = chooseSwapchainExtent(caps, desiredW_, desiredH_);
    if (swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
        return false;  // окно свёрнуто — попробуем позже
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    const VkSurfaceTransformFlagBitsKHR preTransform = choosePreTransform(caps);

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = swapchainExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // одна очередь (graphics+present)
    ci.preTransform = preTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // гарантированный (vsync)
    ci.clipped = VK_TRUE;

    LOGI("Vulkan swapchain %ux%u preTransform=0x%x (current=0x%x supported=0x%x)",
         swapchainExtent_.width, swapchainExtent_.height,
         (unsigned)preTransform, (unsigned)caps.currentTransform,
         (unsigned)caps.supportedTransforms);

    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
    return true;
}

bool VulkanRenderer::createImageViews() {
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = swapchainImages_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = swapchainFormat_;
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &ci, nullptr, &swapchainImageViews_[i]),
                 "vkCreateImageView");
    }
    return true;
}

bool VulkanRenderer::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapchainFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Синхронизация с предыдущим кадром: цвет пишется в COLOR_ATTACHMENT_OUTPUT,
    // глубина — в EARLY/LATE_FRAGMENT_TESTS (store на LATE). Нужно включить оба
    // fragment-test стейджа и depth-write в src, иначе WAW-hazard по общему
    // depth-буферу (он один на все кадры).
    const VkPipelineStageFlags gfx = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags wr = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = gfx;
    dep.srcAccessMask = wr;
    dep.dstStageMask = gfx;
    dep.dstAccessMask = wr;

    VkAttachmentDescription attachments[] = {color, depth};
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 2;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_), "vkCreateRenderPass");
    return true;
}

bool VulkanRenderer::createFramebuffers() {
    framebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        VkImageView attachments[] = {swapchainImageViews_[i], depthView_};
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = renderPass_;
        ci.attachmentCount = 2;
        ci.pAttachments = attachments;
        ci.width = swapchainExtent_.width;
        ci.height = swapchainExtent_.height;
        ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]),
                 "vkCreateFramebuffer");
    }
    return true;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    LOGE("Vulkan: подходящий тип памяти не найден");
    return 0;
}

bool VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags memProps, VkBuffer& buf,
                                  VkDeviceMemory& mem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &buf), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, memProps);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &mem), "vkAllocateMemory(buffer)");
    vkBindBufferMemory(device_, buf, mem, 0);
    return true;
}

VkShaderModule VulkanRenderer::loadShaderModule(const char* path) {
    std::vector<uint8_t> code;
    if (assets_ == nullptr || !assets_->read(path, code) || code.empty() ||
        (code.size() % 4) != 0) {
        LOGE("Vulkan: не удалось прочитать SPIR-V: %s", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS) {
        LOGE("Vulkan: vkCreateShaderModule failed: %s", path);
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool VulkanRenderer::createDepthResources() {
    // D32_SFLOAT если поддержан для depth-attachment (обычно да), иначе D24S8.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_D32_SFLOAT, &fp);
    depthFormat_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                       ? VK_FORMAT_D32_SFLOAT
                       : VK_FORMAT_D24_UNORM_S8_UINT;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = depthFormat_;
    ii.extent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ii, nullptr, &depthImage_), "vkCreateImage(depth)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, depthImage_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &depthMemory_), "vkAllocateMemory(depth)");
    vkBindImageMemory(device_, depthImage_, depthMemory_, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = depthImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = depthFormat_;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &depthView_), "vkCreateImageView(depth)");
    return true;
}

bool VulkanRenderer::createShadowResources() {
    shadowFormat_ = depthFormat_;  // тот же depth-формат, что и основной буфер

    // Depth-only render pass: очистка -> сохранение, финальный layout SHADER_READ,
    // чтобы основной проход мог сэмплить карту теней.
    VkAttachmentDescription depth{};
    depth.format = shadowFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    // Вход: защита от чтения прошлого кадра (fragment read) перед depth-write.
    // Выход: depth-write доступен фрагментной выборке основного прохода.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &depth;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &shadowRenderPass_), "shadowRenderPass");

    // Compare-сэмплер: LINEAR + compareOp LESS_OR_EQUAL -> аппаратный PCF 2x2.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_, &si, nullptr, &shadowSampler_), "shadowSampler");
    return true;
}

bool VulkanRenderer::createDescriptors() {
    constexpr uint32_t kMaxMaterials = 64;  // запас дескрипторов на материалы

    // set 0: binding 0 = uniform buffer (Frame), vertex+fragment;
    //        binding 1 = combined image sampler (карта теней), fragment.
    VkDescriptorSetLayoutBinding b0[2]{};
    b0[0].binding = 0;
    b0[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b0[0].descriptorCount = 1;
    b0[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b0[1].binding = 1;
    b0[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0[1].descriptorCount = 1;
    b0[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo l0{};
    l0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    l0.bindingCount = 2;
    l0.pBindings = b0;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &l0, nullptr, &setLayout0_), "setLayout0");

    // set 1: binding 0 = albedo, binding 1 = нормал-карта (оба combined image sampler,
    // fragment). Делят: материалы, скиннинг-текстуры, белая заглушка, HUD-шрифт —
    // все они пишут ОБА binding'а (нормаль по умолчанию — плоская 1x1).
    VkDescriptorSetLayoutBinding b1[2]{};
    b1[0].binding = 0;
    b1[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1[0].descriptorCount = 1;
    b1[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b1[1].binding = 1;
    b1[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1[1].descriptorCount = 1;
    b1[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo l1{};
    l1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    l1.bindingCount = 2;
    l1.pBindings = b1;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &l1, nullptr, &setLayout1_), "setLayout1");

    // set 2: binding 0 = storage buffer (кости скиннинга), vertex.
    VkDescriptorSetLayoutBinding b2{};
    b2.binding = 0;
    b2.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b2.descriptorCount = 1;
    b2.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo l2{};
    l2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    l2.bindingCount = 1;
    l2.pBindings = &b2;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &l2, nullptr, &setLayout2_), "setLayout2");

    VkDescriptorPoolSize ps[3]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = kMaxFramesInFlight;
    // set1 теперь 2 сэмплера (albedo+normal) на материал -> 2*kMaxMaterials;
    // +kMaxFramesInFlight: карта теней (set0 binding1) на каждый кадровый set.
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = 2 * kMaxMaterials + kMaxFramesInFlight;
    ps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[2].descriptorCount = kMaxFramesInFlight;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxFramesInFlight + kMaxMaterials + kMaxFramesInFlight;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &descriptorPool_), "descriptorPool");

    frames_.resize(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        // UBO host-visible + постоянно замаплен (обновляем каждый кадр).
        if (!createBuffer(sizeof(FrameUBOData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frames_[i].ubo, frames_[i].uboMem)) {
            return false;
        }
        vkMapMemory(device_, frames_[i].uboMem, 0, sizeof(FrameUBOData), 0, &frames_[i].uboMapped);

        // Инстанс-буфер: kMaxInstances матриц модели (host-visible, замаплен).
        const VkDeviceSize instSize = (VkDeviceSize)kMaxInstances * 16 * sizeof(float);
        if (!createBuffer(instSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frames_[i].inst, frames_[i].instMem)) {
            return false;
        }
        vkMapMemory(device_, frames_[i].instMem, 0, instSize, 0, &frames_[i].instMapped);

        // SSBO костей: kMaxBones матриц (host-visible, замаплен).
        const VkDeviceSize bonesSize = (VkDeviceSize)kMaxBones * 16 * sizeof(float);
        if (!createBuffer(bonesSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frames_[i].bones, frames_[i].bonesMem)) {
            return false;
        }
        vkMapMemory(device_, frames_[i].bonesMem, 0, bonesSize, 0, &frames_[i].bonesMapped);

        // Дескрипторы: set0 (UBO) и set2 (кости).
        VkDescriptorSetLayout layouts[2] = {setLayout0_, setLayout2_};
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(device_, &ai, sets), "allocDescriptorSet");
        frames_[i].set = sets[0];
        frames_[i].bonesSet = sets[1];

        // Карта теней этого кадра: depth-образ (attachment + sampled) + view + framebuffer.
        VkImageCreateInfo sii{};
        sii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        sii.imageType = VK_IMAGE_TYPE_2D;
        sii.format = shadowFormat_;
        sii.extent = {kShadowSize, kShadowSize, 1};
        sii.mipLevels = 1;
        sii.arrayLayers = 1;
        sii.samples = VK_SAMPLE_COUNT_1_BIT;
        sii.tiling = VK_IMAGE_TILING_OPTIMAL;
        sii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        sii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &sii, nullptr, &frames_[i].shadowImage), "shadowImage");
        VkMemoryRequirements sreq{};
        vkGetImageMemoryRequirements(device_, frames_[i].shadowImage, &sreq);
        VkMemoryAllocateInfo sai{};
        sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        sai.allocationSize = sreq.size;
        sai.memoryTypeIndex = findMemoryType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &sai, nullptr, &frames_[i].shadowMem), "shadowMem");
        vkBindImageMemory(device_, frames_[i].shadowImage, frames_[i].shadowMem, 0);
        VkImageViewCreateInfo svi{};
        svi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        svi.image = frames_[i].shadowImage;
        svi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        svi.format = shadowFormat_;
        svi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        svi.subresourceRange.levelCount = 1;
        svi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &svi, nullptr, &frames_[i].shadowView), "shadowView");
        VkFramebufferCreateInfo sfi{};
        sfi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        sfi.renderPass = shadowRenderPass_;
        sfi.attachmentCount = 1;
        sfi.pAttachments = &frames_[i].shadowView;
        sfi.width = kShadowSize;
        sfi.height = kShadowSize;
        sfi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &sfi, nullptr, &frames_[i].shadowFb), "shadowFb");

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = frames_[i].ubo;
        uboInfo.range = sizeof(FrameUBOData);
        VkDescriptorBufferInfo bonesInfo{};
        bonesInfo.buffer = frames_[i].bones;
        bonesInfo.range = bonesSize;
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = shadowSampler_;
        shadowInfo.imageView = frames_[i].shadowView;
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w[3]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = frames_[i].set;
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].descriptorCount = 1;
        w[0].pBufferInfo = &uboInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = frames_[i].bonesSet;
        w[1].dstBinding = 0;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].descriptorCount = 1;
        w[1].pBufferInfo = &bonesInfo;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet = frames_[i].set;
        w[2].dstBinding = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].descriptorCount = 1;
        w[2].pImageInfo = &shadowInfo;
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }
    return true;
}

bool VulkanRenderer::createSampler() {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;  // для тайлинга (шахматка пола)
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_, &si, nullptr, &sampler_), "vkCreateSampler");
    return true;
}

bool VulkanRenderer::createDefaultTexture() {
    const uint8_t white[4] = {255, 255, 255, 255};
    VkTexture t;
    if (!uploadTexture(1, 1, white, t)) return false;
    whiteImage_ = t.image;
    whiteMem_ = t.mem;
    whiteView_ = t.view;

    // Плоская нормаль 1x1 (0,0,1) для материалов без нормал-карты.
    const uint8_t flat[4] = {128, 128, 255, 255};
    VkTexture ft;
    if (!uploadTexture(1, 1, flat, ft)) return false;
    flatNormalImage_ = ft.image;
    flatNormalMem_ = ft.mem;
    flatNormalView_ = ft.view;

    whiteSet_ = allocMaterialSet(whiteView_, flatNormalView_);  // set 1 для объектов без текстуры
    return true;
}

bool VulkanRenderer::createPipelines() {
    // Общий layout: set 0 (Frame UBO) + set 1 (albedo) + push-константы.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // цвет нужен только фрагментнику
    pcr.offset = 0;
    pcr.size = sizeof(PushData);
    VkDescriptorSetLayout setLayouts[] = {setLayout0_, setLayout1_};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = setLayouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_), "pipelineLayout");

    // Модули: lit.vert используется для Lit и Unlit; phong.vert — для Phong.
    VkShaderModule litVert = loadShaderModule("shaders/vk/lit.vert.spv");
    VkShaderModule litFrag = loadShaderModule("shaders/vk/lit.frag.spv");
    VkShaderModule unlitFrag = loadShaderModule("shaders/vk/unlit.frag.spv");
    VkShaderModule phongVert = loadShaderModule("shaders/vk/phong.vert.spv");
    VkShaderModule phongFrag = loadShaderModule("shaders/vk/phong.frag.spv");
    bool ok = litVert && litFrag && unlitFrag && phongVert && phongFrag;
    if (ok) {
        ok = createGraphicsPipeline(litVert, litFrag, pipelines_[(int)ShaderType::Lit]) &&
             createGraphicsPipeline(litVert, unlitFrag, pipelines_[(int)ShaderType::Unlit]) &&
             createGraphicsPipeline(phongVert, phongFrag, pipelines_[(int)ShaderType::Phong]);
    }
    if (litVert) vkDestroyShaderModule(device_, litVert, nullptr);
    if (litFrag) vkDestroyShaderModule(device_, litFrag, nullptr);
    if (unlitFrag) vkDestroyShaderModule(device_, unlitFrag, nullptr);
    if (phongVert) vkDestroyShaderModule(device_, phongVert, nullptr);
    if (phongFrag) vkDestroyShaderModule(device_, phongFrag, nullptr);
    return ok;
}

bool VulkanRenderer::createGraphicsPipeline(VkShaderModule vs, VkShaderModule fs, VkPipeline& out) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // Вершинный ввод: binding 0 — вершина (pos3+normal3+uv2=32 байта, per-vertex);
    // binding 1 — инстансная матрица iModel (mat4 = 64 байта, per-instance).
    VkVertexInputBindingDescription binds[2]{};
    binds[0].binding = 0;
    binds[0].stride = sizeof(Vertex);  // pos3+normal3+uv2+tangent3 (тангент читает только GL пока)
    binds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binds[1].binding = 1;
    binds[1].stride = sizeof(float) * 16;
    binds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription attrs[8]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6};
    // iModel (mat4) -> 4 vec4 в локациях 3..6, binding 1.
    attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 4};
    attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8};
    attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 12};
    attrs[7] = {7, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 8};  // тангент (binding 0)
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 2;
    vin.pVertexBindingDescriptions = binds;
    vin.vertexAttributeDescriptionCount = 8;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // Фаза 1: без отсечения (winding не важен)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dynState;
    gp.layout = pipelineLayout_;  // общий layout (создан в createPipelines)
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
    if (r != VK_SUCCESS) {
        LOGE("Vulkan: vkCreateGraphicsPipelines (VkResult=%d)", (int)r);
        return false;
    }
    return true;
}

bool VulkanRenderer::createSkinnedPipeline() {
    // Layout: set0 (Frame) + set1 (albedo) + set2 (кости) + push (model/color/boneOffset).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(SkinnedPushData);
    VkDescriptorSetLayout sls[3] = {setLayout0_, setLayout1_, setLayout2_};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 3;
    pli.pSetLayouts = sls;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &skinnedPipelineLayout_),
             "skinnedPipelineLayout");

    VkShaderModule vs = loadShaderModule("shaders/vk/skin.vert.spv");
    VkShaderModule fs = loadShaderModule("shaders/vk/skin.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // SkinnedVertex = pos3+normal3+uv2+joints4+weights4 = 16 float (64 байта).
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(float) * 16;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8};   // joints
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 12};  // weights
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 5;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dynState;
    gp.layout = skinnedPipelineLayout_;
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &skinnedPipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) {
        LOGE("Vulkan: skinned vkCreateGraphicsPipelines (VkResult=%d)", (int)r);
        return false;
    }
    return true;
}

bool VulkanRenderer::createShadowPipelines() {
    // Depth-only пайплайны прохода теней: только вершинный стейдж (нет цвета),
    // рендерят в shadowRenderPass_. Переиспользуют существующие layout'ы
    // (pipelineLayout_ для инстансного, skinnedPipelineLayout_ для скиннинга).
    VkShaderModule instVs = loadShaderModule("shaders/vk/shadow_depth.vert.spv");
    VkShaderModule skinVs = loadShaderModule("shaders/vk/shadow_skin.vert.spv");
    if (instVs == VK_NULL_HANDLE || skinVs == VK_NULL_HANDLE) return false;

    // Общие стейджи (кроме vertex-input) для обоих пайплайнов.
    auto buildDepthPipeline = [&](VkShaderModule vs, const VkPipelineVertexInputStateCreateInfo& vin,
                                  VkPipelineLayout layout, VkPipeline& out) -> bool {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vs;
        stage.pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendStateCreateInfo cb{};  // без color-attachment
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 0;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynState{};
        dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynState.dynamicStateCount = 2;
        dynState.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 1;
        gp.pStages = &stage;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dynState;
        gp.layout = layout;
        gp.renderPass = shadowRenderPass_;
        gp.subpass = 0;
        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
        if (r != VK_SUCCESS) { LOGE("Vulkan: shadow pipeline (VkResult=%d)", (int)r); return false; }
        return true;
    };

    // Инстансный: те же binding'и, что у lit (вершина 8 float + инстанс-матрица 16).
    // depth-шейдер читает только pos (loc 0) и iModel (loc 3..6) — normal/uv не объявляем
    // (иначе валидация ругается "attribute not consumed").
    VkVertexInputBindingDescription ibinds[2]{};
    ibinds[0].binding = 0; ibinds[0].stride = sizeof(Vertex); ibinds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ibinds[1].binding = 1; ibinds[1].stride = sizeof(float) * 16; ibinds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription iattrs[5]{};
    iattrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    iattrs[1] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    iattrs[2] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 4};
    iattrs[3] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8};
    iattrs[4] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 12};
    VkPipelineVertexInputStateCreateInfo ivin{};
    ivin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    ivin.vertexBindingDescriptionCount = 2;
    ivin.pVertexBindingDescriptions = ibinds;
    ivin.vertexAttributeDescriptionCount = 5;
    ivin.pVertexAttributeDescriptions = iattrs;

    // Скиннинг: SkinnedVertex = 16 float; depth-шейдер читает pos(0), joints(3), weights(4).
    VkVertexInputBindingDescription sbind{};
    sbind.binding = 0; sbind.stride = sizeof(float) * 16; sbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription sattrs[3]{};
    sattrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    sattrs[1] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8};
    sattrs[2] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 12};
    VkPipelineVertexInputStateCreateInfo svin{};
    svin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    svin.vertexBindingDescriptionCount = 1;
    svin.pVertexBindingDescriptions = &sbind;
    svin.vertexAttributeDescriptionCount = 3;
    svin.pVertexAttributeDescriptions = sattrs;

    bool ok = buildDepthPipeline(instVs, ivin, pipelineLayout_, shadowPipeline_) &&
              buildDepthPipeline(skinVs, svin, skinnedPipelineLayout_, shadowSkinPipeline_);
    vkDestroyShaderModule(device_, instVs, nullptr);
    vkDestroyShaderModule(device_, skinVs, nullptr);
    return ok;
}

bool VulkanRenderer::createHud() {
    // Атлас шрифта (RGBA, альфа = покрытие глифа) -> GPU-текстура.
    TextureData atlas = makeFontAtlas();
    VkTexture ft;
    if (!uploadTexture(atlas.width, atlas.height, atlas.rgba.data(), ft)) return false;
    fontImage_ = ft.image;
    fontMem_ = ft.mem;
    fontView_ = ft.view;

    // NEAREST + CLAMP — чёткий пиксельный шрифт без затекания соседних глифов.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(device_, &si, nullptr, &fontSampler_), "fontSampler");

    // Дескриптор атласа (set 0 в HUD-пайплайне = тот же combined-sampler layout).
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = descriptorPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout1_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &dai, &fontSet_), "fontSet");
    VkDescriptorImageInfo fimgs[2]{};
    fimgs[0].sampler = fontSampler_;
    fimgs[0].imageView = fontView_;
    fimgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fimgs[1].sampler = sampler_;               // binding1 не используется HUD-шейдером;
    fimgs[1].imageView = flatNormalView_;      // пишем плоскую нормаль для полного сета
    fimgs[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet ws[2]{};
    ws[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[0].dstSet = fontSet_;
    ws[0].dstBinding = 0;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ws[0].descriptorCount = 1;
    ws[0].pImageInfo = &fimgs[0];
    ws[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[1].dstSet = fontSet_;
    ws[1].dstBinding = 1;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ws[1].descriptorCount = 1;
    ws[1].pImageInfo = &fimgs[1];
    vkUpdateDescriptorSets(device_, 2, ws, 0, nullptr);

    // Per-frame динамический вершинный буфер HUD (x,y,u,v на вершину).
    const VkDeviceSize hudSize = (VkDeviceSize)kMaxHudVerts * 4 * sizeof(float);
    for (FrameRes& f : frames_) {
        if (!createBuffer(hudSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          f.hud, f.hudMem)) {
            return false;
        }
        vkMapMemory(device_, f.hudMem, 0, hudSize, 0, &f.hudMapped);
    }

    // Пайплайн HUD: layout = [setLayout1_] + push (uScreen + uColor).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(HudPushData);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout1_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &hudPipelineLayout_), "hudPipelineLayout");

    VkShaderModule vs = loadShaderModule("shaders/vk/hud.vert.spv");
    VkShaderModule fs = loadShaderModule("shaders/vk/hud.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 4 * sizeof(float);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};              // pos (пиксели)
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)};  // uv
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;   // оверлей поверх всего
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dynState;
    gp.layout = hudPipelineLayout_;
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &hudPipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) {
        LOGE("Vulkan: hud vkCreateGraphicsPipelines (VkResult=%d)", (int)r);
        return false;
    }
    return true;
}

bool VulkanRenderer::createCommandBuffers() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &commandPool_), "vkCreateCommandPool");

    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)commandBuffers_.size();
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()),
             "vkAllocateCommandBuffers");
    return true;
}

bool VulkanRenderer::createSyncObjects() {
    imageAvailable_.resize(kMaxFramesInFlight);
    inFlight_.resize(kMaxFramesInFlight);
    renderFinished_.resize(swapchainImages_.size());  // на образ (без гонок семафоров)
    imagesInFlight_.assign(swapchainImages_.size(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // первый кадр не ждёт

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]), "semaphore");
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]), "fence");
    }
    for (size_t i = 0; i < renderFinished_.size(); ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]), "semaphore");
    }
    return true;
}

void VulkanRenderer::cleanupSwapchain() {
    // Depth зависит от размера — сносим вместе со swapchain.
    if (depthView_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_ != VK_NULL_HANDLE) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
    if (depthMemory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE; }
    for (VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    for (VkImageView v : swapchainImageViews_) vkDestroyImageView(device_, v, nullptr);
    swapchainImageViews_.clear();
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
    renderFinished_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::recreateSwapchain() {
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    if (!createSwapchain()) return false;   // вернёт false при свёрнутом окне
    if (!createImageViews() || !createDepthResources() || !createFramebuffers()) return false;
    // renderFinished_ пересоздаётся под новое число образов.
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished_.resize(swapchainImages_.size());
    for (size_t i = 0; i < renderFinished_.size(); ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]), "semaphore");
    }
    imagesInFlight_.assign(swapchainImages_.size(), VK_NULL_HANDLE);
    return true;
}

bool VulkanRenderer::maybeRecreateSwapchain(VkResult reason) {
    if (reason == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return true;
    }
    if (reason != VK_SUBOPTIMAL_KHR) return false;
    // IDENTITY при currentTransform=ROTATE_90 на Android даёт SUBOPTIMAL каждый кадр:
    // композитор сам крутит буфер. Пересоздавать swapchain бессмысленно, пока extent
    // тот же (иначе — бесконечный recreate и мерцание).
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    const VkExtent2D e = chooseSwapchainExtent(caps, desiredW_, desiredH_);
    if (e.width == swapchainExtent_.width && e.height == swapchainExtent_.height) return false;
    recreateSwapchain();
    return true;
}

void VulkanRenderer::setSurfaceSize(int width, int height) {
    desiredW_ = width > 0 ? width : 1;
    desiredH_ = height > 0 ? height : 1;
}

float VulkanRenderer::aspectRatio() const {
    return swapchainExtent_.height > 0
               ? (float)swapchainExtent_.width / (float)swapchainExtent_.height
               : 1.0f;
}

VkCommandBuffer VulkanRenderer::beginOneTime() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanRenderer::endOneTime(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);  // синхронно (загрузки — редкие, при построении сцены)
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

bool VulkanRenderer::uploadTexture(uint32_t w, uint32_t h, const void* rgba, VkTexture& out) {
    VkDeviceSize size = (VkDeviceSize)w * h * 4;

    // staging-буфер с пикселями.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem)) {
        return false;
    }
    void* p = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &p);
    std::memcpy(p, rgba, (size_t)size);
    vkUnmapMemory(device_, stagingMem);

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ii, nullptr, &out.image), "vkCreateImage(tex)");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, out.image, &req);
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ma, nullptr, &out.mem), "vkAllocateMemory(tex)");
    vkBindImageMemory(device_, out.image, out.mem, 0);

    // UNDEFINED -> TRANSFER_DST -> копирование -> SHADER_READ_ONLY.
    VkCommandBuffer cmd = beginOneTime();
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = out.image;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;

    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    endOneTime(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &out.view), "vkCreateImageView(tex)");
    return true;
}

VkDescriptorSet VulkanRenderer::allocMaterialSet(VkImageView albedo, VkImageView normal) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout1_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        LOGE("Vulkan: не удалось выделить дескриптор материала (пул исчерпан?)");
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo imgs[2]{};
    imgs[0].sampler = sampler_;
    imgs[0].imageView = albedo;
    imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgs[1].sampler = sampler_;
    imgs[1].imageView = normal;
    imgs[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].descriptorCount = 1;
    w[0].pImageInfo = &imgs[0];
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = set;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].descriptorCount = 1;
    w[1].pImageInfo = &imgs[1];
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    return set;
}

MeshHandle VulkanRenderer::createMesh(const MeshData& data) {
    VkDeviceSize vsize = data.vertices.size() * sizeof(Vertex);
    VkDeviceSize isize = data.indices.size() * sizeof(uint32_t);
    if (vsize == 0 || isize == 0) return 0;

    VkMesh mesh;
    mesh.indexCount = (uint32_t)data.indices.size();
    const VkMemoryPropertyFlags hostVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Host-visible буферы (Фаза 1: без staging; меши статичные и небольшие).
    if (!createBuffer(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible,
                      mesh.vbuf, mesh.vmem)) return 0;
    void* p = nullptr;
    vkMapMemory(device_, mesh.vmem, 0, vsize, 0, &p);
    std::memcpy(p, data.vertices.data(), (size_t)vsize);
    vkUnmapMemory(device_, mesh.vmem);

    if (!createBuffer(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostVisible,
                      mesh.ibuf, mesh.imem)) return 0;
    vkMapMemory(device_, mesh.imem, 0, isize, 0, &p);
    std::memcpy(p, data.indices.data(), (size_t)isize);
    vkUnmapMemory(device_, mesh.imem);

    meshes_.push_back(mesh);
    return (MeshHandle)meshes_.size();  // handle = индекс + 1
}

TextureHandle VulkanRenderer::createTexture(const TextureData& data, bool /*clampEdges*/) {
    if (data.width == 0 || data.height == 0 || data.rgba.empty()) return 0;
    VkTexture t;
    if (!uploadTexture(data.width, data.height, data.rgba.data(), t)) return 0;
    textures_.push_back(t);
    // Скиннинг-объекты нормал-карты пока не используют -> плоская нормаль по умолчанию.
    textureSets_.push_back(allocMaterialSet(t.view, flatNormalView_));  // set 1
    imguiTextureSets_.push_back(VK_NULL_HANDLE);       // лениво в getImGuiTexture
    return (TextureHandle)textures_.size();  // handle = индекс + 1
}

uint64_t VulkanRenderer::getImGuiTexture(TextureHandle handle) {
    if (!imguiReady_ || handle == 0 || handle > textures_.size()) return 0;
    const size_t i = (size_t)handle - 1;
    if (imguiTextureSets_.size() < textures_.size())
        imguiTextureSets_.resize(textures_.size(), VK_NULL_HANDLE);
    if (imguiTextureSets_[i] == VK_NULL_HANDLE) {
        imguiTextureSets_[i] = ImGui_ImplVulkan_AddTexture(
            textures_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return (uint64_t)(uintptr_t)imguiTextureSets_[i];
}

void VulkanRenderer::releaseImGuiTexture(uint64_t imguiTexId) {
    if (imguiTexId == 0 || !imguiReady_) return;
    auto* set = (VkDescriptorSet)(uintptr_t)imguiTexId;
    for (VkDescriptorSet& s : imguiTextureSets_) {
        if (s == set) {
            ImGui_ImplVulkan_RemoveTexture(s);
            s = VK_NULL_HANDLE;
            return;
        }
    }
}

MaterialHandle VulkanRenderer::createMaterial(const MaterialDesc& desc) {
    VkImageView view = whiteView_;  // без текстуры -> белая 1x1
    if (desc.albedo >= 1 && desc.albedo <= textures_.size()) {
        view = textures_[desc.albedo - 1].view;
    }
    VkImageView normalView = flatNormalView_;  // без нормал-карты -> плоская нормаль
    if (desc.normal >= 1 && desc.normal <= textures_.size()) {
        normalView = textures_[desc.normal - 1].view;
    }
    VkMaterial mat;
    mat.shader = (uint32_t)desc.shader;
    mat.color = desc.baseColor;
    mat.set = allocMaterialSet(view, normalView);
    materials_.push_back(mat);
    return (MaterialHandle)materials_.size();
}

SkinnedHandle VulkanRenderer::createSkinnedMesh(const SkinnedModel& model) {
    VkDeviceSize vsize = model.vertices.size() * sizeof(SkinnedVertex);
    VkDeviceSize isize = model.indices.size() * sizeof(uint32_t);
    if (vsize == 0 || isize == 0) return 0;

    VkMesh m;
    m.indexCount = (uint32_t)model.indices.size();
    const VkMemoryPropertyFlags hostVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!createBuffer(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible, m.vbuf, m.vmem)) return 0;
    void* p = nullptr;
    vkMapMemory(device_, m.vmem, 0, vsize, 0, &p);
    std::memcpy(p, model.vertices.data(), (size_t)vsize);
    vkUnmapMemory(device_, m.vmem);

    if (!createBuffer(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostVisible, m.ibuf, m.imem)) return 0;
    vkMapMemory(device_, m.imem, 0, isize, 0, &p);
    std::memcpy(p, model.indices.data(), (size_t)isize);
    vkUnmapMemory(device_, m.imem);

    skinnedMeshes_.push_back(m);
    return (SkinnedHandle)skinnedMeshes_.size();  // handle = индекс + 1
}

void VulkanRenderer::renderFrame(const RenderFrame& frame) {
    if (!ready_) return;

    vkWaitForFences(device_, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                         imageAvailable_[currentFrame_], VK_NULL_HANDLE,
                                         &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
        // Пересозданный swapchain несовместим с уже acquired imageIndex — кадр бросаем.
        if (maybeRecreateSwapchain(acq) || acq == VK_ERROR_OUT_OF_DATE_KHR) return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOGE("Vulkan: vkAcquireNextImageKHR (VkResult=%d)", (int)acq);
        return;
    }

    // Если этот образ ещё занят предыдущим кадром — дождаться его fence (иначе
    // гонка: переиспользуем образ и его renderFinished-семафор досрочно).
    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight_[imageIndex] = inFlight_[currentFrame_];

    vkResetFences(device_, 1, &inFlight_[currentFrame_]);

    // Обновить кадровый UBO (viewProj с коррекцией клипа, свет, позиция камеры,
    // матрица света для теней, туман).
    FrameUBOData ubo{};
    Mat4 vp = vulkanClipFix() * (frame.proj * frame.view);
    std::memcpy(ubo.viewProj, vp.m, sizeof(ubo.viewProj));

    // Матрица глазами света (directional): орто-коробка вокруг центра арены.
    // Как в GL-бэкенде, но с коррекцией клипа Vulkan (глубина [0,1], Y вниз).
    const Vec3 L = normalize(frame.lightDir);
    const float R = frame.shadowRadius > 1.0f ? frame.shadowRadius : 14.0f;
    const float sdist = R * 2.5f;
    const Vec3 center{0.0f, 0.0f, 0.0f};
    const Vec3 up = (std::fabs(L.y) > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Mat4 lightVP = vulkanClipFix() * Mat4::ortho(-R, R, -R, R, 0.1f, sdist + R) *
                         Mat4::lookAt(center + L * sdist, center, up);
    std::memcpy(ubo.lightVP, lightVP.m, sizeof(ubo.lightVP));

    ubo.lightDir[0] = L.x;
    ubo.lightDir[1] = L.y;
    ubo.lightDir[2] = L.z;
    ubo.lightDir[3] = frame.shadowBias;
    ubo.viewPos[0] = frame.cameraPos.x;
    ubo.viewPos[1] = frame.cameraPos.y;
    ubo.viewPos[2] = frame.cameraPos.z;
    ubo.fogColor[0] = frame.fogColor.x;
    ubo.fogColor[1] = frame.fogColor.y;
    ubo.fogColor[2] = frame.fogColor.z;
    ubo.fogColor[3] = frame.fogDensity;
    std::memcpy(frames_[currentFrame_].uboMapped, &ubo, sizeof(ubo));

    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);

    // --- Предзаливка инстансов и костей (нужны обоим проходам: теневому и основному) ---
    struct Batch { MeshHandle mesh; MaterialHandle material; uint32_t first; uint32_t count; };
    std::vector<Batch> batches;
    {
        struct Acc { MeshHandle mesh; MaterialHandle material; std::vector<const Mat4*> models; };
        std::vector<Acc> accs;
        for (const RenderItem& it : frame.items) {
            if (it.mesh == 0 || it.mesh > meshes_.size()) continue;
            if (it.material == 0 || it.material > materials_.size()) continue;
            Acc* a = nullptr;
            for (Acc& c : accs) if (c.mesh == it.mesh && c.material == it.material) { a = &c; break; }
            if (a == nullptr) { accs.push_back({it.mesh, it.material, {}}); a = &accs.back(); }
            a->models.push_back(&it.model);
        }
        char* instBase = (char*)frames_[currentFrame_].instMapped;
        uint32_t total = 0;
        for (Acc& a : accs) {
            uint32_t first = total;
            for (const Mat4* mm : a.models) {
                if (total >= kMaxInstances) break;
                std::memcpy(instBase + (size_t)total * 16 * sizeof(float), mm->m, 16 * sizeof(float));
                ++total;
            }
            uint32_t count = total - first;
            if (count > 0) batches.push_back({a.mesh, a.material, first, count});
        }
    }
    struct SkinDraw { const SkinnedItem* item; uint32_t boneOffset; };
    std::vector<SkinDraw> skinDraws;
    {
        char* bonesBase = (char*)frames_[currentFrame_].bonesMapped;
        uint32_t boneTotal = 0;
        for (const SkinnedItem& it : frame.skinned) {
            if (it.mesh == 0 || it.mesh > skinnedMeshes_.size()) continue;
            uint32_t nb = (uint32_t)it.joints.size();
            if (boneTotal + nb > kMaxBones) break;
            uint32_t off = boneTotal;
            for (uint32_t j = 0; j < nb; ++j)
                std::memcpy(bonesBase + (size_t)(off + j) * 16 * sizeof(float), it.joints[j].m, 16 * sizeof(float));
            boneTotal += nb;
            skinDraws.push_back({&it, off});
        }
    }

    // --- Проход 1: карта теней (глазами света) в per-frame depth-образ ---
    {
        VkClearValue sclear{};
        sclear.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo srp{};
        srp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        srp.renderPass = shadowRenderPass_;
        srp.framebuffer = frames_[currentFrame_].shadowFb;
        srp.renderArea.extent = {kShadowSize, kShadowSize};
        srp.clearValueCount = 1;
        srp.pClearValues = &sclear;
        vkCmdBeginRenderPass(cmd, &srp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport sv{};
        sv.width = (float)kShadowSize; sv.height = (float)kShadowSize; sv.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &sv);
        VkRect2D ss{}; ss.extent = {kShadowSize, kShadowSize};
        vkCmdSetScissor(cmd, 0, 1, &ss);
        if (frame.shadowsEnabled) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                    &frames_[currentFrame_].set, 0, nullptr);
            for (const Batch& b : batches) {
                const VkMesh& m = meshes_[b.mesh - 1];
                VkBuffer vb[2] = {m.vbuf, frames_[currentFrame_].inst};
                VkDeviceSize of[2] = {0, 0};
                vkCmdBindVertexBuffers(cmd, 0, 2, vb, of);
                vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, m.indexCount, b.count, 0, 0, b.first);
            }
            if (!skinDraws.empty() && shadowSkinPipeline_ != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowSkinPipeline_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout_, 0, 1,
                                        &frames_[currentFrame_].set, 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout_, 2, 1,
                                        &frames_[currentFrame_].bonesSet, 0, nullptr);
                for (const SkinDraw& sd : skinDraws) {
                    const VkMesh& m = skinnedMeshes_[sd.item->mesh - 1];
                    SkinnedPushData push{};
                    std::memcpy(push.model, sd.item->model.m, sizeof(push.model));
                    push.boneOffset = (int)sd.boneOffset;
                    vkCmdPushConstants(cmd, skinnedPipelineLayout_,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(SkinnedPushData), &push);
                    VkDeviceSize of = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &of);
                    vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
                }
            }
        }
        vkCmdEndRenderPass(cmd);
    }

    // --- Проход 2: основной кадр ---
    VkClearValue clears[2]{};
    // При тумане фон — его цвет (gamma), чтобы дальняя геометрия сливалась с горизонтом.
    if (frame.fogDensity > 0.0f) {
        const float ig = 1.0f / 2.2f;
        clears[0].color = {{std::pow(frame.fogColor.x, ig), std::pow(frame.fogColor.y, ig),
                            std::pow(frame.fogColor.z, ig), 1.0f}};
    } else {
        clears[0].color = {{0.10f, 0.12f, 0.15f, 1.0f}};
    }
    clears[1].depthStencil = {1.0f, 0};               // дальняя плоскость
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffers_[imageIndex];
    rp.renderArea.extent = swapchainExtent_;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = (float)swapchainExtent_.width;
    viewport.height = (float)swapchainExtent_.height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // set 0 (кадровый UBO + карта теней binding1) — общий, привязываем один раз.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &frames_[currentFrame_].set, 0, nullptr);

    // Окружение: инстансные draw'ы по предзаготовленным батчам (инстансы уже залиты).
    uint32_t curPipeline = 0xFFFFFFFFu;
    for (const Batch& b : batches) {
        const VkMesh& m = meshes_[b.mesh - 1];
        const VkMaterial& mat = materials_[b.material - 1];
        uint32_t sh = (mat.shader < 3) ? mat.shader : 0;
        if (sh != curPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[sh]);
            curPipeline = sh;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &mat.set, 0, nullptr);
        PushData push{};
        push.color[0] = mat.color.x;
        push.color[1] = mat.color.y;
        push.color[2] = mat.color.z;
        push.color[3] = 1.0f;
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(PushData), &push);
        VkBuffer vbufs[2] = {m.vbuf, frames_[currentFrame_].inst};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, b.count, 0, 0, b.first);  // firstInstance = first
    }
    // --- Скиннинг: анимированные модели (кости уже залиты в предзаготовке) ---
    if (!skinDraws.empty() && skinnedPipeline_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout_, 0, 1,
                                &frames_[currentFrame_].set, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout_, 2, 1,
                                &frames_[currentFrame_].bonesSet, 0, nullptr);
        for (const SkinDraw& sd : skinDraws) {
            const VkMesh& m = skinnedMeshes_[sd.item->mesh - 1];
            VkDescriptorSet texSet = whiteSet_;
            if (sd.item->texture >= 1 && sd.item->texture <= textureSets_.size()) {
                texSet = textureSets_[sd.item->texture - 1];
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipelineLayout_, 1, 1,
                                    &texSet, 0, nullptr);
            SkinnedPushData push{};
            std::memcpy(push.model, sd.item->model.m, sizeof(push.model));
            push.color[0] = sd.item->color.x;
            push.color[1] = sd.item->color.y;
            push.color[2] = sd.item->color.z;
            push.color[3] = 1.0f;
            push.boneOffset = (int)sd.boneOffset;
            vkCmdPushConstants(cmd, skinnedPipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(SkinnedPushData), &push);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
        }
    }
    // --- HUD-текст (растровый шрифт, оверлей с alpha-blend, без depth) ---
    if (!frame.hud.empty() && hudPipeline_ != VK_NULL_HANDLE) {
        float* hudBase = (float*)frames_[currentFrame_].hudMapped;
        uint32_t vtx = 0;
        const int gc = font::glyphCount();
        const float atlasW = (float)(gc * font::kGlyphW);
        struct Run { uint32_t first; uint32_t count; Vec3 color; };
        std::vector<Run> runs;
        for (const HudText& t : frame.hud) {
            float scale = t.pixelHeight / (float)font::kGlyphH;
            float advance = (float)(font::kGlyphW + 1) * scale;
            float gw = (float)font::kGlyphW * scale;
            float gh = (float)font::kGlyphH * scale;
            uint32_t first = vtx;
            float penX = t.x;
            for (char ch : t.text) {
                int gi = font::glyphIndex(ch);
                if (gi >= 0 && vtx + 6 <= kMaxHudVerts) {
                    float x0 = penX, y0 = t.y, x1 = penX + gw, y1 = t.y + gh;
                    float u0 = (float)(gi * font::kGlyphW) / atlasW;
                    float u1 = (float)((gi + 1) * font::kGlyphW) / atlasW;
                    const float q[24] = {
                        x0, y0, u0, 0.0f,  x1, y0, u1, 0.0f,  x1, y1, u1, 1.0f,
                        x0, y0, u0, 0.0f,  x1, y1, u1, 1.0f,  x0, y1, u0, 1.0f,
                    };
                    std::memcpy(hudBase + (size_t)vtx * 4, q, sizeof(q));
                    vtx += 6;
                }
                penX += advance;
            }
            runs.push_back({first, vtx - first, t.color});
        }
        if (vtx > 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout_, 0, 1,
                                    &fontSet_, 0, nullptr);
            VkDeviceSize off0 = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &frames_[currentFrame_].hud, &off0);
            HudPushData push{};
            push.screen[0] = (float)swapchainExtent_.width;
            push.screen[1] = (float)swapchainExtent_.height;
            for (const Run& run : runs) {
                if (run.count == 0) continue;
                push.color[0] = run.color.x;
                push.color[1] = run.color.y;
                push.color[2] = run.color.z;
                push.color[3] = 1.0f;
                vkCmdPushConstants(cmd, hudPipelineLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(HudPushData), &push);
                vkCmdDraw(cmd, run.count, 1, run.first, 0);
            }
        }
    }
    // --- ImGui поверх всего (панель строит приложение в frame.ui) ---
    if (imguiReady_ && frame.ui) {
        ImGui_ImplVulkan_NewFrame();
#ifdef __ANDROID__
        // На Android нет ImGui_ImplGlfw — размер/dt подаём сами (ввод кормит main).
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)swapchainExtent_.width, (float)swapchainExtent_.height);
        io.DeltaTime = frame.deltaTime > 0.0f ? frame.deltaTime : (1.0f / 60.0f);
#endif  // на десктопе размер/dt/ввод даёт ImGui_ImplGlfw_NewFrame (вызван в main)
        ImGui::NewFrame();
        frame.ui();
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[currentFrame_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished_[imageIndex];
    if (vkQueueSubmit(queue_, 1, &si, inFlight_[currentFrame_]) != VK_SUCCESS) {
        LOGE("Vulkan: vkQueueSubmit failed");
        return;
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(queue_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        maybeRecreateSwapchain(pres);
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

void VulkanRenderer::cleanup() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);

    // ImGui сносим первым (пока device/пул живы), platform-бэкенд гасит main.
    if (imguiReady_) {
        GameUi::unloadSkin(*this);
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }

    cleanupSwapchain();

    // Текстуры + дефолтная белая + сэмплер.
    for (VkTexture& t : textures_) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
    }
    textures_.clear();
    textureSets_.clear();
    imguiTextureSets_.clear();
    if (whiteView_) vkDestroyImageView(device_, whiteView_, nullptr);
    if (whiteImage_) vkDestroyImage(device_, whiteImage_, nullptr);
    if (whiteMem_) vkFreeMemory(device_, whiteMem_, nullptr);
    if (flatNormalView_) vkDestroyImageView(device_, flatNormalView_, nullptr);
    if (flatNormalImage_) vkDestroyImage(device_, flatNormalImage_, nullptr);
    if (flatNormalMem_) vkFreeMemory(device_, flatNormalMem_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    whiteView_ = VK_NULL_HANDLE;
    whiteImage_ = VK_NULL_HANDLE;
    whiteMem_ = VK_NULL_HANDLE;
    flatNormalView_ = VK_NULL_HANDLE;
    flatNormalImage_ = VK_NULL_HANDLE;
    flatNormalMem_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;

    // Меши (вершинные/индексные буферы) — обычные и скиннинг.
    auto freeMesh = [&](VkMesh& m) {
        if (m.vbuf) vkDestroyBuffer(device_, m.vbuf, nullptr);
        if (m.vmem) vkFreeMemory(device_, m.vmem, nullptr);
        if (m.ibuf) vkDestroyBuffer(device_, m.ibuf, nullptr);
        if (m.imem) vkFreeMemory(device_, m.imem, nullptr);
    };
    for (VkMesh& m : meshes_) freeMesh(m);
    for (VkMesh& m : skinnedMeshes_) freeMesh(m);
    meshes_.clear();
    skinnedMeshes_.clear();

    // Кадровые UBO + пул дескрипторов (пул освобождает сами сеты, включая материалы).
    for (FrameRes& f : frames_) {
        if (f.ubo) vkDestroyBuffer(device_, f.ubo, nullptr);
        if (f.uboMem) vkFreeMemory(device_, f.uboMem, nullptr);
        if (f.inst) vkDestroyBuffer(device_, f.inst, nullptr);
        if (f.instMem) vkFreeMemory(device_, f.instMem, nullptr);
        if (f.bones) vkDestroyBuffer(device_, f.bones, nullptr);
        if (f.bonesMem) vkFreeMemory(device_, f.bonesMem, nullptr);
        if (f.hud) vkDestroyBuffer(device_, f.hud, nullptr);
        if (f.hudMem) vkFreeMemory(device_, f.hudMem, nullptr);
        if (f.shadowFb) vkDestroyFramebuffer(device_, f.shadowFb, nullptr);
        if (f.shadowView) vkDestroyImageView(device_, f.shadowView, nullptr);
        if (f.shadowImage) vkDestroyImage(device_, f.shadowImage, nullptr);
        if (f.shadowMem) vkFreeMemory(device_, f.shadowMem, nullptr);
    }
    frames_.clear();
    materials_.clear();
    textureSets_.clear();
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (setLayout0_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, setLayout0_, nullptr);
    if (setLayout1_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, setLayout1_, nullptr);
    if (setLayout2_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, setLayout2_, nullptr);
    for (VkPipeline& pl : pipelines_) {
        if (pl != VK_NULL_HANDLE) vkDestroyPipeline(device_, pl, nullptr);
        pl = VK_NULL_HANDLE;
    }
    if (skinnedPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, skinnedPipeline_, nullptr);
    if (shadowPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, shadowPipeline_, nullptr);
    if (shadowSkinPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, shadowSkinPipeline_, nullptr);
    if (hudPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, hudPipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (skinnedPipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, skinnedPipelineLayout_, nullptr);
    if (hudPipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, hudPipelineLayout_, nullptr);
    // Ресурсы шрифта HUD.
    if (fontSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, fontSampler_, nullptr);
    if (fontView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, fontView_, nullptr);
    if (fontImage_ != VK_NULL_HANDLE) vkDestroyImage(device_, fontImage_, nullptr);
    if (fontMem_ != VK_NULL_HANDLE) vkFreeMemory(device_, fontMem_, nullptr);
    hudPipeline_ = VK_NULL_HANDLE;
    hudPipelineLayout_ = VK_NULL_HANDLE;
    fontSampler_ = VK_NULL_HANDLE;
    fontView_ = VK_NULL_HANDLE;
    fontImage_ = VK_NULL_HANDLE;
    fontMem_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout0_ = VK_NULL_HANDLE;
    setLayout1_ = VK_NULL_HANDLE;
    setLayout2_ = VK_NULL_HANDLE;
    skinnedPipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    skinnedPipelineLayout_ = VK_NULL_HANDLE;

    for (VkSemaphore s : imageAvailable_) vkDestroySemaphore(device_, s, nullptr);
    for (VkFence f : inFlight_) vkDestroyFence(device_, f, nullptr);
    imageAvailable_.clear();
    inFlight_.clear();
    imagesInFlight_.clear();  // не владеем этими fence (копии inFlight_)
    if (renderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (shadowRenderPass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, shadowRenderPass_, nullptr);
    if (shadowSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, shadowSampler_, nullptr);
    shadowPipeline_ = VK_NULL_HANDLE;
    shadowSkinPipeline_ = VK_NULL_HANDLE;
    shadowRenderPass_ = VK_NULL_HANDLE;
    shadowSampler_ = VK_NULL_HANDLE;
    if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (debugMessenger_ != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    ready_ = false;
}
