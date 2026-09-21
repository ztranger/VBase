#pragma once

#include "imgui.h"

#include "engine/render/GameUi.h"
#include "engine/render/UiSkin.h"
#include "engine/render/ui/Dialogs.h"
#include "engine/render/ui/UiTypes.h"

class Scene;
class Renderer;
struct AssetSource;

namespace UiShell {

// Общий контекст кадра для screens / panels / windows.
struct Ctx {
    GameUiState& state;
    Scene& scene;
    const UiSkin::Assets& skin;

    // selected — подсветка «выбрано/активно» (вкладки/списки); enabled=false — затемнена/не кликается.
    bool btn(const char* label, const ImVec2& size = ImVec2(0, 0), bool selected = false,
             bool enabled = true) const;
    bool beginPanel(const char* name, bool* p_open = nullptr,
                    ImGuiWindowFlags flags = 0) const;
    // «Нормальное» окно: приклеено к прямоугольнику (пиксели), без драга/ресайза/сейва.
    // Для игровых панелей — чтобы не таскались как debug-окна. extraFlags — доп. флаги
    // (напр. AlwaysAutoResize, тогда size задаёт только позицию/ширину).
    bool beginPanelRect(const char* name, const ImVec2& pos, const ImVec2& size,
                        bool* p_open = nullptr, ImGuiWindowFlags extraFlags = 0) const;
    void endPanel() const;

    // Прозрачный оверлей-окно (HUD/легенды/chrome/лоадинг-превью): БЕЗ скина, заякорено, не
    // таскается (NoTitleBar|NoMove|NoResize|NoCollapse|NoSavedSettings|NoFocusOnAppearing).
    // bgAlpha — прозрачность фона окна (0 = без фона). endOverlay() звать ВСЕГДА (пара к Begin).
    bool beginOverlay(const char* name, const ImVec2& pos, const ImVec2& size, float bgAlpha,
                      ImGuiWindowFlags extraFlags = 0) const;
    void endOverlay() const;
};

// Якорь окна к краю/центру экрана — единая раскладка вместо захардкоженных позиций (см. #4).
enum class Anchor { TopLeft, TopRight, BottomLeft, BottomRight, Center };
// Верхний-левый угол окна размера size при якоре a с полем margin (<0 -> uiMargin()).
ImVec2 anchorPos(Anchor a, const ImVec2& size, float margin = -1.0f);

// Метрики UI в единицах шрифта — масштабируются под DPI (платформа масштабирует шрифт).
// Звать только внутри кадра ImGui (нужен активный шрифт). ~54/16 px на десктопе (font 18).
float navBarHeight();  // высота нижнего нав-бара главного меню (общая для бара и раскладки)
float uiMargin();      // стандартное поле от краёв экрана и между окнами

// Прямоугольник контент-области меню (над нав-баром, с полями). Панели разделов
// (Главная/Инвентарь/…) якорятся сюда — заполняют область, а не висят коробочкой в углу.
void menuContentRect(ImVec2& pos, ImVec2& size);

// --- Навигация (единый авторитет) ---------------------------------------------------------
// Один источник правды о том, где мы: режим (экран), оверлей поверх него и активная вкладка
// хаба. Платформа берёт отсюда рендер-путь и gameplayActive(), UI — что рисовать. Раньше это
// было размазано по ~6 файлам с дубль-тернаром рендера в двух main; см. docs/UI_SYSTEM.md.

// Сменить экран: запоминает предыдущий режим (для back()), сбрасывает оверлей, а при входе
// в MainMenu возвращает вкладку хаба на Home.
void setMode(UiMode mode);
UiMode mode();
UiMode prevMode();  // режим до последнего setMode (для back())
void back();        // вернуться в предыдущий режим (снимает оверлей); по умолчанию -> MainMenu

// Оверлей поверх текущего режима (пауза/итог матча). None — нет оверлея.
void setOverlay(UiOverlay overlay);
UiOverlay overlay();

// 3D-путь под текущим режимом (одна таблица mode->RenderPath). Платформа рисует по нему мир,
// превью героя или пустой фон — без дубль-тернара в desktop/platform main.
RenderPath renderPath();

void setPanel(MainMenuPanel panel);
MainMenuPanel panel();

void showDebug(bool open);
bool isDebugOpen();
void toggleDebug();

void pushOk(const char* title, const char* text, DialogCallback cb = {});
void pushYesNo(const char* title, const char* text, DialogCallback cb = {});
// Диалог с произвольным набором кнопок (>=1). Каждая кнопка несёт свой DialogResult в callback.
void pushDialog(const char* title, const char* text, std::vector<UiDialogs::Button> buttons,
                DialogCallback cb = {});
// Неблокирующее уведомление (успех/ошибка/инфо), само гаснет. seconds<=0 -> дефолт.
void pushToast(const char* text, UiDialogs::Toast::Kind kind = UiDialogs::Toast::Kind::Info,
               float seconds = 0.0f);
bool hasModal();

// true только когда идёт бой и нет модалки. По нему платформа гейтит игровой ввод:
// в меню/лоадинге/под диалогом WASD и тач-стики не должны двигать героя/камеру.
bool gameplayActive();

// Ассеты лоадинга (текстуры). Вызывать из GameUi::loadSkin / unloadSkin.
void loadLoadingAssets(Renderer& renderer, AssetSource& assets);
void unloadLoadingAssets(Renderer& renderer);
bool hasLoadingArt();

void build(GameUiState& state, Scene& scene, const UiSkin::Assets& skin);

}  // namespace UiShell
