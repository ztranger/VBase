#include "engine/render/ui/screens/LoadingScreen.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

#include "imgui.h"

#include "engine/assets/AssetSource.h"
#include "engine/assets/Assets.h"
#include "engine/core/Log.h"
#include "engine/core/Texture.h"

namespace LoadingScreen {
namespace {

ImTextureID g_bg = ImTextureID_Invalid;
ImTextureID g_glow = ImTextureID_Invalid;
ImTextureID g_beam = ImTextureID_Invalid;
ImTextureID g_mote = ImTextureID_Invalid;
TextureHandle g_bgHandle = 0;
TextureHandle g_glowHandle = 0;
TextureHandle g_beamHandle = 0;
TextureHandle g_moteHandle = 0;
float g_aspect = 16.0f / 9.0f;
bool g_fxLogged = false;

TextureData makeGlowTex(uint32_t size) {
    TextureData t;
    t.width = size;
    t.height = size;
    t.rgba.assign((size_t)size * size * 4, 0);
    const float c = (size - 1) * 0.5f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float dx = ((float)x - c) / c;
            const float dy = ((float)y - c) / c;
            const float d2 = dx * dx + dy * dy;
            float a = std::exp(-d2 * 2.8f);
            if (a < 0.004f) continue;
            const size_t i = ((size_t)y * size + x) * 4;
            t.rgba[i + 0] = 255;
            t.rgba[i + 1] = 255;
            t.rgba[i + 2] = 255;
            t.rgba[i + 3] = (uint8_t)std::clamp(a * 255.0f, 0.0f, 255.0f);
        }
    }
    return t;
}

TextureData makeBeamTex(uint32_t w, uint32_t h) {
    TextureData t;
    t.width = w;
    t.height = h;
    t.rgba.assign((size_t)w * h * 4, 0);
    const float cx = (w - 1) * 0.5f;
    for (uint32_t y = 0; y < h; ++y) {
        const float along = (float)y / (float)std::max(h - 1, 1u);
        const float fallY = std::pow(1.0f - along, 1.25f);
        const float half = (0.18f + along * 0.40f) * w * 0.5f;
        for (uint32_t x = 0; x < w; ++x) {
            const float dx = std::fabs((float)x - cx) / std::max(half, 1.0f);
            const float fallX = std::exp(-dx * dx * 2.4f);
            float a = fallX * fallY;
            if (a < 0.004f) continue;
            const size_t i = ((size_t)y * w + x) * 4;
            t.rgba[i + 0] = 255;
            t.rgba[i + 1] = 255;
            t.rgba[i + 2] = 255;
            t.rgba[i + 3] = (uint8_t)std::clamp(a * 255.0f, 0.0f, 255.0f);
        }
    }
    return t;
}

TextureData makeMoteTex(uint32_t size) {
    TextureData t;
    t.width = size;
    t.height = size;
    t.rgba.assign((size_t)size * size * 4, 0);
    const float c = (size - 1) * 0.5f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float dx = ((float)x - c) / c;
            const float dy = ((float)y - c) / c;
            const float a = std::exp(-(dx * dx + dy * dy) * 5.0f);
            if (a < 0.01f) continue;
            const size_t i = ((size_t)y * size + x) * 4;
            t.rgba[i + 0] = 255;
            t.rgba[i + 1] = 255;
            t.rgba[i + 2] = 255;
            t.rgba[i + 3] = (uint8_t)std::clamp(a * 255.0f, 0.0f, 255.0f);
        }
    }
    return t;
}

ImTextureID upload(Renderer& renderer, const TextureData& data, TextureHandle& outHandle) {
    outHandle = renderer.createTexture(data, true);
    if (outHandle == 0) return ImTextureID_Invalid;
    return (ImTextureID)renderer.getImGuiTexture(outHandle);
}

void coverRect(ImVec2 display, float imgAspect, ImVec2& outMin, ImVec2& outMax) {
    const float dispAspect = display.x / std::max(display.y, 1.0f);
    ImVec2 size, off;
    if (dispAspect > imgAspect) {
        size.x = display.x;
        size.y = display.x / imgAspect;
        off.x = 0.0f;
        off.y = (display.y - size.y) * 0.5f;
    } else {
        size.y = display.y;
        size.x = display.y * imgAspect;
        off.y = 0.0f;
        off.x = (display.x - size.x) * 0.5f;
    }
    outMin = off;
    outMax = ImVec2(off.x + size.x, off.y + size.y);
}

}  // namespace

