#include "game/SceneLoader.h"

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "engine/assets/AssetSource.h"
#include "engine/core/Log.h"

namespace {

using Tokens = std::vector<std::string>;

Tokens tokenize(const std::string& s) {
    Tokens t;
    std::istringstream in(s);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}

float toF(const std::string& s) { return std::strtof(s.c_str(), nullptr); }
int toI(const std::string& s) { return (int)std::strtol(s.c_str(), nullptr, 10); }

// Курсорное чтение аргументов ключа с проверкой границ (i продвигается).
bool readF(const Tokens& t, size_t& i, int line, float& out) {
    if (i >= t.size()) { LOGE("scene: строка %d: ожидалось число", line); return false; }
    out = toF(t[i++]);
    return true;
}
bool readI(const Tokens& t, size_t& i, int line, int& out) {
    if (i >= t.size()) { LOGE("scene: строка %d: ожидалось целое", line); return false; }
    out = toI(t[i++]);
    return true;
}
bool readStr(const Tokens& t, size_t& i, int line, std::string& out) {
    if (i >= t.size()) { LOGE("scene: строка %d: ожидался идентификатор", line); return false; }
    out = t[i++];
    return true;
}
bool readVec3(const Tokens& t, size_t& i, int line, Vec3& out) {
    return readF(t, i, line, out.x) && readF(t, i, line, out.y) && readF(t, i, line, out.z);
}

bool parseShader(const std::string& s, int line, ShaderType& out) {
    if (s == "lit") out = ShaderType::Lit;
    else if (s == "unlit") out = ShaderType::Unlit;
    else if (s == "phong") out = ShaderType::Phong;
    else { LOGE("scene: строка %d: неизвестный шейдер '%s'", line, s.c_str()); return false; }
    return true;
}

}  // namespace

