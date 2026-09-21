#include "engine/render/ui/UiShell.h"

#include "imgui.h"

#include "engine/render/ui/screens/BattleScreen.h"
#include "engine/render/ui/screens/CharacterSelectScreen.h"
#include "engine/render/ui/screens/LoadingScreen.h"
#include "engine/render/ui/screens/MainMenuScreen.h"
#include "game/Scene.h"  // drawLobbyStub/Pause оверлей зовут методы сцены (netConnected/leaveGame)

namespace UiShell {
namespace {

UiMode g_mode = UiMode::MainMenu;
UiMode g_prevMode = UiMode::MainMenu;
UiOverlay g_overlay = UiOverlay::None;
MainMenuPanel g_panel = MainMenuPanel::Home;
bool g_debugOpen = true;
UiDialogs::Stack g_dialogs;

}  // namespace

bool Ctx::btn(const char* label, const ImVec2& size, bool selected, bool enabled) const {
    return UiSkin::Button(label, skin, size, selected, enabled);  // сам ветвится по skin.ready
}

bool Ctx::beginPanel(const char* name, bool* p_open, ImGuiWindowFlags flags) const {
    if (skin.ready) return UiSkin::BeginPanel(name, skin, p_open, flags);
    return ImGui::Begin(name, p_open, flags);
}

bool Ctx::beginPanelRect(const char* name, const ImVec2& pos, const ImVec2& size, bool* p_open,
                         ImGuiWindowFlags extraFlags) const {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   extraFlags;
    return beginPanel(name, p_open, flags);
}

void Ctx::endPanel() const {
    if (skin.ready) UiSkin::EndPanel();
    else ImGui::End();
}

bool Ctx::beginOverlay(const char* name, const ImVec2& pos, const ImVec2& size, float bgAlpha,
                       ImGuiWindowFlags extraFlags) const {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    if (size.x > 0.0f || size.y > 0.0f) ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bgAlpha < 0.0f ? 0.0f : bgAlpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | extraFlags;
    return ImGui::Begin(name, nullptr, flags);
}

void Ctx::endOverlay() const { ImGui::End(); }

float navBarHeight() { return ImGui::GetFontSize() * 3.0f; }
float uiMargin() { return ImGui::GetFontSize() * 0.9f; }

ImVec2 anchorPos(Anchor a, const ImVec2& size, float margin) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float m = margin < 0.0f ? uiMargin() : margin;
    switch (a) {
        case Anchor::TopLeft:     return ImVec2(m, m);
        case Anchor::TopRight:    return ImVec2(disp.x - size.x - m, m);
        case Anchor::BottomLeft:  return ImVec2(m, disp.y - size.y - m);
        case Anchor::BottomRight: return ImVec2(disp.x - size.x - m, disp.y - size.y - m);
        case Anchor::Center:      return ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f);
    }
    return ImVec2(m, m);
}

void menuContentRect(ImVec2& pos, ImVec2& size) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float m = uiMargin();  // поля от краёв экрана и до нав-бара (в единицах шрифта)
    pos = ImVec2(m, m);
    size = ImVec2(disp.x - m * 2.0f, disp.y - navBarHeight() - m * 2.0f);
    if (size.x < 200.0f) size.x = 200.0f;  // страховка на крошечном экране
    if (size.y < 160.0f) size.y = 160.0f;
}

void setMode(UiMode mode) {
    if (mode == g_mode) {
        g_overlay = UiOverlay::None;  // повторный вход в тот же режим просто снимает оверлей
        return;
    }
    g_prevMode = g_mode;
    g_mode = mode;
    g_overlay = UiOverlay::None;             // смена экрана всегда снимает оверлей
    if (mode == UiMode::MainMenu) g_panel = MainMenuPanel::Home;  // хаб открываем на Home
}

UiMode mode() { return g_mode; }

UiMode prevMode() { return g_prevMode; }

void back() {
    // Из боя/лобби/выбора — назад в предыдущий экран; если история пустая/некорректная — в меню.
    UiMode target = (g_prevMode != g_mode) ? g_prevMode : UiMode::MainMenu;
    setMode(target);
}

void setOverlay(UiOverlay overlay) { g_overlay = overlay; }

UiOverlay overlay() { return g_overlay; }

RenderPath renderPath() {
    switch (g_mode) {
        case UiMode::Battle:         return RenderPath::World;
        case UiMode::CharacterSelect: return RenderPath::CharacterPreview;
        case UiMode::Loading:
        case UiMode::MainMenu:
        case UiMode::Lobby:          return RenderPath::MenuBackdrop;
    }
    return RenderPath::MenuBackdrop;
}

void setPanel(MainMenuPanel panel) { g_panel = panel; }

MainMenuPanel panel() { return g_panel; }

void showDebug(bool open) { g_debugOpen = open; }

bool isDebugOpen() { return g_debugOpen; }

void toggleDebug() { g_debugOpen = !g_debugOpen; }

void pushOk(const char* title, const char* text, DialogCallback cb) {
    g_dialogs.pushOk(title, text, std::move(cb));
}

void pushYesNo(const char* title, const char* text, DialogCallback cb) {
    g_dialogs.pushYesNo(title, text, std::move(cb));
}

