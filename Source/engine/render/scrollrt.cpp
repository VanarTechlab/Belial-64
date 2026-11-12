/**
 * @file scrollrt.cpp
 *
 * Implementation of functionality for rendering the dungeons, monsters and calling other render routines.
 */
#include <cstring>
#include <memory>
#include <new>
#include "engine/render/scrollrt.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <ankerl/unordered_dense.h>

#include "DiabloUI/ui_flags.hpp"
#include "automap.h"
#include "controls/control_mode.hpp"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "dead.h"
#include "diablo_msg.hpp"
#include "doom.h"
#include "engine/backbuffer_state.hpp"
#include "engine/displacement.hpp"
#include "engine/dx.h"
#include "engine/point.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/trn.hpp"
#include "engine/world_tile.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "headless_mode.hpp"
#include "help.h"
#include "hwcursor.hpp"
#include "init.hpp"
#include "inv.h"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "lua/lua_event.hpp"
#include "minitext.h"
#include "missiles.h"
#include "nthread.h"
#include "options.h"
#include "panels/charpanel.hpp"
#include "panels/console.hpp"
#include "panels/partypanel.hpp"
#include "panels/quest_log.hpp"
#include "panels/spell_list.hpp"
#include "plrmsg.h"
#include "qol/chatlog.h"
#include "qol/floatingnumbers.h"
#include "qol/itemlabels.h"
#include "qol/monhealthbar.h"
#include "qol/stash.h"
#include "qol/visual_store.h"
#include "qol/xpbar.h"
#include "stores.h"
#include "towners.h"
#include "utils/attributes.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"
#include "utils/sdl_compat.h"
#include "utils/sdl_thread.h"
#include "utils/str_cat.hpp"

#ifndef USE_SDL1
#include "controls/touch/renderers.h"
#endif

#ifdef _DEBUG
#include "debug.h"
#endif

#ifdef DUN_RENDER_STATS
#include "utils/format_int.hpp"
#endif

