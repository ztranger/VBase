#include "GlRenderer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include "Font.h"
#include "Log.h"

// Версия GLSL зависит от платформы: GLES 3.0 на Android, GL 3.3 core на десктопе.
// Тела шейдеров одинаковы (precision-строки десктоп принимает и игнорирует).
#ifdef __ANDROID__
#define GLSL_VERSION "#version 300 es\n"
#define IMGUI_GLSL_VERSION "#version 300 es"
#else
#define GLSL_VERSION "#version 330 core\n"
#define IMGUI_GLSL_VERSION "#version 330"
#endif

namespace {

// Точка связывания UBO с кадровыми данными (совпадает в C++ и шейдерах).
constexpr GLuint kFrameBinding = 0;

// Максимум строк (костей) в bone-текстуре на кадр — суммарно по всем моделям.
constexpr int kBoneTexRows = 1024;

// Раскладка std140 блока Frame. vec3 в std140 выравнивается на 16 байт,
// поэтому явные паддинги. Размер = 96 байт.
struct FrameUBO {
    float viewProj[16];  // 0
    float lightDir[3];   // 64
    float pad0;          // 76
    float viewPos[3];    // 80
    float pad1;          // 92
};

// Общий блок Frame — вставляется в каждый шейдер, который его использует.
#define FRAME_BLOCK \
    "layout(std140) uniform Frame {\n" \
    "    mat4 uViewProj;\n" \
    "    vec3 uLightDir;\n" \
    "    vec3 uViewPos;\n" \
    "};\n"

// Общий вершинный шейдер для Lit/Unlit (viewProj из UBO, model — per-object).
const char* kBasicVert =
    GLSL_VERSION
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "layout(location = 3) in mat4 iModel;\n"  // инстансная матрица (локации 3..6)
    FRAME_BLOCK
    "out vec3 vNormal;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = uViewProj * iModel * vec4(aPos, 1.0);\n"
    "    vNormal = mat3(iModel) * aNormal;\n"
    "    vUV = aUV;\n"
    "}\n";

// Lit: диффуз (Ламберт) + текстура.
const char* kLitFrag =
    GLSL_VERSION
    "precision mediump float;\n"
    FRAME_BLOCK
    "in vec3 vNormal;\n"
    "in vec2 vUV;\n"
    "uniform vec3 uColor;\n"
    "uniform sampler2D uAlbedo;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vNormal);\n"
    "    float diff = max(dot(N, normalize(uLightDir)), 0.0);\n"
    "    vec3 albedo = texture(uAlbedo, vUV).rgb * uColor;\n"
    "    fragColor = vec4(albedo * (0.25 + 0.75 * diff), 1.0);\n"
    "}\n";

// Unlit: плоский цвет/текстура без освещения (Frame-блок в фрагментнике не нужен).
const char* kUnlitFrag =
    GLSL_VERSION
    "precision mediump float;\n"
    "in vec3 vNormal;\n"
    "in vec2 vUV;\n"
    "uniform vec3 uColor;\n"
    "uniform sampler2D uAlbedo;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vec4(texture(uAlbedo, vUV).rgb * uColor, 1.0);\n"
    "}\n";

// Phong: вершинник дополнительно отдаёт мировую позицию (для вектора взгляда).
const char* kPhongVert =
    GLSL_VERSION
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "layout(location = 3) in mat4 iModel;\n"  // инстансная матрица (локации 3..6)
    FRAME_BLOCK
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec4 world = iModel * vec4(aPos, 1.0);\n"
    "    vWorldPos = world.xyz;\n"
    "    gl_Position = uViewProj * world;\n"
    "    vNormal = mat3(iModel) * aNormal;\n"
    "    vUV = aUV;\n"
    "}\n";

// Phong: диффуз + зеркальный блик по позиции камеры.
const char* kPhongFrag =
    GLSL_VERSION
    "precision mediump float;\n"
    FRAME_BLOCK
    "in vec3 vNormal;\n"
    "in vec3 vWorldPos;\n"
    "in vec2 vUV;\n"
    "uniform vec3 uColor;\n"
    "uniform sampler2D uAlbedo;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vNormal);\n"
    "    vec3 L = normalize(uLightDir);\n"
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"
    "    vec3 R = reflect(-L, N);\n"
    "    float diff = max(dot(N, L), 0.0);\n"
    "    float spec = pow(max(dot(V, R), 0.0), 32.0);\n"
    "    vec3 albedo = texture(uAlbedo, vUV).rgb * uColor;\n"
    "    vec3 c = albedo * (0.2 + 0.8 * diff) + vec3(1.0) * (spec * 0.5);\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

// Скиннинг: позиция считается как взвешенная сумма матриц костей.
const char* kSkinVert =
    GLSL_VERSION
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    "layout(location = 3) in vec4 aJoints;\n"
    "layout(location = 4) in vec4 aWeights;\n"
    FRAME_BLOCK
    "uniform mat4 uModel;\n"
    "uniform highp sampler2D uBones;\n"  // кости: строка = кость, 4 texel = 4 столбца
    "uniform int uBoneOffset;\n"          // смещение костей этой модели в текстуре
    "out vec3 vNormal;\n"
    "out vec2 vUV;\n"
    "mat4 boneMat(int j) {\n"
    "    int row = uBoneOffset + j;\n"
    "    return mat4(texelFetch(uBones, ivec2(0, row), 0),\n"
    "               texelFetch(uBones, ivec2(1, row), 0),\n"
    "               texelFetch(uBones, ivec2(2, row), 0),\n"
    "               texelFetch(uBones, ivec2(3, row), 0));\n"
    "}\n"
    "void main() {\n"
    "    mat4 skin = aWeights.x * boneMat(int(aJoints.x))\n"
    "              + aWeights.y * boneMat(int(aJoints.y))\n"
    "              + aWeights.z * boneMat(int(aJoints.z))\n"
    "              + aWeights.w * boneMat(int(aJoints.w));\n"
    "    vec4 worldPos = uModel * skin * vec4(aPos, 1.0);\n"
    "    gl_Position = uViewProj * worldPos;\n"
    "    vNormal = mat3(uModel * skin) * aNormal;\n"
    "    vUV = aUV;\n"
    "}\n";

const char* kSkinFrag =
    GLSL_VERSION
    "precision mediump float;\n"
    FRAME_BLOCK
    "in vec3 vNormal;\n"
    "in vec2 vUV;\n"
    "uniform vec3 uColor;\n"
    "uniform sampler2D uAlbedo;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vNormal);\n"
    "    float diff = max(dot(N, normalize(uLightDir)), 0.0);\n"
    "    vec3 albedo = texture(uAlbedo, vUV).rgb * uColor;\n"
    "    fragColor = vec4(albedo * (0.25 + 0.75 * diff), 1.0);\n"
    "}\n";

// HUD: 2D-текст. Позиции задаём в пикселях, шейдер сам переводит в NDC.
const char* kHudVert =
    GLSL_VERSION
    "layout(location = 0) in vec2 aPos;\n"  // пиксели
    "layout(location = 1) in vec2 aUV;\n"
    "uniform vec2 uScreen;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 ndc = vec2(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0);\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

const char* kHudFrag =
    GLSL_VERSION
    "precision mediump float;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uFont;\n"
    "uniform vec3 uColor;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float a = texture(uFont, vUV).a;\n"
    "    fragColor = vec4(uColor, a);\n"
    "}\n";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

GlRenderer::~GlRenderer() {
    shutdown();
}

bool GlRenderer::init(ANativeWindow* window, void* (*glGetProc)(const char*)) {
#ifdef __ANDROID__
    (void)glGetProc;
    if (!initEgl(window)) {  // EGL создаёт контекст из окна и делает его текущим
        shutdown();
        return false;
    }
#else
    (void)window;  // на десктопе контекст уже создан GLFW и сделан текущим
    if (!glApiLoad(glGetProc)) {
        LOGE("Не удалось загрузить функции OpenGL");
        return false;
    }
    ready_ = true;
#endif
    if (!initGlResources()) {
        shutdown();
        return false;
    }
    return true;
}

// Общая GL-инициализация (одинаково на обеих платформах, контекст уже текущий).
bool GlRenderer::initGlResources() {
    if (!initShaders() || !initSkin() || !initHud()) {
        return false;
    }
    glEnable(GL_DEPTH_TEST);

    // Встроенная белая 1x1: материалы без текстуры ссылаются на неё, поэтому
    // шейдер всегда просто умножает на текстуру — без веток и без пермутаций.
    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &whiteTexture_);
    glBindTexture(GL_TEXTURE_2D, whiteTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // UBO с кадровыми данными, привязанный к общей точке kFrameBinding.
    glGenBuffers(1, &frameUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameUBO), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, kFrameBinding, frameUbo_);

