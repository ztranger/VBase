#pragma once

#include "engine/core/Renderer.h"

struct AssetSource;

namespace LoadingScreen {

void load(Renderer& renderer, AssetSource& assets);
void unload(Renderer& renderer);
bool hasArt();
void draw();

}  // namespace LoadingScreen
