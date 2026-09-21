#include "engine/render/ui/Dialogs.h"

#include <algorithm>

#include "imgui.h"

#include "engine/render/ui/UiPalette.h"

namespace UiDialogs {
namespace {

bool SkinBtn(const char* label, const UiSkin::Assets& skin, const ImVec2& size = ImVec2(0, 0)) {
    if (skin.ready) return UiSkin::Button(label, skin, size);
    return ImGui::Button(label, size);
}

}  // namespace

void Stack::pushOk(const char* title, const char* text, DialogCallback cb) {
    pushCustom(title, text, {{"OK", DialogResult::Ok}}, std::move(cb));
}

void Stack::pushYesNo(const char* title, const char* text, DialogCallback cb) {
    pushCustom(title, text, {{"Да", DialogResult::Yes}, {"Нет", DialogResult::No}}, std::move(cb));
}

void Stack::pushCustom(const char* title, const char* text, std::vector<Button> buttons,
                       DialogCallback cb) {
    Entry e;
    e.title = title ? title : "";
    e.text = text ? text : "";
    e.buttons = std::move(buttons);
    if (e.buttons.empty()) e.buttons.push_back({"OK", DialogResult::Ok});  // страховка
    e.callback = std::move(cb);
    stack_.push_back(std::move(e));
}

void Stack::pushToast(const char* text, Toast::Kind kind, float seconds) {
    Toast t;
    t.kind = kind;
    t.text = text ? text : "";
    t.remaining = seconds > 0.0f ? seconds : 3.0f;
    toasts_.push_back(std::move(t));
}

void Stack::finish(DialogResult result) {
    if (stack_.empty()) return;
    DialogCallback cb = std::move(stack_.back().callback);
    stack_.pop_back();
    if (cb) cb(result);
}

void Stack::draw(const UiSkin::Assets& skin) {
    drawModal(skin);   // верх стека модалок (затемняет, блокирует)
    drawToasts();      // недолгие плашки поверх всего, без блокировки
}

void Stack::drawModal(const UiSkin::Assets& skin) {
    if (stack_.empty()) return;

    const Entry& top = stack_.back();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;

    // Затемнение + блок кликов «под» диалогом.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.55f));
    ImGui::Begin("##uiDialogDimmer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::InvisibleButton("##uiDialogBlock", disp);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    const float dlgW = std::min(disp.x * 0.85f, 420.0f);
    const float dlgH = 220.0f;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f - dlgW * 0.5f, disp.y * 0.5f - dlgH * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();

    const std::string winTitle = top.title + "###uiDialogModal";
    bool open = true;
    bool begun = false;
    if (skin.ready) {
        begun = UiSkin::BeginPanel(winTitle.c_str(), skin, &open,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    } else {
        begun = ImGui::Begin(winTitle.c_str(), &open,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    }
    if (begun) {
        ImGui::TextWrapped("%s", top.text.c_str());
        ImGui::Dummy(ImVec2(0, 16));

        // Ряд кнопок по центру: N кнопок фикс. ширины через ItemSpacing.
        const int n = (int)top.buttons.size();
        const float btnW = 110.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float total = btnW * n + spacing * (n - 1);
        float x = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
        if (x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
        DialogResult chosen = DialogResult::None;
        for (int i = 0; i < n; ++i) {
            if (i > 0) ImGui::SameLine();
            ImGui::PushID(i);
            if (SkinBtn(top.buttons[i].label.c_str(), skin, ImVec2(btnW, 0)))
                chosen = top.buttons[i].result;
            ImGui::PopID();
        }
        // finish() меняет stack_ (top становится висячей ссылкой) — зовём ПОСЛЕ цикла.
        if (chosen != DialogResult::None) {
            if (skin.ready) UiSkin::EndPanel(); else ImGui::End();
            finish(chosen);
            return;
        }
    }
    if (skin.ready) UiSkin::EndPanel();
    else ImGui::End();

    if (!open) finish(DialogResult::Cancel);  // крестик = отмена
}

void Stack::drawToasts() {
    if (toasts_.empty()) return;

    const float dt = ImGui::GetIO().DeltaTime;
    // Состариваем и убираем истёкшие (в один проход, порядок сохраняем).
    for (Toast& t : toasts_) {
        t.age += dt;
        t.remaining -= dt;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [](const Toast& t) { return t.remaining <= 0.0f; }),
                  toasts_.end());
    if (toasts_.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    const float margin = font * 0.9f;
    const float w = std::min(io.DisplaySize.x - margin * 2.0f, font * 20.0f);
    // Стопка снизу вверх у нижнего края (над возможным HUD/панелями рисуем forwardlist'ом).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = io.DisplaySize.y - margin;
    const float padX = font * 0.6f, padY = font * 0.4f;

    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const Toast& t = *it;
        // Прозрачность: быстрый fade-in (0.2 c) и fade-out на последних 0.4 c.
        float a = 1.0f;
        if (t.age < 0.2f) a = t.age / 0.2f;
        if (t.remaining < 0.4f) a = std::min(a, t.remaining / 0.4f);
        if (a < 0.0f) a = 0.0f;
        const int alpha = (int)(a * 255.0f);

        ImVec2 ts = ImGui::CalcTextSize(t.text.c_str());
        if (ts.x > w - padX * 2.0f) ts.x = w - padX * 2.0f;  // клампим ширину плашки
        const float boxW = ts.x + padX * 2.0f;
        const float boxH = ts.y + padY * 2.0f;
        const float x = io.DisplaySize.x - margin - boxW;  // прижато к правому краю
        y -= boxH;

        ImU32 accent = UiPalette::Info;
        if (t.kind == Toast::Kind::Success) accent = UiPalette::Success;
        else if (t.kind == Toast::Kind::Danger) accent = UiPalette::Danger;

        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + boxW, y + boxH),
                          UiPalette::withAlpha(UiPalette::Panel, (int)(a * 235.0f)), font * 0.25f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + boxW, y + boxH),
                    UiPalette::withAlpha(accent, alpha), font * 0.25f, 0, 1.5f);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + font * 0.18f, y + boxH),
                          UiPalette::withAlpha(accent, alpha), font * 0.25f);  // цветная кромка
        dl->AddText(ImVec2(x + padX, y + padY), UiPalette::withAlpha(UiPalette::Text, alpha),
                    t.text.c_str());
        y -= margin * 0.4f;  // зазор между тостами
    }
}

}  // namespace UiDialogs
