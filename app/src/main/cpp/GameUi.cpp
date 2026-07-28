#include "GameUi.h"

#include <cstring>

#include "imgui.h"

#include "Input.h"  // VirtualJoystick (для оверлея джойстика)
#include "Scene.h"

namespace GameUi {

void build(GameUiState& state, Scene& scene) {
    ImGui::SetNextWindowPos(ImVec2(20, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("VBase");
    ImGui::Text("FPS: %.1f", (double)state.fps);
    ImGui::SliderFloat("Light angle", &state.lightAngle, 0.0f, 6.2831f);

    ImGui::SeparatorText("Character");
    ImGui::Text("Speed: %.2f", (double)scene.characterSpeed());
    float yawOff = scene.modelYawOffset();
    if (ImGui::SliderFloat("Model yaw", &yawOff, -3.15f, 3.15f)) {
        scene.setModelYawOffset(yawOff);  // подгонка "морда по движению"
    }
    float mscale = scene.modelScale();
    if (ImGui::SliderFloat("Model scale", &mscale, 0.005f, 0.1f, "%.3f")) {
        scene.setModelScale(mscale);
    }

    ImGui::SeparatorText("Camera");
    float cd = scene.cameraDistance();
    if (ImGui::SliderFloat("Distance", &cd, 2.0f, 15.0f)) scene.setCameraDistance(cd);
    float ch = scene.cameraHeight();
    if (ImGui::SliderFloat("Height", &ch, 0.5f, 10.0f)) scene.setCameraHeight(ch);

    ImGui::SeparatorText("Network");
    if (scene.netConnected()) {
        ImGui::Text("%s | remotes: %d", scene.netHost() ? "HOST" : "CLIENT",
                    scene.remoteCount());
        if (ImGui::Button("Disconnect")) scene.leaveGame();
    } else {
        // Поле ввода IP: на десктопе работает системная клавиатура. На Android
        // системная софт-клавиатура из ImGui не поднимается, поэтому ниже —
        // своя цифровая клавиатура (тапы работают), она пишет в тот же буфер.
        ImGui::InputText("IP", state.joinIp, sizeof(state.joinIp),
                         ImGuiInputTextFlags_CharsDecimal);

        auto key = [&state](char c) {
            size_t l = std::strlen(state.joinIp);
            if (l + 1 < sizeof(state.joinIp)) {
                state.joinIp[l] = c;
                state.joinIp[l + 1] = '\0';
            }
        };
        float b = ImGui::GetFontSize() * 2.2f;
        ImVec2 sz(b, b);
        const char* rows[3] = {"789", "456", "123"};
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 3; ++i) {
                char lbl[2] = {rows[r][i], '\0'};
                if (i > 0) ImGui::SameLine();
                if (ImGui::Button(lbl, sz)) key(rows[r][i]);
            }
        }
        if (ImGui::Button("0", sz)) key('0');
        ImGui::SameLine();
        if (ImGui::Button(".", sz)) key('.');
        ImGui::SameLine();
        if (ImGui::Button("<-", sz)) {
            size_t l = std::strlen(state.joinIp);
            if (l > 0) state.joinIp[l - 1] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Clr", sz)) state.joinIp[0] = '\0';

        if (ImGui::Button("Host")) scene.hostGame();
        ImGui::SameLine();
        if (ImGui::Button("Join")) scene.joinGame(state.joinIp);
    }
    ImGui::End();

    // Виртуальный джойстик поверх всего (появляется под пальцем на тач-экране;
    // на десктопе неактивен, поэтому ничего не рисуется).
    if (scene.joystick().active) {
        const VirtualJoystick& js = scene.joystick();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(js.ox, js.oy), js.radius, IM_COL32(255, 255, 255, 110), 48, 4.0f);
        dl->AddCircleFilled(ImVec2(js.cx, js.cy), js.radius * 0.4f,
                            IM_COL32(255, 255, 255, 190));
    }
}

}  // namespace GameUi
