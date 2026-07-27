#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <vector>

#include "Renderer.h"

/**
 * OpenGL ES 3.0 бэкенд: реестры шейдеров/мешей/текстур/материалов + отрисовка
 * RenderFrame. Вызовы сортируются по (шейдер, материал, меш), а избыточные
 * смены состояния GPU пропускаются (см. renderFrame).
 */
class GlRenderer final : public Renderer {
public:
    ~GlRenderer() override;

    bool init(ANativeWindow* window) override;
    MeshHandle createMesh(const MeshData& data) override;
    SkinnedHandle createSkinnedMesh(const SkinnedModel& model) override;
    TextureHandle createTexture(const TextureData& data) override;
    MaterialHandle createMaterial(const MaterialDesc& desc) override;
    void renderFrame(const RenderFrame& frame) override;
    float aspectRatio() const override;

private:
    struct GlMesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
    };
    // Программа + локации пер-объектных/материальных uniform'ов. Кадровые данные
    // (viewProj/lightDir/viewPos) идут через UBO-блок Frame, а не через glUniform.
    // Отсутствующие локации = -1 (glUniform на -1 — безопасный no-op).
    struct GlShader {
        GLuint program = 0;
        GLint uColor = -1;
        GLint uAlbedo = -1;
        // uModel'а нет: матрица модели приходит инстансным атрибутом iModel.
    };
    struct GlMaterial {
        uint32_t shader = 0;  // индекс в shaders_
        Vec3 baseColor{1.0f, 1.0f, 1.0f};
        GLuint texture = 0;   // разрешённый GL-идентификатор (albedo или белая)
    };

    bool initEgl(ANativeWindow* window);
    bool initShaders();
    bool buildShader(const char* vs, const char* fs, GlShader& out);
    bool initSkin();
    void drawSkinned(const std::vector<SkinnedItem>& items);
    bool initHud();
    void drawHud(const std::vector<HudText>& texts, int screenW, int screenH);
    void shutdown();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;

    GLuint whiteTexture_ = 0;  // 1x1 белая — материалы без текстуры используют её
    GLuint frameUbo_ = 0;      // кадровые данные (Frame block), общие для всех программ
    GLuint instanceVbo_ = 0;   // общий буфер матриц модели для инстансинга

    std::vector<GlShader> shaders_;      // индекс = ShaderType
    std::vector<GlMesh> meshes_;         // handle = индекс + 1
    std::vector<GLuint> textures_;       // handle = индекс + 1
    std::vector<GlMaterial> materials_;  // handle = индекс + 1
    std::vector<Mat4> instanceData_;     // переиспользуемый буфер матриц на батч

    // Скиннинг: отдельная программа и меши со скелетными атрибутами.
    GLuint skinProgram_ = 0;
    GLint uSkinModel_ = -1;
    GLint uSkinColor_ = -1;
    GLint uSkinAlbedo_ = -1;
    GLint uBoneTex_ = -1;     // сэмплер bone-текстуры
    GLint uBoneOffset_ = -1;  // строка начала костей текущей модели
    GLuint boneTexture_ = 0;  // RGBA32F: все матрицы костей кадра (кость = 1 строка)
    std::vector<Mat4> boneData_;         // CPU-накопитель костей на кадр
    std::vector<GlMesh> skinnedMeshes_;  // handle = индекс + 1

    // HUD: 2D-текст растровым шрифтом поверх сцены.
    GLuint hudProgram_ = 0;
    GLint uHudScreen_ = -1;
    GLint uHudColor_ = -1;
    GLint uHudFont_ = -1;
    GLuint hudVao_ = 0;
    GLuint hudVbo_ = 0;
    GLuint fontTexture_ = 0;
    std::vector<float> hudVerts_;  // переиспользуемый буфер вершин (x,y,u,v)

    bool imguiReady_ = false;      // инициализирован ли ImGui + его GL3-бэкенд
};
