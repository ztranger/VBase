#pragma once

// Мини-загрузчик функций OpenGL 3.3 core: на десктопе современные функции GL
// не слинкованы, их надо получить через glfwGetProcAddress. Вместо GLEW/GLAD
// объявляем ровно то, что нужно DesktopRenderer, и грузим указатели вручную.

#ifdef _WIN32
#include <windows.h>  // до <GL/gl.h> — тот использует WINGDIAPI/APIENTRY
#endif
#include <GL/gl.h>    // GL 1.1: типы + базовые функции (glClear, glDrawElements, ...)

#include <cstddef>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

// Константы, которых нет в 1.1-заголовке.
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (*PFN_glCompileShader)(GLuint);
typedef void (*PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFN_glDeleteShader)(GLuint);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_glLinkProgram)(GLuint);
typedef void (*PFN_glDeleteProgram)(GLuint);
typedef void (*PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFN_glUseProgram)(GLuint);
typedef GLint (*PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef void (*PFN_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glDeleteVertexArrays)(GLsizei, const GLuint*);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);

#define GL_FUNCS \
    X(PFN_glCreateShader, glCreateShader) \
    X(PFN_glShaderSource, glShaderSource) \
    X(PFN_glCompileShader, glCompileShader) \
    X(PFN_glGetShaderiv, glGetShaderiv) \
    X(PFN_glGetShaderInfoLog, glGetShaderInfoLog) \
    X(PFN_glDeleteShader, glDeleteShader) \
    X(PFN_glCreateProgram, glCreateProgram) \
    X(PFN_glAttachShader, glAttachShader) \
    X(PFN_glLinkProgram, glLinkProgram) \
    X(PFN_glDeleteProgram, glDeleteProgram) \
    X(PFN_glGetProgramiv, glGetProgramiv) \
    X(PFN_glGetProgramInfoLog, glGetProgramInfoLog) \
    X(PFN_glUseProgram, glUseProgram) \
    X(PFN_glGetUniformLocation, glGetUniformLocation) \
    X(PFN_glUniform3f, glUniform3f) \
    X(PFN_glUniformMatrix4fv, glUniformMatrix4fv) \
    X(PFN_glGenVertexArrays, glGenVertexArrays) \
    X(PFN_glBindVertexArray, glBindVertexArray) \
    X(PFN_glDeleteVertexArrays, glDeleteVertexArrays) \
    X(PFN_glGenBuffers, glGenBuffers) \
    X(PFN_glBindBuffer, glBindBuffer) \
    X(PFN_glBufferData, glBufferData) \
    X(PFN_glDeleteBuffers, glDeleteBuffers) \
    X(PFN_glVertexAttribPointer, glVertexAttribPointer) \
    X(PFN_glEnableVertexAttribArray, glEnableVertexAttribArray)

#define X(type, name) static type name = nullptr;
GL_FUNCS
#undef X

// Загрузить все указатели. getProc — обычно обёртка над glfwGetProcAddress.
static bool loadGlCore(void* (*getProc)(const char*)) {
    bool ok = true;
#define X(type, name) name = (type)getProc(#name); if (name == nullptr) ok = false;
    GL_FUNCS
#undef X
    return ok;
}
