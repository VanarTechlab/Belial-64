/**
 * @file quests.cpp
 *
 * Implementation of functionality for handling quests.
 */
#include "quests.h"

#include <cstdint>

#include "control/control.hpp"
#include "cursor.h"
#include "cursor_defs.hpp"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "engine/world_tile.hpp"
#include "game_mode.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/town.h"
#include "levels/trigs.h"
#include "missiles.h"
#include "monster.h"
#include "options.h"
#include "tables/townerdat.hpp"
#include "towners.h"
#include "utils/format.hpp"
#include "utils/is_of.hpp"
#include "utils/language.h"
#include "diablo_msg.hpp" /* NDS-BOSQUE4 */
#include "menu.h" /* NDS-SUCUBOS */
#include "player.h"
#include "levels/tile_properties.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"
#include <cstdio>
#include <string>

#ifdef _DEBUG
#include "debug.h"
#endif

namespace devilution {

namespace {

int WaterDone;

const char *const QuestTriggerNames[5] = {
	N_(/* TRANSLATORS: Quest Map*/ "King Leoric's Tomb"),
	N_(/* TRANSLATORS: Quest Map*/ "The Chamber of Bone"),
	N_(/* TRANSLATORS: Quest Map*/ "Maze"),
	N_(/* TRANSLATORS: Quest Map*/ "A Dark Passage"),
	N_(/* TRANSLATORS: Quest Map*/ "Unholy Altar")
};

std::array<Color, 32> PureWaterPalette;

void StartPWaterPurify()
{
	PlaySfxLoc(SfxID::QuestDone, MyPlayer->position.tile);
	LoadFileInMem("levels\\l3data\\l3pwater.pal", PureWaterPalette);
	UpdatePWaterPalette();
	WaterDone = 32;
}

} // namespace

void InitQuests()
{
	SetTownerQuestDialog(TOWN_HEALER, Q_MUSHROOM, TEXT_NONE);
	SetTownerQuestDialog(TOWN_WITCH, Q_MUSHROOM, TEXT_MUSH9);

	WaterDone = 0;

	int q = 0;
	for (auto &quest : Quests) {
		quest._qidx = static_cast<quest_id>(q);
		auto &questData = QuestsData[q];
		q++;

		quest._qactive = QUEST_NOTAVAIL;
		quest.position = { 0, 0 };
		quest._qlvltype = questData._qlvlt;
		quest._qslvl = questData._qslvl;
		quest._qvar1 = 0;
		quest._qvar2 = 0;
		quest._qlog = false;
		quest._qmsg = questData._qdmsg;

		if (!UseMultiplayerQuests()) {
			quest._qlevel = questData._qdlvl;
			quest._qactive = QUEST_INIT;
		} else if (!questData.isSinglePlayerOnly) {
			quest._qlevel = questData._qdmultlvl;
			quest._qactive = QUEST_INIT;
		}
	}

	if (!UseMultiplayerQuests() && *GetOptions().Gameplay.randomizeQuests) {
		// Quests are set from the seed used to generate level 15.
		InitialiseQuestPools(DungeonSeeds[15], Quests);
	}

	if (gbIsSpawn) {
		for (auto &quest : Quests) {
			quest._qactive = QUEST_NOTAVAIL;
		}
	}

	if (Quests[Q_SKELKING]._qactive == QUEST_NOTAVAIL)
		Quests[Q_SKELKING]._qvar2 = 2;
	if (Quests[Q_ROCK]._qactive == QUEST_NOTAVAIL)
		Quests[Q_ROCK]._qvar2 = 2;
	Quests[Q_LTBANNER]._qvar1 = 1;
	if (UseMultiplayerQuests())
		Quests[Q_BETRAYER]._qvar1 = 2;
	// In multiplayer items spawn during level generation to avoid desyncs
	if (gbIsMultiplayer && Quests[Q_MUSHROOM]._qactive == QUEST_INIT)
		Quests[Q_MUSHROOM]._qvar1 = QS_TOMESPAWNED;
}

void InitialiseQuestPools(uint32_t seed, Quest quests[])
{
	DiabloGenerator rng(seed);
	quests[rng.pickRandomlyAmong({ Q_SKELKING, Q_PWATER })]._qactive = QUEST_NOTAVAIL;

	if (seed == 988045466) {
		// If someone starts a new game at 1977-12-28 19:44:42 UTC or 2087-02-18 22:43:02 UTC
		//  vanilla Diablo ends up reading QuestGroup1[-2] here. Due to the way the data segment
		//  is laid out (at least in 1.09) this ends up reading the address of the string
		//  "A Dark Passage" and trying to write to Quests[<addr>*8] which lands in read-only memory.
		// The proper result would've been to mark The Butcher unavailable but instead nothing happens.
		rng.discardRandomValues(1);
	} else {
		quests[rng.pickRandomlyAmong({ Q_BUTCHER, Q_LTBANNER, Q_GARBUD })]._qactive = QUEST_NOTAVAIL;
	}

	quests[rng.pickRandomlyAmong({ Q_BLIND, Q_ROCK, Q_BLOOD })]._qactive = QUEST_NOTAVAIL;

	quests[rng.pickRandomlyAmong({ Q_MUSHROOM, Q_ZHAR, Q_ANVIL })]._qactive = QUEST_NOTAVAIL;

	quests[rng.pickRandomlyAmong({ Q_VEIL, Q_WARLORD })]._qactive = QUEST_NOTAVAIL;
}

/* ---- NDS-SUCUBOS ---- */
std::string NdsAvisoTxt;
int NdsAvisoTicks = 0;
std::string NdsSucRuta;
int NdsSucEstado = 0;
bool NdsLuzActiva = false;
bool NdsAndarielVista = false;

void NdsAviso(std::string_view s, int ticks)
{
	NdsAvisoTxt = std::string(s);
	NdsAvisoTicks = ticks;
#ifdef __N64__
	InitDiabloMsg(s, static_cast<uint32_t>(ticks) * 50); /* N64: el aviso sale en el centro de la pantalla (no hay cuadro de HUD) */
#endif
}

const char *NdsHudAvisoTexto()
{
	return NdsAvisoTicks > 0 ? NdsAvisoTxt.c_str() : nullptr;
}

int NdsSucubosEstado()
{
	if (MyPlayer == nullptr)
		return 0;
	const std::string ruta = StrCat(paths::PrefPath(), "nds_sucubos_", gSaveNumber, "_", MyPlayer->_pName, ".txt");
	if (ruta != NdsSucRuta) {
		NdsSucRuta = ruta;
		NdsSucEstado = 0;
		FILE *f = std::fopen(ruta.c_str(), "rb");
		if (f != nullptr) {
			int v = 0;
			if (std::fscanf(f, "%d", &v) == 1)
				NdsSucEstado = v;
			std::fclose(f);
		}
	}
	return NdsSucEstado;
}

void NdsSucubosGuardar(int e)
{
	NdsSucubosEstado();
	NdsSucEstado = e;
	FILE *f = std::fopen(NdsSucRuta.c_str(), "wb");
	if (f != nullptr) {
		std::fprintf(f, "%d\n", e);
		std::fclose(f);
	}
}

bool NdsAndarielViva()
{
	const UniqueMonsterType id = NdsAndarielId();
	if (id == UniqueMonsterType::None)
		return false;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &m = Monsters[ActiveMonsters[i]];
		if (m.uniqueType == id && m.mode != MonsterMode::Death && m.hitPoints > 0)
			return true;
	}
	return false;
}

