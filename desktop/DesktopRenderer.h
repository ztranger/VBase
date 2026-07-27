#pragma once

#include <cstdint>
#include <vector>

#include "MathUtil.h"
#include "Mesh.h"

// Минимальный десктопный GL 3.3-рендер: заливка мешей + отрисовка с одним
// lit-шейдером (позиция+нормаль, цвет на объект). Текстуры/скиннинг/материалы
// пока не поддерживаются (следующий десктоп-шаг). GLEW спрятан в .cpp.
class DesktopRenderer {
public:
    // getProc — загрузчик адресов GL-функций (обёртка над glfwGetProcAddress).
    bool init(void* (*getProc)(const char*));
    void shutdown();

    uint32_t createMesh(const MeshData& data);  // handle = индекс + 1

    void beginFrame(int width, int height, const Mat4& view, const Mat4& proj, Vec3 lightDir);
    void draw(uint32_t mesh, const Mat4& model, Vec3 color);

private:
    struct GlMesh {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
    };
    unsigned int program_ = 0;
    int uMVP_ = -1, uModel_ = -1, uColor_ = -1, uLight_ = -1;
    Mat4 viewProj_;
    std::vector<GlMesh> meshes_;
};
