#include "engine/render/ui/screens/BattleScreen.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "engine/core/Input.h"
#include "engine/render/ui/UiPalette.h"
#include "engine/render/ui/windows/BuildingInfo.h"
#include "engine/render/ui/windows/DebugPanel.h"
#include "game/BuildingConfig.h"
#include "game/NavDebug.h"
#include "game/Scene.h"

namespace BattleScreen {
namespace {

// Опции отображения навигационного оверлея — чисто клиентские, переключаются в окне-легенде.
bool g_navGrid = true;       // линии сетки клеток
bool g_navObstacles = true;  // занятые (obstacle) клетки
bool g_navUnreach = true;    // проходимые клетки без пути к цели
bool g_navFlow = true;       // стрелки поля потока к цели
bool g_navHeat = false;      // хитмап дистанции до цели
bool g_navDist = false;      // числа дистанции в клетках

// Полоса-стат: подпись слева + бар (трек + заливка) с центрированным числом. Ряд фикс. высоты.
void statBar(const char* label, float frac, ImU32 fill, const char* overlay) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float font = ImGui::GetFontSize();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(font * 4.2f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = font * 1.05f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), UiPalette::withAlpha(UiPalette::Stone, 210), 3.0f);
    if (frac > 0.0f)
        dl->AddRectFilled(p, ImVec2(p.x + w * frac, p.y + h), fill, 3.0f);
    if (overlay != nullptr && overlay[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                    UiPalette::withAlpha(UiPalette::Text, 235), overlay);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// Цвет полосы HP героя по доле: низко — danger, средне — amber, высоко — success.
ImU32 hpColor(float f) {
    if (f <= 0.25f) return UiPalette::Danger;
    if (f < 0.6f) return UiPalette::Amber;
    return UiPalette::Success;
}

void drawHud(UiShell::Ctx& ctx) {
    Scene& s = ctx.scene;
    const float font = ImGui::GetFontSize();

    // Верх-лево: база — HP ядра, ресурс, счётчик живых врагов (всё из снапшотов).
    if (s.netConnected()) {
        const ImVec2 baseSz(font * 13.0f, font * 6.2f);
        if (ctx.beginOverlay("##hudBase", UiShell::anchorPos(UiShell::Anchor::TopLeft, baseSz),
                             baseSz, 0.72f, ImGuiWindowFlags_NoNav)) {
            char b[24];
            const float chp = s.coreHp(), cmax = s.coreMaxHp();
            if (chp >= 0.0f && cmax > 0.0f) {
                std::snprintf(b, sizeof(b), "%.0f / %.0f", (double)chp, (double)cmax);
                statBar("Ядро", chp / cmax, UiPalette::Copper, b);
            }
            const float res = s.resourceCurrent(), cap = s.resourceCap();
            if (cap > 0.0f) {
                std::snprintf(b, sizeof(b), "%.0f / %.0f", (double)res, (double)cap);
                statBar("Ресурс", res / cap, UiPalette::Amber, b);
            } else {
                ImGui::Text("Ресурс: %.0f", (double)res);
            }
            ImGui::Text("Врагов: %d", s.enemyCount());
        }
        ctx.endOverlay();
    }

    // Верх-право: статус сети + пауза + debug (всегда — управление матчем).
    const ImVec2 sysSz(font * 13.0f, font * 4.6f);
    if (ctx.beginOverlay("##hudSys", UiShell::anchorPos(UiShell::Anchor::TopRight, sysSz), sysSz,
                         0.72f, ImGuiWindowFlags_NoNav)) {
        if (s.netConnected())
            ImGui::Text("%s · %d союзн. · %d ms", s.netHost() ? "HOST" : "CLIENT", s.remoteCount(),
                        s.netPingMs());
        else
            ImGui::TextDisabled("оффлайн");
        if (ctx.btn("Пауза")) UiShell::setOverlay(UiOverlay::Pause);
        ImGui::SameLine();
        if (ctx.btn("Debug", ImVec2(0, 0), /*selected=*/UiShell::isDebugOpen())) UiShell::toggleDebug();
    }
    ctx.endOverlay();

    // Низ-центр: HP героя крупной полосой (или таймер респауна, когда мёртв).
    if (s.netConnected()) {
        const ImVec2 heroSz(font * 16.0f, font * 3.4f);
        if (ctx.beginOverlay("##hudHero", UiShell::anchorPos(UiShell::Anchor::BottomCenter, heroSz),
                             heroSz, 0.72f, ImGuiWindowFlags_NoNav)) {
            if (s.heroDead()) {
                ImGui::TextColored(UiPalette::v4(UiPalette::Danger), "Возрождение через %.0f с",
                                   (double)std::ceil(s.heroRespawnLeft()));
            } else {
                const float hp = s.heroHp(), mx = s.heroMaxHp();
                const float f = (mx > 0.0f) ? hp / mx : 0.0f;
                char b[24];
                std::snprintf(b, sizeof(b), "%.0f / %.0f", (double)hp, (double)mx);
                statBar("Герой", f, hpColor(f), b);
            }
        }
        ctx.endOverlay();
    }
}

void drawBuild(UiShell::Ctx& ctx) {
    if (!ctx.scene.netConnected()) return;

    // Заякорена слева под HUD (метрики в единицах шрифта — под DPI), авто-высота по контенту.
    const float font = ImGui::GetFontSize();
    if (!ctx.beginPanelRect("Строительство###uiBuildPanel", ImVec2(UiShell::uiMargin(), font * 5.5f),
                            ImVec2(font * 19.0f, 0.0f), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ctx.endPanel();
        return;
    }

    if (!ctx.scene.buildMode()) {
        // Обычные постройки экономики.
        const EntityType kGeneric[] = {EntityType::Generator, EntityType::Storage};
        for (EntityType bt : kGeneric) {
            const BuildingInfo* bi = ctx.scene.buildInfo((int)bt);
            if (bi == nullptr || bi->cost <= 0.0f) continue;
            bool afford = ctx.scene.resourceCurrent() >= bi->cost;
            ImGui::BeginDisabled(!afford);
            std::string lbl = (bi->name.empty() ? std::string("Здание") : bi->name) + " (" +
                              std::to_string((int)bi->cost) + ")";
            if (ctx.btn(lbl.c_str())) ctx.scene.beginBuild((int)bt);
            ImGui::EndDisabled();
        }
        // Виды защиты (башни-стрелки и ловушки) из ростера towers.cfg.
        const int nd = ctx.scene.defenseCount();
        if (nd > 0) ImGui::Separator();
        for (int k = 0; k < nd; ++k) {
            const int cost = ctx.scene.defenseCost(k);
            const int et = ctx.scene.defenseEntityType(k);
            if (cost <= 0 || et < 0) continue;
            bool afford = ctx.scene.resourceCurrent() >= (float)cost;
            ImGui::BeginDisabled(!afford);
            std::string lbl = std::string(ctx.scene.defenseName(k)) + " (" + std::to_string(cost) + ")";
            if (ctx.btn(lbl.c_str())) ctx.scene.beginBuild(et, k);
            ImGui::EndDisabled();
        }
    } else {
        std::string nm;
        const int bt = ctx.scene.buildType();
        if (bt == (int)EntityType::Tower || bt == (int)EntityType::Trap) {
            nm = ctx.scene.defenseName(ctx.scene.buildKind());
        } else {
            const BuildingInfo* bi = ctx.scene.buildInfo(bt);
            nm = (bi != nullptr && !bi->name.empty()) ? bi->name : "Здание";
        }
        ImGui::Text("Ставим: %s", nm.c_str());
        bool valid = ctx.scene.buildGhostValid();
        ImGui::TextColored(valid ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.95f, 0.5f, 0.4f, 1.0f),
                           valid ? "Клетка свободна — ставь" : "Занято / далеко / нет ресурса");
        if (ctx.btn("Поставить")) ctx.scene.confirmBuild();
        ImGui::SameLine();
        if (ctx.btn("Отмена")) ctx.scene.cancelBuild();
    }

    if (ctx.scene.netConnected()) {
        ImGui::Separator();
        if (ctx.btn("Disconnect")) {
            Scene* scene = &ctx.scene;
            UiShell::pushYesNo("Сеть", "Покинуть сессию?", [scene](DialogResult r) {
                if (r == DialogResult::Yes) {
                    scene->leaveGame();
                    UiShell::setMode(UiMode::MainMenu);
                }
            });
        }
    }
    ctx.endPanel();
}

// Читаемость боя: worldspace HP-бары + всплывающие числа урона. Данные производит Scene
// (диф hp из снапшотов), здесь только проецируем мировые точки через view/proj и рисуем.
// Фоновый draw-list — над 3D, но ПОД окнами ImGui (бары не перекрывают HUD/панели).
void drawCombatOverlay(Scene& scene) {
    if (!scene.netConnected()) return;
    const std::vector<Scene::CombatMarker>& markers = scene.combatMarkers();
    const std::vector<Scene::DamageNumber>& numbers = scene.damageNumbers();
    const std::vector<Scene::ImpactSpark>& sparks = scene.impactSparks();
    const std::vector<Scene::BuildPoof>& poofs = scene.buildPoofs();
    Vec3 selPos;
    const bool hasSel = scene.selectedWorldPos(selPos);  // выделенное здание -> ринг
    if (markers.empty() && numbers.empty() && sparks.empty() && poofs.empty() && !hasSel) return;

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const Mat4 vp = scene.projMatrix() * scene.viewMatrix();
    const float* m = vp.m;  // column-major: clip = VP * (x,y,z,1)
    auto project = [&](const Vec3& p, float& sx, float& sy) -> bool {
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (cw <= 0.0001f) return false;  // точка за камерой
        float ndcx = cx / cw, ndcy = cy / cw;
        if (ndcx < -1.3f || ndcx > 1.3f || ndcy < -1.3f || ndcy > 1.3f) return false;  // за краем
        sx = (ndcx * 0.5f + 0.5f) * W;
        sy = (1.0f - (ndcy * 0.5f + 0.5f)) * H;
        return true;
    };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const Scene::CombatMarker& mk : markers) {
        float sx, sy;
        if (!project(mk.pos, sx, sy)) continue;
        const float bw = 46.0f, bh = 6.0f;
        ImVec2 a(sx - bw * 0.5f, sy - bh * 0.5f), b(sx + bw * 0.5f, sy + bh * 0.5f);
        float f = mk.hpFrac < 0.0f ? 0.0f : (mk.hpFrac > 1.0f ? 1.0f : mk.hpFrac);
        float xmid = a.x + (b.x - a.x) * f;
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 190), 2.0f);
        dl->AddRectFilled(ImVec2(xmid, a.y), b, IM_COL32(30, 30, 30, 170), 2.0f);  // пустой остаток
        ImU32 col = IM_COL32((int)(mk.color.x * 255), (int)(mk.color.y * 255), (int)(mk.color.z * 255), 235);
        dl->AddRectFilled(a, ImVec2(xmid, b.y), col, 2.0f);  // заполнение по доле hp
    }

    // Искры в точке хита: радиальный разлёт коротких линий, растёт и гаснет.
    for (const Scene::ImpactSpark& sp : sparks) {
        float sx, sy;
        if (!project(sp.pos, sx, sy)) continue;
        float t = sp.maxAge > 0.0f ? sp.age / sp.maxAge : 1.0f;
        if (t > 1.0f) t = 1.0f;
        int alpha = (int)(235.0f * (1.0f - t));
        if (alpha <= 0) continue;
        ImU32 col = IM_COL32(255, 226, 150, alpha);  // тёплый бело-жёлтый
        const int N = 7;
        float inner = 2.0f + t * 12.0f;              // разлёт наружу со временем
        float outer = inner + 8.0f * (1.0f - t) + 2.0f;
        float base = (float)(sp.seed % 6283) * 0.001f;  // фаза направлений из seed (стабильна на кадрах)
        for (int i = 0; i < N; ++i) {
            float ang = base + (float)i * (6.2831853f / (float)N);
            float ca = std::cos(ang), sa = std::sin(ang);
            dl->AddLine(ImVec2(sx + ca * inner, sy + sa * inner),
                        ImVec2(sx + ca * outer, sy + sa * outer), col, 2.0f);
        }
    }

    // «Пуф» постройки: расходящееся затухающее кольцо на месте размещения.
    for (const Scene::BuildPoof& pf : poofs) {
        float sx, sy;
        if (!project(pf.pos, sx, sy)) continue;
        float t = pf.age / 0.5f;  // синхронно с kPoofLife в Scene
        if (t > 1.0f) t = 1.0f;
        int alpha = (int)(200.0f * (1.0f - t));
        if (alpha <= 0) continue;
        float rad = 6.0f + t * 34.0f;  // расходится наружу
        dl->AddCircle(ImVec2(sx, sy), rad, IM_COL32(180, 220, 255, alpha), 24, 2.5f);
        dl->AddCircle(ImVec2(sx, sy), rad * 0.6f, IM_COL32(220, 240, 255, alpha / 2), 24, 1.5f);
    }

    // Ринг выделения: пульсирующее кольцо + уголки-скобки вокруг выбранного здания.
    if (hasSel) {
        float sx, sy;
        if (project(selPos, sx, sy)) {
            float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.0f);
            float rad = 26.0f + pulse * 4.0f;
            ImU32 col = IM_COL32(120, 230, 255, 235);  // циан
            dl->AddCircle(ImVec2(sx, sy), rad, col, 32, 2.5f);
            const float b = rad + 6.0f, len = 8.0f;  // квадратные уголки-скобки
            const float cs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (const auto& c : cs) {
                float cx = sx + c[0] * b, cy = sy + c[1] * b;
                dl->AddLine(ImVec2(cx, cy), ImVec2(cx - c[0] * len, cy), col, 2.5f);
                dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy - c[1] * len), col, 2.5f);
            }
        }
    }

    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 1.2f;
    for (const Scene::DamageNumber& dn : numbers) {
        float sx, sy;
        if (!project(dn.pos, sx, sy)) continue;
        float lifeT = dn.age / 0.9f;  // синхронно с kDmgLife в Scene
        if (lifeT > 1.0f) lifeT = 1.0f;
        int alpha = (int)(255.0f * (1.0f - lifeT * lifeT));  // держится, потом резко гаснет
        sy -= dn.age * 42.0f;  // всплывает вверх в экранных пикселях (стабильно на любой дистанции)
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", (int)(dn.amount + 0.5f));
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
        ImVec2 tp(sx - ts.x * 0.5f, sy - ts.y * 0.5f);
        dl->AddText(font, fs, ImVec2(tp.x + 1.5f, tp.y + 1.5f), IM_COL32(0, 0, 0, alpha), buf);
        dl->AddText(font, fs, tp,
                    IM_COL32((int)(dn.color.x * 255), (int)(dn.color.y * 255), (int)(dn.color.z * 255), alpha),
                    buf);
    }
}