void NdsBosqueAlCargar(const Player &myPlayer)
{
	NdsLuzActiva = false;
	NdsAndarielVista = false;
	if (NdsBosqueLuz.x <= 0)
		return;
	const int e = NdsSucubosEstado();
	if (e == 2) /* NDS-LUCES: 3 = aceptada (no terminada) */
		return;
	if (e == 1 && NdsAndarielViva()) {
		NdsAndarielVista = true;
		return;
	}
	AddMissile(NdsBosqueLuz, NdsBosqueLuz, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, myPlayer, 0, 0);
	NdsLuzActiva = true;
}

void NdsBosqueSucubos(const Player &me)
{
	if (NdsLuzActiva) {
		const Point p = me.position.tile;
		const int dx = p.x > NdsBosqueLuz.x ? p.x - NdsBosqueLuz.x : NdsBosqueLuz.x - p.x;
		const int dy = p.y > NdsBosqueLuz.y ? p.y - NdsBosqueLuz.y : NdsBosqueLuz.y - p.y;
		if (dx <= 1 && dy <= 1) {
			NdsLuzActiva = false;
			for (Missile &m : Missiles) {
				if (m._mitype == MissileID::RedPortal && m.position.tile == NdsBosqueLuz)
					m._miDelFlag = true;
			}
			static const int8_t off[][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 },
				{ 2, 0 }, { 0, 2 }, { -2, 0 }, { 0, -2 }, { 2, 2 }, { -2, -2 }, { 2, -2 }, { -2, 2 } };
			Point sitio = NdsBosqueLuz;
			for (const auto &o : off) {
				const Point q = NdsBosqueLuz + Displacement { o[0], o[1] };
				if (InDungeonBounds(q) && !IsTileSolid(q) && dMonster[q.x][q.y] == 0 && dPlayer[q.x][q.y] == 0) {
					sitio = q;
					break;
				}
			}
			if (NdsSpawnAndariel(sitio)) {
				NdsSucubosGuardar(1);
				me.Say(HeroSpeech::TimeToDie); /* v0.9.0 (Belial 64): frase sin usar del juego */
				NdsAndarielVista = true;
				NdsAviso(_("Andariel has awakened!"), 80);
			}
		}
	}
	if (NdsSucubosEstado() == 1) {
		if (NdsAndarielViva()) {
			NdsAndarielVista = true;
		} else if (NdsAndarielVista) {
			NdsAndarielVista = false;
			MyPlayer->_pStatPts += 5;
			NdsSucubosGuardar(2);
			MyPlayer->Say(HeroSpeech::YoullLureNoMoreMenToTheirDeaths, 30); /* v0.9.0 (Belial 64) */
			NdsAviso(_("Andariel defeated: +5 stat points"), 160);
		}
	}
}
/* ---- fin NDS-SUCUBOS ---- */