    // Общий буфер инстансных матриц (наполняется по батчам в renderFrame).
    glGenBuffers(1, &instanceVbo_);

    // Dear ImGui: контекст + GL3-бэкенд. Ввод скармливаем вручную.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr;
    if (!ImGui_ImplOpenGL3_Init(IMGUI_GLSL_VERSION)) {
        LOGE("ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    imguiReady_ = true;

    LOGI("GL renderer initialized: %s / %s",
         glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

#ifdef __ANDROID__
bool GlRenderer::initEgl(ANativeWindow* window) {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display_, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig: no suitable config");
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }
    return true;
}
#endif  // __ANDROID__

// Размер поверхности: на Android — из EGL, на десктопе — заданный извне.
void GlRenderer::surfaceSize(int& w, int& h) const {
#ifdef __ANDROID__
    EGLint ww = 0, hh = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &ww);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &hh);
    w = ww;
    h = hh;
#else
    w = surfaceW_;
    h = surfaceH_;
#endif
}

void GlRenderer::setSurfaceSize(int width, int height) {
#ifndef __ANDROID__
    surfaceW_ = width > 0 ? width : 1;
    surfaceH_ = height > 0 ? height : 1;
#else
    (void)width;
    (void)height;
#endif
}

bool GlRenderer::buildShader(const char* vs, const char* fs, GlShader& out) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        return false;
    }
    out.program = glCreateProgram();
    glAttachShader(out.program, v);
    glAttachShader(out.program, f);
    glLinkProgram(out.program);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = GL_FALSE;
    glGetProgramiv(out.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(out.program, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
        return false;
    }

    out.uColor = glGetUniformLocation(out.program, "uColor");
    out.uAlbedo = glGetUniformLocation(out.program, "uAlbedo");

    // Привязываем блок Frame программы к общей точке связывания kFrameBinding.
    GLuint block = glGetUniformBlockIndex(out.program, "Frame");
    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(out.program, block, kFrameBinding);
    }
    // Сэмплер -> текстурный юнит 0. Ставится один раз (константа), не в цикле кадра.
    if (out.uAlbedo >= 0) {
        glUseProgram(out.program);
        glUniform1i(out.uAlbedo, 0);
    }
    return true;
}