// Отладочный оверлей навигации: сетка клеток, закрашенные обстаклы, «мёртвые» клетки без
// пути, цели-ядра и поле потока (стрелки/хитмап). Данные (navDebugFrame) производит Scene —
// реконструкцией навсетки теми же NavGrid/FlowField, что у сервера; тут только проекция
// мировых клеток на экран и рисование (как drawCombatOverlay). Фоновый draw-list — над 3D,
// под окнами; окно-легенда с переключателями — обычное ImGui-окно (поверх).
void drawNavOverlay(Scene& scene) {
    if (!scene.navDebugEnabled() || !scene.netConnected()) return;
    const NavDebugFrame& nf = scene.navDebugFrame();
    if (!nf.valid || nf.cells.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const Mat4 vp = scene.projMatrix() * scene.viewMatrix();
    const float* m = vp.m;  // column-major: clip = VP * (x,y,z,1)
    auto project = [&](const Vec3& p, float& sx, float& sy) -> bool {
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (cw <= 0.0001f) return false;  // точка за камерой
        float ndcx = cx / cw, ndcy = cy / cw;
        sx = (ndcx * 0.5f + 0.5f) * W;
        sy = (1.0f - (ndcy * 0.5f + 0.5f)) * H;
        return true;
    };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float cell = nf.cell;
    const float y = 0.05f;  // чуть над полом, чтобы читалось поверх пола

    for (const NavDebugCell& c : nf.cells) {
        // Кулинг: клетка за камерой или далеко за краем экрана — пропускаем целиком.
        const float wcx = ((float)c.cx + 0.5f) * cell;
        const float wcz = ((float)c.cz + 0.5f) * cell;
        float ccx, ccy;
        if (!project(Vec3{wcx, y, wcz}, ccx, ccy)) continue;
        if (ccx < -256.0f || ccx > W + 256.0f || ccy < -256.0f || ccy > H + 256.0f) continue;

        const float x0 = (float)c.cx * cell, x1 = x0 + cell;
        const float z0 = (float)c.cz * cell, z1 = z0 + cell;
        float px[4], py[4];
        if (!(project(Vec3{x0, y, z0}, px[0], py[0]) && project(Vec3{x1, y, z0}, px[1], py[1]) &&
              project(Vec3{x1, y, z1}, px[2], py[2]) && project(Vec3{x0, y, z1}, px[3], py[3])))
            continue;
        ImVec2 poly[4] = {{px[0], py[0]}, {px[1], py[1]}, {px[2], py[2]}, {px[3], py[3]}};

        // Заливка по состоянию клетки (цель важнее занятости — ядро занято И является целью).
        ImU32 fill = 0;
        if (c.goal) {
            fill = IM_COL32(60, 220, 90, 130);  // цель (ядро) — зелёный
        } else if (c.blocked) {
            if (g_navObstacles) fill = IM_COL32(225, 60, 45, 95);  // обстакл — красный
        } else if (!c.reachable) {
            if (g_navUnreach) fill = IM_COL32(235, 165, 40, 75);  // нет пути к цели — оранжевый
        } else if (g_navHeat && nf.maxDist > 0 && c.dist >= 0) {
            float t = (float)c.dist / (float)nf.maxDist;  // 0 у цели -> 1 далеко
            int rr = (int)(40.0f + t * 205.0f);
            int gg = (int)(190.0f - t * 130.0f);
            int bb = (int)(230.0f - t * 180.0f);
            fill = IM_COL32(rr, gg, bb, 60);
        }
        if (fill != 0) dl->AddConvexPolyFilled(poly, 4, fill);
        if (g_navGrid)
            dl->AddPolyline(poly, 4, IM_COL32(255, 255, 255, 45), ImDrawFlags_Closed, 1.0f);

        // Грубый экранный размер клетки — гейт мелочи (стрелки/числа), чтобы не было каши.
        const float sizePx = std::fabs(px[1] - px[0]) + std::fabs(py[2] - py[1]);

        // Стрелка поля потока: направление, которым моб пойдёт из этой клетки к цели.
        if (g_navFlow && sizePx > 26.0f && c.reachable && !c.blocked && !c.goal &&
            (c.dirX != 0.0f || c.dirZ != 0.0f)) {
            float ax, ay, bx, by;
            if (project(Vec3{wcx, y, wcz}, ax, ay) &&
                project(Vec3{wcx + c.dirX * cell * 0.42f, y, wcz + c.dirZ * cell * 0.42f}, bx, by)) {
                const ImU32 col = IM_COL32(90, 220, 255, 210);
                dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), col, 1.7f);
                float dx = bx - ax, dy = by - ay;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.5f) {
                    dx /= len; dy /= len;
                    const float hx = -dy, hy = dx;  // перпендикуляр для «усов» наконечника
                    const float hs = 5.0f;
                    dl->AddLine(ImVec2(bx, by),
                                ImVec2(bx - dx * hs + hx * hs * 0.55f, by - dy * hs + hy * hs * 0.55f),
                                col, 1.7f);
                    dl->AddLine(ImVec2(bx, by),
                                ImVec2(bx - dx * hs - hx * hs * 0.55f, by - dy * hs - hy * hs * 0.55f),
                                col, 1.7f);
                }
            }
        }

        // Число дистанции до цели (по желанию — только на крупных клетках).
        if (g_navDist && sizePx > 34.0f && c.reachable && c.dist >= 0) {
            char buf[12];
            std::snprintf(buf, sizeof(buf), "%d", c.dist);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(ccx - ts.x * 0.5f, ccy - ts.y * 0.5f), IM_COL32(230, 240, 255, 210), buf);
        }
    }

    // Окно-легенда: счётчики + переключатели отображения + расшифровка цветов.
    const float mrg = UiShell::uiMargin();
    ImGui::SetNextWindowPos(ImVec2(W - 250.0f - mrg, mrg), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    if (ImGui::Begin("Навигация (debug)###navDebug", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("Сетка %dx%d, клетка %.1f", nf.w, nf.h, (double)cell);
        ImGui::Text("Обстаклы: %d  Достижимо: %d  Цели: %d", nf.blockedCount, nf.reachableCount,
                    nf.goalCount);
        if (nf.goalCount == 0)
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "Нет живых ядер — поле потока пусто");
        ImGui::Separator();
        ImGui::Checkbox("Сетка", &g_navGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Обстаклы", &g_navObstacles);
        ImGui::Checkbox("Поток", &g_navFlow);
        ImGui::SameLine();
        ImGui::Checkbox("Тупики", &g_navUnreach);
        ImGui::Checkbox("Хитмап", &g_navHeat);
        ImGui::SameLine();
        ImGui::Checkbox("Дистанции", &g_navDist);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.88f, 0.24f, 0.18f, 1.0f), "красный = обстакл");
        ImGui::TextColored(ImVec4(0.92f, 0.65f, 0.16f, 1.0f), "оранжевый = нет пути (тупик)");
        ImGui::TextColored(ImVec4(0.24f, 0.86f, 0.35f, 1.0f), "зелёный = цель (ядро)");
        ImGui::TextColored(ImVec4(0.35f, 0.86f, 1.0f, 1.0f), "стрелки = поток к цели");
    }
    ImGui::End();
}

void drawJoysticks(Scene& scene) {
    auto drawStick = [](const VirtualJoystick& js, ImU32 ring, ImU32 knob) {
        if (!js.active) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(js.ox, js.oy), js.radius, ring, 48, 4.0f);
        dl->AddCircleFilled(ImVec2(js.cx, js.cy), js.radius * 0.4f, knob);
    };
    drawStick(scene.joystick(), IM_COL32(255, 255, 255, 110), IM_COL32(255, 255, 255, 190));
    drawStick(scene.cameraJoystick(), IM_COL32(120, 210, 255, 120),
              IM_COL32(150, 220, 255, 200));
}

}  // namespace

void draw(UiShell::Ctx& ctx) {
    drawCombatOverlay(ctx.scene);  // HP-бары/числа — под окнами (фоновый draw-list)
    drawNavOverlay(ctx.scene);     // отладка навсетки/пасфайндинга (по кнопке в Debug-панели)
    drawHud(ctx);
    drawBuild(ctx);
    BuildingInfoWindow::draw(ctx);
    DebugPanel::draw(ctx);
    drawJoysticks(ctx.scene);  // исход матча теперь показывает оверлей Results (UiShell)
}

}  // namespace BattleScreen