namespace devilution {

#ifdef __N64__
extern "C" unsigned n64_us(void);
extern "C" void n64_prof_add(int which, unsigned us);
extern "C" void n64_panel_attach(uint8_t *pixels, int pitch, int rows, int panelRow);
extern "C" void n64_panel_frame(void);
extern "C" void n64_panel_visible(int visible);
extern "C" unsigned n64_panel_serial(void);
extern "C" int n64_panel_occluder(int *x0, int *y0, int *x1, int *y1);
void CalculatePanelAreas();
int N64OccX0, N64OccY0, N64OccX1, N64OccY1;
const uint8_t *N64OccPixels;
bool N64UiStoreOnly();
SDL_Surface *n64OverlaySurfaceFwd();
namespace {
// Capa del panel (f2v26): 640x160, el panel ocupa las filas 32..159 y encima queda sitio
// para la parte de arriba de los frascos. Las coordenadas son las de la pantalla (las del
// panel logico), desplazadas para caer en esta capa.
constexpr int N64PanelLayerRows = 160;
constexpr int N64PanelRow = 32;
extern "C" void n64_panel_zone(int on);
extern "C" void n64_rumble_health(int pct, int active); // v0.8.6
extern "C" void n64_world_skip(void); // f2v85
unsigned N64WorldFrozenFrames = 0;
SDL_Surface *n64PanelSurface;
Surface N64PanelOut(const Surface &out)
{
	if (n64OverlaySurfaceFwd() != nullptr && !N64UiStoreOnly())
		return Surface(n64OverlaySurfaceFwd()); // f2v54: modo interfaz: el panel va en la capa de 640x480
	if (gnScreenWidth >= GetMainPanel().size.width)
		return out;
	if (n64PanelSurface == nullptr) {
		n64_panel_zone(1); // f2v83: siempre en su hueco fijo (sdl_n64.c)
		n64PanelSurface = SDL_CreateRGBSurface(0, 640, N64PanelLayerRows, 8, 0, 0, 0, 0);
		n64_panel_zone(0);
		if (n64PanelSurface == nullptr)
			return out;
		memset(n64PanelSurface->pixels, 0, static_cast<size_t>(n64PanelSurface->pitch) * N64PanelLayerRows);
		n64_panel_attach(static_cast<uint8_t *>(n64PanelSurface->pixels), n64PanelSurface->pitch, N64PanelLayerRows, N64PanelRow);
	}
	const Point pos = GetMainPanel().position;
	// at(x, y) = pixels + (region.x + x) + pitch * (region.y + y): el punto pos cae en (pos.x + rx, N64PanelRow)
	const int rx = -pos.x;
	const int ry = N64PanelRow - pos.y;
	return Surface(n64PanelSurface, MakeSdlRect(rx, ry, 640 - rx, pos.y + GetMainPanel().size.height));
}
} // namespace
// N64 (f2v39): se suelta la capa del panel al cargar un nivel (se vuelve a crear al dibujar)
// f2v48: copia del suelo ya dibujado. Si la camara no se mueve y no cambia nada de lo que usa el
// suelo (luz, piezas, ventana), se copia en vez de volver a dibujarlo (f2v47 MEDIDO: suelo ~20 ms).
extern "C" int n64_heap_free_kib(void);
extern "C" int n64_rdp_copy8(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int w, int h);
extern "C" int n64_rdp_fill8(uint8_t *dst, int dpitch, int w, int h, int value);
extern "C" void n64_cel_cache_release(void);
extern "C" void n64_big_zone(int on);
extern "C" int n64_rdp_blit8(uint8_t *dst, int dpitch, int dh, int dx, int dy, const uint8_t *src, int spitch, int sh, int sx, int sy, int w, int h);
extern "C" void *memalign(size_t align, size_t size);
struct N64FreeDeleter {
	void operator()(uint8_t *p) const { free(p); }
};
std::unique_ptr<uint8_t[], N64FreeDeleter> N64FloorSave; // f2v53: alineado a 64 para el RDP
size_t N64FloorSaveSize = 0;
uint32_t N64FloorKey = 0;
bool N64FloorValid = false;
uint32_t N64FloorLastKey = 1;
unsigned N64FloorReused = 0;
unsigned N64FloorDrawn = 0;
void N64FreeFloorSave()
{
	N64FloorSave = nullptr;
	N64FloorSaveSize = 0;
	N64FloorValid = false;
}
// f2v50: capa aparte de 640x480 para el texto de las misiones y las tiendas (hechos para 640x480).
// El RDP la reduce a toda la pantalla encima del mundo (el indice 0 es transparente). Asi el juego
// sigue a su resolucion (f2v49 cambiaba a 640x480 y la vista se alejaba).
extern "C" void n64_overlay_attach(uint8_t *pixels, int pitch);
extern Rectangle UIRectangle;
extern bool N64OverlayDrawing;
SDL_Surface *n64OverlaySurface;
extern bool N64UiMode;
extern "C" void n64_infotext_attach(uint8_t *px, int pitch, int w, int h);
extern "C" void n64_infotext_show(int show);
extern const Surface *N64InfoTextOut;
SDL_Surface *n64OverlaySurfaceFwd() { return N64UiMode ? n64OverlaySurface : nullptr; }
void N64FreeOverlay()
{
	if (n64OverlaySurface != nullptr) {
		n64_overlay_attach(nullptr, 0);
		SDL_FreeSurface(n64OverlaySurface);
		n64OverlaySurface = nullptr;
	}
}
// f2v54: "modo interfaz de 640x480". Con el inventario, el personaje, los hechizos, las misiones,
// las tiendas, el texto de las misiones o el menu de START abiertos, toda la interfaz (paneles, panel
// de abajo, cursor) se dibuja en la capa de 640x480 con las coordenadas de 640x480, como en el PC, y
// el RDP la pone encima del mundo (que sigue a 400x300). Asi caben enteros.
bool N64UiMode = false;
bool n64OverlayInHeap = false; // v0.8.6 prueba 7
extern "C" int n64_heap_largest_kib(void);
extern "C" void n64_log(const char *fmt, ...);
void N64RecalcUIRectangle();
bool N64WantUiMode()
{
	return invflag || CharFlag || SpellbookFlag || QuestLogIsOpen || SpellSelectFlag || IsStashOpen || IsVisualStoreOpen
	    || IsPlayerInStore() || qtextflag || gmenu_is_active();
}
// f2v59: hablando con un aldeano (tiendas, textos) se queda el panel pequeno, no el de PC
bool N64UiStoreOnly()
{
	return N64UiMode && !(invflag || CharFlag || SpellbookFlag || QuestLogIsOpen || SpellSelectFlag || IsStashOpen || IsVisualStoreOpen || gmenu_is_active());
}
void N64UpdateUiMode()
{
	const bool want = gnScreenWidth < 640 && N64WantUiMode();
	if (want && n64OverlaySurface == nullptr) {
		N64FreeFloorSave(); // deja sitio
		// f2v75: siempre se suelta antes la cache de CEL (un bloque grande y seguido) para que la capa
		// caiga en su hueco; al cerrar la capa se suelta otra vez para que la cache vuelva a ese hueco.
		// Antes la memoria se iba troceando: la cache quedaba pequena (el pueblo iba a 6 fps) y al
		// guardar no cabia el bloque del guardado.
		// v0.8.6 prueba 7: si sobra memoria (un hueco de 301 KiB + 200 KiB de margen), la capa va a la RAM
		// normal y la cache de CEL se queda entera (512 KiB): mas fps con menus abiertos. Si no, como antes.
		// La letra grande del menu de START (~90 KiB) se carga la primera vez que se dibuja; si eso pasa con
		// la capa ya puesta, queda en medio y trocea la memoria (MEDIDO en la prueba 7). Se carga antes.
		if (gmenu_is_active())
			(void)GetLineWidth("A", GameFont46, 2);
		n64OverlayInHeap = n64_heap_largest_kib() >= 301 + 200;
		if (n64OverlayInHeap) {
			n64OverlaySurface = SDL_CreateRGBSurface(0, 640, 480, 8, 0, 0, 0, 0);
			n64_log("capa de menus en la RAM normal: cache de CEL entera (libre %d KiB)", n64_heap_free_kib());
		}
		if (n64OverlaySurface == nullptr) {
			n64OverlayInHeap = false;
			n64_cel_cache_release();
			// f2v80: la capa (300 KiB) va al bloque fijo de la cache de CEL; el anillo usa lo que sobra
			n64_big_zone(1);
			n64OverlaySurface = SDL_CreateRGBSurface(0, 640, 480, 8, 0, 0, 0, 0);
			n64_big_zone(0);
		}
	}
	const bool on = want && n64OverlaySurface != nullptr;
	if (on != N64UiMode) {
		const Rectangle oldMain = GetMainPanel();
		const Rectangle oldLeft = GetLeftPanel();
		const Rectangle oldRight = GetRightPanel();
		N64UiMode = on;
		CalculatePanelAreas();
		N64RecalcUIRectangle();
		// el cursor (lo coloca el mando en una casilla) pasa a las coordenadas nuevas
		auto remap = [](const Rectangle &oldR, const Rectangle &newR) {
			if (oldR.contains(MousePosition)) {
				MousePosition += newR.position - oldR.position;
				return true;
			}
			return false;
		};
		if (!remap(oldRight, GetRightPanel()) && !remap(oldLeft, GetLeftPanel()))
			remap(oldMain, GetMainPanel());
		GetDrawnCursor().rect.size = { 0, 0 }; // lo guardado detras del cursor era de la otra imagen
		RedrawEverything();
	}
	if (!want && n64OverlaySurface != nullptr) {
		N64FreeOverlay();
		if (!n64OverlayInHeap)
			n64_cel_cache_release(); // f2v75: que la cache vuelva al hueco grande
		n64OverlayInHeap = false;
	}
	if (N64UiMode)
		n64_overlay_attach(static_cast<uint8_t *>(n64OverlaySurface->pixels), n64OverlaySurface->pitch);
	// f2v76: con el menu de START abierto nunca se ve el panel (tampoco si no hubo capa de menus)
	n64_panel_visible((N64UiMode && !N64UiStoreOnly()) || gmenu_is_active() ? 0 : 1);
	static bool n64StoreOnlySeen = false;
	if (N64UiStoreOnly() != n64StoreOnlySeen) {
		n64StoreOnlySeen = N64UiStoreOnly();
		RedrawEverything(); // el panel cambia de capa
	}
}
Surface N64UiOut(const Surface &out)
{
	if (N64UiMode && n64OverlaySurface != nullptr)
		return Surface(n64OverlaySurface);
	return out;
}
void N64FreePanelLayer()
{
	N64FreeFloorSave();
	N64FreeOverlay();
	n64OverlayInHeap = false;
	if (n64PanelSurface != nullptr) {
		n64_panel_attach(nullptr, 0, 0, 0);
		SDL_FreeSurface(n64PanelSurface);
		n64PanelSurface = nullptr;
	}
}
#endif

#ifdef __N64__
bool N64TownerHasQuest(const Towner &towner); // towners.cpp, v0.9.0
#endif
enum OutlineColors : uint8_t {
	OutlineColorsPlayer1 = (PAL16_ORANGE + 7),
	OutlineColorsPlayer2 = (PAL16_YELLOW + 7),
	OutlineColorsPlayer3 = (PAL16_RED + 7),
	OutlineColorsPlayer4 = (PAL16_BLUE + 7),
	OutlineColorsObject = (PAL16_YELLOW + 2),
	OutlineColorsTowner = (PAL16_BEIGE + 6),
	OutlineColorsMonster = (PAL16_RED + 9),
};

bool AutoMapShowItems;

// DevilutionX extension.
extern void DrawControllerModifierHints(const Surface &out);

bool frameflag;

namespace {

constexpr auto RightFrameDisplacement = Displacement { DunFrameWidth, 0 };

[[nodiscard]] DVL_ALWAYS_INLINE bool IsFloor(Point tilePosition)
{
	return !TileHasAny(tilePosition, TileProperties::Solid | TileProperties::BlockMissile);
}

[[nodiscard]] DVL_ALWAYS_INLINE bool IsWall(Point tilePosition)
{
	return !IsFloor(tilePosition) || dSpecial[tilePosition.x][tilePosition.y] != 0;
}

/**
 * @brief Contains all Missile at rendering position
 */
ankerl::unordered_dense::map<WorldTilePosition, std::vector<Missile *>> MissilesAtRenderingTile;

/**
 * @brief Could the missile (at the next game tick) collide? This method is a simplified version of CheckMissileCol (for example without random).
 */
bool CouldMissileCollide(Point tile, bool checkPlayerAndMonster)
{
	if (!InDungeonBounds(tile))
		return true;
	if (checkPlayerAndMonster) {
		if (dMonster[tile.x][tile.y] > 0)
			return true;
		if (dPlayer[tile.x][tile.y] > 0)
			return true;
	}

	return IsMissileBlockedByTile(tile);
}

void UpdateMissilePositionForRendering(Missile &m, int progress)
{
	DisplacementOf<int64_t> velocity = m.position.velocity;
	velocity *= progress;
	velocity /= AnimationInfo::baseValueFraction;
	const Displacement pixelsTravelled = (m.position.traveled + Displacement { static_cast<int>(velocity.deltaX), static_cast<int>(velocity.deltaY) }) >> 16;
	const Displacement tileOffset = pixelsTravelled.screenToMissile();

	// calculate the future missile position
	m.position.tileForRendering = m.position.start + tileOffset;
	m.position.offsetForRendering = pixelsTravelled + tileOffset.worldToScreen();
}

void UpdateMissileRendererData(Missile &m)
{
	m.position.tileForRendering = m.position.tile;
	m.position.offsetForRendering = m.position.offset;

	const MissileMovementDistribution missileMovement = GetMissileData(m._mitype).movementDistribution;
	// don't calculate missile position if they don't move
	if (missileMovement == MissileMovementDistribution::Disabled || m.position.velocity == Displacement {})
		return;

	int progress = ProgressToNextGameTick;
	UpdateMissilePositionForRendering(m, progress);

	// In some cases this calculated position is invalid.
	// For example a missile shouldn't move inside a wall.
	// In this case the game logic don't advance the missile position and removes the missile or shows an explosion animation at the old position.
	// For the animation distribution logic this means we are not allowed to move to a tile where the missile could collide, because this could be a invalid position.

	// If we are still at the current tile, this tile was already checked and is a valid tile
	if (m.position.tileForRendering == m.position.tile)
		return;

	// If no collision can happen at the new tile we can advance
	if (!CouldMissileCollide(m.position.tileForRendering, missileMovement == MissileMovementDistribution::Blockable))
		return;

	// The new tile could be invalid, so don't advance to it.
	// We search the last offset that is in the old (valid) tile.
	// Implementation note: If someone knows the correct math to calculate this without the loop, I would really appreciate it.
	while (m.position.tile != m.position.tileForRendering) {
		progress -= 1;

		if (progress <= 0) {
			m.position.tileForRendering = m.position.tile;
			m.position.offsetForRendering = m.position.offset;
			return;
		}

		UpdateMissilePositionForRendering(m, progress);
	}
}

void UpdateMissilesRendererData()
{
	MissilesAtRenderingTile.clear();

	for (auto &m : Missiles) {
		UpdateMissileRendererData(m);
		MissilesAtRenderingTile[m.position.tileForRendering].push_back(&m);
	}
}

uint32_t lastFpsUpdateInMs;

Rectangle PrevCursorRect;

void BlitCursor(uint8_t *dst, uint32_t dstPitch, uint8_t *src, uint32_t srcPitch, uint32_t srcWidth, uint32_t srcHeight)
{
	for (std::uint32_t i = 0; i < srcHeight; ++i, src += srcPitch, dst += dstPitch) {
		memcpy(dst, src, srcWidth);
	}
}

/**
 * @brief Remove the cursor from the buffer
 */
void UndrawCursor(const Surface &out)
{
	DrawnCursor &cursor = GetDrawnCursor();
	BlitCursor(&out[cursor.rect.position], out.pitch(), cursor.behindBuffer, cursor.rect.size.width, cursor.rect.size.width, cursor.rect.size.height);
	PrevCursorRect = cursor.rect;
}

bool ShouldShowCursor()
{
	if (ControlMode == ControlTypes::KeyboardAndMouse)
		return true;
	if (pcurs == CURSOR_TELEPORT)
		return true;
	if (invflag)
		return true;
	if (CharFlag && MyPlayer->_pStatPts > 0)
		return true;

	return false;
}

/**
 * @brief Blit CL2 sprite, and apply lighting, to the given buffer at the given coordinates
 * @param out Output buffer
 * @param position Target buffer coordinate
 * @param clx CLX frame
 */
inline void ClxDrawLight(const Surface &out, Point position, ClxSprite clx, int lightTableIndex)
{
	if (lightTableIndex != 0) {
		ClxDrawTRN(out, position, clx, LightTables[lightTableIndex].data());
	} else {
		ClxDraw(out, position, clx);
	}
}

/**
 * @brief Blit CL2 sprite, and apply lighting and transparency blending, to the given buffer at the given coordinates
 * @param out Output buffer
 * @param position Target buffer coordinate
 * @param clx CLX frame
 */
inline void ClxDrawLightBlended(const Surface &out, Point position, ClxSprite clx, int lightTableIndex)
{
	if (lightTableIndex != 0) {
		ClxDrawBlendedTRN(out, position, clx, LightTables[lightTableIndex].data());
	} else {
		ClxDrawBlended(out, position, clx);
	}
}

/**
 * @brief Save the content behind the cursor to a temporary buffer, then draw the cursor.
 */
void DrawCursor(const Surface &out)
{
	DrawnCursor &cursor = GetDrawnCursor();
	if (IsHardwareCursor()) {
		SetHardwareCursorVisible(ShouldShowCursor());
		cursor.rect.size = { 0, 0 };
		return;
	}

	if (pcurs <= CURSOR_NONE || !ShouldShowCursor()) {
		cursor.rect.size = { 0, 0 };
		return;
	}

	const Size cursSize = GetInvItemSize(pcurs);
	if (cursSize.width == 0 || cursSize.height == 0) {
		cursor.rect.size = { 0, 0 };
		return;
	}

	constexpr auto Clip = [](int &pos, int &length, int posEnd) {
		if (pos + length <= 0 || pos >= posEnd) {
			pos = 0;
			length = 0;
		} else if (pos < 0) {
			length += pos;
			pos = 0;
		} else if (pos + length > posEnd) {
			length = posEnd - pos;
		}
	};

	// Copy the buffer before the item cursor and its 1px outline are drawn to a temporary buffer.
	const int outlineWidth = !MyPlayer->HoldItem.isEmpty() ? 1 : 0;
	const Displacement offset = !MyPlayer->HoldItem.isEmpty() ? Displacement { cursSize / 2 } : Displacement { 0 };
	const Point cursPosition = MousePosition - offset;

	Rectangle &rect = cursor.rect;
	rect.position.x = cursPosition.x - outlineWidth;
	rect.size.width = cursSize.width + 2 * outlineWidth;
	Clip(rect.position.x, rect.size.width, out.w());

	rect.position.y = cursPosition.y - outlineWidth;
	rect.size.height = cursSize.height + 2 * outlineWidth;
	Clip(rect.position.y, rect.size.height, out.h());

	if (rect.size.width == 0 || rect.size.height == 0)
		return;

	BlitCursor(cursor.behindBuffer, rect.size.width, &out[rect.position], out.pitch(), rect.size.width, rect.size.height);
	DrawSoftwareCursor(out, cursPosition + Displacement { 0, cursSize.height - 1 }, pcurs);
}

/**
 * @brief Render a missile sprite
 * @param out Output buffer
 * @param missile Pointer to Missile struct
 * @param targetBufferPosition Output buffer coordinate
 * @param pre Is the sprite in the background
 */
void DrawMissilePrivate(const Surface &out, const Missile &missile, Point targetBufferPosition, bool pre, int lightTableIndex)
{
	if (missile._miPreFlag != pre || !missile._miDrawFlag)
		return;

	const Point missileRenderPosition { targetBufferPosition + missile.position.offsetForRendering - Displacement { missile._miAnimWidth2, 0 } };
	const ClxSprite sprite = (*missile._miAnimData)[missile._miAnimFrame - 1];
	if (missile._miUniqTrans != 0) {
		ClxDrawTRN(out, missileRenderPosition, sprite, Monsters[missile._misource].uniqueMonsterTRN.get());
	} else if (missile._miLightFlag) {
		ClxDrawLight(out, missileRenderPosition, sprite, lightTableIndex);
	} else {
		ClxDraw(out, missileRenderPosition, sprite);
	}
}

/**
 * @brief Render a missile sprites for a given tile
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawMissile(const Surface &out, WorldTilePosition tilePosition, Point targetBufferPosition, bool pre, int lightTableIndex)
{
	const auto it = MissilesAtRenderingTile.find(tilePosition);
	if (it == MissilesAtRenderingTile.end()) return;
	for (Missile *missile : it->second) {
		DrawMissilePrivate(out, *missile, targetBufferPosition, pre, lightTableIndex);
	}
}

/**
 * @brief Render a monster sprite
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param monster Monster reference
 */
void DrawMonster(const Surface &out, Point tilePosition, Point targetBufferPosition, const Monster &monster, int lightTableIndex)
{
	if (!monster.animInfo.sprites) {
		Log("Draw Monster \"{}\": NULL Cel Buffer", monster.name());
		return;
	}

	const ClxSprite sprite = monster.animInfo.currentSprite();

	if (!IsTileLit(tilePosition)) {
		ClxDrawTRN(out, targetBufferPosition, sprite, GetInfravisionTRN());
		return;
	}
	uint8_t *trn = nullptr;
	if (monster.isUnique())
		trn = monster.uniqueMonsterTRN.get();
	if (monster.mode == MonsterMode::Petrified)
		trn = GetStoneTRN();
	if (MyPlayer->_pInfraFlag && lightTableIndex > 8)
		trn = GetInfravisionTRN();
	if (trn != nullptr)
		ClxDrawTRN(out, targetBufferPosition, sprite, trn);
	else
		ClxDrawLight(out, targetBufferPosition, sprite, lightTableIndex);
}

/**
 * @brief Helper for rendering a specific player icon (Mana Shield or Reflect)
 */
void DrawPlayerIconHelper(const Surface &out, MissileGraphicID missileGraphicId, Point position, const Player &player, bool infraVision, int lightTableIndex)
{
	const bool lighting = &player != MyPlayer;

	if (player.isWalking())
		position += GetOffsetForWalking(player.AnimInfo, player._pdir);

	position.x -= GetMissileSpriteData(missileGraphicId).animWidth2;
#ifdef __N64__
	(void)GetMissileSpriteData(missileGraphicId).LoadGFX(); // carga bajo demanda
#endif

	const ClxSprite sprite = (*GetMissileSpriteData(missileGraphicId).sprites).list()[0];

	if (!lighting) {
		ClxDraw(out, position, sprite);
		return;
	}

	if (infraVision) {
		ClxDrawTRN(out, position, sprite, GetInfravisionTRN());
		return;
	}

	ClxDrawLight(out, position, sprite, lightTableIndex);
}

/**
 * @brief Helper for rendering player icons (Mana Shield and Reflect)
 * @param out Output buffer
 * @param player Player reference
 * @param position Output buffer coordinates
 * @param infraVision Should infravision be applied
 */
void DrawPlayerIcons(const Surface &out, const Player &player, Point position, bool infraVision, int lightTableIndex)
{
	if (player.pManaShield)
		DrawPlayerIconHelper(out, MissileGraphicID::ManaShield, position, player, infraVision, lightTableIndex);
	if (player.wReflections > 0)
		DrawPlayerIconHelper(out, MissileGraphicID::Reflect, position + Displacement { 0, 16 }, player, infraVision, lightTableIndex);
}

uint8_t GetPlayerOutlineColor(int id)
{
	static constexpr uint8_t PlayerOutlineColors[] = {
		OutlineColorsPlayer1,
		OutlineColorsPlayer2,
		OutlineColorsPlayer3,
		OutlineColorsPlayer4,
	};

	if (id < 0 || id >= static_cast<int>(SDL_arraysize(PlayerOutlineColors)))
		return OutlineColorsPlayer1;

	return PlayerOutlineColors[id];
}

/**
 * @brief Render a player sprite
 * @param out Output buffer
 * @param player Player reference
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawPlayer(const Surface &out, const Player &player, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	if (!IsTileLit(tilePosition) && !MyPlayer->_pInfraFlag && !MyPlayer->isOnArenaLevel() && leveltype != DTYPE_TOWN) {
		return;
	}

	const ClxSprite sprite = player.currentSprite();
	const Point spriteBufferPosition = targetBufferPosition + player.getRenderingOffset(sprite);

	if (&player == PlayerUnderCursor)
		ClxDrawOutlineSkipColorZero(out, GetPlayerOutlineColor(player.getId()), spriteBufferPosition, sprite);

	if (&player == MyPlayer && IsNoneOf(leveltype, DTYPE_NEST, DTYPE_CRYPT)) {
		ClxDraw(out, spriteBufferPosition, sprite);
		DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/false, lightTableIndex);
		return;
	}

	if (!IsTileLit(tilePosition) || ((MyPlayer->_pInfraFlag || MyPlayer->isOnArenaLevel()) && lightTableIndex > 8)) {
		ClxDrawTRN(out, spriteBufferPosition, sprite, GetInfravisionTRN());
		DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/true, lightTableIndex);
		return;
	}

	lightTableIndex = std::max(lightTableIndex - 5, 0);
	ClxDrawLight(out, spriteBufferPosition, sprite, lightTableIndex);
	DrawPlayerIcons(out, player, targetBufferPosition, /*infraVision=*/false, lightTableIndex);
}

