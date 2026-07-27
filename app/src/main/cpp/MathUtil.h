#pragma once

#include <cmath>

// Минимальная математика для 3D. Матрицы — column-major (как ждёт OpenGL:
// glUniformMatrix4fv с transpose = GL_FALSE).
// NB: файл называется MathUtil.h, а не Math.h — на регистронезависимой ФС
// (Windows) имя Math.h столкнулось бы со стандартным <math.h>.

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline Vec3 normalize(Vec3 v) {
    float len = std::sqrt(dot(v, v));
    return len > 0.0f ? v * (1.0f / len) : v;
}

struct Mat4 {
    float m[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    static Mat4 translation(Vec3 t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }

    static Mat4 rotationX(float a) {
        float c = std::cos(a), s = std::sin(a);
        Mat4 r;
        r.m[5] = c;  r.m[6] = s;
        r.m[9] = -s; r.m[10] = c;
        return r;
    }

    static Mat4 rotationY(float a) {
        float c = std::cos(a), s = std::sin(a);
        Mat4 r;
        r.m[0] = c; r.m[2] = -s;
        r.m[8] = s; r.m[10] = c;
        return r;
    }

    static Mat4 rotationZ(float a) {
        float c = std::cos(a), s = std::sin(a);
        Mat4 r;
        r.m[0] = c;  r.m[1] = s;
        r.m[4] = -s; r.m[5] = c;
        return r;
    }

    // fovY в радианах.
    static Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
        float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = 0.0f;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
        Vec3 f = normalize(center - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x;  r.m[1] = u.x;  r.m[2] = -f.x;  r.m[3] = 0.0f;
        r.m[4] = s.y;  r.m[5] = u.y;  r.m[6] = -f.y;  r.m[7] = 0.0f;
        r.m[8] = s.z;  r.m[9] = u.z;  r.m[10] = -f.z; r.m[11] = 0.0f;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        r.m[15] = 1.0f;
        return r;
    }
};

// C = A * B (применяется сначала B, затем A: v' = A * B * v).
inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

// Кватернион (x, y, z, w) — как хранит повороты glTF. Единичный = (0,0,0,1).
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

inline Quat normalize(Quat q) {
    float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 0.0f) return Quat{};
    float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Сферическая интерполяция (с откатом на линейную при близких кватернионах).
inline Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {  // берём кратчайшую дугу
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {  // почти совпадают — линейно
        return normalize(Quat{a.x + (b.x - a.x) * t,
                              a.y + (b.y - a.y) * t,
                              a.z + (b.z - a.z) * t,
                              a.w + (b.w - a.w) * t});
    }
    float theta0 = std::acos(d);
    float theta = theta0 * t;
    float sin0 = std::sin(theta0);
    float s0 = std::sin(theta0 - theta) / sin0;
    float s1 = std::sin(theta) / sin0;
    return {a.x * s0 + b.x * s1, a.y * s0 + b.y * s1,
            a.z * s0 + b.z * s1, a.w * s0 + b.w * s1};
}

// Кватернион -> матрица поворота (column-major).
inline Mat4 quatToMat4(Quat q) {
    q = normalize(q);
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r;
    r.m[0] = 1.0f - 2.0f * (yy + zz); r.m[1] = 2.0f * (xy + wz);        r.m[2] = 2.0f * (xz - wy);        r.m[3] = 0.0f;
    r.m[4] = 2.0f * (xy - wz);        r.m[5] = 1.0f - 2.0f * (xx + zz); r.m[6] = 2.0f * (yz + wx);        r.m[7] = 0.0f;
    r.m[8] = 2.0f * (xz + wy);        r.m[9] = 2.0f * (yz - wx);        r.m[10] = 1.0f - 2.0f * (xx + yy); r.m[11] = 0.0f;
    r.m[12] = 0.0f;                   r.m[13] = 0.0f;                   r.m[14] = 0.0f;                    r.m[15] = 1.0f;
    return r;
}

// Локальная матрица узла: T * R * S.
inline Mat4 trs(Vec3 t, Quat r, Vec3 s) {
    return Mat4::translation(t) * quatToMat4(r) * Mat4::scale(s);
}

// Углы: привести к [-pi, pi] и интерполировать по кратчайшей дуге.
inline float wrapAngle(float a) {
    const float tau = 6.28318530718f;
    a = std::fmod(a + 3.14159265f, tau);
    if (a < 0.0f) a += tau;
    return a - 3.14159265f;
}
inline float lerpAngle(float a, float b, float t) {
    return a + wrapAngle(b - a) * t;
}

// Общая обратная матрица 4x4 (нужна для формулы скиннинга).
inline Mat4 inverse(const Mat4& in) {
    const float* m = in.m;
    Mat4 out;
    float* o = out.m;
    o[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    o[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    o[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    o[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    o[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    o[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    o[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    o[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    o[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    o[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    o[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    o[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    o[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    o[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    o[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    o[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*o[0] + m[1]*o[4] + m[2]*o[8] + m[3]*o[12];
    if (det == 0.0f) return Mat4::identity();
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) o[i] *= invDet;
    return out;
}
