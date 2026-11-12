#pragma once
// v0.8.6 prueba 6 (Belial 64): graficos del pueblo en la memoria del cartucho (ver monster.cpp).
#ifdef __N64__
#include "engine/clx_sprite.hpp"

namespace devilution {
/** Lista CLX en el cartucho; si no cabe se carga en 'fallback' (RAM) y se devuelve esa. */
OptionalClxSpriteList N64TownList(const char *path, OptionalOwnedClxSpriteList &fallback);
OptionalClxSpriteSheet N64TownSheet(const char *path, OptionalOwnedClxSpriteSheet &fallback);
/** Se llama al soltar los graficos del pueblo. */
void N64TownGfxFree();
} // namespace devilution
#endif