void pushDialog(const char* title, const char* text, std::vector<UiDialogs::Button> buttons,
                DialogCallback cb) {
    g_dialogs.pushCustom(title, text, std::move(buttons), std::move(cb));
}

void pushToast(const char* text, UiDialogs::Toast::Kind kind, float seconds) {
    g_dialogs.pushToast(text, kind, seconds);
}

bool hasModal() { return g_dialogs.hasModal(); }

bool gameplayActive() {
    // Ввод в мир идёт только в бою, без модалки и без оверлея (пауза/итог глушат управление).
    return g_mode == UiMode::Battle && g_overlay == UiOverlay::None && !g_dialogs.hasModal();
}

void loadLoadingAssets(Renderer& renderer, AssetSource& assets) {
    LoadingScreen::load(renderer, assets);
}

void unloadLoadingAssets(Renderer& renderer) { LoadingScreen::unload(renderer); }

bool hasLoadingArt() { return LoadingScreen::hasArt(); }

namespace {

// Минимальные заглушки лобби/оверлеев — инфраструктура навигатора должна компилироваться и
// ходить между экранами уже сейчас; наполнение (списки игр, готовность, награды) — в большом
// проходе по окнам. См. docs/UI_SYSTEM.md и NEXT_STEPS «UI».

void drawLobbyStub(UiShell::Ctx& ctx) {
    ImVec2 pos, size;
    menuContentRect(pos, size);
    if (ctx.beginPanelRect("Лобби", pos, size)) {
        ImGui::TextUnformatted("Лобби (заглушка): здесь будет создание/поиск игры и готовность.");
        ImGui::Spacing();
        if (ctx.btn("В бой")) setMode(UiMode::Battle);
        ImGui::SameLine();
        if (ctx.btn("Назад")) back();
    }
    ctx.endPanel();
}

// Оверлей поверх боя: затемняем экран и рисуем панель по центру. Возвращает выбранное действие
// косвенно через setMode/setOverlay.
void drawPauseOverlay(UiShell::Ctx& ctx) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    // Затемняющая подложка на весь экран (глушит клики по HUD под ней).
    if (ctx.beginOverlay("##pauseDim", ImVec2(0, 0), disp, 0.55f)) {
        const ImVec2 pSize(ImGui::GetFontSize() * 16.0f, ImGui::GetFontSize() * 9.0f);
        if (ctx.beginPanelRect("Пауза", anchorPos(Anchor::Center, pSize, 0.0f), pSize)) {
            if (ctx.btn("Продолжить", ImVec2(-1, 0))) setOverlay(UiOverlay::None);
            if (ctx.btn("В главное меню", ImVec2(-1, 0))) {
                // В живой сессии выход подтверждаем (покидаем игру), иначе — сразу в меню.
                if (ctx.scene.netConnected()) {
                    Scene* scene = &ctx.scene;
                    pushYesNo("Меню", "Покинуть сессию и выйти в меню?", [scene](DialogResult r) {
                        if (r == DialogResult::Yes) {
                            scene->leaveGame();
                            setMode(UiMode::MainMenu);
                        }
                    });
                } else {
                    setMode(UiMode::MainMenu);
                }
            }
        }
        ctx.endPanel();
    }
    ctx.endOverlay();
}

void drawResultsOverlay(UiShell::Ctx& ctx) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (ctx.beginOverlay("##resultsDim", ImVec2(0, 0), disp, 0.6f)) {
        const ImVec2 pSize(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 11.0f);
        if (ctx.beginPanelRect("Итог матча", anchorPos(Anchor::Center, pSize, 0.0f), pSize)) {
            ImGui::TextUnformatted("Итог матча (заглушка): результат и награды.");
            ImGui::Spacing();
            if (ctx.btn("В главное меню", ImVec2(-1, 0))) setMode(UiMode::MainMenu);
        }
        ctx.endPanel();
    }
    ctx.endOverlay();
}

}  // namespace

void build(GameUiState& state, Scene& scene, const UiSkin::Assets& skin) {
    Ctx ctx{state, scene, skin};

    switch (g_mode) {
        case UiMode::Loading: {
            LoadingScreen::draw();
            if (ctx.beginOverlay("##loadingPreviewCtrl", anchorPos(Anchor::TopLeft, ImVec2(0, 0)),
                                 ImVec2(0, 0), 0.65f, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Превью лоадинга");
                if (ctx.btn("Скрыть")) setMode(UiMode::MainMenu);
                ImGui::SameLine();
                ImGui::TextDisabled("(или --loading)");
            }
            ctx.endOverlay();
            break;
        }
        case UiMode::MainMenu:
            MainMenuScreen::draw(ctx);
            break;
        case UiMode::Lobby:
            drawLobbyStub(ctx);
            break;
        case UiMode::CharacterSelect:
            CharacterSelectScreen::draw(ctx);
            break;
        case UiMode::Battle:
            BattleScreen::draw(ctx);
            break;
    }

    // Оверлеи рисуются ПОВЕРХ базового режима (обычно Battle) — рендер-путь мира сохраняется.
    switch (g_overlay) {
        case UiOverlay::None:    break;
        case UiOverlay::Pause:   drawPauseOverlay(ctx); break;
        case UiOverlay::Results: drawResultsOverlay(ctx); break;
    }

    g_dialogs.draw(skin);
}

}  // namespace UiShell