bool loadSceneDesc(AssetSource& assets, const char* path, SceneDesc& out) {
    std::vector<uint8_t> bytes;
    if (!assets.read(path, bytes)) {
        LOGE("scene: файл не найден: %s", path);
        return false;
    }
    out = SceneDesc{};  // дефолты (свет/камера)

    std::string text(bytes.begin(), bytes.end());
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();  // CRLF-файлы
        size_t hash = raw.find('#');  // комментарий до конца строки
        if (hash != std::string::npos) raw.erase(hash);
        Tokens t = tokenize(raw);
        if (t.empty()) continue;

        const std::string& cmd = t[0];

        if (cmd == "mesh") {
            // mesh <name> plane <size> [uvTiles] | cube <size> | sphere <r> [stacks] [slices]
            if (t.size() < 4) { LOGE("scene: строка %d: mesh требует имя, тип и параметры", line); return false; }
            MeshSpec m;
            m.name = t[1];
            const std::string& kind = t[2];
            if (kind == "plane") {
                m.kind = MeshSpec::Plane;
                m.a = toF(t[3]);
                m.b = (t.size() > 4) ? toF(t[4]) : 1.0f;
            } else if (kind == "cube") {
                m.kind = MeshSpec::Cube;
                m.a = toF(t[3]);
            } else if (kind == "sphere") {
                m.kind = MeshSpec::Sphere;
                m.a = toF(t[3]);
                if (t.size() > 4) m.stacks = toI(t[4]);
                if (t.size() > 5) m.slices = toI(t[5]);
            } else {
                LOGE("scene: строка %d: неизвестный примитив '%s'", line, kind.c_str());
                return false;
            }
            out.meshes.push_back(m);

        } else if (cmd == "texture") {
            // texture <name> procedural <size> <cells> | image <path>
            if (t.size() < 3) { LOGE("scene: строка %d: texture требует имя и тип", line); return false; }
            TextureSpec tx;
            tx.name = t[1];
            const std::string& kind = t[2];
            if (kind == "procedural") {
                tx.kind = TextureSpec::Checker;
                if (t.size() < 5) { LOGE("scene: строка %d: procedural <size> <cells>", line); return false; }
                tx.size = toI(t[3]);
                tx.cells = toI(t[4]);
            } else if (kind == "image") {
                tx.kind = TextureSpec::Image;
                if (t.size() < 4) { LOGE("scene: строка %d: image <path>", line); return false; }
                tx.path = t[3];
            } else {
                LOGE("scene: строка %d: неизвестный тип текстуры '%s'", line, kind.c_str());
                return false;
            }
            out.textures.push_back(tx);

        } else if (cmd == "material") {
            // material <name> <shader> [color r g b] [tex <ref>]
            if (t.size() < 3) { LOGE("scene: строка %d: material требует имя и шейдер", line); return false; }
            MaterialSpec m;
            m.name = t[1];
            if (!parseShader(t[2], line, m.shader)) return false;
            size_t i = 3;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "color") { if (!readVec3(t, i, line, m.color)) return false; }
                else if (k == "tex") { if (!readStr(t, i, line, m.tex)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ material '%s'", line, k.c_str()); return false; }
            }
            out.materials.push_back(m);

        } else if (cmd == "object" || cmd == "ring") {
            // object <mesh> mat <mat> [pos x y z] [scale s] [rot x y z] [spin s]
            // ring   <mesh> mat <mat> count <n> radius <r> [y <y>] [scale s] [spin s]
            if (t.size() < 2) { LOGE("scene: строка %d: %s требует имя меша", line, cmd.c_str()); return false; }
            ObjectSpec o;
            o.mesh = t[1];
            o.ring = (cmd == "ring");
            size_t i = 2;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "mat") { if (!readStr(t, i, line, o.material)) return false; }
                else if (k == "pos") { if (!readVec3(t, i, line, o.pos)) return false; }
                else if (k == "rot") { if (!readVec3(t, i, line, o.rot)) return false; }
                else if (k == "scale") { if (!readF(t, i, line, o.scale)) return false; }
                else if (k == "spin") { if (!readF(t, i, line, o.spin)) return false; }
                else if (k == "count") { if (!readI(t, i, line, o.ringCount)) return false; }
                else if (k == "radius") { if (!readF(t, i, line, o.ringRadius)) return false; }
                else if (k == "y") { if (!readF(t, i, line, o.ringY)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ %s '%s'", line, cmd.c_str(), k.c_str()); return false; }
            }
            out.objects.push_back(o);

        } else if (cmd == "collider") {
            // collider box center <x y z> half <hx hy hz>
            if (t.size() < 2) { LOGE("scene: строка %d: collider требует тип", line); return false; }
            ColliderSpec c;
            const std::string& kind = t[1];
            if (kind != "box") { LOGE("scene: строка %d: неизвестный тип коллайдера '%s'", line, kind.c_str()); return false; }
            c.kind = ColliderSpec::Box;
            size_t i = 2;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "center") { if (!readVec3(t, i, line, c.center)) return false; }
                else if (k == "half") { if (!readVec3(t, i, line, c.half)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ collider '%s'", line, k.c_str()); return false; }
            }
            out.colliders.push_back(c);

        } else if (cmd == "generator" || cmd == "storage" || cmd == "spawner" ||
                   cmd == "core" || cmd == "tower") {
            // generator pos <x y z> rate <r>  |  storage pos <x y z> cap <c>
            // spawner   pos <x y z> interval <s> max <n>  |  core pos <x y z>
            // tower     pos <x y z>   (параметры damage/range/interval — из конфига)
            BuildingSpec b;
            if (cmd == "generator") b.kind = BuildingSpec::Generator;
            else if (cmd == "storage") b.kind = BuildingSpec::Storage;
            else if (cmd == "spawner") b.kind = BuildingSpec::Spawner;
            else if (cmd == "tower") b.kind = BuildingSpec::Tower;
            else b.kind = BuildingSpec::Core;
            size_t i = 1;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "pos") { if (!readVec3(t, i, line, b.pos)) return false; }
                else if (k == "rate") { if (!readF(t, i, line, b.rate)) return false; }
                else if (k == "cap") { if (!readF(t, i, line, b.cap)) return false; }
                else if (k == "interval") { if (!readF(t, i, line, b.rate)) return false; }  // spawner/tower: интервал -> rate
                else if (k == "max") { if (!readF(t, i, line, b.cap)) return false; }        // spawner: максимум -> cap
                else if (k == "hp") { if (!readF(t, i, line, b.hp)) return false; }          // core
                else if (k == "damage") { if (!readF(t, i, line, b.damage)) return false; }  // tower
                else if (k == "range") { if (!readF(t, i, line, b.range)) return false; }    // tower
                else if (k == "team") { int tm = 0; if (!readI(t, i, line, tm)) return false; b.team = (uint8_t)tm; }
                else { LOGE("scene: строка %d: неизвестный ключ %s '%s'", line, cmd.c_str(), k.c_str()); return false; }
            }
            out.buildings.push_back(b);

        } else if (cmd == "player") {
            // player model <path> [pos x y z] [scale s] [yaw o] [capsule <radius> <cylHalf>]
            //               [hide sub1,sub2,...]  <- скрыть меши-узлы по подстроке имени
            out.player.present = true;
            size_t i = 1;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "model") { if (!readStr(t, i, line, out.player.model)) return false; }
                else if (k == "pos") { if (!readVec3(t, i, line, out.player.pos)) return false; }
                else if (k == "scale") { if (!readF(t, i, line, out.player.scale)) return false; }
                else if (k == "yaw") { if (!readF(t, i, line, out.player.yawOffset)) return false; }
                else if (k == "capsule") {
                    if (!readF(t, i, line, out.player.colliderRadius)) return false;
                    if (!readF(t, i, line, out.player.colliderCylHalf)) return false;
                }
                else if (k == "hide") {
                    std::string list;
                    if (!readStr(t, i, line, list)) return false;
                    size_t s = 0;  // разбор списка "a,b,c" по запятым
                    while (s < list.size()) {
                        size_t e = list.find(',', s);
                        if (e == std::string::npos) e = list.size();
                        std::string item = list.substr(s, e - s);
                        if (!item.empty()) out.player.hideNodes.push_back(item);
                        s = e + 1;
                    }
                }
                else { LOGE("scene: строка %d: неизвестный ключ player '%s'", line, k.c_str()); return false; }
            }

        } else if (cmd == "spawn") {
            // spawn team <n> pos <x y z> — точка спавна стороны (PvP; сервер сажает игрока сюда)
            SpawnSpec sp;
            size_t i = 1;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "team") { int tm = 0; if (!readI(t, i, line, tm)) return false; sp.team = (uint8_t)tm; }
                else if (k == "pos") { if (!readVec3(t, i, line, sp.pos)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ spawn '%s'", line, k.c_str()); return false; }
            }
            out.spawns.push_back(sp);

        } else if (cmd == "matchrestart") {
            // matchrestart <сек> — авто-рестарт матча после исхода (0/нет директивы = выкл)
            size_t i = 1;
            if (!readF(t, i, line, out.matchRestartDelay)) return false;

        } else if (cmd == "light") {
            // light dir <x> <y> <z>
            size_t i = 1;
            if (i < t.size() && t[i] == "dir") { ++i; if (!readVec3(t, i, line, out.lightDir)) return false; }
            else { LOGE("scene: строка %d: light dir <x> <y> <z>", line); return false; }

        } else if (cmd == "camera") {
            // camera [distance d] [height h] [lookHeight l] [fov f] [near n] [far f]
            size_t i = 1;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "distance") { if (!readF(t, i, line, out.camera.distance)) return false; }
                else if (k == "height") { if (!readF(t, i, line, out.camera.height)) return false; }
                else if (k == "pitch") { if (!readF(t, i, line, out.camera.pitch)) return false; }
                else if (k == "lookHeight") { if (!readF(t, i, line, out.camera.lookHeight)) return false; }
                else if (k == "fov") { if (!readF(t, i, line, out.camera.fovY)) return false; }
                else if (k == "near") { if (!readF(t, i, line, out.camera.nearZ)) return false; }
                else if (k == "far") { if (!readF(t, i, line, out.camera.farZ)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ camera '%s'", line, k.c_str()); return false; }
            }

        } else if (cmd == "grid") {
            // grid cell <размер клетки> arena <полуразмер зоны строительства>
            size_t i = 1;
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "cell") { if (!readF(t, i, line, out.grid.cell)) return false; }
                else if (k == "arena") { if (!readF(t, i, line, out.grid.arenaHalf)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ grid '%s'", line, k.c_str()); return false; }
            }

        } else {
            LOGE("scene: строка %d: неизвестная директива '%s'", line, cmd.c_str());
            return false;
        }
    }
    return true;
}