bool NdsTownerTieneMision(int ttype) /* NDS-LUCES: "!" encima del NPC */
{
	if (MyPlayer == nullptr)
		return false;
	switch (ttype) {
	case TOWN_BMAID:
		return !gbIsMultiplayer && MyPlayer->getCharacterLevel() >= 18 && NdsSucubosEstado() == 0;
	case TOWN_DEADGUY: {
		const Quest &q = Quests[Q_BUTCHER];
		return q._qactive != QUEST_NOTAVAIL && q._qactive != QUEST_DONE && q._qvar1 != 1;
	}
	case TOWN_FARMER: {
		const Quest &q = Quests[Q_FARMER];
		return (q._qactive == QUEST_INIT || q._qactive == QUEST_NOTAVAIL) && (MyPlayer->_pLvlVisited[9] || MyPlayer->getCharacterLevel() >= 15);
	}
	case TOWN_GIRL: {
		const Quest &q = Quests[Q_GIRL];
		return q._qactive == QUEST_INIT || q._qactive == QUEST_NOTAVAIL;
	}
	default:
		return false;
	}
}

void CheckQuests()
{
	{ /* NDS-BOSQUE: borde oeste del pueblo <-> bosque (solo un jugador) */
		const Player &me = *MyPlayer;
		if (NdsAvisoTicks > 0) /* NDS-SUCUBOS */
			NdsAvisoTicks--;
		if (!gbIsMultiplayer && NdsBosque())
			NdsBosqueSucubos(me);
		if (!gbIsMultiplayer && !setlevel && leveltype == DTYPE_TOWN) { /* NDS-BOSQUE4: aviso al acercarte a la entrada */
			static bool ndsAvisoBosque = false;
			const int dx = me.position.tile.x - 10;
			const int dy = me.position.tile.y - 52;
			const int ax = dx < 0 ? -dx : dx;
			const int ay = dy < 0 ? -dy : dy;
			const int dist = ax > ay ? ax : ay;
			if (dist <= 4 && !ndsAvisoBosque) {
				ndsAvisoBosque = true;
				/* NDS-BOSQUE6: sin recuadro grande; el texto sale en el cuadro del HUD */
			} else if (dist >= 10) {
				ndsAvisoBosque = false;
			}
		}
		if (NdsBosque() && !NdsBosqueSalidaLista) { /* NDS-BOSQUE3: la salida se activa al alejarte del Inicio */
			const Point p = me.position.tile;
			if (p.x - NdsBosqueInicio.x >= 4 || NdsBosqueInicio.x - p.x >= 4 || p.y - NdsBosqueInicio.y >= 4 || NdsBosqueInicio.y - p.y >= 4)
				NdsBosqueSalidaLista = true;
		}
		if (!gbIsMultiplayer && me._pmode == PM_STAND) {
			const Point t = me.position.tile;
			if (!setlevel && leveltype == DTYPE_TOWN && t.x <= 10 && t.y >= 49 && t.y <= 54 && me.getCharacterLevel() < 18) /* NDS-LUCES: nivel 18 */
				NdsAviso(_("You need level 18 to enter the Forest"), 40);
			if (!setlevel && leveltype == DTYPE_TOWN && t.x <= 10 && t.y >= 49 && t.y <= 54 && me.getCharacterLevel() >= 18 && NdsSucubosEstado() == 0) /* NDS-LUCES */
				NdsAviso(_("Talk to Gillian first"), 40);
			if (!setlevel && leveltype == DTYPE_TOWN && t.x <= 10 && t.y >= 49 && t.y <= 54 && me.getCharacterLevel() >= 18 && NdsSucubosEstado() != 0) {
				setlvltype = DTYPE_CATHEDRAL;
				StartNewLvl(*MyPlayer, WM_DIABSETLVL, SL_BOSQUE);
				return;
			}
			if (NdsBosque() && NdsBosqueW > 0 /* NDS-BOSQUE5: salida = casillas con la marca 5 */
			    && ((NdsBosqueSalidaB.x >= NdsBosqueSalidaA.x
			            && t.x >= NdsBosqueSalidaA.x && t.x <= NdsBosqueSalidaB.x && t.y >= NdsBosqueSalidaA.y && t.y <= NdsBosqueSalidaB.y)
			        || (NdsBosqueSalidaB.x < NdsBosqueSalidaA.x && NdsBosqueSalidaLista /* NDS-BOSQUE3: sin marca 5, volver al Inicio */
			            && t.x - NdsBosqueInicio.x <= 1 && NdsBosqueInicio.x - t.x <= 1 && t.y - NdsBosqueInicio.y <= 1 && NdsBosqueInicio.y - t.y <= 1))) {
				StartNewLvl(*MyPlayer, WM_DIABRTNLVL, 0);
				return;
			}
		}
	}
	if (gbIsSpawn)
		return;

	auto &quest = Quests[Q_BETRAYER];
	if (quest.IsAvailable() && UseMultiplayerQuests() && quest._qvar1 == 2) {
		AddObject(OBJ_ALTBOY, SetPiece.position.megaToWorld() + Displacement { 4, 6 });
		quest._qvar1 = 3;
		NetSendCmdQuest(true, quest);
	}

	{ /* NDS-GUARIDA9: portal rojo a la guarida del Carnicero */
		Quest &butcherQuest = Quests[Q_BUTCHER];
		if (!setlevel && currlevel == butcherQuest._qlevel && butcherQuest._qvar2 >= 1 && butcherQuest._qactive != QUEST_DONE /* NDS-GUARIDA10 */
		    && butcherQuest.position != Point { 0, 0 }) {
			bool shown = false;
			for (auto &m : Missiles) {
				if (m._mitype == MissileID::RedPortal && m.position.tile == butcherQuest.position) {
					shown = true;
					break;
				}
			}
			if (!shown)
				AddMissile(butcherQuest.position, butcherQuest.position, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, *MyPlayer, 0, 0);
			if (MyPlayer->_pmode == PM_STAND && MyPlayer->position.tile == butcherQuest.position) {
				setlvltype = DTYPE_CATHEDRAL;
				StartNewLvl(*MyPlayer, WM_DIABSETLVL, SL_BUTCHERLAIR);
				return;
			}
		}
	}
	if (UseMultiplayerQuests()) {
		return;
	}

	if (currlevel == quest._qlevel
	    && !setlevel
	    && quest._qvar1 >= 2
	    && (quest._qactive == QUEST_ACTIVE || quest._qactive == QUEST_DONE)
	    && (quest._qvar2 == 0 || quest._qvar2 == 2)) {
		// Spawn a portal at the quest trigger location
		AddMissile(quest.position, quest.position, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, *MyPlayer, 0, 0);
		quest._qvar2 = 1;
		if (quest._qactive == QUEST_ACTIVE && quest._qvar1 == 2) {
			quest._qvar1 = 3;
		}
	}

	if (quest._qactive == QUEST_DONE
	    && setlevel
	    && setlvlnum == SL_VILEBETRAYER
	    && quest._qvar2 == 4) {
		const Point portalLocation { 35, 32 };
		AddMissile(portalLocation, portalLocation, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, *MyPlayer, 0, 0);
		quest._qvar2 = 3;
	}

	if (setlevel) {
		Quest &poisonWater = Quests[Q_PWATER];
		if (setlvlnum == poisonWater._qslvl
		    && poisonWater._qactive != QUEST_INIT
		    && leveltype == poisonWater._qlvltype
		    && ActiveMonsterCount == 4
		    && poisonWater._qactive != QUEST_DONE) {
			poisonWater._qactive = QUEST_DONE;
			poisonWater._qlog = true; // even if the player skips talking to Pepin completely they should at least notice the water being purified once they cleanse the level
			NetSendCmdQuest(true, poisonWater);
			StartPWaterPurify();
		}
	} else if (MyPlayer->_pmode == PM_STAND) {
		for (auto &currentQuest : Quests) {
			if (currlevel == currentQuest._qlevel
			    && currentQuest._qslvl != 0
			    && currentQuest._qactive != QUEST_NOTAVAIL
			    && MyPlayer->position.tile == currentQuest.position
			    && (currentQuest._qidx != Q_BETRAYER || currentQuest._qvar1 >= 3)) {
				if (currentQuest._qlvltype != DTYPE_NONE) {
					setlvltype = currentQuest._qlvltype;
				}
				StartNewLvl(*MyPlayer, WM_DIABSETLVL, currentQuest._qslvl);
			}
		}
	}
}

