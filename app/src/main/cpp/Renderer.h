#pragma once

#include "Mesh.h"
#include "Model.h"
#include "RenderFrame.h"
#include "Texture.h"

// Forward-declare: на десктопе <android/native_window.h> нет, а тип нужен только
// как непрозрачный указатель (используется реально лишь на Android).
struct ANativeWindow;
struct AssetSource;  // источник ассетов (рендер грузит через него шейдеры)

/**
 * Интерфейс рендера. Знает только про меши, текстуры, материалы, камеру и свет —
 * ничего про игровые объекты. Реализации: GlRenderer (сейчас),
 * VulkanRenderer (портируется под тот же контракт).
 */
class Renderer {
public:
    virtual ~Renderer() = default;

    // Инициализация. Контекст GL должен быть уже текущим (создаёт платформа:
    // EGL на Android из window; GLFW на десктопе). glGetProc — загрузчик адресов
    // GL-функций (нужен на десктопе; на Android null — функции слинкованы).
    // assets — источник для загрузки шейдеров (shaders/*.vert|frag).
    virtual bool init(ANativeWindow* window, void* (*glGetProc)(const char*),
                      AssetSource& assets) = 0;

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