void unload(Renderer& renderer) {
    auto release = [&](ImTextureID& id, TextureHandle& h) {
        if (id != ImTextureID_Invalid) {
            renderer.releaseImGuiTexture((uint64_t)id);
            id = ImTextureID_Invalid;
        }
        h = 0;
    };
    release(g_bg, g_bgHandle);
    release(g_glow, g_glowHandle);
    release(g_beam, g_beamHandle);
    release(g_mote, g_moteHandle);
    g_fxLogged = false;
}

void load(Renderer& renderer, AssetSource& assets) {
    unload(renderer);

    TextureData bg;
    if (loadImageAsset(assets, "ui/loading_core.png", bg)) {
        g_bgHandle = renderer.createTexture(bg, true);
        g_bg = (ImTextureID)renderer.getImGuiTexture(g_bgHandle);
        if (bg.height > 0) g_aspect = (float)bg.width / (float)bg.height;
        LOGI("LoadingScreen: ui/loading_core.png (%ux%u)", bg.width, bg.height);
    } else {
        LOGW("LoadingScreen: ui/loading_core.png не найден");
    }

    g_glow = upload(renderer, makeGlowTex(256), g_glowHandle);
    g_beam = upload(renderer, makeBeamTex(96, 320), g_beamHandle);
    g_mote = upload(renderer, makeMoteTex(32), g_moteHandle);
    LOGI("LoadingScreen FX: glow=%d beam=%d mote=%d", g_glow != ImTextureID_Invalid,
         g_beam != ImTextureID_Invalid, g_mote != ImTextureID_Invalid);
}

bool hasArt() { return g_bg != ImTextureID_Invalid; }

