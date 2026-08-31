#include "engine/render/ui/panels/HomePanel.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"

#include "game/Scene.h"

namespace {

void drawStubBody(UiShell::Ctx& ctx, const char* title, const char* tip) {
    if (!ctx.beginPanel(title)) {
        ctx.endPanel();
        return;
    }
    ImGui::TextWrapped("%s", tip);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextDisabled("Заглушка — контент появится позже.");
    if (ctx.btn("На главную")) UiShell::setPanel(MainMenuPanel::Home);
    ctx.endPanel();
}

}  // namespace

namespace HomePanel {

void draw(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 520), ImGuiCond_FirstUseEver);

    if (!ctx.beginPanel("VBase###uiHomePanel")) {
        ctx.endPanel();
        return;
    }

    ImGui::TextWrapped("Главное меню. Выбери раздел внизу или войди в бой.");
    ImGui::Dummy(ImVec2(0, 6));

    if (ctx.btn("В бой", ImVec2(-1, 0))) UiShell::setMode(UiMode::CharacterSelect);

    ImGui::SeparatorText("Сеть");
    if (ctx.scene.netConnected()) {
        ImGui::Text("%s | remotes: %d | ping: %d ms", ctx.scene.netHost() ? "HOST" : "CLIENT",
                    ctx.scene.remoteCount(), ctx.scene.netPingMs());
        if (ctx.btn("В бой (сессия)")) UiShell::setMode(UiMode::CharacterSelect);
        ImGui::SameLine();
        if (ctx.btn("Отключиться")) {
            Scene* scene = &ctx.scene;
            UiShell::pushYesNo("Сеть", "Разорвать соединение?", [scene](DialogResult r) {
                if (r == DialogResult::Yes) scene->leaveGame();
            });
        }
    } else if (ctx.scene.netConnecting() || ctx.scene.netConnectionLost()) {
        // Идёт (пере)подключение: не показываем клавиатуру ввода IP, чтобы обрыв с
        // авто-реконнектом не выглядел как «отвалились в меню». Даём только отмену.
        if (ctx.scene.netConnectionLost())
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "Соединение потеряно — переподключение...");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Подключение...");
        if (ctx.btn("Отмена")) ctx.scene.leaveGame();
    } else {
        ImGui::InputText("IP", ctx.state.joinIp, sizeof(ctx.state.joinIp),
                         ImGuiInputTextFlags_CharsDecimal);

        auto key = [&ctx](char c) {
            size_t l = std::strlen(ctx.state.joinIp);
            if (l + 1 < sizeof(ctx.state.joinIp)) {
                ctx.state.joinIp[l] = c;
                ctx.state.joinIp[l + 1] = '\0';
            }
        };
        float b = ImGui::GetFontSize() * 2.2f;
        ImVec2 sz(b, b);
        const char* rows[3] = {"789", "456", "123"};
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 3; ++i) {
                char lbl[2] = {rows[r][i], '\0'};
                if (i > 0) ImGui::SameLine();
                if (ctx.btn(lbl, sz)) key(rows[r][i]);
            }
        }
        if (ctx.btn("0", sz)) key('0');
        ImGui::SameLine();
        if (ctx.btn(".", sz)) key('.');
        ImGui::SameLine();
        if (ctx.btn("<-", sz)) {
            size_t l = std::strlen(ctx.state.joinIp);
            if (l > 0) ctx.state.joinIp[l - 1] = '\0';
        }
        ImGui::SameLine();
        if (ctx.btn("Clr", sz)) ctx.state.joinIp[0] = '\0';

        if (ctx.btn("Host")) {
            ctx.scene.hostGame();
            UiShell::setMode(UiMode::CharacterSelect);
        }
        ImGui::SameLine();
        if (ctx.btn("Join")) {
            ctx.scene.joinGame(ctx.state.joinIp);
            UiShell::setMode(UiMode::CharacterSelect);
        }
    }

    // Выбор сцены — только вне сессии: смена сцены = полная пересборка мира, а в
    // онлайне клиент обязан рисовать ту же геометрию, что симулирует сервер.
    if (!ctx.scene.netConnected() && !ctx.scene.netConnecting()) {
        ImGui::SeparatorText("Сцена");
        int n = ctx.scene.sceneListCount();
        if (n == 0) {
            ImGui::TextDisabled("Манифест не найден (config/scenes.cfg)");
        } else {
            int cur = ctx.scene.currentSceneIndex();
            for (int i = 0; i < n; ++i) {
                bool isCur = (i == cur);
                if (isCur) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
                std::string lbl = (isCur ? "> " : "   ") + std::string(ctx.scene.sceneListName(i));
                if (ctx.btn(lbl.c_str(), ImVec2(-1, 0)) && !isCur) {
                    ctx.scene.leaveGame();  // разорвать сессию до перезагрузки мира
                    std::snprintf(ctx.state.requestScenePath, sizeof(ctx.state.requestScenePath),
                                  "%s", ctx.scene.sceneListPath(i));
                }
                if (isCur) ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("Клик — перезагрузить мир этой сценой (до Host).");
        }
    }

    ImGui::SeparatorText("Окна");
    if (ctx.btn(UiShell::isDebugOpen() ? "Скрыть Debug" : "Показать Debug")) {
        UiShell::toggleDebug();
    }

    ctx.endPanel();
}

}  // namespace HomePanel

namespace MenuStubPanels {

void drawInventory(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    drawStubBody(ctx, "Инвентарь###uiInvPanel", "Инвентарь героя и расходники.");
}

void drawQuests(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    drawStubBody(ctx, "Квесты###uiQuestsPanel", "Журнал заданий и наград.");
}

void drawShop(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    drawStubBody(ctx, "Магазин###uiShopPanel", "Покупка построек и улучшений.");
}

void drawEvents(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    drawStubBody(ctx, "События###uiEventsPanel", "Временные ивенты и награды.");
}

}  // namespace MenuStubPanels
