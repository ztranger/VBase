#include "engine/assets/Model.h"

#include <cctype>
#include <cmath>
#include <cstring>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "engine/assets/Assets.h"
#include "engine/core/Log.h"

namespace {

// Индекс узла по указателю cgltf.
int nodeIndex(const cgltf_data* data, const cgltf_node* node) {
    if (node == nullptr) return -1;
    return (int)(node - data->nodes);
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

// Трансформ точки матрицей (column-major, v' = M*v).
Vec3 mulPoint(const Mat4& m, Vec3 p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}
// Трансформ направления (без переноса) — для нормалей (годится для жёстких трансформов).
Vec3 mulDir(const Mat4& m, Vec3 d) {
    return {m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
            m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
            m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z};
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

// Из позы (локальные T/R/S) -> матрицы костей: local -> global по иерархии,
// затем global * inverseBind на каждую кость.
void poseToJoints(const SkinnedModel& m,
                  const std::vector<Vec3>& T, const std::vector<Quat>& R,
                  const std::vector<Vec3>& S, std::vector<Mat4>& out) {
    size_t n = m.nodes.size();
    std::vector<Mat4> local(n);
    for (size_t i = 0; i < n; ++i) local[i] = trs(T[i], R[i], S[i]);

    std::vector<Mat4> global(n);
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; ++i) computeGlobal(m.nodes, (int)i, local, global, done);

    out.resize(m.jointNodes.size());
    for (size_t j = 0; j < m.jointNodes.size(); ++j) {
        out[j] = global[m.jointNodes[j]] * m.inverseBind[j];
    }
}

} // namespace

void SkinnedModel::samplePose(int animIndex, float time,
                              std::vector<Vec3>& T, std::vector<Quat>& R,
                              std::vector<Vec3>& S) const {
    size_t n = nodes.size();
    T.resize(n);
    R.resize(n);
    S.resize(n);
    for (size_t i = 0; i < n; ++i) {  // база = TRS узла
        T[i] = nodes[i].t;
        R[i] = nodes[i].r;
        S[i] = nodes[i].s;
    }

    if (animIndex < 0 || animIndex >= (int)animations.size()) return;
    const Animation& a = animations[animIndex];
    float t = (a.duration > 0.0f) ? std::fmod(time, a.duration) : 0.0f;
    for (const AnimChannel& c : a.channels) {
        if (c.node < 0 || c.times.empty()) continue;
        int k = 0;
        float f = 0.0f;
        findKey(c.times, t, k, f);
        if (c.step) f = 0.0f;
        int k1 = (k + 1 < (int)c.times.size()) ? k + 1 : k;

        if (c.path == 0 || c.path == 2) {  // translation / scale
            Vec3 v0{c.values[k * 3 + 0], c.values[k * 3 + 1], c.values[k * 3 + 2]};
            Vec3 v1{c.values[k1 * 3 + 0], c.values[k1 * 3 + 1], c.values[k1 * 3 + 2]};
            Vec3 v = v0 + (v1 - v0) * f;
            if (c.path == 0) T[c.node] = v; else S[c.node] = v;
        } else if (c.path == 1) {  // rotation
            Quat q0{c.values[k * 4 + 0], c.values[k * 4 + 1], c.values[k * 4 + 2], c.values[k * 4 + 3]};
            Quat q1{c.values[k1 * 4 + 0], c.values[k1 * 4 + 1], c.values[k1 * 4 + 2], c.values[k1 * 4 + 3]};
            R[c.node] = slerp(q0, q1, f);
        }
    }
}

void SkinnedModel::sampleAnimation(int animIndex, float time, std::vector<Mat4>& out) const {
    std::vector<Vec3> T, S;
    std::vector<Quat> R;
    samplePose(animIndex, time, T, R, S);
    poseToJoints(*this, T, R, S, out);
}

void SkinnedModel::sampleBlend(int animA, float timeA, int animB, float timeB,
                               float blend, std::vector<Mat4>& out) const {
    std::vector<Vec3> Ta, Sa, Tb, Sb;
    std::vector<Quat> Ra, Rb;
    samplePose(animA, timeA, Ta, Ra, Sa);
    samplePose(animB, timeB, Tb, Rb, Sb);

    size_t n = nodes.size();
    std::vector<Vec3> T(n), S(n);
    std::vector<Quat> R(n);
    for (size_t i = 0; i < n; ++i) {
        T[i] = Ta[i] + (Tb[i] - Ta[i]) * blend;  // позиция линейно
        S[i] = Sa[i] + (Sb[i] - Sa[i]) * blend;  // масштаб линейно
        R[i] = slerp(Ra[i], Rb[i], blend);       // поворот по дуге
    }
    poseToJoints(*this, T, R, S, out);
}

int SkinnedModel::findAnimation(const std::vector<std::string>& keywords, int fallback) const {
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    // Ключевые слова приводим к нижнему регистру заранее — можно передавать имя клипа
    // в любом регистре (напр. "Spellcast_Shoot" из ростера).
    std::vector<std::string> kws;
    kws.reserve(keywords.size());
    for (const std::string& kw : keywords) kws.push_back(lower(kw));

    // Проход 1: точное совпадение имени (без регистра) с любым ключевым словом.
    for (size_t i = 0; i < animations.size(); ++i) {
        std::string nm = lower(animations[i].name);
        for (const std::string& kw : kws) {
            if (nm == kw) return (int)i;
        }
    }
    // Проход 2: имя содержит ключевое слово как подстроку (Walking_A -> "walk" и т.п.).
    for (size_t i = 0; i < animations.size(); ++i) {
        std::string nm = lower(animations[i].name);
        for (const std::string& kw : kws) {
            if (!kw.empty() && nm.find(kw) != std::string::npos) return (int)i;
        }
    }
    return fallback;
}

bool loadGltfModel(AssetSource& src, const char* path, SkinnedModel& out,
                   const std::vector<std::string>* hideNodes) {
    std::vector<uint8_t> bytes;
    if (!src.read(path, bytes)) {
        LOGW("glTF-ассет не найден: %s", path);
        return false;
    }

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

    // Скин: кости + обратные bind-матрицы (первый скин). Читаем ДО геометрии — нужно,
    // чтобы привязать незаскиненные меши-аксессуары к ближайшей кости.
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

    // Узел -> индекс кости (для поиска кости-предка у статичных мешей).
    std::vector<int> nodeToJoint(out.nodes.size(), -1);
    for (size_t j = 0; j < out.jointNodes.size(); ++j) {
        if (out.jointNodes[j] >= 0) nodeToJoint[out.jointNodes[j]] = (int)j;
    }
    // Мировые матрицы узлов в bind-позе (по базовым TRS) — чтобы пред-трансформировать
    // вершины статичных аксессуаров (шляпа/плащ/посох) в мировое положение скелета.
    std::vector<Mat4> restGlobal(out.nodes.size());
    {
        std::vector<Mat4> local(out.nodes.size());
        for (size_t i = 0; i < out.nodes.size(); ++i)
            local[i] = trs(out.nodes[i].t, out.nodes[i].r, out.nodes[i].s);
        std::vector<char> done(out.nodes.size(), 0);
        for (size_t i = 0; i < out.nodes.size(); ++i)
            computeGlobal(out.nodes, (int)i, local, restGlobal, done);
    }
    // Узел, ссылающийся на каждый меш (для трансформа и фильтра по имени).
    std::vector<int> meshNode(data->meshes_count, -1);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        if (data->nodes[i].mesh) {
            size_t mi = (size_t)(data->nodes[i].mesh - data->meshes);
            if (mi < (size_t)data->meshes_count) meshNode[mi] = (int)i;
        }
    }
    // Ближайшая кость вверх по иерархии от узла (включая сам узел).
    auto ancestorJoint = [&](int node) -> int {
        for (int c = node; c >= 0; c = out.nodes[c].parent)
            if (nodeToJoint[c] >= 0) return nodeToJoint[c];
        return -1;
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };

    // Геометрия: собираем все примитивы всех мешей в один буфер.
    for (size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& mesh = data->meshes[m];
        int meshN = (m < (size_t)meshNode.size()) ? meshNode[m] : -1;
        // Фильтр: скрыть меши по подстроке имени узла (лишний реквизит KayKit и т.п.).
        if (hideNodes && meshN >= 0 && data->nodes[meshN].name) {
            std::string nm = lower(data->nodes[meshN].name);
            bool hidden = false;
            for (const std::string& h : *hideNodes) {
                if (!h.empty() && nm.find(lower(h)) != std::string::npos) { hidden = true; break; }
            }
            if (hidden) continue;
        }
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

            // Незаскиненный меш (аксессуар: шляпа/плащ/посох) — привязываем к ближайшей
            // кости-предку и пред-трансформируем вершины в мировое положение bind-позы.
            // Формула точна: скиннинг даёт global(B)*inverseBind[B]*pos', где inverseBind[B]
            // == restGlobal(B)^-1, а pos' = restGlobal(N)*pos -> итог = animGlobal(N)*pos.
            const bool skinned = (joints != nullptr);
            const int attachJoint = skinned ? -1 : ((meshN >= 0) ? ancestorJoint(meshN) : -1);
            const Mat4 xform = (!skinned && meshN >= 0) ? restGlobal[meshN] : Mat4::identity();

            uint32_t base = (uint32_t)out.vertices.size();
            size_t count = pos->count;
            for (size_t i = 0; i < count; ++i) {
                SkinnedVertex v{};
                cgltf_accessor_read_float(pos, i, &v.px, 3);
                if (nrm) cgltf_accessor_read_float(nrm, i, &v.nx, 3);
                else { v.ny = 1.0f; }
                if (uv) cgltf_accessor_read_float(uv, i, &v.u, 2);
                if (skinned) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(joints, i, j, 4);
                    for (int q = 0; q < 4; ++q) v.joints[q] = (float)j[q];
                    if (weights) cgltf_accessor_read_float(weights, i, v.weights, 4);
                    else { v.weights[0] = 1.0f; }
                } else {
                    Vec3 pp = mulPoint(xform, Vec3{v.px, v.py, v.pz});
                    Vec3 dn = mulDir(xform, Vec3{v.nx, v.ny, v.nz});
                    v.px = pp.x; v.py = pp.y; v.pz = pp.z;
                    v.nx = dn.x; v.ny = dn.y; v.nz = dn.z;
                    v.joints[0] = (float)(attachJoint >= 0 ? attachJoint : 0);
                    v.weights[0] = 1.0f;
                }
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

    // Альбедо-текстура: берём base color первого материала, декодируем из
    // байтов buffer_view (для .glb картинка встроена в бинарный чанк).
    const cgltf_image* image = nullptr;
    if (data->materials_count > 0) {
        const cgltf_material& mat = data->materials[0];
        if (mat.has_pbr_metallic_roughness &&
            mat.pbr_metallic_roughness.base_color_texture.texture &&
            mat.pbr_metallic_roughness.base_color_texture.texture->image) {
            image = mat.pbr_metallic_roughness.base_color_texture.texture->image;
        }
    }
    if (image == nullptr && data->images_count > 0) {
        image = &data->images[0];  // запасной вариант
    }
    if (image && image->buffer_view) {
        const cgltf_buffer_view* bv = image->buffer_view;
        if (bv->buffer && bv->buffer->data) {
            const uint8_t* bytes = (const uint8_t*)bv->buffer->data + bv->offset;
            if (decodeImageBuffer(bytes, bv->size, out.baseColor)) {
                out.hasTexture = true;
                LOGI("glTF текстура: %ux%u", out.baseColor.width, out.baseColor.height);
            } else {
                LOGW("Не удалось декодировать встроенную текстуру glTF");
            }
        }
    }

    LOGI("glTF загружен: %s — %u верш., %u инд., %u костей, %u анимаций", path,
         (uint32_t)out.vertices.size(), (uint32_t)out.indices.size(),
         (uint32_t)out.jointNodes.size(), (uint32_t)out.animations.size());

    cgltf_free(data);
    return !out.vertices.empty() && !out.jointNodes.empty();
}