namespace {
std::string trimSpaces(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}
bool typeFromName(const std::string& s, EntityType& out) {
    if (s == "generator") out = EntityType::Generator;
    else if (s == "storage") out = EntityType::Storage;
    else if (s == "spawner") out = EntityType::Spawner;
    else if (s == "enemy") out = EntityType::Enemy;
    else if (s == "tower") out = EntityType::Tower;
    else if (s == "core") out = EntityType::Core;
    else if (s == "hero") out = EntityType::Hero;
    else return false;
    return true;
}
}  // namespace

bool loadBuildingConfig(AssetSource& assets, const char* path, BuildingConfig& out) {
    std::vector<uint8_t> bytes;
    if (!assets.read(path, bytes)) {
        LOGW("config: файл не найден: %s (здания на дефолтах)", path);
        return false;
    }
    out = BuildingConfig{};

    std::string text(bytes.begin(), bytes.end());
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    EntityType cur = EntityType::Hero;
    bool haveCur = false;
    // Формат: блок `building <тип>`, затем ключи `name`/`desc` (весь остаток строки —
    // значение) и `rate`/`cap`/`interval`/`max` (число).
    while (std::getline(in, raw)) {
        ++line;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.erase(hash);
        std::string lt = trimSpaces(raw);
        if (lt.empty()) continue;
        size_t sp = lt.find_first_of(" \t");
        std::string key = (sp == std::string::npos) ? lt : lt.substr(0, sp);
        std::string val = (sp == std::string::npos) ? "" : trimSpaces(lt.substr(sp + 1));

        if (key == "building") {
            if (!typeFromName(val, cur)) { LOGE("config: строка %d: неизвестный тип '%s'", line, val.c_str()); return false; }
            haveCur = true;
            out.byType[(int)cur].defined = true;
        } else if (!haveCur) {
            LOGE("config: строка %d: ключ '%s' вне блока building", line, key.c_str());
            return false;
        } else {
            BuildingInfo& bi = out.byType[(int)cur];
            if (key == "name") bi.name = val;
            else if (key == "desc") bi.desc = val;
            else if (key == "rate" || key == "interval") bi.rate = toF(val);
            else if (key == "cap" || key == "max") bi.cap = toF(val);
            else if (key == "hp") bi.hp = toF(val);
            else if (key == "damage") bi.damage = toF(val);
            else if (key == "range") bi.range = toF(val);
            else if (key == "cost") bi.cost = toF(val);
            // --- Визуал (клиент) ---
            else if (key == "shape") {
                Tokens a = tokenize(val);
                if (a.empty()) { LOGE("config: строка %d: shape без формы", line); return false; }
                if (a[0] == "cube") {
                    bi.shape = MeshShape::Cube;
                    if (a.size() > 1) bi.shapeSize = toF(a[1]);
                } else if (a[0] == "sphere") {
                    bi.shape = MeshShape::Sphere;
                    if (a.size() > 1) bi.shapeSize = toF(a[1]);
                    if (a.size() > 2) bi.shapeStacks = toI(a[2]);
                    if (a.size() > 3) bi.shapeSlices = toI(a[3]);
                } else {
                    LOGE("config: строка %d: неизвестная форма '%s'", line, a[0].c_str());
                    return false;
                }
            } else if (key == "material") {
                Tokens a = tokenize(val);
                if (a.empty() || !parseShader(a[0], line, bi.shader)) return false;
                if (a.size() > 1) bi.color.x = toF(a[1]);
                if (a.size() > 2) bi.color.y = toF(a[2]);
                if (a.size() > 3) bi.color.z = toF(a[3]);
            } else if (key == "model") {
                // model <obj> <texture> [scale] [yawDeg] — статичная модель вместо shape.
                Tokens a = tokenize(val);
                if (a.size() < 2) { LOGE("config: строка %d: model требует путь и текстуру", line); return false; }
                bi.model = a[0];
                bi.modelTex = a[1];
                if (a.size() > 2) bi.modelScale = toF(a[2]);
                if (a.size() > 3) bi.modelYawDeg = toF(a[3]);
            } else if (key == "yoffset") bi.yOffset = toF(val);
            else if (key == "pickradius") bi.pickRadius = toF(val);
            else { LOGE("config: строка %d: неизвестный ключ '%s'", line, key.c_str()); return false; }
        }
    }
    return true;
}

