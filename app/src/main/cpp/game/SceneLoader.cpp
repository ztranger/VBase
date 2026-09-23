#include "game/SceneLoader.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    else if (s == "ground") out = ShaderType::Ground;  // земля: тинт к горизонту (край карты)
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
    return parseSceneDesc(std::string(bytes.begin(), bytes.end()), out);
}

bool parseSceneDesc(const std::string& text, SceneDesc& out) {
    out = SceneDesc{};  // дефолты (свет/камера)

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
            } else if (kind == "rect") {
                // mesh <name> rect <sizeX> <sizeZ> [uvX] [uvZ] — прямоугольная плоскость (дорожки)
                if (t.size() < 5) { LOGE("scene: строка %d: rect <sizeX> <sizeZ> [uvX] [uvZ]", line); return false; }
                m.kind = MeshSpec::Rect;
                m.a = toF(t[3]);
                m.b = toF(t[4]);
                m.uvX = (t.size() > 5) ? toF(t[5]) : 1.0f;
                m.uvZ = (t.size() > 6) ? toF(t[6]) : 1.0f;
            } else if (kind == "terrain") {
                // mesh <name> terrain <size> <cells> <amp> <freq> [uvTiles] — подразбитый пол с рельефом
                if (t.size() < 7) { LOGE("scene: строка %d: terrain <size> <cells> <amp> <freq> [uvTiles]", line); return false; }
                m.kind = MeshSpec::Terrain;
                m.a = toF(t[3]);
                m.stacks = toI(t[4]);
                m.amp = toF(t[5]);
                m.freq = toF(t[6]);
                m.b = (t.size() > 7) ? toF(t[7]) : 1.0f;
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
            } else if (kind == "procnormal") {
                tx.kind = TextureSpec::Bump;
                if (t.size() < 5) { LOGE("scene: строка %d: procnormal <size> <freq>", line); return false; }
                tx.size = toI(t[3]);
                tx.cells = toI(t[4]);  // частота волн
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
                else if (k == "normal") { if (!readStr(t, i, line, m.normal)) return false; }
                else { LOGE("scene: строка %d: неизвестный ключ material '%s'", line, k.c_str()); return false; }
            }
            out.materials.push_back(m);

        } else if (cmd == "object" || cmd == "ring") {
            // Процедурный меш:  object <mesh> mat <mat> [pos x y z] [scale s] [rot x y z] [spin s]
            //                   ring   <mesh> mat <mat> count <n> radius <r> [y <y>] [scale s] [spin s]
            // glTF-модель (декор): object model <path.glb> [tex <path>] [shader lit|unlit|phong]
            //                             [pos x y z] [rot x y z] [scale s] [spin s]   (+ ring: count/radius/y)
            if (t.size() < 2) { LOGE("scene: строка %d: %s требует имя меша или model", line, cmd.c_str()); return false; }
            ObjectSpec o;
            o.ring = (cmd == "ring");
            size_t i = 2;
            if (t[1] == "model") {          // форма glTF-модели: путь идёт вторым токеном
                if (!readStr(t, i, line, o.model)) return false;
            } else {
                o.mesh = t[1];              // процедурная форма: имя меша
            }
            while (i < t.size()) {
                std::string k = t[i++];
                if (k == "mat") { if (!readStr(t, i, line, o.material)) return false; }
                else if (k == "tex") { if (!readStr(t, i, line, o.modelTex)) return false; }
                else if (k == "color") { if (!readVec3(t, i, line, o.color)) return false; }  // тинт model
                else if (k == "shader") {
                    std::string s;
                    if (!readStr(t, i, line, s) || !parseShader(s, line, o.shader)) return false;
                }
                else if (k == "pos") { if (!readVec3(t, i, line, o.pos)) return false; }
                else if (k == "rot") { if (!readVec3(t, i, line, o.rot)) return false; }
                else if (k == "scale") {  // `scale <s>` (равномерно) или `scale <x y z>` (по осям)
                    float sx = 0.0f;
                    if (!readF(t, i, line, sx)) return false;
                    auto looksNum = [](const std::string& s) {
                        return !s.empty() && (std::isdigit((unsigned char)s[0]) || s[0] == '-' ||
                                              s[0] == '+' || s[0] == '.');
                    };
                    if (i + 1 < t.size() && looksNum(t[i]) && looksNum(t[i + 1])) {
                        float sy = 0.0f, sz = 0.0f;
                        if (!readF(t, i, line, sy) || !readF(t, i, line, sz)) return false;
                        o.scale = {sx, sy, sz};
                    } else {
                        o.scale = {sx, sx, sx};
                    }
                }
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

        } else if (cmd == "horizon") {
            // horizon <r> <g> <b> — цвет горизонта/фона (sRGB); дальняя земля тонируется в него
            size_t i = 1;
            if (!readVec3(t, i, line, out.horizonColor)) return false;

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
// Компактный вывод числа (без лишних нулей; %g). Парсер читает через toF, так что точность ок.
std::string fnum(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", (double)v);
    return b;
}
const char* shaderName(ShaderType s) {
    switch (s) {
        case ShaderType::Unlit: return "unlit";
        case ShaderType::Phong: return "phong";
        case ShaderType::Ground: return "ground";
        default: return "lit";
    }
}
}  // namespace

std::string serializeSceneDesc(const SceneDesc& d) {
    std::ostringstream o;
    o << "# VBase scene (сериализовано редактором)\n\n";

    // Мир: сетка, свет, камера, авто-рестарт.
    o << "grid cell " << fnum(d.grid.cell) << " arena " << fnum(d.grid.arenaHalf) << "\n";
    o << "light dir " << fnum(d.lightDir.x) << " " << fnum(d.lightDir.y) << " " << fnum(d.lightDir.z)
      << "\n";
    // Горизонт эмитим только если отличается от дефолта (0.07,0.07,0.12) — чтобы не засорять
    // сцены без него (и round-trip оставался идемпотентным на обеих ветках).
    if (std::fabs(d.horizonColor.x - 0.07f) > 1e-4f || std::fabs(d.horizonColor.y - 0.07f) > 1e-4f ||
        std::fabs(d.horizonColor.z - 0.12f) > 1e-4f) {
        o << "horizon " << fnum(d.horizonColor.x) << " " << fnum(d.horizonColor.y) << " "
          << fnum(d.horizonColor.z) << "\n";
    }
    o << "camera distance " << fnum(d.camera.distance) << " pitch " << fnum(d.camera.pitch)
      << " lookHeight " << fnum(d.camera.lookHeight) << " fov " << fnum(d.camera.fovY) << " near "
      << fnum(d.camera.nearZ) << " far " << fnum(d.camera.farZ) << "\n";
    if (d.matchRestartDelay > 0.0f) o << "matchrestart " << fnum(d.matchRestartDelay) << "\n";

    // Текстуры.
    if (!d.textures.empty()) o << "\n";
    for (const TextureSpec& t : d.textures) {
        o << "texture " << t.name << " ";
        if (t.kind == TextureSpec::Checker) o << "procedural " << t.size << " " << t.cells;
        else if (t.kind == TextureSpec::Bump) o << "procnormal " << t.size << " " << t.cells;
        else o << "image " << t.path;
        o << "\n";
    }
    // Материалы.
    if (!d.materials.empty()) o << "\n";
    for (const MaterialSpec& m : d.materials) {
        o << "material " << m.name << " " << shaderName(m.shader) << " color " << fnum(m.color.x)
          << " " << fnum(m.color.y) << " " << fnum(m.color.z);
        if (!m.tex.empty()) o << " tex " << m.tex;
        if (!m.normal.empty()) o << " normal " << m.normal;
        o << "\n";
    }
    // Меши-примитивы.
    if (!d.meshes.empty()) o << "\n";
    for (const MeshSpec& m : d.meshes) {
        o << "mesh " << m.name << " ";
        if (m.kind == MeshSpec::Plane) o << "plane " << fnum(m.a) << " " << fnum(m.b);
        else if (m.kind == MeshSpec::Rect)
            o << "rect " << fnum(m.a) << " " << fnum(m.b) << " " << fnum(m.uvX) << " " << fnum(m.uvZ);
        else if (m.kind == MeshSpec::Terrain)
            o << "terrain " << fnum(m.a) << " " << m.stacks << " " << fnum(m.amp) << " " << fnum(m.freq)
              << " " << fnum(m.b);
        else if (m.kind == MeshSpec::Cube) o << "cube " << fnum(m.a);
        else o << "sphere " << fnum(m.a) << " " << m.stacks << " " << m.slices;
        o << "\n";
    }
    // Объекты (процедурные + glTF-модели, одиночные + кольцевые).
    if (!d.objects.empty()) o << "\n";
    for (const ObjectSpec& os : d.objects) {
        o << (os.ring ? "ring " : "object ");
        if (!os.model.empty()) {
            o << "model " << os.model;
            if (!os.modelTex.empty()) o << " tex " << os.modelTex;
            o << " shader " << shaderName(os.shader);
            if (os.color.x != 1.0f || os.color.y != 1.0f || os.color.z != 1.0f)
                o << " color " << fnum(os.color.x) << " " << fnum(os.color.y) << " " << fnum(os.color.z);
        } else {
            o << os.mesh;
            if (!os.material.empty()) o << " mat " << os.material;
        }
        o << " pos " << fnum(os.pos.x) << " " << fnum(os.pos.y) << " " << fnum(os.pos.z);
        if (os.rot.x != 0.0f || os.rot.y != 0.0f || os.rot.z != 0.0f)
            o << " rot " << fnum(os.rot.x) << " " << fnum(os.rot.y) << " " << fnum(os.rot.z);
        if (os.scale.x == os.scale.y && os.scale.y == os.scale.z) {  // равномерный — компактно
            if (os.scale.x != 1.0f) o << " scale " << fnum(os.scale.x);
        } else {  // по-осевой
            o << " scale " << fnum(os.scale.x) << " " << fnum(os.scale.y) << " " << fnum(os.scale.z);
        }
        if (os.spin != 0.0f) o << " spin " << fnum(os.spin);
        if (os.ring)
            o << " count " << os.ringCount << " radius " << fnum(os.ringRadius) << " y "
              << fnum(os.ringY);
        o << "\n";
    }
    // Коллайдеры.
    if (!d.colliders.empty()) o << "\n";
    for (const ColliderSpec& c : d.colliders) {
        o << "collider box center " << fnum(c.center.x) << " " << fnum(c.center.y) << " "
          << fnum(c.center.z) << " half " << fnum(c.half.x) << " " << fnum(c.half.y) << " "
          << fnum(c.half.z) << "\n";
    }
    // Здания базы (только scene-authored: тип + pos + team + непустые числа; wave*/дефолты — из config).
    if (!d.buildings.empty()) o << "\n";
    for (const BuildingSpec& b : d.buildings) {
        const char* kind = "core";
        switch (b.kind) {
            case BuildingSpec::Generator: kind = "generator"; break;
            case BuildingSpec::Storage: kind = "storage"; break;
            case BuildingSpec::Spawner: kind = "spawner"; break;
            case BuildingSpec::Tower: kind = "tower"; break;
            case BuildingSpec::Core: kind = "core"; break;
        }
        o << kind << " pos " << fnum(b.pos.x) << " " << fnum(b.pos.y) << " " << fnum(b.pos.z);
        if (b.team != 0) o << " team " << (int)b.team;
        if (b.rate != 0.0f) o << " rate " << fnum(b.rate);
        if (b.cap != 0.0f) o << " cap " << fnum(b.cap);
        if (b.hp != 0.0f) o << " hp " << fnum(b.hp);
        if (b.damage != 0.0f) o << " damage " << fnum(b.damage);
        if (b.range != 0.0f) o << " range " << fnum(b.range);
        o << "\n";
    }
    // Точки спавна сторон (PvP).
    if (!d.spawns.empty()) o << "\n";
    for (const SpawnSpec& s : d.spawns)
        o << "spawn team " << (int)s.team << " pos " << fnum(s.pos.x) << " " << fnum(s.pos.y) << " "
          << fnum(s.pos.z) << "\n";
    // Игрок.
    if (d.player.present) {
        o << "\nplayer model " << d.player.model << " pos " << fnum(d.player.pos.x) << " "
          << fnum(d.player.pos.y) << " " << fnum(d.player.pos.z) << " scale " << fnum(d.player.scale)
          << " yaw " << fnum(d.player.yawOffset) << " capsule " << fnum(d.player.colliderRadius)
          << " " << fnum(d.player.colliderCylHalf);
        if (!d.player.hideNodes.empty()) {
            o << " hide ";
            for (size_t i = 0; i < d.player.hideNodes.size(); ++i) {
                if (i) o << ",";
                o << d.player.hideNodes[i];
            }
        }
        o << "\n";
    }
    return o.str();
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
            else if (key == "wavesize") bi.waveSize = toI(val);
            else if (key == "wavepause") bi.wavePause = toF(val);
            else if (key == "wavegrow") bi.waveGrow = toI(val);
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
            b.waveSize = bi.waveSize;    // spawner: бесконечные волны (0 = легаси по cap)
            b.wavePause = bi.wavePause;
            b.waveGrow = bi.waveGrow;
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

namespace {
// Хелперы санитизации: конечное значение или дефолт; кламп в [lo,hi] с дефолтом при NaN/inf.
float sanF(float v, float def) { return std::isfinite(v) ? v : def; }
float clampF(float v, float lo, float hi, float def) {
    if (!std::isfinite(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}
void sanVec3(Vec3& v) { v.x = sanF(v.x, 0.0f); v.y = sanF(v.y, 0.0f); v.z = sanF(v.z, 0.0f); }
}  // namespace

void validateSceneDesc(SceneDesc& desc) {
    // Сетка: cell — делитель (Grid::cellOf), обязана быть > 0 и конечной; arenaHalf ограничивает
    // размер навсетки (kMaxSide=512) — держим в разумных пределах.
    desc.grid.cell = clampF(desc.grid.cell, 0.25f, 64.0f, 2.0f);
    desc.grid.arenaHalf = clampF(desc.grid.arenaHalf, 1.0f, 1000.0f, 11.0f);

    // Меши: параметры формы + stacks/slices сферы (генератор клампит и сам, чистим и тут).
    for (MeshSpec& m : desc.meshes) {
        m.a = clampF(m.a, 0.001f, 1e4f, 1.0f);
        m.b = clampF(m.b, 0.001f, 1e4f, 1.0f);
        if (m.stacks < 2) m.stacks = 2;
        if (m.stacks > 512) m.stacks = 512;
        if (m.slices < 3) m.slices = 3;
        if (m.slices > 512) m.slices = 512;
    }
    // Текстуры: size (потолок против OOM) и cells (≤ size, иначе деление на ноль в генераторе).
    for (TextureSpec& t : desc.textures) {
        if (t.size < 1) t.size = 1;
        if (t.size > 4096) t.size = 4096;
        if (t.cells < 1) t.cells = 1;
        if (t.cells > t.size) t.cells = t.size;
    }
    // Коллайдеры: центр конечен, полуразмеры конечны и ≥ 0 (иначе Jolt-бокс невалиден).
    for (ColliderSpec& c : desc.colliders) {
        sanVec3(c.center);
        c.half.x = clampF(c.half.x, 0.0f, 1e4f, 0.5f);
        c.half.y = clampF(c.half.y, 0.0f, 1e4f, 0.5f);
        c.half.z = clampF(c.half.z, 0.0f, 1e4f, 0.5f);
    }
    // Здания: позиция конечна, статы конечны и ≥ 0; волновые счётчики в разумных границах.
    for (BuildingSpec& b : desc.buildings) {
        sanVec3(b.pos);
        b.rate = clampF(b.rate, 0.0f, 1e6f, 0.0f);
        b.cap = clampF(b.cap, 0.0f, 1e9f, 0.0f);
        b.hp = clampF(b.hp, 0.0f, 1e12f, 0.0f);
        b.damage = clampF(b.damage, 0.0f, 1e9f, 0.0f);
        b.range = clampF(b.range, 0.0f, 1e4f, 0.0f);
        if (b.waveSize < 0) b.waveSize = 0;
        if (b.waveSize > 100000) b.waveSize = 100000;
        b.wavePause = clampF(b.wavePause, 0.0f, 1e4f, 0.0f);
        if (b.waveGrow < 0) b.waveGrow = 0;
        if (b.waveGrow > 100000) b.waveGrow = 100000;
    }
    for (SpawnSpec& s : desc.spawns) sanVec3(s.pos);

    // Враг по умолчанию: attackInterval — делитель кулдауна, обязан быть > 0.
    desc.enemy.hp = clampF(desc.enemy.hp, 0.1f, 1e9f, 10.0f);
    desc.enemy.damage = clampF(desc.enemy.damage, 0.0f, 1e9f, 5.0f);
    desc.enemy.attackInterval = clampF(desc.enemy.attackInterval, 0.05f, 1e4f, 1.0f);

    // Шаблоны построек героя.
    for (BuildTemplate& t : desc.build) {
        t.cost = clampF(t.cost, 0.0f, 1e9f, 0.0f);
        t.rate = clampF(t.rate, 0.0f, 1e6f, 0.0f);
        t.cap = clampF(t.cap, 0.0f, 1e9f, 0.0f);
        t.hp = clampF(t.hp, 0.0f, 1e12f, 0.0f);
        t.damage = clampF(t.damage, 0.0f, 1e9f, 0.0f);
        t.range = clampF(t.range, 0.0f, 1e4f, 0.0f);
    }

    // Игрок: масштаб/капсула > 0 (капсула Jolt), hp > 0.
    sanVec3(desc.player.pos);
    desc.player.scale = clampF(desc.player.scale, 1e-4f, 1e3f, 0.03f);
    desc.player.yawOffset = sanF(desc.player.yawOffset, 0.0f);
    desc.player.colliderRadius = clampF(desc.player.colliderRadius, 0.01f, 100.0f, 0.3f);
    desc.player.colliderCylHalf = clampF(desc.player.colliderCylHalf, 0.01f, 100.0f, 0.3f);
    desc.player.hp = clampF(desc.player.hp, 1.0f, 1e9f, 100.0f);
    desc.player.respawnDelay = clampF(desc.player.respawnDelay, 0.0f, 1e4f, 5.0f);

    // Камера: nearZ > 0, farZ > nearZ, fovY в (0,π).
    desc.camera.distance = clampF(desc.camera.distance, 0.1f, 1e4f, 16.0f);
    desc.camera.pitch = clampF(desc.camera.pitch, 0.05f, 1.5f, 0.9f);
    desc.camera.lookHeight = clampF(desc.camera.lookHeight, -1e3f, 1e3f, 1.0f);
    desc.camera.fovY = clampF(desc.camera.fovY, 0.1f, 3.0f, 0.9f);
    desc.camera.nearZ = clampF(desc.camera.nearZ, 1e-3f, 1e4f, 0.1f);
    desc.camera.farZ = clampF(desc.camera.farZ, desc.camera.nearZ + 1e-3f, 1e6f, 200.0f);

    sanVec3(desc.lightDir);
    desc.horizonColor.x = clampF(desc.horizonColor.x, 0.0f, 1.0f, 0.07f);
    desc.horizonColor.y = clampF(desc.horizonColor.y, 0.0f, 1.0f, 0.07f);
    desc.horizonColor.z = clampF(desc.horizonColor.z, 0.0f, 1.0f, 0.12f);
    desc.matchRestartDelay = clampF(desc.matchRestartDelay, 0.0f, 1e4f, 0.0f);

    // Ростеры: attackInterval — делитель, > 0; остальные статы конечны и ≥ 0.
    auto sanChar = [](CharacterDesc& c) {
        c.hp = clampF(c.hp, 0.0f, 1e9f, 0.0f);
        c.damage = clampF(c.damage, 0.0f, 1e9f, 0.0f);
        c.speed = clampF(c.speed, 0.0f, 1e4f, 0.0f);
        c.attackInterval = clampF(c.attackInterval, 0.05f, 1e4f, 1.0f);
        c.range = clampF(c.range, 0.0f, 1e4f, 0.0f);
        c.scale = clampF(c.scale, 1e-4f, 1e3f, 1.0f);
        c.yawOffset = sanF(c.yawOffset, 0.0f);
    };
    for (CharacterDesc& c : desc.heroTypes) sanChar(c);
    for (CharacterDesc& c : desc.enemyTypes) sanChar(c);

    // Ростер башен/ловушек (towers.cfg): статы конечны и в разумных пределах; rate — делитель > 0;
    // тир и множители не должны уводить в бесконечность/ноль.
    for (TowerDesc& d : desc.towerTypes) {
        d.cost = clampF(d.cost, 0.0f, 1e9f, 0.0f);
        d.hp = clampF(d.hp, 0.0f, 1e9f, 0.0f);
        d.rate = clampF(d.rate, 0.05f, 1e4f, 1.0f);
        d.range = clampF(d.range, 0.0f, 1e4f, 0.0f);
        d.damage = clampF(d.damage, 0.0f, 1e9f, 0.0f);
        d.slowFactor = clampF(d.slowFactor, 0.05f, 1.0f, 0.5f);
        d.effectDur = clampF(d.effectDur, 0.0f, 1e4f, 0.0f);
        d.splashRadius = clampF(d.splashRadius, 0.0f, 1e3f, 0.0f);
        d.burnDps = clampF(d.burnDps, 0.0f, 1e9f, 0.0f);
        if (d.maxTier < 1) d.maxTier = 1;
        if (d.maxTier > 15) d.maxTier = 15;  // тир кодируется 4 битами в сети (charType)
        d.tierDamageMul = clampF(d.tierDamageMul, 1.0f, 100.0f, 1.6f);
        d.tierRangeMul = clampF(d.tierRangeMul, 0.5f, 10.0f, 1.12f);
        d.tierRateMul = clampF(d.tierRateMul, 0.1f, 10.0f, 0.85f);
        d.upgradeCostMul = clampF(d.upgradeCostMul, 0.0f, 100.0f, 0.8f);
    }
}
