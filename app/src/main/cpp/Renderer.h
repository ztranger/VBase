#pragma once

#include <android/native_window.h>

#include "Mesh.h"
#include "Model.h"
#include "RenderFrame.h"
#include "Texture.h"

/**
 * Интерфейс рендера. Знает только про меши, текстуры, материалы, камеру и свет —
 * ничего про игровые объекты. Реализации: GlRenderer (сейчас),
 * VulkanRenderer (портируется под тот же контракт).
 */
class Renderer {
public:
    virtual ~Renderer() = default;

    // Создать контекст/свопчейн на данном окне. false — бэкенд недоступен.
    virtual bool init(ANativeWindow* window) = 0;

    // Залить геометрию в GPU и вернуть handle для использования в RenderItem.
    virtual MeshHandle createMesh(const MeshData& data) = 0;

    // Залить скиннинг-геометрию (позиции/нормали/UV + кости/веса).
    virtual SkinnedHandle createSkinnedMesh(const SkinnedModel& model) = 0;

    // Залить пиксели текстуры в GPU.
    virtual TextureHandle createTexture(const TextureData& data) = 0;

    // Зарегистрировать материал (цвет + текстура). Рендер разрешит albedo=0
    // в встроенную белую текстуру, чтобы шейдер был один без ветвлений.
    virtual MaterialHandle createMaterial(const MaterialDesc& desc) = 0;

    // Нарисовать и показать один кадр по его описанию.
    virtual void renderFrame(const RenderFrame& frame) = 0;

    // Текущее соотношение сторон окна — нужно сцене для матрицы проекции.
    virtual float aspectRatio() const = 0;
};