bool ForceQuests()
{
	if (gbIsSpawn)
		return false;

	if (UseMultiplayerQuests()) {
		return false;
	}

	for (auto &quest : Quests) {
		if (quest._qidx != Q_BETRAYER && currlevel == quest._qlevel && quest._qslvl != 0) {
			const int ql = quest._qslvl - 1;

			if (EntranceBoundaryContains(quest.position, cursPosition)) {
				InfoString = FormatRuntime(_(/* TRANSLATORS: Used for Quest Portals. {:s} is a Map Name */ "To {:s}"), _(QuestTriggerNames[ql]));
				cursPosition = quest.position;
				return true;
			}
		}
	}

	return false;
}

void CheckQuestKill(const Monster &monster, bool sendmsg)
{
	if (gbIsSpawn)
		return;

	const Player &myPlayer = *MyPlayer;

	if (monster.type().type == MT_SKING) {
		auto &quest = Quests[Q_SKELKING];
		quest._qactive = QUEST_DONE;
		myPlayer.Say(HeroSpeech::RestWellLeoricIllFindYourSon, 30);
		if (sendmsg)
			NetSendCmdQuest(true, quest);

	} else if (monster.type().type == MT_CLEAVER) {
		auto &quest = Quests[Q_BUTCHER];
		quest._qactive = QUEST_DONE;
		myPlayer.Say(HeroSpeech::TheSpiritsOfTheDeadAreNowAvenged, 30);
		if (sendmsg)
			NetSendCmdQuest(true, quest);
	} else if (monster.uniqueType == UniqueMonsterType::Garbud) { //"Gharbad the Weak"
		Quests[Q_GARBUD]._qactive = QUEST_DONE;
		NetSendCmdQuest(true, Quests[Q_GARBUD]);
		myPlayer.Say(HeroSpeech::ImNotImpressed, 30);
	} else if (monster.uniqueType == UniqueMonsterType::Zhar) { //"Zhar the Mad"
		Quests[Q_ZHAR]._qactive = QUEST_DONE;
		NetSendCmdQuest(true, Quests[Q_ZHAR]);
		myPlayer.Say(HeroSpeech::ImSorryDidIBreakYourConcentration, 30);
	} else if (monster.uniqueType == UniqueMonsterType::Lazarus) { //"Arch-Bishop Lazarus"
		auto &betrayerQuest = Quests[Q_BETRAYER];
		betrayerQuest._qactive = QUEST_DONE;
		myPlayer.Say(HeroSpeech::YourMadnessEndsHereBetrayer, 30);
		betrayerQuest._qvar1 = 7;
		auto &diabloQuest = Quests[Q_DIABLO];
		diabloQuest._qactive = QUEST_ACTIVE;

		if (UseMultiplayerQuests()) {
			for (WorldTileCoord j = 0; j < MAXDUNY; j++) {
				for (WorldTileCoord i = 0; i < MAXDUNX; i++) {
					if (dPiece[i][j] == 369) {
						trigs[numtrigs].position = { i, j };
						trigs[numtrigs]._tmsg = WM_DIABNEXTLVL;
						numtrigs++;
					}
				}
			}
		} else {
			InitVPTriggers();
			betrayerQuest._qvar2 = 4;
			AddMissile({ 35, 32 }, { 35, 32 }, Direction::South, MissileID::RedPortal, TARGET_MONSTERS, myPlayer, 0, 0);
		}
		if (sendmsg) {
			NetSendCmdQuest(true, betrayerQuest);
			NetSendCmdQuest(true, diabloQuest);
		}
	} else if (monster.uniqueType == UniqueMonsterType::WarlordOfBlood) {
		Quests[Q_WARLORD]._qactive = QUEST_DONE;
		NetSendCmdQuest(true, Quests[Q_WARLORD]);
		myPlayer.Say(HeroSpeech::YourReignOfPainHasEnded, 30);
	}
}

