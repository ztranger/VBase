#pragma once

#include <string>
#include <vector>

#include "engine/render/GlApi.h"   // GL на обеих платформах (GLES3 / desktop loader)
#include "engine/core/Renderer.h"

struct AssetSource;
struct ANativeWindow;  // Android-тип живёт в бэкенд-заголовке (в initEgl), не в нейтральном Renderer.h

#ifdef __ANDROID__
#include <EGL/egl.h>  // контекст/поверхность на Android
#endif

/**
 * OpenGL-бэкенд, общий для Android (GLES3 + EGL) и десктопа (GL 3.3 + GLFW).
 * Реестры шейдеров/мешей/текстур/материалов + отрисовка RenderFrame. Вызовы
 * сортируются по (шейдер, материал, меш), избыточные смены состояния — пропуск.
 * Контекст создаёт платформа; на десктопе своп/размер — снаружи (setSurfaceSize).
 */
class GlRenderer final : public Renderer {
public:
    using GlGetProcFn = void* (*)(const char*);
    // glGetProc — загрузчик адресов GL-функций (десктоп: обёртка glfwGetProcAddress).
    // На Android null: GLES слинкованы напрямую. GL-специфика бэкенда, не в интерфейсе.
    explicit GlRenderer(GlGetProcFn glGetProc = nullptr);
    ~GlRenderer() override;

    bool init(void* nativeWindow, AssetSource& assets) override;
    void setSurfaceSize(int width, int height) override;
    MeshHandle createMesh(const MeshData& data) override;
    SkinnedHandle createSkinnedMesh(const SkinnedModel& model) override;
    TextureHandle createTexture(const TextureData& data, bool clampEdges = false) override;
    uint64_t getImGuiTexture(TextureHandle handle) override;
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

    bool initGlResources();  // общая GL-инициализация (шейдеры/UBO/текстуры/...)
    bool initShaders();
    // Собрать программу из файлов шейдеров (пути в assets, напр. "shaders/lit.frag").
    bool buildShader(const char* vsPath, const char* fsPath, GlShader& out);
    // Прочитать ассет целиком в строку через assets_.
    bool readAsset(const char* path, std::string& out);
    // Загрузить исходник шейдера: readAsset + развернуть #include "file" (1 уровень).
    bool loadShaderSource(const char* path, std::string& out);
    // Собрать полный исходник: строка версии + precision (для фрагментного) + тело.
    bool assembleShaderSource(GLenum type, const char* path, std::string& out);
    bool initSkin();
    void drawSkinned(const std::vector<SkinnedItem>& items);
    bool initHud();
    void drawHud(const std::vector<HudText>& texts, int screenW, int screenH);
    void surfaceSize(int& w, int& h) const;  // размер поверхности (EGL/сохранённый)
    void shutdown();

    GlGetProcFn glGetProc_ = nullptr;  // загрузчик GL-функций (десктоп; на Android не нужен)

#ifdef __ANDROID__
    bool initEgl(ANativeWindow* window);
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
#else
    bool ready_ = false;           // инициализирован ли (контекст создаёт платформа)
    int surfaceW_ = 1, surfaceH_ = 1;  // размер задаётся setSurfaceSize
#endif

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

    AssetSource* assets_ = nullptr;  // источник шейдер-файлов (задаётся в init)
};