/**
 * @brief Render a player sprite
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawDeadPlayer(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	dFlags[tilePosition.x][tilePosition.y] &= ~DungeonFlag::DeadPlayer;

	for (const Player &player : Players) {
		if (player.plractive && player.hasNoLife() && player.isOnActiveLevel() && player.position.tile == tilePosition) {
			dFlags[tilePosition.x][tilePosition.y] |= DungeonFlag::DeadPlayer;
			const Point playerRenderPosition { targetBufferPosition };
			DrawPlayer(out, player, tilePosition, playerRenderPosition, lightTableIndex);
		}
	}
}

/**
 * @brief Render an object sprite
 * @param out Output buffer
 * @param objectToDraw Dungeone object to draw
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawObject(const Surface &out, const Object &objectToDraw, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	const ClxSprite sprite = objectToDraw.currentSprite();

	const Point screenPosition = targetBufferPosition + objectToDraw.getRenderingOffset(sprite, tilePosition);

	if (&objectToDraw == ObjectUnderCursor) {
		ClxDrawOutlineSkipColorZero(out, OutlineColorsObject, screenPosition, sprite);
	}
	if (objectToDraw.applyLighting) {
		ClxDrawLight(out, screenPosition, sprite, lightTableIndex);
	} else {
		ClxDraw(out, screenPosition, sprite);
	}
}

static void DrawDungeon(const Surface & /*out*/, const Lightmap & /*lightmap*/, Point /*tilePosition*/, Point /*targetBufferPosition*/);

/**
 * @brief Render a cell
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 */
void DrawCell(const Surface &out, const Lightmap lightmap, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	const uint16_t levelPieceId = dPiece[tilePosition.x][tilePosition.y];
	if (levelPieceId >= MAXTILES) return;
	const MICROS *pMap = &DPieceMicros[levelPieceId];

	const uint8_t *tbl = LightTables[lightTableIndex].data();
	const uint8_t *foliageTbl = tbl;
#ifdef _DEBUG
	int walkpathIdx = -1;
	Point originalTargetBufferPosition;
	if (DebugPath) {
		walkpathIdx = MyPlayer->GetPositionPathIndex(tilePosition);
		if (walkpathIdx != -1) {
			originalTargetBufferPosition = targetBufferPosition;
			tbl = GetPauseTRN();
		}
	}
#endif

	bool transparency = TileHasAny(tilePosition, TileProperties::Transparent) && TransList[dTransVal[tilePosition.x][tilePosition.y]];
#ifdef _DEBUG
	if ((SDL_GetModState() & SDL_KMOD_ALT) != 0) {
		transparency = false;
	}
#endif

	const auto getFirstTileMaskLeft = [=](TileType tile) -> MaskType {
		if (transparency) {
			switch (tile) {
			case TileType::LeftTrapezoid:
			case TileType::TransparentSquare:
				return TileHasAny(tilePosition, TileProperties::TransparentLeft)
				    ? MaskType::Left
				    : MaskType::Solid;
			case TileType::LeftTriangle:
				return MaskType::Solid;
			default:
				return MaskType::Transparent;
			}
		}
		return MaskType::Solid;
	};

	const auto getFirstTileMaskRight = [=](TileType tile) -> MaskType {
		if (transparency) {
			switch (tile) {
			case TileType::RightTrapezoid:
			case TileType::TransparentSquare:
				return TileHasAny(tilePosition, TileProperties::TransparentRight)
				    ? MaskType::Right
				    : MaskType::Solid;
			case TileType::RightTriangle:
				return MaskType::Solid;
			default:
				return MaskType::Transparent;
			}
		}
		return MaskType::Solid;
	};

	// Create a special lightmap buffer to bleed light up walls
	uint8_t lightmapBuffer[TILE_WIDTH * TILE_HEIGHT];
	const Lightmap bleedLightmap = Lightmap::bleedUp(*GetOptions().Graphics.perPixelLighting, lightmap, targetBufferPosition, lightmapBuffer);

	// If the first micro tile is a floor tile, it may be followed
	// by foliage which should be rendered now.
	const bool isFloor = IsFloor(tilePosition);
	if (const LevelCelBlock levelCelBlock { pMap->mt[0] }; levelCelBlock.hasValue()) {
		const TileType tileType = levelCelBlock.type();
		if (!isFloor || tileType == TileType::TransparentSquare) {
			if (isFloor && tileType == TileType::TransparentSquare) {
				RenderTileFoliage(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock, foliageTbl);
			} else {
				RenderTile(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock, getFirstTileMaskLeft(tileType), tbl);
			}
		}
	}
	if (const LevelCelBlock levelCelBlock { pMap->mt[1] }; levelCelBlock.hasValue()) {
		const TileType tileType = levelCelBlock.type();
		if (!isFloor || tileType == TileType::TransparentSquare) {
			if (isFloor && tileType == TileType::TransparentSquare) {
				RenderTileFoliage(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock, foliageTbl);
			} else {
				RenderTile(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock, getFirstTileMaskRight(tileType), tbl);
			}
		}
	}
	targetBufferPosition.y -= TILE_HEIGHT;

	for (uint_fast8_t i = 2, n = MicroTileLen; i < n; i += 2) {
		{
			const LevelCelBlock levelCelBlock { pMap->mt[i] };
			if (levelCelBlock.hasValue()) {
				RenderTile(out, bleedLightmap, targetBufferPosition,
				    pDungeonCels.get(), levelCelBlock,
				    transparency ? MaskType::Transparent : MaskType::Solid, foliageTbl);
			}
		}
		{
			const LevelCelBlock levelCelBlock { pMap->mt[i + 1] };
			if (levelCelBlock.hasValue()) {
				RenderTile(out, bleedLightmap, targetBufferPosition + RightFrameDisplacement,
				    pDungeonCels.get(), levelCelBlock,
				    transparency ? MaskType::Transparent : MaskType::Solid, foliageTbl);
			}
		}
		targetBufferPosition.y -= TILE_HEIGHT;
	}

#ifdef _DEBUG
	if (DebugPath && walkpathIdx != -1) {
		DrawString(out, StrCat(walkpathIdx),
		    Rectangle(originalTargetBufferPosition + Displacement { 0, -TILE_HEIGHT }, Size { TILE_WIDTH, TILE_HEIGHT }),
		    TextRenderOptions {
		        .flags = UiFlags::AlignCenter | UiFlags::VerticalCenter
		            | (IsTileSolid(tilePosition) ? UiFlags::ColorYellow : UiFlags::ColorWhite) });
	}
#endif
}

/**
 * @brief Render a floor tile.
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinate
 */