int GetMapReturnLevel()
{
	switch (setlvlnum) {
	case SL_SKELKING:
		return Quests[Q_SKELKING]._qlevel;
	case SL_BONECHAMB:
		return Quests[Q_SCHAMB]._qlevel;
	case SL_POISONWATER:
		return Quests[Q_PWATER]._qlevel;
	case SL_VILEBETRAYER:
		return Quests[Q_BETRAYER]._qlevel;
	case SL_BUTCHERLAIR: /* NDS-GUARIDA */
		return Quests[Q_BUTCHER]._qlevel;
	default:
		return 0;
	}
}

Point GetMapReturnPosition()
{
#ifdef _DEBUG
	if (!TestMapPath.empty())
		return ViewPosition;
#endif

	switch (setlvlnum) {
	case SL_SKELKING:
		return Quests[Q_SKELKING].position + Direction::SouthEast;
	case SL_BONECHAMB:
		return Quests[Q_SCHAMB].position + Direction::SouthEast;
	case SL_POISONWATER:
		return Quests[Q_PWATER].position + Direction::SouthWest;
	case SL_VILEBETRAYER:
		return Quests[Q_BETRAYER].position + Direction::South;
	case SL_BUTCHERLAIR: /* NDS-GUARIDA9: vuelta junto al portal rojo, no encima */
		return Quests[Q_BUTCHER].position + Direction::South;
	case SL_BOSQUE: /* NDS-BOSQUE: vuelta al borde oeste del pueblo */
		return { 12, 52 };
	default:
		return GetTowner(TOWN_DRUNK)->position + Direction::SouthEast;
	}
}

void LoadPWaterPalette()
{
	if (!setlevel || setlvlnum != Quests[Q_PWATER]._qslvl || Quests[Q_PWATER]._qactive == QUEST_INIT || leveltype != Quests[Q_PWATER]._qlvltype)
		return;

	if (Quests[Q_PWATER]._qactive == QUEST_DONE)
		LoadPaletteAndInitBlending("levels\\l3data\\l3pwater.pal");
	else
		LoadPaletteAndInitBlending("levels\\l3data\\l3pfoul.pal");
}

