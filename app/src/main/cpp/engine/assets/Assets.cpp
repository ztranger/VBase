#include "engine/assets/Assets.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/Log.h"
#include "engine/core/MathUtil.h"

namespace {

bool readText(AssetSource& src, const char* path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!src.read(path, bytes)) {
        LOGW("Asset не найден: %s", path);
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace

TextureData makeCheckerboard(uint32_t size, uint32_t cells) {
    TextureData tex;
    tex.width = size;
    tex.height = size;
    tex.rgba.resize((size_t)size * size * 4);
    uint32_t cell = size / cells;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            bool light = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t c = light ? 220 : 60;
            size_t i = ((size_t)y * size + x) * 4;
            tex.rgba[i + 0] = c;
            tex.rgba[i + 1] = c;
            tex.rgba[i + 2] = c;
            tex.rgba[i + 3] = 255;
        }
    }
    return tex;
}

TextureData makeBumpNormal(uint32_t size, uint32_t freq) {
    TextureData tex;
    tex.width = size;
    tex.height = size;
    tex.rgba.resize((size_t)size * size * 4);
    const float pi = 3.14159265358979323846f;
    const float f = (float)(freq == 0 ? 1u : freq);
    const float amp = 0.7f;  // крутизна бампов (больше -> резче нормали)
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float u = (float)x / (float)size;
            float v = (float)y / (float)size;
            // высота h = sin(u)*sin(v); нормаль = normalize(-dh/du, -dh/dv, 1).
            float du = amp * std::cos(u * f * 2.0f * pi) * std::sin(v * f * 2.0f * pi);
            float dv = amp * std::sin(u * f * 2.0f * pi) * std::cos(v * f * 2.0f * pi);
            float nx = -du, ny = -dv, nz = 1.0f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            size_t i = ((size_t)y * size + x) * 4;
            tex.rgba[i + 0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f);
            tex.rgba[i + 1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f);
            tex.rgba[i + 2] = (uint8_t)((nz * 0.5f + 0.5f) * 255.0f);
            tex.rgba[i + 3] = 255;
        }
    }
    return tex;
}

bool decodeImageBuffer(const void* data, size_t size, TextureData& out) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(data), (int)size, &w, &h, &channels, 4);  // форсим RGBA
    if (pixels == nullptr) {
        LOGW("stb_image: не удалось декодировать (%s)", stbi_failure_reason());
        return false;
    }
    out.width = (uint32_t)w;
    out.height = (uint32_t)h;
    out.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return true;
}

bool loadImageAsset(AssetSource& src, const char* path, TextureData& out) {
    std::vector<uint8_t> bytes;
    if (!src.read(path, bytes)) {
        LOGW("Изображение не найдено: %s", path);
        return false;
    }
    return decodeImageBuffer(bytes.data(), bytes.size(), out);
}

bool loadObjAsset(AssetSource& src, const char* path, MeshData& out) {
    std::string text;
    if (!readText(src, path, text)) {
        return false;
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<float> uvs;  // пары (u,v)

    // Уникальные вершины по ключу "vi/ti/ni" -> индекс в out.vertices.
    std::unordered_map<std::string, uint32_t> unique;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;

        if (tag == "v") {
            Vec3 p;
            ls >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vn") {
            Vec3 n;
            ls >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (tag == "vt") {
            float u = 0, v = 0;
            ls >> u >> v;
            uvs.push_back(u);
            uvs.push_back(v);
        } else if (tag == "f") {
            // Собираем вершины грани, затем триангулируем веером.
            std::vector<uint32_t> face;
            std::string vert;
            while (ls >> vert) {
                auto it = unique.find(vert);
                if (it != unique.end()) {
                    face.push_back(it->second);
                    continue;
                }
                // Парсим "vi", "vi/ti", "vi//ni", "vi/ti/ni" (индексы 1-based).
                int vi = 0, ti = 0, ni = 0;
                std::sscanf(vert.c_str(), "%d/%d/%d", &vi, &ti, &ni);
                if (ti == 0) std::sscanf(vert.c_str(), "%d//%d", &vi, &ni);
                if (vi == 0) std::sscanf(vert.c_str(), "%d", &vi);

                Vertex out_v{};
                if (vi > 0 && vi <= (int)positions.size()) {
                    Vec3 p = positions[vi - 1];
                    out_v.px = p.x; out_v.py = p.y; out_v.pz = p.z;
                }
                if (ti > 0 && (size_t)(ti * 2) <= uvs.size()) {
                    out_v.u = uvs[(ti - 1) * 2 + 0];
                    out_v.v = uvs[(ti - 1) * 2 + 1];
                }
                if (ni > 0 && ni <= (int)normals.size()) {
                    Vec3 n = normals[ni - 1];
                    out_v.nx = n.x; out_v.ny = n.y; out_v.nz = n.z;
                }
                uint32_t index = (uint32_t)out.vertices.size();
                out.vertices.push_back(out_v);
                unique[vert] = index;
                face.push_back(index);
            }
            for (size_t k = 2; k < face.size(); ++k) {
                out.indices.push_back(face[0]);
                out.indices.push_back(face[k - 1]);
                out.indices.push_back(face[k]);
            }
        }
    }

    // Если в файле не было нормалей — считаем плоские по граням и разворачиваем
    // наружу от центроида (устойчиво для выпуклых форм, не зависит от winding).
    if (normals.empty() && !out.vertices.empty()) {
        Vec3 centroid{};
        for (const Vertex& v : out.vertices) {
            centroid = centroid + Vec3{v.px, v.py, v.pz};
        }
        centroid = centroid * (1.0f / (float)out.vertices.size());

        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            uint32_t ia = out.indices[i], ib = out.indices[i + 1], ic = out.indices[i + 2];
            Vec3 a{out.vertices[ia].px, out.vertices[ia].py, out.vertices[ia].pz};
            Vec3 b{out.vertices[ib].px, out.vertices[ib].py, out.vertices[ib].pz};
            Vec3 c{out.vertices[ic].px, out.vertices[ic].py, out.vertices[ic].pz};
            Vec3 nrm = normalize(cross(b - a, c - a));
            Vec3 faceCenter = (a + b + c) * (1.0f / 3.0f);
            if (dot(nrm, faceCenter - centroid) < 0.0f) {
                nrm = nrm * -1.0f;  // развернуть наружу
            }
            for (uint32_t idx : {ia, ib, ic}) {
                out.vertices[idx].nx = nrm.x;
                out.vertices[idx].ny = nrm.y;
                out.vertices[idx].nz = nrm.z;
            }
        }
    }

    // Тангент по умолчанию: устойчивый перпендикуляр к нормали. OBJ-меши обычно без
    // нормал-карты, точный тангент не нужен — важна лишь валидность TBN в шейдере.
    for (Vertex& v : out.vertices) {
        Vec3 N{v.nx, v.ny, v.nz};
        Vec3 ref = (std::fabs(N.y) < 0.99f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        Vec3 T = normalize(cross(ref, N));
        v.tx = T.x; v.ty = T.y; v.tz = T.z;
    }

    LOGI("OBJ загружен: %s — %u вершин, %u индексов", path,
         (uint32_t)out.vertices.size(), (uint32_t)out.indices.size());
    return !out.vertices.empty();
}