void DrawFloorTile(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	const int lightTableIndex = dLight[tilePosition.x][tilePosition.y];

	const uint8_t *tbl = LightTables[lightTableIndex].data();
#ifdef _DEBUG
	if (DebugPath && MyPlayer->GetPositionPathIndex(tilePosition) != -1)
		tbl = GetPauseTRN();
#endif

	const uint16_t levelPieceId = dPiece[tilePosition.x][tilePosition.y];
	if (levelPieceId >= MAXTILES) return;
	{
		const LevelCelBlock levelCelBlock { DPieceMicros[levelPieceId].mt[0] };
		if (levelCelBlock.hasValue()) {
			RenderTileFrame(out, lightmap, targetBufferPosition, TileType::LeftTriangle,
			    GetDunFrame(pDungeonCels.get(), levelCelBlock.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
		}
	}
	{
		const LevelCelBlock levelCelBlock { DPieceMicros[levelPieceId].mt[1] };
		if (levelCelBlock.hasValue()) {
			RenderTileFrame(out, lightmap, targetBufferPosition + RightFrameDisplacement, TileType::RightTriangle,
			    GetDunFrame(pDungeonCels.get(), levelCelBlock.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
		}
	}
}

/**
 * @brief Draw item for a given tile
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 * @param pre Is the sprite in the background
 */
void DrawItem(const Surface &out, int8_t itemIndex, Point targetBufferPosition, int lightTableIndex)
{
	const Item &item = Items[itemIndex];
	const ClxSprite sprite = item.AnimInfo.currentSprite();
	const Point position = targetBufferPosition + item.getRenderingOffset(sprite);
	if (!IsPlayerInStore() && (itemIndex == pcursitem || AutoMapShowItems)) {
		ClxDrawOutlineSkipColorZero(out, GetOutlineColor(item, false), position, sprite);
	}
	ClxDrawLight(out, position, sprite, lightTableIndex);
	if (item.AnimInfo.isLastFrame() || item._iCurs == ICURS_MAGIC_ROCK)
		AddItemToLabelQueue(itemIndex, position);
}

/**
 * @brief Check if and how a monster should be rendered
 * @param out Output buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Output buffer coordinates
 */
void DrawMonsterHelper(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	int mi = dMonster[tilePosition.x][tilePosition.y];

	mi = std::abs(mi) - 1;

	if (leveltype == DTYPE_TOWN) {
		auto &towner = Towners[mi];
		const Point position = targetBufferPosition + towner.getRenderingOffset();
		const ClxSprite sprite = towner.currentSprite();
		if (mi == pcursmonst) {
			ClxDrawOutlineSkipColorZero(out, OutlineColorsTowner, position, sprite);
		}
		ClxDraw(out, position, sprite);
#ifdef __N64__
		if (N64TownerHasQuest(towner)) { // v0.9.0: "!" de mision, como en Hellspawn
			DrawString(out, "!", Rectangle { Point { position.x, position.y - sprite.height() - 6 }, Size { sprite.width(), 26 } },
			    { .flags = UiFlags::AlignCenter | UiFlags::ColorGold | UiFlags::FontSize24 });
		}
#endif
		return;
	}

	if (!IsTileLit(tilePosition) && !(MyPlayer->_pInfraFlag && IsFloor(tilePosition))) {
		return;
	}

	if (static_cast<size_t>(mi) >= MaxMonsters) {
		Log("Draw Monster: tried to draw illegal monster {}", mi);
		return;
	}

	const auto &monster = Monsters[mi];
	if ((monster.flags & MFLAG_HIDDEN) != 0) {
		return;
	}

	const ClxSprite sprite = monster.animInfo.currentSprite();
	const Displacement offset = monster.getRenderingOffset(sprite);

	const Point monsterRenderPosition = targetBufferPosition + offset;
	if (mi == pcursmonst) {
		ClxDrawOutlineSkipColorZero(out, OutlineColorsMonster, monsterRenderPosition, sprite);
	}
	DrawMonster(out, tilePosition, monsterRenderPosition, monster, lightTableIndex);
}

/**
 * @brief Render object sprites
 * @param out Target buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 */
void DrawDungeon(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	assert(InDungeonBounds(tilePosition));
	const int lightTableIndex = dLight[tilePosition.x][tilePosition.y];

#ifdef __N64__
	const unsigned n64C0 = n64_us();
	DrawCell(out, lightmap, tilePosition, targetBufferPosition, lightTableIndex);
	n64_prof_add(29, n64_us() - n64C0); // f2v48: paredes (piezas de la casilla)
#else
	DrawCell(out, lightmap, tilePosition, targetBufferPosition, lightTableIndex);
#endif

	const int8_t bDead = dCorpse[tilePosition.x][tilePosition.y];
	const int8_t bMap = dTransVal[tilePosition.x][tilePosition.y];

#ifdef _DEBUG
	if (DebugVision && IsTileLit(tilePosition)) {
		ClxDraw(out, targetBufferPosition, (*pSquareCel)[0]);
	}
#endif

	if (MissilePreFlag) {
		DrawMissile(out, tilePosition, targetBufferPosition, true, lightTableIndex);
	}

	if (lightTableIndex < LightsMax && bDead != 0) {
		const Corpse &corpse = Corpses[(bDead & 0x1F) - 1];
		const Point position { targetBufferPosition.x - CalculateSpriteTileCenterX(corpse.width), targetBufferPosition.y };
		const ClxSprite sprite = corpse.spritesForDirection(static_cast<Direction>((bDead >> 5) & 7))[corpse.frame];
		if (corpse.translationPaletteIndex != 0) {
			const uint8_t *trn = Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get();
			ClxDrawTRN(out, position, sprite, trn);
		} else {
			ClxDrawLight(out, position, sprite, lightTableIndex);
		}
	}

	const int8_t bItem = dItem[tilePosition.x][tilePosition.y];
	const Object *object = lightTableIndex < LightsMax
	    ? FindObjectAtPosition(tilePosition)
	    : nullptr;
	if (object != nullptr && object->_oPreFlag) {
		DrawObject(out, *object, tilePosition, targetBufferPosition, lightTableIndex);
	}
	if (bItem > 0 && !Items[bItem - 1]._iPostDraw) {
		DrawItem(out, static_cast<int8_t>(bItem - 1), targetBufferPosition, lightTableIndex);
	}

	if (TileContainsDeadPlayer(tilePosition)) {
		DrawDeadPlayer(out, tilePosition, targetBufferPosition, lightTableIndex);
	}
	Player *player = PlayerAtPosition(tilePosition);
	if (player != nullptr) {
		const uint8_t pid = player->getId();
		assert(pid < MAX_PLRS);
		int playerId = static_cast<int>(pid) + 1;
		// If sprite is moving southwards or east, we want to draw it offset from the tile it's moving to, so we need negative ID
		// This respests the order that tiles are drawn. By using the negative id, we ensure that the sprite is drawn with priority
		if (player->_pmode == PM_WALK_SOUTHWARDS || (player->_pmode == PM_WALK_SIDEWAYS && player->_pdir == Direction::East))
			playerId = -playerId;
		if (dPlayer[tilePosition.x][tilePosition.y] == playerId) {
			auto tempTilePosition = tilePosition;
			auto tempTargetBufferPosition = targetBufferPosition;

			// Offset the sprite to the tile it's moving from
			if (player->_pmode == PM_WALK_SOUTHWARDS) {
				switch (player->_pdir) {
				case Direction::SouthWest:
					tempTargetBufferPosition += { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				case Direction::South:
					tempTargetBufferPosition += { 0, -TILE_HEIGHT };
					break;
				case Direction::SouthEast:
					tempTargetBufferPosition += { -TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				default:
					DVL_UNREACHABLE();
				}
				tempTilePosition += Opposite(player->_pdir);
			} else if (player->_pmode == PM_WALK_SIDEWAYS && player->_pdir == Direction::East) {
				tempTargetBufferPosition += { -TILE_WIDTH, 0 };
				tempTilePosition += Opposite(player->_pdir);
			}
			DrawPlayer(out, *player, tempTilePosition, tempTargetBufferPosition, lightTableIndex);
		}
	}

	Monster *monster = FindMonsterAtPosition(tilePosition);
	if (monster != nullptr) {
		auto mid = monster->getId();
		assert(mid < MaxMonsters);
		int monsterId = static_cast<int>(mid) + 1;
		// If sprite is moving southwards or east, we want to draw it offset from the tile it's moving to, so we need negative ID
		// This respests the order that tiles are drawn. By using the negative id, we ensure that the sprite is drawn with priority
		if (monster->mode == MonsterMode::MoveSouthwards || (monster->mode == MonsterMode::MoveSideways && monster->direction == Direction::East))
			monsterId = -monsterId;
		if (dMonster[tilePosition.x][tilePosition.y] == monsterId) {
			auto tempTilePosition = tilePosition;
			auto tempTargetBufferPosition = targetBufferPosition;

			// Offset the sprite to the tile it's moving from
			if (monster->mode == MonsterMode::MoveSouthwards) {
				switch (monster->direction) {
				case Direction::SouthWest:
					tempTargetBufferPosition += { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				case Direction::South:
					tempTargetBufferPosition += { 0, -TILE_HEIGHT };
					break;
				case Direction::SouthEast:
					tempTargetBufferPosition += { -TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
					break;
				default:
					DVL_UNREACHABLE();
				}
				tempTilePosition += Opposite(monster->direction);
			} else if (monster->mode == MonsterMode::MoveSideways && monster->direction == Direction::East) {
				tempTargetBufferPosition += { -TILE_WIDTH, 0 };
				tempTilePosition += Opposite(monster->direction);
			}
			DrawMonsterHelper(out, tempTilePosition, tempTargetBufferPosition, lightTableIndex);
		}
	}

	DrawMissile(out, tilePosition, targetBufferPosition, false, lightTableIndex);

	if (object != nullptr && !object->_oPreFlag) {
		DrawObject(out, *object, tilePosition, targetBufferPosition, lightTableIndex);
	}
	if (bItem > 0 && Items[bItem - 1]._iPostDraw) {
		DrawItem(out, static_cast<int8_t>(bItem - 1), targetBufferPosition, lightTableIndex);
	}

	if (leveltype != DTYPE_TOWN) {
		const bool perPixelLighting = *GetOptions().Graphics.perPixelLighting;
		const int8_t bArch = dSpecial[tilePosition.x][tilePosition.y] - 1;
		if (bArch >= 0) {
			bool transparency = TransList[bMap];
#ifdef _DEBUG
			// Turn transparency off here for debugging
			transparency = transparency && (SDL_GetModState() & SDL_KMOD_ALT) == 0;
#endif
			if (perPixelLighting) {
				// Create a special lightmap buffer to bleed light up walls
				uint8_t lightmapBuffer[TILE_WIDTH * TILE_HEIGHT];
				const Lightmap bleedLightmap = Lightmap::bleedUp(*GetOptions().Graphics.perPixelLighting, lightmap, targetBufferPosition, lightmapBuffer);

				if (transparency)
					ClxDrawBlendedWithLightmap(out, targetBufferPosition, (*pSpecialCels)[bArch], bleedLightmap);
				else
					ClxDrawWithLightmap(out, targetBufferPosition, (*pSpecialCels)[bArch], bleedLightmap);
			} else if (transparency) {
				ClxDrawLightBlended(out, targetBufferPosition, (*pSpecialCels)[bArch], lightTableIndex);
			} else {
				ClxDrawLight(out, targetBufferPosition, (*pSpecialCels)[bArch], lightTableIndex);
			}
		}
	} else {
		// Tree leaves should always cover player when entering or leaving the tile,
		// So delay the rendering until after the next row is being drawn.
		// This could probably have been better solved by sprites in screen space.
		if (tilePosition.x > 0 && tilePosition.y > 0 && targetBufferPosition.y > TILE_HEIGHT) {
			const int8_t bArch = dSpecial[tilePosition.x - 1][tilePosition.y - 1] - 1;
			if (bArch >= 0)
				ClxDraw(out, targetBufferPosition + Displacement { 0, -TILE_HEIGHT }, (*pSpecialCels)[bArch]);
		}
	}
}

/**
 * @brief Render a row of tiles
 * @param out Buffer to render to
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawFloor(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	for (int i = 0; i < rows; i++) {
		for (int j = 0; j < columns; j++, tilePosition += Direction::East, targetBufferPosition.x += TILE_WIDTH) {
			if (!InDungeonBounds(tilePosition))
				continue;
			if (IsFloor(tilePosition)) {
				DrawFloorTile(out, lightmap, tilePosition, targetBufferPosition);
			}
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

/**
 * @brief Renders the floor tiles
 * @param out Output buffer
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawTileContent(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	// Keep evaluating until MicroTiles can't affect screen
	rows += MicroTileLen;

#ifdef _DEBUG
	DebugCoordsMap.reserve(rows * columns);
#endif

	for (int i = 0; i < rows; i++) {
		bool skip = false;
		for (int j = 0; j < columns; j++) {
			if (InDungeonBounds(tilePosition)) {
				bool skipNext = false;
#ifdef _DEBUG
				DebugCoordsMap[tilePosition.x + (tilePosition.y * MAXDUNX)] = targetBufferPosition;
#endif
				if (tilePosition.x + 1 < MAXDUNX && tilePosition.y - 1 >= 0 && targetBufferPosition.x + TILE_WIDTH <= gnScreenWidth) {
					// Render objects behind walls first to prevent sprites, that are moving
					// between tiles, from poking through the walls as they exceed the tile bounds.
					// A proper fix for this would probably be to layout the scene and render by
					// sprite screen position rather than tile position.
					if (IsWall(tilePosition) && (IsWall(tilePosition + Displacement { 1, 0 }) || (tilePosition.x > 0 && IsWall(tilePosition + Displacement { -1, 0 })))) { // Part of a wall aligned on the x-axis
						if (IsTileNotSolid(tilePosition + Displacement { 1, -1 }) && IsTileNotSolid(tilePosition + Displacement { 0, -1 })) {                              // Has walkable area behind it
							DrawDungeon(out, lightmap, tilePosition + Direction::East, { targetBufferPosition.x + TILE_WIDTH, targetBufferPosition.y });
							skipNext = true;
						}
					}
				}
				if (!skip) {
					DrawDungeon(out, lightmap, tilePosition, targetBufferPosition);
				}
				skip = skipNext;
			}
			tilePosition += Direction::East;
			targetBufferPosition.x += TILE_WIDTH;
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

void DrawDirtTile(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition)
{
	// This should be the *top-left* of the 2×2 dirt pattern in the actual dungeon.
	// You might need to tweak these to where your dirt patch actually lives.
	constexpr Point base { 0, 0 };

	// Decide which of the 4 tiles of the 2×2 block to use,
	// based on where this OOB tile is in the world grid.
	const int ox = (tilePosition.x & 1); // 0 or 1
	const int oy = (tilePosition.y & 1); // 0 or 1

	Point sample {
		base.x + ox,
		base.y + oy,
	};

	// Safety: clamp in case tilePosition is wildly outside and base+offset ever escapes
	sample.x = std::clamp(sample.x, 0, MAXDUNX - 1);
	sample.y = std::clamp(sample.y, 0, MAXDUNY - 1);

	if (!InDungeonBounds(sample) || dPiece[sample.x][sample.y] == 0) {
		// Failsafe: if our sample somehow isn't valid, fall back to black
		world_draw_black_tile(out, targetBufferPosition.x, targetBufferPosition.y);
		return;
	}

	const int lightTableIndex = dLight[sample.x][sample.y];

	// Let the normal dungeon tile renderer compose the full tile
	DrawCell(out, lightmap, sample, targetBufferPosition, lightTableIndex);
}

/**
 * @brief Render a row of tiles
 * @param out Buffer to render to
 * @param lightmap Per-pixel light buffer
 * @param tilePosition dPiece coordinates
 * @param targetBufferPosition Target buffer coordinates
 * @param rows Number of rows
 * @param columns Tile in a row
 */
void DrawOOB(const Surface &out, const Lightmap &lightmap, Point tilePosition, Point targetBufferPosition, int rows, int columns)
{
	for (int i = 0; i < rows + 5; i++) { // 5 extra rows needed to make sure everything gets rendered at the bottom half of the screen
		for (int j = 0; j < columns; j++, tilePosition += Direction::East, targetBufferPosition.x += TILE_WIDTH) {
			if (!InDungeonBounds(tilePosition)) {
				if (leveltype == DTYPE_TOWN) {
					world_draw_black_tile(out, targetBufferPosition.x, targetBufferPosition.y);
				} else {
					DrawDirtTile(out, lightmap, tilePosition, targetBufferPosition);
				}
			}
		}
		// Return to start of row
		tilePosition += Displacement(Direction::West) * columns;
		targetBufferPosition.x -= columns * TILE_WIDTH;

		// Jump to next row
		targetBufferPosition.y += TILE_HEIGHT / 2;
		if ((i & 1) != 0) {
			tilePosition.x++;
			columns--;
			targetBufferPosition.x += TILE_WIDTH / 2;
		} else {
			tilePosition.y++;
			columns++;
			targetBufferPosition.x -= TILE_WIDTH / 2;
		}
	}
}

/**
 * @brief Scale up the top left part of the buffer 2x.
 */
void Zoom(const Surface &out)
{
	int viewportWidth = out.w();
	int viewportOffsetX = 0;
	if (CanPanelsCoverView()) {
		if (IsLeftPanelOpen()) {
			viewportWidth -= SidePanelSize.width;
			viewportOffsetX = SidePanelSize.width;
		} else if (IsRightPanelOpen()) {
			viewportWidth -= SidePanelSize.width;
		}
	}

	// We round to even for the source width and height.
	// If the width / height was odd, we copy just one extra pixel / row later on.
	const int srcWidth = (viewportWidth + 1) / 2;
	const int doubleableWidth = viewportWidth / 2;
	const int srcHeight = (out.h() + 1) / 2;
	const int doubleableHeight = out.h() / 2;

	uint8_t *src = out.at(srcWidth - 1, srcHeight - 1);
	uint8_t *dst = out.at(viewportOffsetX + viewportWidth - 1, out.h() - 1);
	const bool oddViewportWidth = (viewportWidth % 2) == 1;

	for (int hgt = 0; hgt < doubleableHeight; hgt++) {
		// Double the pixels in the line.
		for (int i = 0; i < doubleableWidth; i++) {
			*dst-- = *src;
			*dst-- = *src;
			--src;
		}

		// Copy a single extra pixel if the output width is odd.
		if (oddViewportWidth) {
			*dst-- = *src;
			--src;
		}

		// Skip the rest of the source line.
		src -= (out.pitch() - srcWidth);

		// Double the line.
		memcpy(dst - out.pitch() + 1, dst + 1, viewportWidth);

		// Skip the rest of the destination line.
		dst -= 2 * out.pitch() - viewportWidth;
	}
	if ((out.h() % 2) == 1) {
		memcpy(dst - out.pitch() + 1, dst + 1, viewportWidth);
	}
}

Displacement tileOffset;
Displacement tileShift;
int tileColumns;
int tileRows;

void CalcFirstTilePosition(Point &position, Displacement &offset)
{
	// Adjust by player offset and tile grid alignment
	const Player &myPlayer = *MyPlayer;
	offset = tileOffset;
	if (myPlayer.isWalking()) {
		offset += GetOffsetForWalking(myPlayer.AnimInfo, myPlayer._pdir, true);
#ifdef __N64__
		// f2v72: desplazamiento horizontal siempre par: asi el suelo guardado se puede mover con el
		// RDP en todos los cuadros (los impares obligaban a dibujar el suelo entero). El mundo se
		// mueve a saltos de 2 pixeles en horizontal (0,8 pixeles de la tele).
		offset.deltaX &= ~1;
#endif
	}

	position += tileShift;

	// Skip rendering parts covered by the panels
#ifdef __N64__
	if (N64UiMode && CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
		// f2v55: el panel (320 de 640) tapa la mitad del mundo (gnScreenWidth / 2, no 320):
		// el jugador se corre un cuarto del ancho (64 de la casilla + el resto)
		position += Direction::East;
		offset.deltaX -= gnScreenWidth / 4 - TILE_WIDTH;
		if (IsLeftPanelOpen())
			offset.deltaX += gnScreenWidth / 2;
	} else
#endif
	if (CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
		const int multiplier = (*GetOptions().Graphics.zoom) ? 1 : 2;
		position += Displacement(Direction::East) * multiplier;
		offset.deltaX += -TILE_WIDTH * multiplier / 2 / 2;

		if (IsLeftPanelOpen() && !*GetOptions().Graphics.zoom) {
			offset.deltaX += SidePanelSize.width;
			// SidePanelSize.width accounted for in Zoom()
		}
	}

	// Draw areas moving in and out of the screen
	if (myPlayer.isWalking()) {
		switch (myPlayer._pdir) {
		case Direction::North:
		case Direction::NorthEast:
			offset.deltaY -= TILE_HEIGHT;
			position += Direction::North;
			break;
		case Direction::SouthWest:
		case Direction::West:
			offset.deltaX -= TILE_WIDTH;
			position += Direction::West;
			break;
		case Direction::NorthWest:
			offset.deltaX -= TILE_WIDTH / 2;
			offset.deltaY -= TILE_HEIGHT / 2;
			position += Direction::NorthWest;
		default:
			break;
		}
	}
}

/**
 * @brief Configure render and process screen rows
 * @param fullOut Buffer to render to
 * @param position First tile of view in dPiece coordinate
 * @param offset Amount to offset the rendering in screen space
 */
void DrawGame(const Surface &fullOut, Point position, Displacement offset)
{
	// Limit rendering to the view area
	const Surface &out = !*GetOptions().Graphics.zoom
	    ? fullOut.subregionY(0, gnViewportHeight)
	    : fullOut.subregionY(0, (gnViewportHeight + 1) / 2);

	int columns = tileColumns;
	int rows = tileRows;

	// Skip rendering parts covered by the panels
	if (CanPanelsCoverView() && (IsLeftPanelOpen() || IsRightPanelOpen())) {
#ifdef __N64__
		// f2v55: quitar 4 columnas (256) dejaba franjas negras: el panel solo tapa gnScreenWidth / 2
		columns -= N64UiMode ? 2 : ((*GetOptions().Graphics.zoom) ? 2 : 4);
#else
		columns -= (*GetOptions().Graphics.zoom) ? 2 : 4;
#endif
	}

	UpdateMissilesRendererData();

	// Draw areas moving in and out of the screen
	if (MyPlayer->isWalking()) {
		switch (MyPlayer->_pdir) {
		case Direction::NoDirection:
			break;
		case Direction::North:
		case Direction::South:
			rows += 2;
			break;
		case Direction::NorthEast:
			columns++;
			rows += 2;
			break;
		case Direction::East:
		case Direction::West:
			columns++;
			break;
		case Direction::SouthEast:
		case Direction::SouthWest:
		case Direction::NorthWest:
			columns++;
			rows++;
			break;
		}
	}

#ifdef DUN_RENDER_STATS
	DunRenderStats.clear();
#endif

	Lightmap lightmap = Lightmap::build(*GetOptions().Graphics.perPixelLighting, position, Point {} + offset,
	    gnScreenWidth, gnViewportHeight, rows, columns,
	    out.at(0, 0), out.pitch(), LightTables, FullyLitLightTable, FullyDarkLightTable,
	    dLight, MicroTileLen);

#ifdef __N64__
	const unsigned n64F0 = n64_us();
	{
		// f2v62: suelo guardado que se desplaza. La imagen del suelo del cuadro anterior (solo suelo, lo
		// demas a 0) se mueve con el RDP lo que se movio la camara y solo se dibujan las casillas que
		// entran por el borde, las que cambiaron de luz o de pieza y las que estaban bajo el panel.
		// Quieto es lo mismo que antes (copia entera); al andar ya no se redibuja todo el suelo.
		static uint32_t keyTab[64][64];
		static Point bPos;
		static Displacement bOff;
		static uint32_t bEnv = 0;
		const int fw = out.w();
		const int fh = out.h();
		const bool occOn = N64OccPixels == out.begin();
		uint32_t env = 2166136261U;
		{
			auto mix = [&env](uint32_t v) { env = (env ^ v) * 16777619U; };
			mix(static_cast<uint32_t>(fw) | (static_cast<uint32_t>(fh) << 16));
			mix(occOn ? (static_cast<uint32_t>(N64OccX0) ^ (static_cast<uint32_t>(N64OccY0) << 8) ^ (static_cast<uint32_t>(N64OccX1) << 16) ^ (static_cast<uint32_t>(N64OccY1) << 24)) : 0U);
			mix(static_cast<uint32_t>(currlevel) | (static_cast<uint32_t>(leveltype) << 8) | (static_cast<uint32_t>(setlevel) << 16) | (*GetOptions().Graphics.perPixelLighting ? (1U << 24) : 0U));
		}
		auto tileKey = [](Point t) -> uint32_t {
			uint32_t k = static_cast<uint32_t>(dPiece[t.x][t.y]) & 0xFFFFU;
			k |= static_cast<uint32_t>(dLight[t.x][t.y]) << 16;
			if (!*GetOptions().Graphics.perPixelLighting)
				return k; // f2v63: con luz por casillas solo cuenta la suya
			uint32_t nh = 0; // la luz por pixel mezcla la de las casillas vecinas
			for (int ny = -1; ny <= 1; ny++) {
				for (int nx = -1; nx <= 1; nx++) {
					const Point n { t.x + nx, t.y + ny };
					if ((nx | ny) != 0 && InDungeonBounds(n))
						nh = nh * 31U + dLight[n.x][n.y];
				}
			}
			return k | ((nh ^ (nh >> 8) ^ (nh >> 16)) & 0xFFU) << 24;
		};
		auto forEachTile = [&](auto &&fn) {
			Point tp = position;
			Point sp = Point {} + offset;
			int cols = columns;
			for (int i = 0; i < rows; i++) {
				for (int j = 0; j < cols; j++, tp += Direction::East, sp.x += TILE_WIDTH) {
					if (InDungeonBounds(tp))
						fn(tp, sp);
				}
				tp += Displacement(Direction::West) * cols;
				sp.x -= cols * TILE_WIDTH;
				sp.y += TILE_HEIGHT / 2;
				if ((i & 1) != 0) {
					tp.x++;
					cols--;
					sp.x += TILE_WIDTH / 2;
				} else {
					tp.y++;
					cols++;
					sp.x -= TILE_WIDTH / 2;
				}
			}
		};
		const size_t need = static_cast<size_t>(fw) * static_cast<size_t>(fh);
		bool full = true;
		int sdx = 0;
		int sdy = 0;
		if (N64FloorValid && N64FloorSaveSize == need && env == bEnv) {
			const Displacement sh = (position - bPos).worldToScreen() + (offset - bOff);
			sdx = sh.deltaX;
			sdy = sh.deltaY;
			if (std::abs(sdx) < fw - TILE_WIDTH && std::abs(sdy) < fh - TILE_HEIGHT) {
				if ((sdx & 1) == 0)
					full = false;
				else
					n64_prof_add(34, 1);
			}
		}
		if (!full) {
			const int cw = fw - std::abs(sdx);
			const int ch = fh - std::abs(sdy);
			const int dx0 = std::max(0, sdx);
			const int dy0 = std::max(0, sdy);
			if (n64_rdp_blit8(out.at(0, 0), out.pitch(), fh, dx0, dy0, N64FloorSave.get(), fw, fh, std::max(0, -sdx), std::max(0, -sdy), cw, ch) == 0) {
				full = true;
				n64_prof_add(34, 1);
			} else {
				for (int y = 0; y < fh; y++) {
					uint8_t *row = out.at(0, y);
					if (y < dy0 || y >= dy0 + ch) {
						memset(row, 0, static_cast<size_t>(fw));
					} else {
						if (dx0 > 0)
							memset(row, 0, static_cast<size_t>(dx0));
						if (dx0 + cw < fw)
							memset(row + dx0 + cw, 0, static_cast<size_t>(fw - dx0 - cw));
					}
				}
				unsigned redrawn = 0;
				bool needFull = false;
				forEachTile([&](Point tp, Point sp) {
					uint32_t &slot = keyTab[tp.x & 63][tp.y & 63];
					const uint32_t k = tileKey(tp);
					if (IsFloor(tp)) {
						// f2v63: basta con que la parte visible de la casilla estuviera en la imagen movida
						// (antes se exigia la casilla entera y las del borde se redibujaban siempre)
						const int vx0 = std::max(0, sp.x);
						const int vy0 = std::max(0, sp.y - (TILE_HEIGHT - 1));
						const int vx1 = std::min(fw, sp.x + TILE_WIDTH);
						const int vy1 = std::min(fh, sp.y + 1);
						const int x0 = sp.x - sdx;
						const int y0 = sp.y - (TILE_HEIGHT - 1) - sdy;
						bool ok = slot == k && (vx0 >= vx1 || vy0 >= vy1 || (vx0 >= dx0 && vy0 >= dy0 && vx1 <= dx0 + cw && vy1 <= dy0 + ch));
						if (ok && occOn && x0 < N64OccX1 && x0 + TILE_WIDTH > N64OccX0 && y0 < N64OccY1 && y0 + TILE_HEIGHT > N64OccY0)
							ok = false; // estaba bajo el panel: no se dibujo
						if (!ok) {
							DrawFloorTile(out, lightmap, tp, sp);
							redrawn++;
						}
					} else if ((slot & 0xFFFFU) != (k & 0xFFFFU)) {
						needFull = true; // cambio de pieza (puerta): lo de antes podria quedar
					}
					slot = k;
				});
				if (needFull) {
					full = true;
				} else {
					n64_prof_add(32, 1);
					n64_prof_add(33, redrawn);
					if (redrawn == 0)
						n64_prof_add(27, 1);
					N64FloorReused++;
				}
			}
		}
		if (full) {
			if (n64_rdp_fill8(out.at(0, 0), out.pitch(), fw, fh, 0) == 0) {
				for (int y = 0; y < fh; y++)
					memset(out.at(0, y), 0, static_cast<size_t>(fw));
			}
			DrawFloor(out, lightmap, position, Point {} + offset, rows, columns);
			forEachTile([&](Point tp, Point) { keyTab[tp.x & 63][tp.y & 63] = tileKey(tp); });
			N64FloorDrawn++;
			n64_prof_add(28, 1);
		}
		// se guarda el suelo de este cuadro para el siguiente
		N64FloorValid = false;
		if (N64FloorSaveSize != need) {
			N64FloorSave = nullptr;
			N64FloorSaveSize = 0;
			if (n64_heap_free_kib() > static_cast<int>(need / 1024) + 400) {
				N64FloorSave.reset(static_cast<uint8_t *>(memalign(64, need)));
				if (N64FloorSave != nullptr)
					N64FloorSaveSize = need;
			}
		}
		if (N64FloorSave != nullptr) {
			if (n64_rdp_copy8(N64FloorSave.get(), fw, out.at(0, 0), out.pitch(), fw, fh) == 0) {
				for (int y = 0; y < fh; y++)
					memcpy(&N64FloorSave[static_cast<size_t>(y) * fw], out.at(0, y), static_cast<size_t>(fw));
			}
			bPos = position;
			bOff = offset;
			bEnv = env;
			N64FloorValid = true;
		}
	}
	const unsigned n64F1 = n64_us();
	n64_prof_add(3, n64F1 - n64F0);
	DrawTileContent(out, lightmap, position, Point {} + offset, rows, columns);
	n64_prof_add(4, n64_us() - n64F1);
#else
	DrawFloor(out, lightmap, position, Point {} + offset, rows, columns);
	DrawTileContent(out, lightmap, position, Point {} + offset, rows, columns);
#endif
	DrawOOB(out, lightmap, position, Point {} + offset, rows, columns);

	if (*GetOptions().Graphics.zoom) {
		Zoom(fullOut.subregionY(0, gnViewportHeight));
	}

#ifdef DUN_RENDER_STATS
	std::vector<std::pair<DunRenderType, size_t>> sortedStats(DunRenderStats.begin(), DunRenderStats.end());
	c_sort(sortedStats, [](const std::pair<DunRenderType, size_t> &a, const std::pair<DunRenderType, size_t> &b) {
		return a.first.maskType == b.first.maskType
		    ? static_cast<uint8_t>(a.first.tileType) < static_cast<uint8_t>(b.first.tileType)
		    : static_cast<uint8_t>(a.first.maskType) < static_cast<uint8_t>(b.first.maskType);
	});
	Point pos { 100, 20 };
	for (size_t i = 0; i < sortedStats.size(); ++i) {
		const auto &stat = sortedStats[i];
		DrawString(out, StrCat(i, "."), Rectangle(pos, Size { 20, 16 }), { .flags = UiFlags::AlignRight });
		DrawString(out, MaskTypeToString(stat.first.maskType), { pos.x + 24, pos.y });
		DrawString(out, TileTypeToString(stat.first.tileType), { pos.x + 184, pos.y });
		DrawString(out, FormatInteger(stat.second), Rectangle({ pos.x + 354, pos.y }, Size(40, 16)), { .flags = UiFlags::AlignRight });
		pos.y += 16;
	}
#endif
}

/**
 * @brief Start rendering of screen, town variation
 * @param out Buffer to render to
 * @param startPosition Center of view in dPiece coordinates
 */
#ifdef __N64__
/**
 * f2v55: minimapa siempre a la vista, pequeno, en el hueco de la izquierda que deja el panel
 * (ahora pegado a la derecha). Si el panel es grande y no cabe, arriba a la izquierda.
 * Con el mapa grande abierto o con la interfaz en la capa de 640x480 no se dibuja.
 */
void N64DrawMinimap(const Surface &out)
{
	if (N64UiMode || gmenu_is_active() || N64OccPixels != out.begin() || gnScreenWidth < 200)
		return;
	const int margin = gnScreenWidth / 24; // los televisores cortan los bordes
	const int space = N64OccX0 - 3 - margin - 8;
	Rectangle rect;
	if (space >= 96) {
		const int w = std::min(space, 128);
		const int h = w / 2;
		rect = Rectangle { { margin, gnViewportHeight - h - 6 }, Size { w, h } };
	} else {
		rect = Rectangle { { margin, margin / 2 + 2 }, Size { 112, 56 } };
	}
	// f2v56: dibujarlo costaba ~10 ms por cuadro (MEDIDO en f2v55): se guarda la imagen y solo se
	// vuelve a dibujar al cambiar de casilla, de recuadro o cada 45 cuadros
	static uint8_t cache[(128 + 4) * (64 + 4)];
	static uint32_t cacheKey = 0;
	static int cacheAge = 1000;
	const int cx = rect.position.x - 2;
	const int cy = rect.position.y - 2;
	const int cw = rect.size.width + 4;
	const int ch = rect.size.height + 4;
	if (cx < 0 || cy < 0 || cx + cw > out.w() || cy + ch > out.h() || cw * ch > static_cast<int>(sizeof(cache)))
		return;
	uint32_t key = 2166136261U;
	auto mix = [&key](uint32_t v) { key = (key ^ v) * 16777619U; };
	mix(static_cast<uint32_t>(ViewPosition.x) | (static_cast<uint32_t>(ViewPosition.y) << 16));
	mix(static_cast<uint32_t>(cx) | (static_cast<uint32_t>(cy) << 10) | (static_cast<uint32_t>(cw) << 20));
	mix(static_cast<uint32_t>(AutomapOffset.deltaX & 0xFFFF) | (static_cast<uint32_t>(AutomapOffset.deltaY & 0xFFFF) << 16));
	mix(static_cast<uint32_t>(currlevel) | (static_cast<uint32_t>(leveltype) << 8) | (static_cast<uint32_t>(setlevel) << 16) | (static_cast<uint32_t>(MinimapScale) << 20));
	if (key != cacheKey || ++cacheAge > 45) {
		const Rectangle savedRect = MinimapRect;
		const AutomapType savedType = GetAutomapType();
		MinimapRect = rect;
		SetAutomapType(AutomapType::Minimap);
		DrawAutomap(out.subregionY(0, gnViewportHeight));
		SetAutomapType(savedType);
		MinimapRect = savedRect;
		for (int y = 0; y < ch; y++)
			memcpy(&cache[static_cast<size_t>(y) * cw], out.at(cx, cy + y), static_cast<size_t>(cw));
		cacheKey = key;
		cacheAge = 0;
		n64_prof_add(31, 1);
	} else {
		for (int y = 0; y < ch; y++)
			memcpy(out.at(cx, cy + y), &cache[static_cast<size_t>(y) * cw], static_cast<size_t>(cw));
	}
}
#endif

void DrawView(const Surface &out, Point startPosition)
{
#ifdef _DEBUG
	DebugCoordsMap.clear();
#endif
	Displacement offset = {};
	CalcFirstTilePosition(startPosition, offset);
#ifdef __N64__
	// f2v85: en el pueblo, con una tienda o un dialogo abierto (el heroe no se mueve), el mundo de detras
	// se dibuja una vez y despues se congela: se vuelve a mostrar esa imagen completa (sdl_n64.c) y solo
	// cambia la capa de la interfaz. Antes las tiendas iban a 6-10 fps (la cache de CEL queda pequena).
	static bool n64FreezeReady = false;
	// f2v91: tambien con el menu de START en un jugador, en cualquier sitio: el juego esta en pausa
	// (diablo.cpp no avanza la logica con gmenu_is_active()), asi que el mundo no cambia
	const bool n64Freeze = !MyPlayerIsDead && N64UiMode
	    && ((leveltype == DTYPE_TOWN && N64UiStoreOnly()) || (!gbIsMultiplayer && gmenu_is_active()));
	const bool n64SkipWorld = n64Freeze && n64FreezeReady;
	n64FreezeReady = n64Freeze;
	if (n64SkipWorld) {
		n64_world_skip();
		N64WorldFrozenFrames++;
	} else {
		DrawGame(out, startPosition, offset);
		const unsigned n64Am0 = n64_us();
		if (AutomapActive) {
			DrawAutomap(out.subregionY(0, gnViewportHeight));
		} else {
			N64DrawMinimap(out);
		}
		n64_prof_add(30, n64_us() - n64Am0);
	}
#else
	DrawGame(out, startPosition, offset);
	if (AutomapActive) {
		DrawAutomap(out.subregionY(0, gnViewportHeight));
	}
#endif
#ifdef _DEBUG
	bool debugGridTextNeeded = IsDebugGridTextNeeded();
	if (debugGridTextNeeded || DebugGrid) {
		// force redrawing or debug stuff stays on panel on 640x480 resolution
		RedrawEverything();
		std::string debugGridText;
		bool megaTiles = IsDebugGridInMegatiles();

		for (auto [dunCoordVal, pixelCoords] : DebugCoordsMap) {
			Point dunCoords = { dunCoordVal % MAXDUNX, dunCoordVal / MAXDUNX };
			if (megaTiles && (dunCoords.x % 2 == 1 || dunCoords.y % 2 == 1))
				continue;
			if (megaTiles)
				pixelCoords += Displacement { 0, TILE_HEIGHT / 2 };
			if (*GetOptions().Graphics.zoom)
				pixelCoords *= 2;
			if (debugGridTextNeeded && GetDebugGridText(dunCoords, debugGridText)) {
				Size tileSize = { TILE_WIDTH, TILE_HEIGHT };
				if (*GetOptions().Graphics.zoom)
					tileSize *= 2;
				DrawString(out, debugGridText, { pixelCoords - Displacement { 0, tileSize.height }, tileSize },
				    { .flags = UiFlags::ColorRed | UiFlags::AlignCenter | UiFlags::VerticalCenter });
			}
			if (DebugGrid) {
				int halfTileWidth = TILE_WIDTH / 2;
				int halfTileHeight = TILE_HEIGHT / 2;
				if (*GetOptions().Graphics.zoom) {
					halfTileWidth *= 2;
					halfTileHeight *= 2;
				}
				const Point center { pixelCoords.x + halfTileWidth, pixelCoords.y - halfTileHeight };

				if (megaTiles) {
					halfTileWidth *= 2;
					halfTileHeight *= 2;
				}

				const uint8_t col = PAL16_BEIGE;
				for (const auto &[originX, dx] : { std::pair(center.x - halfTileWidth, 1), std::pair(center.x + halfTileWidth, -1) }) {
					// We only need to draw half of the grid cell boundaries (one triangle).
					// The other triangle will be drawn when drawing the adjacent grid cells.
					const int dy = 1;
					Point from { originX, center.y };
					int height = halfTileHeight;
					if (out.InBounds(from) && out.InBounds(from + Displacement { 2 * dx * height, dy * height })) {
						uint8_t *dst = out.at(from.x, from.y);
						const int pitch = out.pitch();
						while (height-- > 0) {
							*dst = col;
							dst += dx;
							*dst = col;
							dst += dx;
							dst += static_cast<ptrdiff_t>(dy * pitch);
						}
					} else {
						while (height-- > 0) {
							out.SetPixel(from, col);
							from.x += dx;
							out.SetPixel(from, col);
							from.x += dx;
							from.y += dy;
						}
					}
				}
			}
		}
	}
#endif
#ifdef __N64__
	if (!n64SkipWorld) {
		DrawItemNameLabels(out);
		DrawMonsterHealthBar(out);
		DrawFloatingNumbers(out, startPosition, offset);
	}
#else
	DrawItemNameLabels(out);
	DrawMonsterHealthBar(out);
	DrawFloatingNumbers(out, startPosition, offset);
#endif

#ifdef __N64__
	// f2v54: en modo interfaz todo esto va a la capa de 640x480 (se borra en cada fotograma)
	if (N64UiMode && n64OverlaySurface != nullptr)
		memset(n64OverlaySurface->pixels, 0, static_cast<size_t>(n64OverlaySurface->pitch) * 480);
	const Surface uiOut = N64UiOut(out);
	N64OverlayDrawing = uiOut.surface != out.surface; // fondos semitransparentes como trama
#else
	const Surface &uiOut = out;
#endif
	if (IsPlayerInStore() && !qtextflag)
		DrawSText(uiOut);
	if (invflag) {
		DrawInv(uiOut);
	} else if (SpellbookFlag) {
		DrawSpellBook(uiOut);
	}

	DrawDurIcon(uiOut);

	DrawLevelButton(uiOut);

	if (CharFlag) {
		DrawChr(uiOut);
	} else if (QuestLogIsOpen) {
		DrawQuestLog(uiOut);
	} else if (IsStashOpen) {
		DrawStash(uiOut);
	} else if (IsVisualStoreOpen) {
		DrawVisualStore(uiOut);
	}

	if (ShowUniqueItemInfoBox) {
		DrawUniqueInfo(uiOut);
	}
	if (qtextflag) {
		DrawQText(uiOut);
	}
	if (SpellSelectFlag) {
		DrawSpellList(uiOut);
	}
	if (DropGoldFlag) {
		DrawGoldSplit(uiOut);
	}
	DrawGoldWithdraw(uiOut);
	if (HelpFlag) {
		DrawHelp(uiOut);
	}
	if (ChatLogFlag) {
		DrawChatLog(uiOut);
	}
	if (MyPlayerIsDead) {
		RedBack(out);
		DrawDeathText(out);
	} else if (PauseMode != 0) {
		gmenu_draw_pause(out);
	}
	if (IsDiabloMsgAvailable()) {
		DrawDiabloMsg(uiOut.subregionY(0, uiOut.h() - GetMainPanel().size.height));
	}

	DrawControllerModifierHints(uiOut);
	DrawPlrMsg(uiOut);
	gmenu_draw(uiOut);
	doom_draw(uiOut);
#ifdef __N64__
	{
		const Surface panelOut = N64PanelOut(out);
		if (panelOut.surface != out.surface && panelOut.surface != n64OverlaySurface && n64PanelSurface != nullptr) {
			// encima del panel: transparente (indice 0) salvo lo que dibujen los frascos
			memset(n64PanelSurface->pixels, 0, static_cast<size_t>(n64PanelSurface->pitch) * N64PanelRow);
		}
		if (!(N64UiMode && gmenu_is_active())) {
			// f2v56: texto del cuadro en su capa (a tamano del juego) salvo en el modo interfaz
			n64_infotext_show(0);
			if (panelOut.surface != out.surface && panelOut.surface != n64OverlaySurface)
				N64InfoTextOut = &panelOut; // f2v59: solo como marca (el texto lo escribe el RDP)
			DrawInfoBox(panelOut);
			N64InfoTextOut = nullptr;
			UpdateLifeManaPercent(); // Update life/mana totals before rendering any portion of the flask.
			DrawLifeFlaskUpper(panelOut);
			DrawManaFlaskUpper(panelOut);
		}
		N64OverlayDrawing = false;
	}
#else
	DrawInfoBox(out);
	UpdateLifeManaPercent(); // Update life/mana totals before rendering any portion of the flask.
	DrawLifeFlaskUpper(out);
	DrawManaFlaskUpper(out);
#endif
}

/**
 * @brief Display the current average FPS over 1 sec
 */
void DrawFPS(const Surface &out)
{
	static int framesSinceLastUpdate = 0;
	static std::string_view formatted {};

	if (!frameflag || !gbActive) {
		return;
	}

	framesSinceLastUpdate++;
	const uint32_t runtimeInMs = SDL_GetTicks();
	const uint32_t msSinceLastUpdate = runtimeInMs - lastFpsUpdateInMs;
	if (msSinceLastUpdate >= 1000) {
		lastFpsUpdateInMs = runtimeInMs;
		constexpr int FpsPow10 = 10;
		const uint32_t fps = 1000 * FpsPow10 * framesSinceLastUpdate / msSinceLastUpdate;
		framesSinceLastUpdate = 0;

		static char buf[15] {};
		const char *end = fps >= 100 * FpsPow10
		    ? BufCopy(buf, fps / FpsPow10, " FPS")
		    : BufCopy(buf, fps / FpsPow10, ".", fps % FpsPow10, " FPS");
		formatted = { buf, static_cast<std::string_view::size_type>(end - buf) };
	};
	DrawString(out, formatted, Point { 8, 8 }, { .flags = UiFlags::ColorRed });
}

/**
 * @brief Update part of the screen from the back buffer
 */
void DoBlitScreen(Rectangle area)
{
#ifdef DEBUG_DO_BLIT_SCREEN
	const Surface &out = GlobalBackBuffer();
	const uint8_t debugColor = PAL8_RED;
	DrawHorizontalLine(out, area.position, area.size.width, debugColor);
	DrawHorizontalLine(out, area.position + Displacement { 0, area.size.height - 1 }, area.size.width, debugColor);
	DrawVerticalLine(out, area.position, area.size.height, debugColor);
	DrawVerticalLine(out, area.position + Displacement { area.size.width - 1, 0 }, area.size.height, debugColor);
#endif
	SDL_Rect srcRect = MakeSdlRect(area);
	SDL_Rect dstRect = MakeSdlRect(area);
	BltFast(&srcRect, &dstRect);
}

/**
 * @brief Check render pipeline and update individual screen parts
 * @param out Output surface.
 * @param dwHgt Section of screen to update from top to bottom
 * @param drawDesc Render info box
 * @param drawHp Render health bar
 * @param drawMana Render mana bar
 * @param drawSbar Render belt
 * @param drawBtn Render panel buttons
 */
void DrawMain(int dwHgt, bool drawDesc, bool drawHp, bool drawMana, bool drawSbar, bool drawBtn)
{
	if (!gbActive) {
		return;
	}

	assert(dwHgt >= 0 && dwHgt <= gnScreenHeight);

	if (dwHgt > 0) {
		DoBlitScreen({ { 0, 0 }, { gnScreenWidth, dwHgt } });
	}
	if (dwHgt < gnScreenHeight) {
		const Point mainPanelPosition = GetMainPanel().position;
		if (drawSbar) {
			DoBlitScreen({ mainPanelPosition + Displacement { 204, 5 }, { 232, 28 } });
		}
		if (drawDesc) {
			if (ChatFlag) {
				// When chat input is displayed, the belt is hidden and the chat moves up.
				DoBlitScreen({ mainPanelPosition + Displacement { 171, 6 }, { 298, 116 } });
			} else {
				DoBlitScreen({ mainPanelPosition + Displacement { InfoBoxRect.position.x, InfoBoxRect.position.y }, { InfoBoxRect.size } });
			}
		}
		if (drawMana) {
			DoBlitScreen({ mainPanelPosition + Displacement { 460, 0 }, { 88, 72 } });
			DoBlitScreen({ mainPanelPosition + Displacement { 564, 64 }, { 56, 56 } });
		}
		if (drawHp) {
			DoBlitScreen({ mainPanelPosition + Displacement { 96, 0 }, { 88, 72 } });
		}
		if (drawBtn) {
			DoBlitScreen({ mainPanelPosition + Displacement { 8, 7 }, { 74, 114 } });
			DoBlitScreen({ mainPanelPosition + Displacement { 559, 7 }, { 74, 48 } });
			if (gbIsMultiplayer) {
				DoBlitScreen({ mainPanelPosition + Displacement { 86, 91 }, { 34, 32 } });
				DoBlitScreen({ mainPanelPosition + Displacement { 526, 91 }, { 34, 32 } });
			}
		}
		if (PrevCursorRect.size.width != 0 && PrevCursorRect.size.height != 0) {
			DoBlitScreen(PrevCursorRect);
		}
		const Rectangle &cursorRect = GetDrawnCursor().rect;
		if (cursorRect.size.width != 0 && cursorRect.size.height != 0) {
			DoBlitScreen(cursorRect);
		}
	}
}

void OptionShowFPSChanged()
{
	if (*GetOptions().Graphics.showFPS)
		EnableFrameCount();
	else
		frameflag = false;
}
const auto OptionChangeHandlerShowFPS = (GetOptions().Graphics.showFPS.SetValueChangedCallback(OptionShowFPSChanged), true);

} // namespace

Displacement GetOffsetForWalking(const AnimationInfo &animationInfo, const Direction dir, bool cameraMode /*= false*/)
{
	// clang-format off
	//                                           South,        SouthWest,    West,         NorthWest,    North,        NorthEast,     East,         SouthEast,
	constexpr Displacement MovingOffset[8]   = { {   0,  32 }, { -32,  16 }, { -64,   0 }, { -32, -16 }, {   0, -32 }, {  32, -16 },  {  64,   0 }, {  32,  16 } };
	// clang-format on

	const uint8_t animationProgress = animationInfo.getAnimationProgress();
	Displacement offset = MovingOffset[static_cast<size_t>(dir)];
	offset *= animationProgress;
	offset /= AnimationInfo::baseValueFraction;

	if (cameraMode) {
		offset = -offset;
	}

	return offset;
}

void ClearCursor() // CODE_FIX: this was supposed to be in cursor.cpp
{
	PrevCursorRect = {};
}

void ShiftGrid(Point *offset, int horizontal, int vertical)
{
	offset->x += vertical + horizontal;
	offset->y += vertical - horizontal;
}

int RowsCoveredByPanel()
{
	const auto &mainPanelSize = GetMainPanel().size;
	if (GetScreenWidth() <= mainPanelSize.width) {
		return 0;
	}

	int rows = mainPanelSize.height / TILE_HEIGHT;
	if (*GetOptions().Graphics.zoom) {
		rows /= 2;
	}

	return rows;
}

void CalcTileOffset(int *offsetX, int *offsetY)
{
	const uint16_t screenWidth = GetScreenWidth();
	const uint16_t viewportHeight = GetViewportHeight();

	int x;
	int y;

	if (!*GetOptions().Graphics.zoom) {
		x = screenWidth % TILE_WIDTH;
		y = viewportHeight % TILE_HEIGHT;
	} else {
		x = (screenWidth / 2) % TILE_WIDTH;
		y = (viewportHeight / 2) % TILE_HEIGHT;
	}

	if (x != 0)
		x = (TILE_WIDTH - x) / 2;
	if (y != 0)
		y = (TILE_HEIGHT - y) / 2;

	*offsetX = x;
	*offsetY = y;
}

void TilesInView(int *rcolumns, int *rrows)
{
	const uint16_t screenWidth = GetScreenWidth();
	const uint16_t viewportHeight = GetViewportHeight();

	int columns = screenWidth / TILE_WIDTH;
	if ((screenWidth % TILE_WIDTH) != 0) {
		columns++;
	}
	int rows = viewportHeight / TILE_HEIGHT;
	if ((viewportHeight % TILE_HEIGHT) != 0) {
		rows++;
	}

	if (*GetOptions().Graphics.zoom) {
		// Half the number of tiles, rounded up
		if ((columns & 1) != 0) {
			columns++;
		}
		columns /= 2;
		if ((rows & 1) != 0) {
			rows++;
		}
		rows /= 2;
	}

	*rcolumns = columns;
	*rrows = rows;
}

void CalcViewportGeometry()
{
	const int zoomFactor = *GetOptions().Graphics.zoom ? 2 : 1;
	const int screenWidth = GetScreenWidth() / zoomFactor;
	const int screenHeight = GetScreenHeight() / zoomFactor;
	const int panelHeight = GetMainPanel().size.height / zoomFactor;
	const int pixelsToPanel = screenHeight - panelHeight;
#ifdef __N64__
	// N64 (f2v28): el personaje en el centro de la pantalla (el panel va reducido encima)
	(void)pixelsToPanel;
	Point playerPosition { screenWidth / 2, screenHeight / 2 };
#else
	Point playerPosition { screenWidth / 2, pixelsToPanel / 2 };
#endif

	if (*GetOptions().Graphics.zoom)
		playerPosition.y += TILE_HEIGHT / 4;

	const int tilesToTop = (playerPosition.y + TILE_HEIGHT - 1) / TILE_HEIGHT;
	const int tilesToLeft = (playerPosition.x + TILE_WIDTH - 1) / TILE_WIDTH;

	// Location of the center of the tile from which to start rendering, relative to the viewport origin
	Point startPosition = playerPosition - Displacement { tilesToLeft * TILE_WIDTH, tilesToTop * TILE_HEIGHT };

	// Position of the tile from which to start rendering in tile space,
	// relative to the tile the player character occupies
	tileShift = { 0, 0 };
	tileShift += Displacement(Direction::North) * tilesToTop;
	tileShift += Displacement(Direction::West) * tilesToLeft;

	// The rendering loop expects to start on a row with fewer columns
	if (tilesToLeft * TILE_WIDTH >= playerPosition.x) {
		startPosition += Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
		tileShift += Displacement(Direction::NorthEast);
	} else if (tilesToTop * TILE_HEIGHT < playerPosition.y) {
		// There is one row above the current row that needs to be rendered,
		// but we skip to the row above it because it has too many columns
		startPosition += Displacement { 0, -TILE_HEIGHT };
		tileShift += Displacement(Direction::North);
	}

	// Location of the bottom-left corner of the bounding box around the
	// tile from which to start rendering, relative to the viewport origin
	tileOffset = { startPosition.x - (TILE_WIDTH / 2), startPosition.y + (TILE_HEIGHT / 2) - 1 };

	// Compute the number of rows to be rendered as well as
	// the number of columns to be rendered in the first row
	const int viewportHeight = GetViewportHeight() / zoomFactor;
	const Point renderStart = startPosition - Displacement { TILE_WIDTH / 2, TILE_HEIGHT / 2 };
	tileRows = (viewportHeight - renderStart.y + TILE_HEIGHT / 2 - 1) / (TILE_HEIGHT / 2);
	tileColumns = (screenWidth - renderStart.x + TILE_WIDTH - 1) / TILE_WIDTH;
}

Point GetScreenPosition(Point tile)
{
	Point firstTile = ViewPosition;
	Displacement offset = {};
	CalcFirstTilePosition(firstTile, offset);

	const Displacement delta = firstTile - tile;

	Point position {};
	position += delta.worldToScreen();
	position += offset;
	return position;
}

extern SDL_Surface *PalSurface;

void ClearScreenBuffer()
{
	if (HeadlessMode)
		return;

	assert(PalSurface != nullptr);
	SDL_FillSurfaceRect(PalSurface, nullptr, 0);
}

#ifdef _DEBUG
void ScrollView()
{
	if (!MyPlayer->HoldItem.isEmpty())
		return;

	if (MousePosition.x < 20) {
		if (dmaxPosition.y - 1 <= ViewPosition.y || dminPosition.x >= ViewPosition.x) {
			if (dmaxPosition.y - 1 > ViewPosition.y) {
				ViewPosition.y++;
			}
			if (dminPosition.x < ViewPosition.x) {
				ViewPosition.x--;
			}
		} else {
			ViewPosition.y++;
			ViewPosition.x--;
		}
	}
	if (MousePosition.x > gnScreenWidth - 20) {
		if (dmaxPosition.x - 1 <= ViewPosition.x || dminPosition.y >= ViewPosition.y) {
			if (dmaxPosition.x - 1 > ViewPosition.x) {
				ViewPosition.x++;
			}
			if (dminPosition.y < ViewPosition.y) {
				ViewPosition.y--;
			}
		} else {
			ViewPosition.y--;
			ViewPosition.x++;
		}
	}
	if (MousePosition.y < 20) {
		if (dminPosition.y >= ViewPosition.y || dminPosition.x >= ViewPosition.x) {
			if (dminPosition.y < ViewPosition.y) {
				ViewPosition.y--;
			}
			if (dminPosition.x < ViewPosition.x) {
				ViewPosition.x--;
			}
		} else {
			ViewPosition.x--;
			ViewPosition.y--;
		}
	}
	if (MousePosition.y > gnScreenHeight - 20) {
		if (dmaxPosition.y - 1 <= ViewPosition.y || dmaxPosition.x - 1 <= ViewPosition.x) {
			if (dmaxPosition.y - 1 > ViewPosition.y) {
				ViewPosition.y++;
			}
			if (dmaxPosition.x - 1 > ViewPosition.x) {
				ViewPosition.x++;
			}
		} else {
			ViewPosition.x++;
			ViewPosition.y++;
		}
	}
}
#endif

void EnableFrameCount()
{
	frameflag = true;
	lastFpsUpdateInMs = SDL_GetTicks();
}

void scrollrt_draw_game_screen()
{
	if (HeadlessMode)
		return;

	int hgt = 0;

	if (IsRedrawEverything()) {
		RedrawComplete();
		hgt = gnScreenHeight;
	}

	const Surface &out = GlobalBackBuffer();
	UndrawCursor(out);
	DrawCursor(out);
	DrawMain(hgt, false, false, false, false, false);

	RenderPresent();
}

#ifdef __N64__
extern "C" void n64_lua_check(const char *where);
extern "C" unsigned n64_us(void);
extern "C" void n64_prof_add(int which, unsigned us);
namespace {
struct N64BlitTimer {
	unsigned t0 = n64_us();
	~N64BlitTimer() { n64_prof_add(2, n64_us() - t0); }
};
} // namespace
extern "C" int n64_check_guards(const char *where);
static int n64DrawChecks;
#define N64_GUARD(name) \
	do { \
		if (n64DrawChecks < 3) n64_check_guards(name); \
	} while (0)
#else
#define N64_GUARD(name) \
	do { \
	} while (0)
#endif
void DrawAndBlit()
{
	if (!gbRunGame || HeadlessMode) {
		return;
	}
#ifdef __N64__
	N64BlitTimer n64BlitTimer;
	if (n64DrawChecks < 3)
		n64_lua_check("inicio de DrawAndBlit");
	N64UpdateUiMode();
	if (N64UiMode)
		RedrawEverything(); // la capa se borra en cada fotograma: el panel se vuelve a dibujar entero
	{
		void N64PouchSync(Player &player); // f2v89: el oro no ocupa casillas
		N64PouchSync(*MyPlayer);
		// v0.8.6: latido con menos del 30 % de vida (no con el menu de START ni en pausa)
		const int64_t n64MaxHp = std::max<int64_t>(1, MyPlayer->_pMaxHP.raw());
		n64_rumble_health(static_cast<int>(std::max<int64_t>(0, MyPlayer->_pHitPoints.raw()) * 100 / n64MaxHp),
		    !gmenu_is_active() && PauseMode == 0 && !MyPlayerIsDead ? 1 : 0);
	}
#endif

	int hgt = 0;
	bool drawHealth = IsRedrawComponent(PanelDrawComponent::Health);
	bool drawMana = IsRedrawComponent(PanelDrawComponent::Mana);
	bool drawControlButtons = IsRedrawComponent(PanelDrawComponent::ControlButtons);
	bool drawBelt = IsRedrawComponent(PanelDrawComponent::Belt);
	const bool drawChatInput = ChatFlag;
	bool drawInfoBox = false;
	bool drawCtrlPan = false;

	const Rectangle &mainPanel = GetMainPanel();

	if (gnScreenWidth > mainPanel.size.width || IsRedrawEverything()) {
		drawHealth = true;
		drawMana = true;
		drawControlButtons = true;
		drawBelt = true;
		drawInfoBox = false;
		drawCtrlPan = true;
		hgt = gnScreenHeight;
	} else if (IsRedrawViewport()) {
		drawInfoBox = true;
		drawCtrlPan = false;
		hgt = gnViewportHeight;
	}

	const Surface &out = GlobalBackBuffer();
#ifdef __N64__
	if (!N64UiMode)
		UndrawCursor(out);
	if (N64UiMode && gmenu_is_active()) {
		// f2v53/54: con el menu de START no se ve el panel
		drawHealth = drawMana = drawControlButtons = drawBelt = drawInfoBox = drawCtrlPan = false;
	}
#else
	UndrawCursor(out);
#endif
#ifdef __N64__
	{
		// L+R+C arriba/abajo cambia el tamano del panel: recalcular el hueco del mundo
		static unsigned n64PanelSerialSeen;
		if (n64_panel_serial() != n64PanelSerialSeen) {
			n64PanelSerialSeen = n64_panel_serial();
			CalculatePanelAreas();
			RedrawEverything();
		}
	}
	const Surface panelOut = N64PanelOut(out);
	n64_panel_frame();
	if (panelOut.surface != out.surface && n64_panel_occluder(&N64OccX0, &N64OccY0, &N64OccX1, &N64OccY1))
		N64OccPixels = out.begin();
	else
		N64OccPixels = nullptr;
#else
	const Surface &panelOut = out;
#endif

	nthread_UpdateProgressToNextGameTick();

	this_sdl_thread::yield();
#ifdef __N64__
	const unsigned n64T0 = n64_us();
#endif
	DrawView(out, ViewPosition);
#ifdef __N64__
	n64_prof_add(1, n64_us() - n64T0);
#endif
#ifdef __N64__
	if (n64DrawChecks < 3)
		n64_lua_check("tras DrawView");
#endif
	if (drawCtrlPan) {
		DrawMainPanel(panelOut);
	}
	N64_GUARD("tras DrawMainPanel");

	if (drawHealth) {
		DrawLifeFlaskLower(panelOut, !drawCtrlPan);
	}
	N64_GUARD("tras DrawLifeFlaskLower");

	if (drawMana) {
		DrawManaFlaskLower(panelOut, !drawCtrlPan);

		DrawSpell(panelOut);
	}
	N64_GUARD("tras DrawSpell");

	if (drawControlButtons) {
		DrawMainPanelButtons(panelOut);
	}
	N64_GUARD("tras DrawMainPanelButtons");

	if (drawBelt) {
		DrawInvBelt(panelOut);
	}
	N64_GUARD("tras DrawInvBelt");

	if (drawChatInput) {
		DrawChatBox(panelOut);
	}
	DrawXPBar(panelOut);
	N64_GUARD("tras DrawXPBar");
	if (*GetOptions().Gameplay.showHealthValues)
		DrawFlaskValues(panelOut, { mainPanel.position.x + 134, mainPanel.position.y + 28 }, MyPlayer->_pHitPoints.whole(), MyPlayer->_pMaxHP.whole());
	if (*GetOptions().Gameplay.showManaValues)
		DrawFlaskValues(panelOut, { mainPanel.position.x + mainPanel.size.width - 138, mainPanel.position.y + 28 },
		    (HasAnyOf(InspectPlayer->_pIFlags, ItemSpecialEffect::NoMana) || MyPlayer->hasNoMana()) ? 0 : MyPlayer->_pMana.whole(),
		    HasAnyOf(InspectPlayer->_pIFlags, ItemSpecialEffect::NoMana) ? 0 : MyPlayer->_pMaxMana.whole());
#ifdef __N64__
	if (*GetOptions().Gameplay.floatingInfoBox)
		DrawFloatingInfoBox(N64UiOut(out));
#else
	if (*GetOptions().Gameplay.floatingInfoBox)
		DrawFloatingInfoBox(out);
#endif
	N64_GUARD("tras DrawFloatingInfoBox");

	if ((*GetOptions().Gameplay.showMultiplayerPartyInfo != PartyPortraits::Disabled) && PartySidePanelOpen)
		DrawPartyMemberInfoPanel(out);

#ifdef __N64__
	DrawCursor(N64UiOut(out));
#else
	DrawCursor(out);
#endif

	DrawFPS(out);
#if defined(__N64__) && defined(N64_COORDS)
	// SOLO PRUEBAS (como f2v90): coordenadas del heroe arriba a la derecha en el pueblo
	if (leveltype == DTYPE_TOWN && !N64UiMode && MyPlayer != nullptr) {
		const Point n64P = MyPlayer->position.tile;
		DrawString(out, StrCat(n64P.x, ",", n64P.y), Rectangle(Point { gnScreenWidth - 70, 4 }, Size { 66, 16 }),
		    { .flags = UiFlags::ColorWhite | UiFlags::AlignRight });
	}
#endif
	N64_GUARD("tras DrawCursor+DrawFPS");

#ifdef __N64__
	if (n64DrawChecks < 3) {
		n64DrawChecks++;
		n64_lua_check("antes de GameDrawComplete");
	}
#endif
	lua::GameDrawComplete();

	this_sdl_thread::yield();
	DrawMain(hgt, drawInfoBox, drawHealth, drawMana, drawBelt, drawControlButtons);

#ifdef _DEBUG
	DrawConsole(out);
#endif

	RedrawComplete();
	for (const PanelDrawComponent component : enum_values<PanelDrawComponent>()) {
		if (IsRedrawComponent(component)) {
			RedrawComponentComplete(component);
		}
	}

	RenderPresent();
}

} // namespace devilution
