#include "Assets.h"

#include <android/bitmap.h>
#include <android/imagedecoder.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Log.h"
#include "MathUtil.h"

namespace {

bool readAsset(AAssetManager* mgr, const char* path, std::string& out) {
    AAsset* asset = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        LOGW("Asset не найден: %s", path);
        return false;
    }
    off_t len = AAsset_getLength(asset);
    out.resize((size_t)len);
    int read = AAsset_read(asset, out.data(), (size_t)len);
    AAsset_close(asset);
    return read == (int)len;
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

bool loadImageAsset(AAssetManager* mgr, const char* path, TextureData& out) {
    AAsset* asset = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        LOGW("Изображение не найдено: %s", path);
        return false;
    }

    // AImageDecoder введён в API 30 — доступен всегда, т.к. minSdk = 30.
    bool ok = false;
    AImageDecoder* decoder = nullptr;
    if (AImageDecoder_createFromAAsset(asset, &decoder) == ANDROID_IMAGE_DECODER_SUCCESS) {
        const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(decoder);
        int w = AImageDecoderHeaderInfo_getWidth(info);
        int h = AImageDecoderHeaderInfo_getHeight(info);
        AImageDecoder_setAndroidBitmapFormat(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888);

        size_t stride = AImageDecoder_getMinimumStride(decoder);
        std::vector<uint8_t> tmp(stride * (size_t)h);
        if (AImageDecoder_decodeImage(decoder, tmp.data(), stride, tmp.size()) ==
            ANDROID_IMAGE_DECODER_SUCCESS) {
            out.width = (uint32_t)w;
            out.height = (uint32_t)h;
            out.rgba.resize((size_t)w * h * 4);
            // Уплотняем строки, если stride шире w*4.
            for (int row = 0; row < h; ++row) {
                std::memcpy(out.rgba.data() + (size_t)row * w * 4,
                            tmp.data() + (size_t)row * stride,
                            (size_t)w * 4);
            }
            ok = true;
        }
        AImageDecoder_delete(decoder);
    }

    AAsset_close(asset);
    return ok;
}

bool loadObjAsset(AAssetManager* mgr, const char* path, MeshData& out) {
    std::string text;
    if (!readAsset(mgr, path, text)) {
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

    LOGI("OBJ загружен: %s — %u вершин, %u индексов", path,
         (uint32_t)out.vertices.size(), (uint32_t)out.indices.size());
    return !out.vertices.empty();
}