bool GlRenderer::initShaders() {
    // Порядок обязан совпадать с enum ShaderType (индекс = тип).
    shaders_.resize((size_t)ShaderType::Count);
    return buildShader(kBasicVert, kLitFrag,   shaders_[(size_t)ShaderType::Lit]) &&
           buildShader(kBasicVert, kUnlitFrag, shaders_[(size_t)ShaderType::Unlit]) &&
           buildShader(kPhongVert, kPhongFrag, shaders_[(size_t)ShaderType::Phong]);
}

bool GlRenderer::initSkin() {
    GlShader s;
    if (!buildShader(kSkinVert, kSkinFrag, s)) {
        return false;
    }
    skinProgram_ = s.program;
    // buildShader уже привязал блок Frame к kFrameBinding для этой программы.
    uSkinModel_ = glGetUniformLocation(skinProgram_, "uModel");
    uSkinColor_ = glGetUniformLocation(skinProgram_, "uColor");
    uSkinAlbedo_ = glGetUniformLocation(skinProgram_, "uAlbedo");
    uBoneTex_ = glGetUniformLocation(skinProgram_, "uBones");
    uBoneOffset_ = glGetUniformLocation(skinProgram_, "uBoneOffset");

    // Bone-текстура: ширина 4 texel (4 столбца mat4), высота = лимит строк-костей.
    glGenTextures(1, &boneTexture_);
    glBindTexture(GL_TEXTURE_2D, boneTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, kBoneTexRows, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

SkinnedHandle GlRenderer::createSkinnedMesh(const SkinnedModel& model) {
    GlMesh mesh;
    mesh.indexCount = (GLsizei)model.indices.size();

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(model.vertices.size() * sizeof(SkinnedVertex)),
                 model.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(model.indices.size() * sizeof(uint32_t)),
                 model.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(SkinnedVertex);  // 16 float
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(12 * sizeof(float)));
    glEnableVertexAttribArray(4);

    glBindVertexArray(0);

    skinnedMeshes_.push_back(mesh);
    return (SkinnedHandle)skinnedMeshes_.size();
}

void GlRenderer::drawSkinned(const std::vector<SkinnedItem>& items) {
    // Собираем кости ВСЕХ моделей в один буфер и запоминаем смещение каждой.
    boneData_.clear();
    std::vector<int> offsets(items.size(), 0);
    for (size_t i = 0; i < items.size(); ++i) {
        offsets[i] = (int)boneData_.size();
        for (const Mat4& j : items[i].joints) {
            if ((int)boneData_.size() >= kBoneTexRows) break;  // защита от переполнения
            boneData_.push_back(j);
        }
    }

    // Одна заливка костей в текстуру на весь кадр (вместо uniform'ов на каждый draw).
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, boneTexture_);
    if (!boneData_.empty()) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, (GLsizei)boneData_.size(),
                        GL_RGBA, GL_FLOAT, boneData_[0].m);
    }

    glUseProgram(skinProgram_);
    glUniform1i(uSkinAlbedo_, 0);
    glUniform1i(uBoneTex_, 1);

    for (size_t i = 0; i < items.size(); ++i) {
        const SkinnedItem& item = items[i];
        if (item.mesh == 0 || item.mesh > skinnedMeshes_.size()) continue;
        const GlMesh& mesh = skinnedMeshes_[item.mesh - 1];

        glUniformMatrix4fv(uSkinModel_, 1, GL_FALSE, item.model.m);
        glUniform3f(uSkinColor_, item.color.x, item.color.y, item.color.z);
        glUniform1i(uBoneOffset_, offsets[i]);  // где кости этой модели в текстуре

        glActiveTexture(GL_TEXTURE0);
        GLuint tex = (item.texture != 0 && item.texture <= textures_.size())
                         ? textures_[item.texture - 1] : whiteTexture_;
        glBindTexture(GL_TEXTURE_2D, tex);

        glBindVertexArray(mesh.vao);
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

bool GlRenderer::initHud() {
    // Программа для 2D-текста.
    GLuint vs = compileShader(GL_VERTEX_SHADER, kHudVert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kHudFrag);
    if (!vs || !fs) {
        return false;
    }
    hudProgram_ = glCreateProgram();
    glAttachShader(hudProgram_, vs);
    glAttachShader(hudProgram_, fs);
    glLinkProgram(hudProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(hudProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("HUD program link failed");
        return false;
    }
    uHudScreen_ = glGetUniformLocation(hudProgram_, "uScreen");
    uHudColor_ = glGetUniformLocation(hudProgram_, "uColor");
    uHudFont_ = glGetUniformLocation(hudProgram_, "uFont");

    // Атлас шрифта: NEAREST (чёткие пиксели), без мипмапов.
    TextureData atlas = makeFontAtlas();
    glGenTextures(1, &fontTexture_);
    glBindTexture(GL_TEXTURE_2D, fontTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)atlas.width, (GLsizei)atlas.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, atlas.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Динамический VBO квадов: атрибуты 0 = pos(vec2), 1 = uv(vec2), страйд 16.
    glGenVertexArrays(1, &hudVao_);
    glBindVertexArray(hudVao_);
    glGenBuffers(1, &hudVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    return true;
}

void GlRenderer::drawHud(const std::vector<HudText>& texts, int screenW, int screenH) {
    // Оверлей: без глубины, с альфа-блендингом.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(hudProgram_);
    glUniform2f(uHudScreen_, (float)screenW, (float)screenH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fontTexture_);
    glUniform1i(uHudFont_, 0);
    glBindVertexArray(hudVao_);

    const int count = font::glyphCount();
    const float atlasW = (float)(count * font::kGlyphW);

    for (const HudText& t : texts) {
        float scale = t.pixelHeight / (float)font::kGlyphH;
        float advance = (float)(font::kGlyphW + 1) * scale;
        float glyphW = (float)font::kGlyphW * scale;
        float glyphH = (float)font::kGlyphH * scale;

        hudVerts_.clear();
        float penX = t.x;
        for (char ch : t.text) {
            int gi = font::glyphIndex(ch);
            if (gi >= 0) {
                float x0 = penX, y0 = t.y, x1 = penX + glyphW, y1 = t.y + glyphH;
                float u0 = (float)(gi * font::kGlyphW) / atlasW;
                float u1 = (float)((gi + 1) * font::kGlyphW) / atlasW;
                const float quad[] = {
                    x0, y0, u0, 0.0f,  x1, y0, u1, 0.0f,  x1, y1, u1, 1.0f,
                    x0, y0, u0, 0.0f,  x1, y1, u1, 1.0f,  x0, y1, u0, 1.0f,
                };
                hudVerts_.insert(hudVerts_.end(), quad, quad + 24);
            }
            penX += advance;
        }
        if (hudVerts_.empty()) {
            continue;
        }
        glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(hudVerts_.size() * sizeof(float)),
                     hudVerts_.data(), GL_DYNAMIC_DRAW);
        glUniform3f(uHudColor_, t.color.x, t.color.y, t.color.z);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(hudVerts_.size() / 4));
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

MeshHandle GlRenderer::createMesh(const MeshData& data) {
    GlMesh mesh;
    mesh.indexCount = (GLsizei)data.indices.size();

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);

    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(data.indices.size() * sizeof(uint32_t)),
                 data.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(Vertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Инстансный mat4 iModel: локации 3..6, по одному значению на инстанс
    // (divisor = 1), читается из общего instanceVbo_. Пишется в VAO меша.
    glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
    for (int col = 0; col < 4; ++col) {
        GLuint loc = 3 + (GLuint)col;
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(Mat4),
                              (const void*)(size_t)(col * 4 * sizeof(float)));
        glVertexAttribDivisor(loc, 1);
    }

    glBindVertexArray(0);

    meshes_.push_back(mesh);
    return (MeshHandle)meshes_.size();
}

TextureHandle GlRenderer::createTexture(const TextureData& data) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)data.width, (GLsizei)data.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    textures_.push_back(tex);
    return (TextureHandle)textures_.size();
}

