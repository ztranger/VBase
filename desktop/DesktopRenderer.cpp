#include "DesktopRenderer.h"

#include "GlCore.h"

#include <cstdio>

namespace {

const char* kVert =
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uModel;\n"
    "out vec3 vN;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vN = mat3(uModel) * aNormal;\n"
    "}\n";

const char* kFrag =
    "#version 330 core\n"
    "in vec3 vN;\n"
    "uniform vec3 uColor;\n"
    "uniform vec3 uLight;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vN);\n"
    "    float d = max(dot(N, normalize(uLight)), 0.0);\n"
    "    fragColor = vec4(uColor * (0.25 + 0.75 * d), 1.0);\n"
    "}\n";

unsigned int compile(unsigned int type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader error: %s\n", log);
    }
    return s;
}

}  // namespace

bool DesktopRenderer::init(void* (*getProc)(const char*)) {
    if (!loadGlCore(getProc)) {
        std::fprintf(stderr, "Не удалось загрузить функции OpenGL\n");
        return false;
    }
    unsigned int vs = compile(GL_VERTEX_SHADER, kVert);
    unsigned int fs = compile(GL_FRAGMENT_SHADER, kFrag);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        std::fprintf(stderr, "program link failed\n");
        return false;
    }
    uMVP_ = glGetUniformLocation(program_, "uMVP");
    uModel_ = glGetUniformLocation(program_, "uModel");
    uColor_ = glGetUniformLocation(program_, "uColor");
    uLight_ = glGetUniformLocation(program_, "uLight");
    glEnable(GL_DEPTH_TEST);
    return true;
}

void DesktopRenderer::shutdown() {
    for (const GlMesh& m : meshes_) {
        glDeleteVertexArrays(1, &m.vao);
        glDeleteBuffers(1, &m.vbo);
        glDeleteBuffers(1, &m.ebo);
    }
    meshes_.clear();
    if (program_) glDeleteProgram(program_);
    program_ = 0;
}

uint32_t DesktopRenderer::createMesh(const MeshData& data) {
    GlMesh m;
    m.indexCount = (int)data.indices.size();
    glGenVertexArrays(1, &m.vao);
    glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.vertices.size() * sizeof(Vertex)),
                 data.vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(data.indices.size() * sizeof(uint32_t)),
                 data.indices.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(Vertex);  // pos(3)+normal(3)+uv(2)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    meshes_.push_back(m);
    return (uint32_t)meshes_.size();
}

void DesktopRenderer::beginFrame(int width, int height, const Mat4& view, const Mat4& proj,
                                 Vec3 lightDir) {
    glViewport(0, 0, width, height);
    glClearColor(0.07f, 0.07f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program_);
    glUniform3f(uLight_, lightDir.x, lightDir.y, lightDir.z);
    viewProj_ = proj * view;
}

void DesktopRenderer::draw(uint32_t mesh, const Mat4& model, Vec3 color) {
    if (mesh == 0 || mesh > meshes_.size()) return;
    const GlMesh& m = meshes_[mesh - 1];
    Mat4 mvp = viewProj_ * model;
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.m);
    glUniform3f(uColor_, color.x, color.y, color.z);
    glBindVertexArray(m.vao);
    glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
}
