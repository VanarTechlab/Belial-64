#include "levels/setmaps.h"

#include <cstdint>
#include <expected>

#ifdef _DEBUG
#include "debug.h"
#endif
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "levels/drlg_l1.h"
#include "levels/drlg_l2.h"
#include "levels/drlg_l3.h"
#include "levels/drlg_l4.h"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "msg.h"
#include "objects.h"
#include "quests.h"
#include "tables/objdat.h"
#include "utils/language.h"
#include "utils/status_macros.hpp"
#include "utils/endian_swap.hpp" /* NDS-BOSQUE */
#include <cstring>
#include "engine/light_tables.hpp" /* NDS-BOSQUE2 */

namespace devilution {

int NdsBosqueW = 0; /* NDS-BOSQUE */
int NdsBosqueH = 0;
Point NdsBosqueInicio = { 0, 0 }; /* NDS-BOSQUE3 */
bool NdsBosqueSalidaLista = false;
Point NdsBosqueSalidaA = { 0, 0 }; /* NDS-BOSQUE5 */
Point NdsBosqueSalidaB = { -1, -1 };
Point NdsBosqueLuz = { 0, 0 }; /* NDS-SUCUBOS */

/** Maps from quest level to quest level names. */
const char *const QuestLevelNames[] = {
	"",
	N_("Skeleton King's Lair"),
	N_("Chamber of Bone"),
	N_("Maze"),
	N_("Poisoned Water Supply"),
	N_("Archbishop Lazarus' Lair"),
	N_("Butcher's Lair"), /* NDS-GUARIDA */
	N_("Forest"), /* NDS-BOSQUE */
	N_("Church Arena"),
	N_("Hell Arena"),
	N_("Circle of Life Arena"),
};

namespace {

void AddSKingObjs()
{
	constexpr WorldTileRectangle SmallSecretRoom { { 20, 7 }, { 3, 3 } };
	ObjectAtPosition({ 64, 34 }).InitializeLoadedObject(SmallSecretRoom, 1);

	constexpr WorldTileRectangle Gate { { 20, 14 }, { 1, 2 } };
	ObjectAtPosition({ 64, 59 }).InitializeLoadedObject(Gate, 2);

	constexpr WorldTileRectangle LargeSecretRoom { { 8, 1 }, { 7, 10 } };
	ObjectAtPosition({ 27, 37 }).InitializeLoadedObject(LargeSecretRoom, 3);
	ObjectAtPosition({ 46, 35 }).InitializeLoadedObject(LargeSecretRoom, 3);
	ObjectAtPosition({ 49, 53 }).InitializeLoadedObject(LargeSecretRoom, 3);
	ObjectAtPosition({ 27, 53 }).InitializeLoadedObject(LargeSecretRoom, 3);
}

void AddSChamObjs()
{
	ObjectAtPosition({ 37, 30 }).InitializeLoadedObject({ { 17, 0 }, { 4, 5 } }, 1);
	ObjectAtPosition({ 37, 46 }).InitializeLoadedObject({ { 13, 0 }, { 3, 5 } }, 2);
}

void AddVileObjs()
{
	ObjectAtPosition({ 26, 45 }).InitializeLoadedObject({ { 1, 1 }, { 8, 9 } }, 1);
	ObjectAtPosition({ 45, 46 }).InitializeLoadedObject({ { 11, 1 }, { 9, 9 } }, 2);
	ObjectAtPosition({ 35, 36 }).InitializeLoadedObject({ { 7, 11 }, { 6, 7 } }, 3);
}

void SetMapTransparency(const char *path)
{
	auto dunData = LoadFileInMem<uint16_t>(path);
	LoadTransparency(dunData.get());
}

/* NDS-BOSQUE: mapa hecho con piezas grandes del pueblo (town.til). Marcas en la capa 0: 1 = Inicio */
std::expected<void, std::string> NdsLoadBosque(const char *path)
{
	RETURN_IF_ERROR(LoadDungeonBase(path, { 0, 0 }, 0, 0)); /* limpia el nivel; capas de monstruos/objetos vacias */
	memset(dungeon, 0, sizeof(dungeon)); /* sin mapa automatico de Catedral */
	auto dunData = LoadFileInMem<uint16_t>(path);
	const WorldTileSize size = GetDunSize(dunData.get());
	NdsBosqueW = size.width;
	NdsBosqueH = size.height;
	for (int x = 0; x < MAXDUNX; x++)
		for (int y = 0; y < MAXDUNY; y++)
			dPiece[x][y] = 426; /* negro solido, como fuera del pueblo */
	const uint16_t *tileLayer = &dunData[2];
	for (int j = 0; j < NdsBosqueH; j++) {
		for (int i = 0; i < NdsBosqueW; i++) {
			const int tileId = Swap16LE(tileLayer[(j * NdsBosqueW) + i]) - 1;
			if (tileId < 0)
				continue;
			const MegaTile mega = pMegaTiles[tileId];
			const int xx = 16 + 2 * i;
			const int yy = 16 + 2 * j;
			dPiece[xx + 0][yy + 0] = Swap16LE(mega.micro1);
			dPiece[xx + 1][yy + 0] = Swap16LE(mega.micro2);
			dPiece[xx + 0][yy + 1] = Swap16LE(mega.micro3);
			dPiece[xx + 1][yy + 1] = Swap16LE(mega.micro4);
		}
	}
	ViewPosition = { 16 + NdsBosqueW, 16 + NdsBosqueH };
	const uint16_t *marks = &dunData[2 + (NdsBosqueW * NdsBosqueH)];
	for (int j = 0; j < 2 * NdsBosqueH; j++)
		for (int i = 0; i < 2 * NdsBosqueW; i++)
			if (Swap16LE(marks[(j * 2 * NdsBosqueW) + i]) == 1)
				ViewPosition = { 16 + i, 16 + j };
	NdsBosqueInicio = ViewPosition; /* NDS-BOSQUE3 */
	NdsBosqueSalidaLista = false;
	NdsBosqueLuz = { 0, 0 }; /* NDS-SUCUBOS: luz roja = marca 4 */
	for (int j = 0; j < 2 * NdsBosqueH; j++)
		for (int i = 0; i < 2 * NdsBosqueW; i++)
			if (Swap16LE(marks[(j * 2 * NdsBosqueW) + i]) == 4)
				NdsBosqueLuz = { 16 + i, 16 + j };
	NdsBosqueSalidaA = { 0, 0 }; /* NDS-BOSQUE5: casillas con la marca 5 = salida al pueblo */
	NdsBosqueSalidaB = { -1, -1 };
	for (int j = 0; j < 2 * NdsBosqueH; j++) {
		for (int i = 0; i < 2 * NdsBosqueW; i++) {
			if (Swap16LE(marks[(j * 2 * NdsBosqueW) + i]) != 5)
				continue;
			const int x = 16 + i;
			const int y = 16 + j;
			if (NdsBosqueSalidaB.x < NdsBosqueSalidaA.x) {
				NdsBosqueSalidaA = { x, y };
				NdsBosqueSalidaB = { x + 1, y + 1 };
				continue;
			}
			if (x < NdsBosqueSalidaA.x) NdsBosqueSalidaA.x = x;
			if (y < NdsBosqueSalidaA.y) NdsBosqueSalidaA.y = y;
			if (x + 1 > NdsBosqueSalidaB.x) NdsBosqueSalidaB.x = x + 1;
			if (y + 1 > NdsBosqueSalidaB.y) NdsBosqueSalidaB.y = y + 1;
		}
	}
	{ /* NDS-BOSQUE3: borde siempre cerrado con trozos de roca solida */
		const MegaTile roca = pMegaTiles[63 - 1];
		const uint16_t rocaP[4] = { Swap16LE(roca.micro1), Swap16LE(roca.micro2), Swap16LE(roca.micro3), Swap16LE(roca.micro4) };
		const int x0 = 16;
		const int y0 = 16;
		const int x1 = 16 + (2 * NdsBosqueW) - 1;
		const int y1 = 16 + (2 * NdsBosqueH) - 1;
		for (int x = x0; x <= x1; x++) {
			for (int y = y0; y <= y1; y++) {
				if (x != x0 && x != x1 && y != y0 && y != y1)
					continue;
				if (TileHasAny(Point { x, y }, TileProperties::Solid))
					continue;
				dPiece[x][y] = rocaP[(((y - y0) & 1) * 2) + ((x - x0) & 1)];
			}
		}
	}
	LoadRndLvlPal(DTYPE_TOWN);
	/* NDS-BOSQUE2: tablas de oscuridad hechas para la paleta del pueblo (color mas parecido) */
	for (int L = 1; L < 15; L++) {
		const int f = 15 - L; /* brillo en quinceavos */
		for (int c = 1; c < 128; c++) {
			const int r = logical_palette[c].r * f / 15;
			const int g = logical_palette[c].g * f / 15;
			const int b = logical_palette[c].b * f / 15;
			int best = 0;
			int bestDist = 0x7FFFFFFF;
			for (int k = 0; k < 128; k++) {
				const int dr = logical_palette[k].r - r;
				const int dg = logical_palette[k].g - g;
				const int db = logical_palette[k].b - b;
				const int d = (2 * dr * dr) + (4 * dg * dg) + (3 * db * db);
				if (d < bestDist) {
					bestDist = d;
					best = k;
				}
			}
			LightTables[L][c] = static_cast<uint8_t>(best);
		}
	}
	return {};
}

std::expected<void, std::string> LoadCustomMap(const char *path, Point viewPosition)
{
	switch (setlvltype) {
	case DTYPE_CATHEDRAL:
	case DTYPE_CRYPT:
		RETURN_IF_ERROR(LoadL1Dungeon(path, viewPosition));
		break;
	case DTYPE_CATACOMBS:
		RETURN_IF_ERROR(LoadL2Dungeon(path, viewPosition));
		break;
	case DTYPE_CAVES:
	case DTYPE_NEST:
		RETURN_IF_ERROR(LoadL3Dungeon(path, viewPosition));
		break;
	case DTYPE_HELL:
		RETURN_IF_ERROR(LoadL4Dungeon(path, viewPosition));
		break;
	case DTYPE_TOWN:
	case DTYPE_NONE:
		break;
	}
	LoadRndLvlPal(setlvltype);
	return {};
}

std::expected<void, std::string> LoadArenaMap(const char *path, Point viewPosition, Point exitTrigger)
{
	RETURN_IF_ERROR(LoadCustomMap(path, viewPosition));
	trigflag = false;
	numtrigs = 1;
	trigs[0].position = exitTrigger;
	trigs[0]._tmsg = WM_DIABRTNLVL;
	return {};
}

} // namespace

std::expected<void, std::string> LoadSetMap()
{
	switch (setlvlnum) {
	case SL_SKELKING:
		if (Quests[Q_SKELKING]._qactive == QUEST_INIT) {
			Quests[Q_SKELKING]._qactive = QUEST_ACTIVE;
			Quests[Q_SKELKING]._qvar1 = 1;
			NetSendCmdQuest(true, Quests[Q_SKELKING]);
		}
		LoadPreL1Dungeon("levels\\l1data\\sklkng1.dun");
		RETURN_IF_ERROR(LoadL1Dungeon("levels\\l1data\\sklkng2.dun", { 83, 44 }));
		SetMapTransparency("levels\\l1data\\sklkngt.dun");
		LoadPaletteAndInitBlending("levels\\l1data\\l1_2.pal");
		AddSKingObjs();
		InitSKingTriggers();
		break;
	case SL_BONECHAMB:
		LoadPreL2Dungeon("levels\\l2data\\bonecha2.dun");
		RETURN_IF_ERROR(LoadL2Dungeon("levels\\l2data\\bonecha1.dun", { 70, 40 }));
		SetMapTransparency("levels\\l2data\\bonechat.dun");
		LoadPaletteAndInitBlending("levels\\l2data\\l2_2.pal");
		AddSChamObjs();
		InitSChambTriggers();
		break;
	case SL_MAZE:
		break;
	case SL_POISONWATER:
		if (Quests[Q_PWATER]._qactive == QUEST_INIT)
			Quests[Q_PWATER]._qactive = QUEST_ACTIVE;
		RETURN_IF_ERROR(LoadL3Dungeon("levels\\l3data\\foulwatr.dun", { 31, 83 }));
		LoadPaletteAndInitBlending("levels\\l3data\\l3pfoul.pal");
		InitPWaterTriggers();
		break;
	case SL_VILEBETRAYER:
		if (Quests[Q_BETRAYER]._qactive == QUEST_DONE) {
			Quests[Q_BETRAYER]._qvar2 = 4;
		} else if (Quests[Q_BETRAYER]._qactive == QUEST_ACTIVE) {
			Quests[Q_BETRAYER]._qvar2 = 3;
		}
		LoadPreL1Dungeon("levels\\l1data\\vile1.dun");
		RETURN_IF_ERROR(LoadL1Dungeon("levels\\l1data\\vile2.dun", { 35, 36 }));
		SetMapTransparency("levels\\l1data\\vile1.dun");
		LoadPaletteAndInitBlending("levels\\l1data\\l1_2.pal");
		AddVileObjs();
		InitNoTriggers();
		break;
	case SL_BUTCHERLAIR: /* NDS-GUARIDA: mapa propio con piezas de la Catedral */
		setlvltype = DTYPE_CATHEDRAL;
		RETURN_IF_ERROR(LoadCustomMap("nds\\guarida.dun", { 23, 30 }));
		trigflag = false;
		numtrigs = 1;
		trigs[0].position = { 21, 32 }; /* portal rojo de vuelta (NDS-GUARIDA5) */
		trigs[0]._tmsg = WM_DIABRTNLVL;
		break;
	case SL_BOSQUE: /* NDS-BOSQUE */
		setlvltype = DTYPE_CATHEDRAL;
		RETURN_IF_ERROR(NdsLoadBosque("nds\\bosque.dun"));
		InitNoTriggers();
		break;
	case SL_ARENA_CHURCH:
		RETURN_IF_ERROR(LoadArenaMap("arena\\church.dun", { 29, 22 }, { 28, 20 }));
		break;
	case SL_ARENA_HELL:
		RETURN_IF_ERROR(LoadArenaMap("arena\\hell.dun", { 34, 26 }, { 33, 26 }));
		break;
	case SL_ARENA_CIRCLE_OF_LIFE:
		RETURN_IF_ERROR(LoadArenaMap("arena\\circle_of_death.dun", { 30, 26 }, { 29, 26 }));
		break;
	case SL_NONE:
#ifdef _DEBUG
		RETURN_IF_ERROR(LoadCustomMap(TestMapPath.c_str(), ViewPosition));
		InitNoTriggers();
#endif
		break;
	}
	return {};
}

} // namespace devilution