void UpdatePWaterPalette()
{
	if (WaterDone > 0) {
		// `WaterDone` is in [1, 32], so `colorIndex` is in [0, 31].
		const unsigned colorIndex = 32 - WaterDone;
		SetLogicalPaletteColor(colorIndex, PureWaterPalette[colorIndex].toSDL());
		WaterDone--;
		return;
	}
	palette_update_caves();
}

void ResyncMPQuests()
{
	if (gbIsSpawn)
		return;

	auto &kingQuest = Quests[Q_SKELKING];
	if (kingQuest._qactive == QUEST_INIT
	    && currlevel >= kingQuest._qlevel - 1
	    && currlevel <= kingQuest._qlevel + 1) {
		kingQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, kingQuest);
	}

	auto &butcherQuest = Quests[Q_BUTCHER];
	if (butcherQuest._qactive == QUEST_INIT
	    && currlevel >= butcherQuest._qlevel - 1
	    && currlevel <= butcherQuest._qlevel + 1) {
		butcherQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, butcherQuest);
	}

	auto &betrayerQuest = Quests[Q_BETRAYER];
	if (betrayerQuest._qactive == QUEST_INIT && currlevel == betrayerQuest._qlevel - 1) {
		betrayerQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, betrayerQuest);
	}
	if (betrayerQuest.IsAvailable())
		AddObject(OBJ_ALTBOY, SetPiece.position.megaToWorld() + Displacement { 4, 6 });

	auto &cryptQuest = Quests[Q_GRAVE];
	if (cryptQuest._qactive == QUEST_INIT && currlevel == cryptQuest._qlevel - 1) {
		cryptQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, cryptQuest);
	}

	auto &defilerQuest = Quests[Q_DEFILER];
	if (defilerQuest._qactive == QUEST_INIT && currlevel == defilerQuest._qlevel - 1) {
		defilerQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, defilerQuest);
	}

	auto &nakrulQuest = Quests[Q_NAKRUL];
	if (nakrulQuest._qactive == QUEST_INIT && currlevel == nakrulQuest._qlevel - 1) {
		nakrulQuest._qactive = QUEST_ACTIVE;
		NetSendCmdQuest(true, nakrulQuest);
	}
}

