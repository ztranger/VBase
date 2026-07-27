#pragma once

#include "Texture.h"

// Минимальный встроенный растровый шрифт 5x7 для HUD (цифры, ':', '.', 'F/P/S/M',
// пробел). Данные зашиты в код, атлас генерится процедурно — без файлов-ассетов.
namespace font {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

extern const char kChars[];  // набор символов в порядке ячеек атласа

int glyphCount();
int glyphIndex(char c);      // индекс символа в атласе или -1

} // namespace font

// Атлас-полоса: ширина = glyphCount*kGlyphW, высота = kGlyphH, RGBA
// (белый непрозрачный там, где пиксель шрифта; иначе прозрачный).
TextureData makeFontAtlas();