MaterialHandle GlRenderer::createMaterial(const MaterialDesc& desc) {
    GlMaterial mat;
    mat.shader = (uint32_t)desc.shader;
    mat.baseColor = desc.baseColor;
    if (desc.albedo != 0 && desc.albedo <= textures_.size()) {
        mat.texture = textures_[desc.albedo - 1];
    } else {
        mat.texture = whiteTexture_;
    }
    materials_.push_back(mat);
    return (MaterialHandle)materials_.size();
}

void GlRenderer::renderFrame(const RenderFrame& frame) {
#ifdef __ANDROID__
    if (display_ == EGL_NO_DISPLAY) return;
#else
    if (!ready_) return;
#endif

    int width = 0, height = 0;
    surfaceSize(width, height);
    glViewport(0, 0, width, height);

    glClearColor(0.07f, 0.07f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);

    // Кадровые данные -> в UBO один раз. Все программы видят их сразу через
    // блок Frame, поэтому при смене шейдера ничего до-устанавливать не нужно.
    const Mat4 viewProj = frame.proj * frame.view;
    FrameUBO fd;
    std::memcpy(fd.viewProj, viewProj.m, sizeof(fd.viewProj));
    fd.lightDir[0] = frame.lightDir.x;
    fd.lightDir[1] = frame.lightDir.y;
    fd.lightDir[2] = frame.lightDir.z;
    fd.viewPos[0] = frame.cameraPos.x;
    fd.viewPos[1] = frame.cameraPos.y;
    fd.viewPos[2] = frame.cameraPos.z;
    glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameUBO), &fd);

    // Сортируем по (шейдер, материал, меш): смена программы — самая дорогая,
    // поэтому она первичный ключ; одинаковые состояния идут подряд.
    std::vector<uint32_t> order(frame.items.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const RenderItem& ia = frame.items[a];
        const RenderItem& ib = frame.items[b];
        uint32_t sa = (ia.material && ia.material <= materials_.size())
                          ? materials_[ia.material - 1].shader : 0;
        uint32_t sb = (ib.material && ib.material <= materials_.size())
                          ? materials_[ib.material - 1].shader : 0;
        if (sa != sb) return sa < sb;
        if (ia.material != ib.material) return ia.material < ib.material;
        return ia.mesh < ib.mesh;
    });

    // Идём прогонами: после сортировки элементы с одинаковыми (материал, меш)
    // идут подряд и рисуются ОДНИМ glDrawElementsInstanced. Матрицы модели всех
    // инстансов прогона грузим в общий instanceVbo_.
    int curShader = -1;
    const GlShader* shader = nullptr;
    size_t i = 0;
    while (i < order.size()) {
        const RenderItem& first = frame.items[order[i]];
        if (first.mesh == 0 || first.mesh > meshes_.size() ||
            first.material == 0 || first.material > materials_.size()) {
            ++i;
            continue;
        }
        const GlMaterial& mat = materials_[first.material - 1];

        // Границы прогона: тот же материал и меш (шейдер задаётся материалом).
        size_t j = i + 1;
        while (j < order.size()) {
            const RenderItem& it = frame.items[order[j]];
            if (it.material != first.material || it.mesh != first.mesh) break;
            ++j;
        }

        // Программа (только при смене шейдера) + материал (раз на батч).
        if ((int)mat.shader != curShader) {
            shader = &shaders_[mat.shader];
            glUseProgram(shader->program);
            curShader = (int)mat.shader;
        }
        glUniform3f(shader->uColor, mat.baseColor.x, mat.baseColor.y, mat.baseColor.z);
        glBindTexture(GL_TEXTURE_2D, mat.texture);

        const GlMesh& mesh = meshes_[first.mesh - 1];
        glBindVertexArray(mesh.vao);

        // Матрицы всех инстансов прогона -> instance VBO.
        instanceData_.clear();
        for (size_t k = i; k < j; ++k) {
            instanceData_.push_back(frame.items[order[k]].model);
        }
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(instanceData_.size() * sizeof(Mat4)),
                     instanceData_.data(), GL_DYNAMIC_DRAW);

        glDrawElementsInstanced(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT,
                                nullptr, (GLsizei)instanceData_.size());
        i = j;
    }
    glBindVertexArray(0);

    // Анимированные (скиннинг) модели — своей программой.
    if (!frame.skinned.empty()) {
        drawSkinned(frame.skinned);
    }

    // HUD-текст поверх сцены.
    if (!frame.hud.empty()) {
        drawHud(frame.hud, width, height);
    }

    // Dear ImGui поверх всего. UI строит приложение в колбэке frame.ui.
    // Renderer-бэкенд (imgui_impl_opengl3) — общий; platform-бэкенд — за пределами
    // GlRenderer (Android: ручной ввод в main.cpp; десктоп: imgui_impl_glfw).
    if (imguiReady_) {
        ImGui_ImplOpenGL3_NewFrame();
#ifdef __ANDROID__
        // Платформенного бэкенда нет — размер экрана и dt подаём вручную.
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);
        io.DeltaTime = frame.deltaTime > 0.0f ? frame.deltaTime : (1.0f / 60.0f);
