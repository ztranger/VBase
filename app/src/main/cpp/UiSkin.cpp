#include "UiSkin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Assets.h"
#include "AssetSource.h"
#include "Log.h"

namespace UiSkin {
namespace {

void putPixel(TextureData& t, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
              uint8_t a) {
    if (x >= t.width || y >= t.height) return;
    size_t i = ((size_t)y * t.width + x) * 4;
    t.rgba[i + 0] = r;
    t.rgba[i + 1] = g;
    t.rgba[i + 2] = b;
    t.rgba[i + 3] = a;
}

TextureData makePanelTexture() {
    const uint32_t S = 64;
    const uint32_t B = 16;
    TextureData t;
    t.width = S;
    t.height = S;
    t.rgba.assign((size_t)S * S * 4, 0);
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const bool edge = x < B || y < B || x >= S - B || y >= S - B;
            if (edge) {
                const bool rim = x < 2 || y < 2 || x >= S - 2 || y >= S - 2 ||
                                 (x >= B - 2 && x < B) || (y >= B - 2 && y < B) ||
                                 (x >= S - B && x < S - B + 2) ||
                                 (y >= S - B && y < S - B + 2);
                if (rim) putPixel(t, x, y, 90, 140, 210, 255);
                else putPixel(t, x, y, 28, 42, 68, 255);
            } else {
                putPixel(t, x, y, 12, 16, 28, 210);
            }
        }
    }
    return t;
}

TextureData makeButtonTexture(uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t W = 96;
    const uint32_t H = 40;
    const float rad = 10.0f;
    TextureData t;
    t.width = W;
    t.height = H;
    t.rgba.assign((size_t)W * H * 4, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            float dx = 0.0f, dy = 0.0f;
            if (x < rad) dx = rad - (float)x;
            else if (x >= W - rad) dx = (float)x - (W - 1 - rad);
            if (y < rad) dy = rad - (float)y;
            else if (y >= H - rad) dy = (float)y - (H - 1 - rad);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > rad + 0.5f) continue;
            uint8_t a = 255;
            if (d > rad - 0.5f)
                a = (uint8_t)std::clamp(255.0f * (rad + 0.5f - d), 0.0f, 255.0f);
            const float shade = 1.0f + 0.12f * (1.0f - (float)y / (float)H);
            putPixel(t, x, y, (uint8_t)std::clamp(r * shade, 0.0f, 255.0f),
                     (uint8_t)std::clamp(g * shade, 0.0f, 255.0f),
                     (uint8_t)std::clamp(b * shade, 0.0f, 255.0f), a);
        }
    }
    return t;
}

TextureData loadOrMake(AssetSource& assets, const char* path, TextureData (*fallback)()) {
    TextureData img;
    if (loadImageAsset(assets, path, img)) {
        LOGI("UiSkin: загружен %s (%ux%u)", path, img.width, img.height);
        return img;
    }
    return fallback();
}

void drawNineSlice(ImDrawList* dl, ImTextureID tex, ImVec2 pmin, ImVec2 pmax, float borderPx,
                   float texW, float texH, float borderTexels) {
    if (tex == ImTextureID_Invalid || dl == nullptr) return;
    if (pmax.x <= pmin.x || pmax.y <= pmin.y) return;

    const float bw = std::min(borderPx, (pmax.x - pmin.x) * 0.5f);
    const float bh = std::min(borderPx, (pmax.y - pmin.y) * 0.5f);
    const float u = borderTexels / texW;
    const float v = borderTexels / texH;

    const float x0 = pmin.x, x1 = pmin.x + bw, x2 = pmax.x - bw, x3 = pmax.x;
    const float y0 = pmin.y, y1 = pmin.y + bh, y2 = pmax.y - bh, y3 = pmax.y;
    const float u0 = 0.0f, u1 = u, u2 = 1.0f - u, u3 = 1.0f;
    const float v0 = 0.0f, v1 = v, v2 = 1.0f - v, v3 = 1.0f;

    ImTextureRef ref(tex);
    auto quad = [&](float xa, float xb, float ya, float yb, float ua, float ub, float va,
                    float vb) {
        if (xb <= xa || yb <= ya) return;
        dl->AddImage(ref, ImVec2(xa, ya), ImVec2(xb, yb), ImVec2(ua, va), ImVec2(ub, vb));
    };

    quad(x0, x1, y0, y1, u0, u1, v0, v1);
    quad(x1, x2, y0, y1, u1, u2, v0, v1);
    quad(x2, x3, y0, y1, u2, u3, v0, v1);
    quad(x0, x1, y1, y2, u0, u1, v1, v2);
    quad(x1, x2, y1, y2, u1, u2, v1, v2);
    quad(x2, x3, y1, y2, u2, u3, v1, v2);
    quad(x0, x1, y2, y3, u0, u1, v2, v3);
    quad(x1, x2, y2, y3, u1, u2, v2, v3);
    quad(x2, x3, y2, y3, u2, u3, v2, v3);
}

std::string titleOnly(const char* name) {
    const char* hash = std::strstr(name, "###");
    if (hash == nullptr) return name;
    return std::string(name, hash);
}

}  // namespace

