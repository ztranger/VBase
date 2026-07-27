#include "Model.h"

#include <cmath>
#include <cstring>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "Log.h"

namespace {

// Индекс узла по указателю cgltf.
int nodeIndex(const cgltf_data* data, const cgltf_node* node) {
    if (node == nullptr) return -1;
    return (int)(node - data->nodes);
}

// Прочитать весь ассет в память.
bool readAssetBytes(AAssetManager* mgr, const char* path, std::vector<uint8_t>& out) {
    AAsset* asset = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        LOGW("glTF-ассет не найден: %s", path);
        return false;
    }
    off_t len = AAsset_getLength(asset);
    out.resize((size_t)len);
    int read = AAsset_read(asset, out.data(), (size_t)len);
    AAsset_close(asset);
    return read == (int)len;
}

// Рекурсивно посчитать мировую матрицу узла (родитель раньше потомка).
void computeGlobal(const std::vector<ModelNode>& nodes, int i,
                   const std::vector<Mat4>& local, std::vector<Mat4>& global,
                   std::vector<char>& done) {
    if (done[i]) return;
    int p = nodes[i].parent;
    if (p >= 0) {
        computeGlobal(nodes, p, local, global, done);
        global[i] = global[p] * local[i];
    } else {
        global[i] = local[i];
    }
    done[i] = 1;
}

// Найти интервал ключей [k, k+1] для времени t и фактор смешивания f.
void findKey(const std::vector<float>& times, float t, int& k, float& f) {
    if (times.empty()) { k = 0; f = 0.0f; return; }
    if (t <= times.front()) { k = 0; f = 0.0f; return; }
    if (t >= times.back()) { k = (int)times.size() - 1; f = 0.0f; return; }
    for (int i = 0; i + 1 < (int)times.size(); ++i) {
        if (t >= times[i] && t < times[i + 1]) {
            k = i;
            f = (t - times[i]) / (times[i + 1] - times[i]);
            return;
        }
    }
    k = (int)times.size() - 1;
    f = 0.0f;
}

} // namespace

void SkinnedModel::sampleAnimation(int animIndex, float time, std::vector<Mat4>& out) const {
    size_t n = nodes.size();
    std::vector<Vec3> T(n);
    std::vector<Quat> R(n);
    std::vector<Vec3> S(n);
    for (size_t i = 0; i < n; ++i) {
        T[i] = nodes[i].t;
        R[i] = nodes[i].r;
        S[i] = nodes[i].s;
    }

    if (animIndex >= 0 && animIndex < (int)animations.size()) {
        const Animation& a = animations[animIndex];
        float t = (a.duration > 0.0f) ? std::fmod(time, a.duration) : 0.0f;
        for (const AnimChannel& c : a.channels) {
            if (c.node < 0 || c.times.empty()) continue;
            int k = 0;
            float f = 0.0f;
            findKey(c.times, t, k, f);
            if (c.step) f = 0.0f;
            int k1 = (k + 1 < (int)c.times.size()) ? k + 1 : k;

            if (c.path == 0 || c.path == 2) {  // translation / scale (vec3)
                Vec3 v0{c.values[k * 3 + 0], c.values[k * 3 + 1], c.values[k * 3 + 2]};
                Vec3 v1{c.values[k1 * 3 + 0], c.values[k1 * 3 + 1], c.values[k1 * 3 + 2]};
                Vec3 v = v0 + (v1 - v0) * f;
                if (c.path == 0) T[c.node] = v; else S[c.node] = v;
            } else if (c.path == 1) {  // rotation (quat)
                Quat q0{c.values[k * 4 + 0], c.values[k * 4 + 1], c.values[k * 4 + 2], c.values[k * 4 + 3]};
                Quat q1{c.values[k1 * 4 + 0], c.values[k1 * 4 + 1], c.values[k1 * 4 + 2], c.values[k1 * 4 + 3]};
                R[c.node] = slerp(q0, q1, f);
            }
        }
    }

    std::vector<Mat4> local(n);
    for (size_t i = 0; i < n; ++i) local[i] = trs(T[i], R[i], S[i]);

    std::vector<Mat4> global(n);
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; ++i) computeGlobal(nodes, (int)i, local, global, done);

    out.resize(jointNodes.size());
    for (size_t j = 0; j < jointNodes.size(); ++j) {
        out[j] = global[jointNodes[j]] * inverseBind[j];
    }
}