void applyBuildingConfig(SceneDesc& desc, const BuildingConfig& cfg) {
    for (BuildingSpec& b : desc.buildings) {
        EntityType t;
        switch (b.kind) {
            case BuildingSpec::Generator: t = EntityType::Generator; break;
            case BuildingSpec::Storage:   t = EntityType::Storage;   break;
            case BuildingSpec::Spawner:   t = EntityType::Spawner;   break;
            case BuildingSpec::Tower:     t = EntityType::Tower;     break;
            case BuildingSpec::Core:      t = EntityType::Core;      break;
            default: continue;
        }
        const BuildingInfo& bi = cfg.get(t);
        if (bi.defined) {
            b.rate = bi.rate;
            b.cap = bi.cap;
            b.hp = bi.hp;
            b.damage = bi.damage;
            b.range = bi.range;
        }
    }
    // Боевые статы врага (враг не размещается в сцене — берём из блока `building enemy`).
    const BuildingInfo& en = cfg.get(EntityType::Enemy);
    if (en.defined) {
        if (en.hp > 0.0f) desc.enemy.hp = en.hp;
        if (en.damage > 0.0f) desc.enemy.damage = en.damage;
        if (en.rate > 0.0f) desc.enemy.attackInterval = en.rate;
    }

    // Ставки героя (блок `building hero`): hp = здоровье, rate = задержка респауна, сек.
    const BuildingInfo& hr = cfg.get(EntityType::Hero);
    if (hr.defined) {
        if (hr.hp > 0.0f) desc.player.hp = hr.hp;
        if (hr.rate > 0.0f) desc.player.respawnDelay = hr.rate;
    }

    // Шаблоны построек героя: любой тип с cost>0 в конфиге становится доступным к возведению.
    for (int t = 0; t < 8; ++t) {
        const BuildingInfo& bi = cfg.byType[t];
        BuildTemplate& bt = desc.build[t];
        bt.buildable = bi.defined && bi.cost > 0.0f;
        bt.cost = bi.cost;
        bt.rate = bi.rate;
        bt.cap = bi.cap;
        bt.hp = bi.hp;
        bt.damage = bi.damage;
        bt.range = bi.range;
    }
}