bool load(Renderer& renderer, AssetSource& assets, Assets& out) {
    unload(renderer, out);

    TextureData panel = loadOrMake(assets, "ui/panel.png", []() { return makePanelTexture(); });
    TextureData bn = loadOrMake(assets, "ui/button_normal.png",
                                []() { return makeButtonTexture(45, 110, 190); });
    TextureData bh = loadOrMake(assets, "ui/button_hover.png",
                                []() { return makeButtonTexture(70, 145, 230); });
    TextureData ba = loadOrMake(assets, "ui/button_active.png",
                                []() { return makeButtonTexture(30, 80, 150); });

    out.panelHandle = renderer.createTexture(panel, true);
    out.btnNormalHandle = renderer.createTexture(bn, true);
    out.btnHoverHandle = renderer.createTexture(bh, true);
    out.btnActiveHandle = renderer.createTexture(ba, true);
    if (out.panelHandle == 0 || out.btnNormalHandle == 0 || out.btnHoverHandle == 0 ||
        out.btnActiveHandle == 0) {
        LOGW("UiSkin: не удалось создать GPU-текстуры");
        unload(renderer, out);
        return false;
    }

    out.panel = (ImTextureID)renderer.getImGuiTexture(out.panelHandle);
    out.btnNormal = (ImTextureID)renderer.getImGuiTexture(out.btnNormalHandle);
    out.btnHover = (ImTextureID)renderer.getImGuiTexture(out.btnHoverHandle);
    out.btnActive = (ImTextureID)renderer.getImGuiTexture(out.btnActiveHandle);
    if (out.panel == ImTextureID_Invalid || out.btnNormal == ImTextureID_Invalid ||
        out.btnHover == ImTextureID_Invalid || out.btnActive == ImTextureID_Invalid) {
        LOGW("UiSkin: getImGuiTexture вернул invalid");
        unload(renderer, out);
        return false;
    }

    out.panelTexW = (float)panel.width;
    out.panelTexH = (float)panel.height;
    out.panelBorderTexels = std::min(out.panelTexW, out.panelTexH) * 0.25f;
    out.ready = true;
    LOGI("UiSkin: готов (panel %ux%u)", panel.width, panel.height);
    return true;
}

void unload(Renderer& renderer, Assets& assets) {
    auto release = [&](ImTextureID& id) {
        if (id != ImTextureID_Invalid) {
            renderer.releaseImGuiTexture((uint64_t)id);
            id = ImTextureID_Invalid;
        }
    };
    release(assets.panel);
    release(assets.btnNormal);
    release(assets.btnHover);
    release(assets.btnActive);
    assets.panelHandle = 0;
    assets.btnNormalHandle = 0;
    assets.btnHoverHandle = 0;
    assets.btnActiveHandle = 0;
    assets.ready = false;
}

bool BeginPanel(const char* name, const Assets& skin, bool* p_open, ImGuiWindowFlags flags) {
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground;
    const bool open = ImGui::Begin(name, p_open, flags);
    if (!open) return false;

    if (skin.ready) {
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        const float border = std::max(8.0f, ImGui::GetFontSize() * 0.75f);
        drawNineSlice(ImGui::GetWindowDrawList(), skin.panel, p0,
                      ImVec2(p0.x + sz.x, p0.y + sz.y), border, skin.panelTexW, skin.panelTexH,
                      skin.panelBorderTexels);

        const std::string title = titleOnly(name);
        if (!title.empty()) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + border, p0.y + border * 0.35f),
                                                ImGui::GetColorU32(ImGuiCol_Text),
                                                title.c_str());
        }
        ImGui::SetCursorPos(ImVec2(border, border + ImGui::GetFontSize() * 1.15f));
    }
    return true;
}

void EndPanel() { ImGui::End(); }

bool Button(const char* label, const Assets& skin, const ImVec2& size) {
    if (!skin.ready) return ImGui::Button(label, size);

    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 sz = size;
    if (sz.x <= 0.0f) sz.x = labelSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (sz.y <= 0.0f) sz.y = labelSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
    // Чуть больше дефолтной кнопки — текстура читается лучше на таче.
    if (size.x <= 0.0f) sz.x = std::max(sz.x, ImGui::GetFontSize() * 3.0f);
    if (size.y <= 0.0f) sz.y = std::max(sz.y, ImGui::GetFontSize() * 1.6f);

    const bool pressed = ImGui::InvisibleButton(label, sz);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImTextureID tex = skin.btnNormal;
    if (held) tex = skin.btnActive;
    else if (hovered) tex = skin.btnHover;

    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();
    const ImU32 col = IM_COL32(255, 255, 255, (int)(255.0f * ImGui::GetStyle().Alpha));
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(tex), rmin, rmax, ImVec2(0, 0),
                                         ImVec2(1, 1), col);

    const ImVec2 textPos(rmin.x + (rmax.x - rmin.x - labelSize.x) * 0.5f,
                         rmin.y + (rmax.y - rmin.y - labelSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), label);
    return pressed;
}

}  // namespace UiSkin