bool loadGltfModel(AAssetManager* mgr, const char* path, SkinnedModel& out) {
    std::vector<uint8_t> bytes;
    if (!readAssetBytes(mgr, path, bytes)) return false;

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
        LOGE("cgltf_parse failed: %s", path);
        return false;
    }
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success) {
        LOGE("cgltf_load_buffers failed: %s", path);
        cgltf_free(data);
        return false;
    }

    // Узлы + иерархия.
    out.nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& nd = data->nodes[i];
        ModelNode& mn = out.nodes[i];
        mn.t = {nd.translation[0], nd.translation[1], nd.translation[2]};
        mn.r = {nd.rotation[0], nd.rotation[1], nd.rotation[2], nd.rotation[3]};
        mn.s = {nd.scale[0], nd.scale[1], nd.scale[2]};
        mn.parent = nodeIndex(data, nd.parent);
    }

    // Геометрия: собираем все примитивы всех мешей в один буфер.
    for (size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& mesh = data->meshes[m];
        for (size_t p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& prim = mesh.primitives[p];
            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv = nullptr;
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                switch (at.type) {
                    case cgltf_attribute_type_position: pos = at.data; break;
                    case cgltf_attribute_type_normal:   nrm = at.data; break;
                    case cgltf_attribute_type_texcoord: if (!uv) uv = at.data; break;
                    case cgltf_attribute_type_joints:   if (!joints) joints = at.data; break;
                    case cgltf_attribute_type_weights:  if (!weights) weights = at.data; break;
                    default: break;
                }
            }
            if (pos == nullptr) continue;

            uint32_t base = (uint32_t)out.vertices.size();
            size_t count = pos->count;
            for (size_t i = 0; i < count; ++i) {
                SkinnedVertex v{};
                cgltf_accessor_read_float(pos, i, &v.px, 3);
                if (nrm) cgltf_accessor_read_float(nrm, i, &v.nx, 3);
                else { v.ny = 1.0f; }
                if (uv) cgltf_accessor_read_float(uv, i, &v.u, 2);
                if (joints) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(joints, i, j, 4);
                    for (int q = 0; q < 4; ++q) v.joints[q] = (float)j[q];
                }
                if (weights) cgltf_accessor_read_float(weights, i, v.weights, 4);
                else { v.weights[0] = 1.0f; }
                out.vertices.push_back(v);
            }

            if (prim.indices) {
                for (size_t i = 0; i < prim.indices->count; ++i) {
                    out.indices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
                }
            } else {
                for (size_t i = 0; i < count; ++i) out.indices.push_back(base + (uint32_t)i);
            }
        }
    }

    // Скин: кости + обратные bind-матрицы (берём первый).
    if (data->skins_count > 0) {
        const cgltf_skin& skin = data->skins[0];
        out.jointNodes.resize(skin.joints_count);
        out.inverseBind.resize(skin.joints_count);
        for (size_t j = 0; j < skin.joints_count; ++j) {
            out.jointNodes[j] = nodeIndex(data, skin.joints[j]);
            Mat4 ib;  // glTF хранит матрицы column-major -> прямо в .m
            if (skin.inverse_bind_matrices) {
                cgltf_accessor_read_float(skin.inverse_bind_matrices, j, ib.m, 16);
            }
            out.inverseBind[j] = ib;
        }
        if (skin.joints_count > (size_t)SkinnedModel::kMaxJoints) {
            LOGW("Костей %u > лимита %d — часть не влезет в uJoints[]",
                 (uint32_t)skin.joints_count, SkinnedModel::kMaxJoints);
        }
    }

    // Анимации.
    for (size_t ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& anim = data->animations[ai];
        Animation A;
        A.name = anim.name ? anim.name : ("anim" + std::to_string(ai));
        for (size_t ci = 0; ci < anim.channels_count; ++ci) {
            const cgltf_animation_channel& ch = anim.channels[ci];
            if (!ch.target_node || !ch.sampler) continue;
            AnimChannel c;
            c.node = nodeIndex(data, ch.target_node);
            if (ch.target_path == cgltf_animation_path_type_translation) c.path = 0;
            else if (ch.target_path == cgltf_animation_path_type_rotation) c.path = 1;
            else if (ch.target_path == cgltf_animation_path_type_scale) c.path = 2;
            else continue;  // weights и прочее не поддерживаем
            c.step = (ch.sampler->interpolation == cgltf_interpolation_type_step);

            const cgltf_accessor* in = ch.sampler->input;
            const cgltf_accessor* outAcc = ch.sampler->output;
            size_t nk = in->count;
            c.times.resize(nk);
            for (size_t k = 0; k < nk; ++k) cgltf_accessor_read_float(in, k, &c.times[k], 1);
            int comp = (c.path == 1) ? 4 : 3;
            c.values.resize(nk * comp);
            for (size_t k = 0; k < nk; ++k) {
                cgltf_accessor_read_float(outAcc, k, &c.values[k * comp], comp);
            }
            if (!c.times.empty()) A.duration = std::fmax(A.duration, c.times.back());
            A.channels.push_back(std::move(c));
        }
        out.animations.push_back(std::move(A));
    }

    LOGI("glTF загружен: %s — %u верш., %u инд., %u костей, %u анимаций", path,
         (uint32_t)out.vertices.size(), (uint32_t)out.indices.size(),
         (uint32_t)out.jointNodes.size(), (uint32_t)out.animations.size());

    cgltf_free(data);
    return !out.vertices.empty() && !out.jointNodes.empty();
}
