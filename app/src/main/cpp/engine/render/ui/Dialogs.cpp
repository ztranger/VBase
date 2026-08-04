#include "engine/render/ui/Dialogs.h"

#include <algorithm>

#include "imgui.h"

namespace UiDialogs {
namespace {

bool SkinBtn(const char* label, const UiSkin::Assets& skin, const ImVec2& size = ImVec2(0, 0)) {
    if (skin.ready) return UiSkin::Button(label, skin, size);
    return ImGui::Button(label, size);
}

}  // namespace

void Stack::pushOk(const char* title, const char* text, DialogCallback cb) {
    Entry e;
    e.kind = Entry::Kind::Ok;
    e.title = title ? title : "";
    e.text = text ? text : "";
    e.callback = std::move(cb);
    stack_.push_back(std::move(e));
}

void Stack::pushYesNo(const char* title, const char* text, DialogCallback cb) {
    Entry e;
    e.kind = Entry::Kind::YesNo;
    e.title = title ? title : "";
    e.text = text ? text : "";
    e.callback = std::move(cb);
    stack_.push_back(std::move(e));
}

void Stack::finish(DialogResult result) {
    if (stack_.empty()) return;
    DialogCallback cb = std::move(stack_.back().callback);
    stack_.pop_back();
    if (cb) cb(result);
}

void Stack::draw(const UiSkin::Assets& skin) {
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

        const float btnW = 110.0f;
        if (top.kind == Entry::Kind::Ok) {
            float x = (ImGui::GetContentRegionAvail().x - btnW) * 0.5f;
            if (x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
            if (SkinBtn("OK", skin, ImVec2(btnW, 0))) finish(DialogResult::Ok);
        } else {
            float total = btnW * 2 + ImGui::GetStyle().ItemSpacing.x;
            float x = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
            if (x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
            if (SkinBtn("Да", skin, ImVec2(btnW, 0))) finish(DialogResult::Yes);
            ImGui::SameLine();
            if (SkinBtn("Нет", skin, ImVec2(btnW, 0))) finish(DialogResult::No);
        }
    }
    if (skin.ready) UiSkin::EndPanel();
    else ImGui::End();

    if (!open) finish(DialogResult::Cancel);
}

}  // namespace UiDialogs