void ResyncQuests()
{
	if (gbIsSpawn)
		return;

	LoadingMapObjects = true;

	if (Quests[Q_LTBANNER].IsAvailable()) {
		Monster *snotSpill = FindUniqueMonster(UniqueMonsterType::SnotSpill);
		if (Quests[Q_LTBANNER]._qvar1 == 1) {
			ObjChangeMapResync(
			    SetPiece.position.x + SetPiece.size.width - 2,
			    SetPiece.position.y + SetPiece.size.height - 2,
			    SetPiece.position.x + SetPiece.size.width + 1,
			    SetPiece.position.y + SetPiece.size.height + 1);
		}
		if (Quests[Q_LTBANNER]._qvar1 == 2) {
			ObjChangeMapResync(
			    SetPiece.position.x + SetPiece.size.width - 2,
			    SetPiece.position.y + SetPiece.size.height - 2,
			    SetPiece.position.x + SetPiece.size.width + 1,
			    SetPiece.position.y + SetPiece.size.height + 1);
			ObjChangeMapResync(SetPiece.position.x, SetPiece.position.y, SetPiece.position.x + (SetPiece.size.width / 2) + 2, SetPiece.position.y + (SetPiece.size.height / 2) - 2);
			for (int i = 0; i < ActiveObjectCount; i++)
				SyncObjectAnim(Objects[ActiveObjects[i]]);
			auto tren = TransVal;
			TransVal = 9;
			DRLG_MRectTrans({ SetPiece.position, WorldTileSize((SetPiece.size.width / 2) + 4, SetPiece.size.height / 2) });
			TransVal = tren;
			if (gbIsMultiplayer && snotSpill != nullptr && snotSpill->talkMsg != TEXT_BANNER12) {
				snotSpill->goal = MonsterGoal::Inquiring;
				snotSpill->talkMsg = Quests[Q_LTBANNER]._qactive == QUEST_DONE ? TEXT_BANNER12 : TEXT_BANNER11;
				snotSpill->flags |= MFLAG_QUEST_COMPLETE;
			}
		}
		if (Quests[Q_LTBANNER]._qvar1 == 3) {
			ObjChangeMapResync(SetPiece.position.x, SetPiece.position.y, SetPiece.position.x + SetPiece.size.width + 1, SetPiece.position.y + SetPiece.size.height + 1);
			for (int i = 0; i < ActiveObjectCount; i++)
				SyncObjectAnim(Objects[ActiveObjects[i]]);
			auto tren = TransVal;
			TransVal = 9;
			DRLG_MRectTrans({ SetPiece.position, WorldTileSize((SetPiece.size.width / 2) + 4, SetPiece.size.height / 2) });
			TransVal = tren;
			if (gbIsMultiplayer && snotSpill != nullptr) {
				snotSpill->goal = MonsterGoal::Normal;
				snotSpill->flags |= MFLAG_QUEST_COMPLETE;
				snotSpill->talkMsg = TEXT_NONE;
				snotSpill->activeForTicks = UINT8_MAX;
				RedoPlayerVision();
			}
		}
	}
	if (currlevel == Quests[Q_MUSHROOM]._qlevel && !setlevel) {
		if (Quests[Q_MUSHROOM]._qactive == QUEST_INIT && Quests[Q_MUSHROOM]._qvar1 == QS_INIT) {
			SpawnQuestItem(IDI_FUNGALTM, { 0, 0 }, 5, SelectionRegion::Bottom, true);
			Quests[Q_MUSHROOM]._qvar1 = QS_TOMESPAWNED;
			NetSendCmdQuest(true, Quests[Q_MUSHROOM]);
		} else {
			if (Quests[Q_MUSHROOM]._qactive == QUEST_ACTIVE) {
				if (Quests[Q_MUSHROOM]._qvar1 >= QS_MUSHGIVEN) {
					SetTownerQuestDialog(TOWN_WITCH, Q_MUSHROOM, TEXT_NONE);
					SetTownerQuestDialog(TOWN_HEALER, Q_MUSHROOM, TEXT_MUSH3);
				} else if (Quests[Q_MUSHROOM]._qvar1 >= QS_BRAINGIVEN) {
					SetTownerQuestDialog(TOWN_HEALER, Q_MUSHROOM, TEXT_NONE);
				}
			}
		}
	}
	if (currlevel == Quests[Q_VEIL]._qlevel + 1 && Quests[Q_VEIL]._qactive == QUEST_ACTIVE && Quests[Q_VEIL]._qvar1 == 0 && !gbIsMultiplayer) {
		Quests[Q_VEIL]._qvar1 = 1;
		SpawnQuestItem(IDI_GLDNELIX, { 0, 0 }, 5, SelectionRegion::Bottom, true);
		NetSendCmdQuest(true, Quests[Q_VEIL]);
	}
	if (setlevel && setlvlnum == SL_VILEBETRAYER) {
		if (Quests[Q_BETRAYER]._qvar1 >= 4)
			ObjChangeMapResync(1, 11, 20, 18);
		if (Quests[Q_BETRAYER]._qvar1 >= 6) {
			ObjChangeMapResync(1, 18, 20, 24);
			if (gbIsMultiplayer) {
				Monster *lazarus = FindUniqueMonster(UniqueMonsterType::Lazarus);
				if (lazarus != nullptr) {
					// Ensure lazarus starts attacking again after returning to the level
					lazarus->goal = MonsterGoal::Normal;
					lazarus->talkMsg = TEXT_NONE;
				}
			}
		}
		if (Quests[Q_BETRAYER]._qvar1 >= 7)
			InitVPTriggers();
		for (int i = 0; i < ActiveObjectCount; i++)
			SyncObjectAnim(Objects[ActiveObjects[i]]);
	}
	if (currlevel == Quests[Q_BETRAYER]._qlevel
	    && !setlevel
	    && (Quests[Q_BETRAYER]._qvar2 == 1 || Quests[Q_BETRAYER]._qvar2 >= 3)
	    && (Quests[Q_BETRAYER]._qactive == QUEST_ACTIVE || Quests[Q_BETRAYER]._qactive == QUEST_DONE)) {
		Quests[Q_BETRAYER]._qvar2 = 2;
		NetSendCmdQuest(true, Quests[Q_BETRAYER]);
	}
	if (currlevel == Quests[Q_DIABLO]._qlevel
	    && !setlevel
	    && Quests[Q_DIABLO]._qactive == QUEST_ACTIVE
	    && gbIsMultiplayer) {
		const Point posPentagram = Quests[Q_DIABLO].position;
		ObjChangeMapResync(posPentagram.x, posPentagram.y, posPentagram.x + 5, posPentagram.y + 5);
		InitL4Triggers();
	}
	if (currlevel == 0
	    && Quests[Q_PWATER]._qactive == QUEST_DONE
	    && gbIsMultiplayer) {
		CleanTownFountain();
	}
	if (Quests[Q_GARBUD].IsAvailable() && gbIsMultiplayer) {
		Monster *garbud = FindUniqueMonster(UniqueMonsterType::Garbud);
		if (garbud != nullptr && Quests[Q_GARBUD]._qvar1 != QS_GHARBAD_INIT) {
			switch (Quests[Q_GARBUD]._qvar1) {
			case QS_GHARBAD_FIRST_ITEM_READY:
				garbud->goal = MonsterGoal::Inquiring;
				break;
			case QS_GHARBAD_FIRST_ITEM_SPAWNED:
				garbud->talkMsg = TEXT_GARBUD2;
				garbud->flags |= MFLAG_QUEST_COMPLETE;
				garbud->goal = MonsterGoal::Talking;
				break;
			case QS_GHARBAD_SECOND_ITEM_NEARLY_DONE:
				garbud->talkMsg = TEXT_GARBUD3;
				garbud->flags |= MFLAG_QUEST_COMPLETE;
				garbud->goal = MonsterGoal::Inquiring;
				break;
			case QS_GHARBAD_SECOND_ITEM_READY:
				garbud->talkMsg = TEXT_GARBUD4;
				garbud->flags |= MFLAG_QUEST_COMPLETE;
				garbud->goal = MonsterGoal::Inquiring;
				break;
			case QS_GHARBAD_ATTACKING:
				garbud->talkMsg = TEXT_NONE;
				garbud->flags |= MFLAG_QUEST_COMPLETE;
				garbud->goal = MonsterGoal::Normal;
				garbud->activeForTicks = UINT8_MAX;
				break;
			}
		}
	}
	if (Quests[Q_ZHAR].IsAvailable() && gbIsMultiplayer) {
		Monster *zhar = FindUniqueMonster(UniqueMonsterType::Zhar);
		if (zhar != nullptr && Quests[Q_ZHAR]._qvar1 != QS_ZHAR_INIT) {
			zhar->flags |= MFLAG_QUEST_COMPLETE;

			switch (Quests[Q_ZHAR]._qvar1) {
			case QS_ZHAR_ITEM_SPAWNED:
				zhar->goal = MonsterGoal::Talking;
				break;
			case QS_ZHAR_ANGRY:
				zhar->talkMsg = TEXT_ZHAR2;
				zhar->goal = MonsterGoal::Inquiring;
				break;
			case QS_ZHAR_ATTACKING:
				zhar->talkMsg = TEXT_NONE;
				zhar->goal = MonsterGoal::Normal;
				zhar->activeForTicks = UINT8_MAX;
				break;
			}
		}
	}
	if (Quests[Q_WARLORD].IsAvailable() && gbIsMultiplayer) {
		Monster *warlord = FindUniqueMonster(UniqueMonsterType::WarlordOfBlood);
		if (warlord != nullptr && Quests[Q_WARLORD]._qvar1 == QS_WARLORD_ATTACKING) {
			warlord->activeForTicks = UINT8_MAX;
			warlord->talkMsg = TEXT_NONE;
			warlord->goal = MonsterGoal::Normal;
		}
	}
	if (Quests[Q_VEIL].IsAvailable() && gbIsMultiplayer) {
		Monster *lachdan = FindUniqueMonster(UniqueMonsterType::Lachdan);
		if (lachdan != nullptr) {
			switch (Quests[Q_VEIL]._qvar2) {
			case QS_VEIL_EARLY_RETURN:
				lachdan->talkMsg = TEXT_VEIL10;
				lachdan->goal = MonsterGoal::Inquiring;
				break;
			case QS_VEIL_ITEM_SPAWNED:
				if (lachdan->talkMsg == TEXT_VEIL11)
					break;
				lachdan->talkMsg = TEXT_VEIL11;
				lachdan->flags |= MFLAG_QUEST_COMPLETE;
				lachdan->goal = MonsterGoal::Inquiring;
				break;
			}
		}
	}

	LoadingMapObjects = false;
}