#endif  // на десктопе DisplaySize/DeltaTime/scale задаёт ImGui_ImplGlfw_NewFrame
        ImGui::NewFrame();
        if (frame.ui) {
            frame.ui();
        }
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

#ifdef __ANDROID__
    if (!eglSwapBuffers(display_, surface_)) {
        LOGW("eglSwapBuffers failed: 0x%x", eglGetError());
    }
#endif  // на десктопе своп делает GLFW в главном цикле
}

float GlRenderer::aspectRatio() const {
    int width = 0, height = 0;
    surfaceSize(width, height);
    return height > 0 ? (float)width / (float)height : 1.0f;
}

void GlRenderer::shutdown() {
    // Ничего не создавали — выходим (иначе на десктопе GL-функции ещё не загружены).
#ifdef __ANDROID__
    if (display_ == EGL_NO_DISPLAY) return;
#else
    if (!ready_) return;
#endif

    // ImGui + GL-объекты сносим, пока контекст ещё текущий.
    if (imguiReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    for (const GlMesh& mesh : meshes_) {
        glDeleteVertexArrays(1, &mesh.vao);
        glDeleteBuffers(1, &mesh.vbo);
        glDeleteBuffers(1, &mesh.ebo);
    }
    for (const GlMesh& mesh : skinnedMeshes_) {
        glDeleteVertexArrays(1, &mesh.vao);
        glDeleteBuffers(1, &mesh.vbo);
        glDeleteBuffers(1, &mesh.ebo);
    }
    if (skinProgram_) glDeleteProgram(skinProgram_);
    if (boneTexture_) glDeleteTextures(1, &boneTexture_);
    if (!textures_.empty()) {
        glDeleteTextures((GLsizei)textures_.size(), textures_.data());
    }
    if (whiteTexture_) glDeleteTextures(1, &whiteTexture_);
    if (fontTexture_) glDeleteTextures(1, &fontTexture_);
    if (frameUbo_) glDeleteBuffers(1, &frameUbo_);
    if (instanceVbo_) glDeleteBuffers(1, &instanceVbo_);
    if (hudVbo_) glDeleteBuffers(1, &hudVbo_);
    if (hudVao_) glDeleteVertexArrays(1, &hudVao_);
    if (hudProgram_) glDeleteProgram(hudProgram_);
    for (const GlShader& s : shaders_) {
        if (s.program) glDeleteProgram(s.program);
    }

#ifdef __ANDROID__
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    eglTerminate(display_);
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
#else
    ready_ = false;
#endif

    shaders_.clear();
    meshes_.clear();
    skinnedMeshes_.clear();
    skinProgram_ = 0;
    boneTexture_ = 0;
    textures_.clear();
    materials_.clear();
    whiteTexture_ = 0;
    frameUbo_ = 0;
    instanceVbo_ = 0;
    hudProgram_ = 0;
    hudVao_ = 0;
    hudVbo_ = 0;
    fontTexture_ = 0;
}
