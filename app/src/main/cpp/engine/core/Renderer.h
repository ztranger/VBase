#pragma once

#include "engine/assets/Mesh.h"
#include "engine/assets/Model.h"
#include "engine/core/RenderFrame.h"
#include "engine/core/Texture.h"

struct AssetSource;  // источник ассетов (рендер грузит через него шейдеры)

/**
 * Интерфейс рендера. Знает только про меши, текстуры, материалы, камеру и свет —
 * ничего про игровые объекты. Реализации: GlRenderer (сейчас),
 * VulkanRenderer (портируется под тот же контракт).
 */
class Renderer {
public:
    virtual ~Renderer() = default;

    // Инициализация. nativeWindow — НЕПРОЗРАЧНЫЙ платформенный хэндл окна/поверхности
    // (Android: ANativeWindow*; десктоп: GLFWwindow*), рендер трактует его по своему
    // бэкенду. Контекст GL должен быть уже текущим (создаёт платформа: EGL на Android
    // из окна; GLFW на десктопе — там загрузчик GL-функций передаётся в конструктор
    // GlRenderer). assets — источник для загрузки шейдеров (shaders/*.vert|frag).
    virtual bool init(void* nativeWindow, AssetSource& assets) = 0;

    // Размер поверхности в пикселях (десктоп задаёт каждый кадр; Android берёт
    // из EGL и это игнорирует).
    virtual void setSurfaceSize(int width, int height) {}

    // Залить геометрию в GPU и вернуть handle для использования в RenderItem.
    virtual MeshHandle createMesh(const MeshData& data) = 0;

    // Залить скиннинг-геометрию (позиции/нормали/UV + кости/веса).
    virtual SkinnedHandle createSkinnedMesh(const SkinnedModel& model) = 0;

    // Залить пиксели текстуры в GPU.
    // clampEdges=true — CLAMP_TO_EDGE без mipmaps (UI / 9-slice).
    virtual TextureHandle createTexture(const TextureData& data, bool clampEdges = false) = 0;

    // ImTextureID для Dear ImGui (GL: GLuint; Vulkan: DescriptorSet через AddTexture).
    // 0 = invalid. Vulkan кэширует дескриптор до releaseImGuiTexture / shutdown.
    virtual uint64_t getImGuiTexture(TextureHandle handle) = 0;
    virtual void releaseImGuiTexture(uint64_t /*imguiTexId*/) {}

    // Зарегистрировать материал (цвет + текстура). Рендер разрешит albedo=0
    // в встроенную белую текстуру, чтобы шейдер был один без ветвлений.
    virtual MaterialHandle createMaterial(const MaterialDesc& desc) = 0;

    // Нарисовать и показать один кадр по его описанию.
    virtual void renderFrame(const RenderFrame& frame) = 0;

    // Текущее соотношение сторон окна — нужно сцене для матрицы проекции.
    virtual float aspectRatio() const = 0;
};