void draw() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    if (disp.x < 1.0f || disp.y < 1.0f) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 origin = vp ? vp->Pos : ImVec2(0, 0);
    const ImVec2 screen = vp ? vp->Size : disp;
    dl->PushClipRect(origin, ImVec2(origin.x + screen.x, origin.y + screen.y), false);

    if (!g_fxLogged) {
        LOGI("LoadingScreen draw: bg=%d glow=%d beam=%d mote=%d size=%.0fx%.0f",
             g_bg != ImTextureID_Invalid, g_glow != ImTextureID_Invalid,
             g_beam != ImTextureID_Invalid, g_mote != ImTextureID_Invalid, screen.x, screen.y);
        g_fxLogged = true;
    }

    const float t = (float)ImGui::GetTime();
    const float breath = 0.5f + 0.5f * std::sin(t * 1.6f);
    const float breath2 = 0.5f + 0.5f * std::sin(t * 1.6f + 1.7f);

    ImVec2 imgMin, imgMax;
    coverRect(screen, g_aspect, imgMin, imgMax);
    imgMin.x += origin.x;
    imgMin.y += origin.y;
    imgMax.x += origin.x;
    imgMax.y += origin.y;
    const float imgW = imgMax.x - imgMin.x;
    const float imgH = imgMax.y - imgMin.y;

    if (g_bg != ImTextureID_Invalid) {
        dl->AddImage(ImTextureRef(g_bg), imgMin, imgMax);
    } else {
        dl->AddRectFilled(imgMin, imgMax, IM_COL32(26, 20, 16, 255));
    }

    const ImVec2 core(imgMin.x + imgW * 0.50f, imgMin.y + imgH * 0.47f);

    if (g_glow != ImTextureID_Invalid) {
        const float g0 = imgH * (0.62f + 0.04f * breath);
        const float g1 = imgH * (0.34f + 0.03f * breath2);
        dl->AddImage(ImTextureRef(g_glow), ImVec2(core.x - g0, core.y - g0),
                     ImVec2(core.x + g0, core.y + g0), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 140, 40, (int)(90 + 50 * breath)));
        dl->AddImage(ImTextureRef(g_glow), ImVec2(core.x - g1, core.y - g1),
                     ImVec2(core.x + g1, core.y + g1), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 210, 120, (int)(110 + 60 * breath2)));
    }

    if (g_beam != ImTextureID_Invalid) {
        const float beamH = imgH * 0.68f;
        const float beamW = imgW * (0.22f + 0.02f * breath);
        dl->AddImage(ImTextureRef(g_beam), ImVec2(core.x - beamW * 0.5f, core.y - imgH * 0.02f),
                     ImVec2(core.x + beamW * 0.5f, core.y + beamH), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 170, 70, (int)(130 + 50 * breath)));
        const float beamW2 = beamW * 1.45f;
        const float yOff = imgH * 0.012f * std::sin(t * 0.9f);
        dl->AddImage(ImTextureRef(g_beam), ImVec2(core.x - beamW2 * 0.5f, core.y + yOff),
                     ImVec2(core.x + beamW2 * 0.5f, core.y + beamH * 0.92f + yOff),
                     ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 200, 110, (int)(70 + 35 * breath2)));
    }

    for (int i = 0; i < 22; ++i) {
        const float seed = (float)i * 17.13f;
        const float along = std::fmod(0.05f + t * (0.06f + 0.03f * std::fmod(seed, 1.0f)) +
                                          std::fmod(seed * 0.11f, 1.0f),
                                      1.0f);
        const float side = std::sin(seed + t * 0.45f) * (imgW * (0.012f + along * 0.055f));
        const ImVec2 p(core.x + side, core.y + along * imgH * 0.60f);
        const float tw = 0.40f + 0.60f * (0.5f + 0.5f * std::sin(t * 3.0f + seed));
        const float r = imgH * (0.006f + along * 0.008f) * (0.75f + 0.5f * tw);
        const int a = (int)(200.0f * tw * (1.0f - along * 0.4f));
        if (g_mote != ImTextureID_Invalid) {
            dl->AddImage(ImTextureRef(g_mote), ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r),
                         ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 230, 170, a));
        } else {
            dl->AddCircleFilled(p, r, IM_COL32(255, 230, 170, a), 12);
        }
    }

    const ImVec2 bot0(origin.x, origin.y + screen.y * 0.70f);
    const ImVec2 bot1(origin.x + screen.x, origin.y + screen.y);
    dl->AddRectFilledMultiColor(bot0, bot1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200));

    const char* title = "VBase";
    const float titleFs = ImGui::GetFontSize() * 2.4f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(titleFs, FLT_MAX, 0.0f, title);
    ImVec2 titlePos(origin.x + screen.x * 0.5f - ts.x * 0.5f, origin.y + screen.y * 0.78f);
    dl->AddText(ImGui::GetFont(), titleFs, ImVec2(titlePos.x + 2, titlePos.y + 2),
                IM_COL32(0, 0, 0, 180), title);
    dl->AddText(ImGui::GetFont(), titleFs, titlePos, IM_COL32(242, 230, 212, 255), title);

    const char* tip = "Готовим оборону…";
    ImVec2 tipSz = ImGui::CalcTextSize(tip);
    dl->AddText(ImVec2(origin.x + screen.x * 0.5f - tipSz.x * 0.5f, titlePos.y + titleFs + 8.0f),
                IM_COL32(184, 169, 148, 255), tip);

    const float barW = std::min(screen.x * 0.45f, 420.0f);
    const float barH = 8.0f;
    const ImVec2 barMin(origin.x + screen.x * 0.5f - barW * 0.5f, titlePos.y + titleFs + 36.0f);
    const ImVec2 barMax(barMin.x + barW, barMin.y + barH);
    dl->AddRectFilled(barMin, barMax, IM_COL32(42, 34, 28, 230), 4.0f);
    dl->AddRect(barMin, barMax, IM_COL32(196, 165, 116, 160), 4.0f, 0, 1.0f);
    float prog = 0.5f + 0.5f * std::sin(t * 0.7f);
    prog = std::clamp(prog, 0.08f, 1.0f);
    dl->AddRectFilled(barMin, ImVec2(barMin.x + barW * prog, barMax.y),
                      IM_COL32(232, 161, 58, 255), 4.0f);

    dl->PopClipRect();
}

}  // namespace LoadingScreen