void SetMultiQuest(int q, quest_state s, bool log, int v1, int v2, int16_t qmsg)
{
	if (gbIsSpawn)
		return;

	auto &quest = Quests[q];
	const quest_state oldQuestState = quest._qactive;
	if (quest._qactive != QUEST_DONE) {
		if (s > quest._qactive || (IsAnyOf(s, QUEST_ACTIVE, QUEST_DONE) && IsAnyOf(quest._qactive, QUEST_HIVE_TEASE1, QUEST_HIVE_TEASE2, QUEST_HIVE_ACTIVE)))
			quest._qactive = s;
		if (log)
			quest._qlog = true;
	}
	if (v1 > quest._qvar1)
		quest._qvar1 = v1;
	quest._qvar2 = v2;
	quest._qmsg = static_cast<_speech_id>(qmsg);
	if (!UseMultiplayerQuests()) {
		// Ensure that changes on another client is also updated on our own
		ResyncQuests();

		const bool questGotCompleted = oldQuestState != QUEST_DONE && quest._qactive == QUEST_DONE;
		// Ensure that water also changes for remote players
		if (quest._qidx == Q_PWATER && questGotCompleted && MyPlayer->isOnLevel(quest._qslvl))
			StartPWaterPurify();
		if (quest._qidx == Q_GIRL && questGotCompleted && MyPlayer->isOnLevel(0))
			UpdateGirlAnimAfterQuestComplete();
		if (quest._qidx == Q_JERSEY && questGotCompleted && MyPlayer->isOnLevel(0))
			UpdateCowFarmerAnimAfterQuestComplete();
	}
}

} // namespace devilution
