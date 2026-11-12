/**
 * @file dead.cpp
 *
 * Implementation of functions for placing dead monsters.
 */
#include "dead.h"

#include <cstdint>

#include "diablo.h"
#include "headless_mode.hpp"
#include "levels/gendung.h"
#include "lighting.h"
#include "monster.h"
#include "tables/misdat.h"

namespace devilution {

Corpse Corpses[MaxCorpses];
static int8_t NdsCorpsesUsed = 0; /* NDS-LUCES2: huecos de cadaver ocupados */
int8_t stonendx;

namespace {
void InitDeadAnimationFromMonster(Corpse &corpse, const CMonster &mon)
{
	const AnimStruct &animData = mon.getAnimData(MonsterGraphic::Death);
	if (animData.sprites) {
		corpse.sprites.emplace(*animData.sprites);
	} else {
		corpse.sprites = std::nullopt;
	}
	corpse.frame = animData.frames - 1;
	corpse.width = animData.width;
}

void MoveLightToCorpse(Monster &monster)
{
	for (int dx = 0; dx < MAXDUNX; dx++) {
		for (int dy = 0; dy < MAXDUNY; dy++) {
			if ((dCorpse[dx][dy] & 0x1F) == monster.corpseId) {
				ChangeLightXY(monster.lightId, { dx, dy });
				return;
			}
		}
	}
	AddUnLight(monster.lightId);
}
} // namespace

void InitCorpses()
{
	int8_t mtypes[MaxMonsters] = {};

	int8_t nd = 0;

	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		CMonster &monsterType = LevelMonsterTypes[i];
		if (mtypes[monsterType.type] != 0)
			continue;

		InitDeadAnimationFromMonster(Corpses[nd], monsterType);
		Corpses[nd].translationPaletteIndex = 0;
		nd++;

		monsterType.corpseId = nd;
		mtypes[monsterType.type] = nd;
	}

	nd++; // Unused blood spatter

	if (!HeadlessMode) {
#ifdef __N64__
		(void)GetMissileSpriteData(MissileGraphicID::StoneCurseShatter).LoadGFX(); // misiles bajo demanda en N64
#endif
		Corpses[nd].sprites.emplace(*GetMissileSpriteData(MissileGraphicID::StoneCurseShatter).sprites);
	}
	Corpses[nd].frame = 11;
	Corpses[nd].width = 128;
	Corpses[nd].translationPaletteIndex = 0;
	nd++;

	stonendx = nd;

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		auto &monster = Monsters[ActiveMonsters[i]];
		if (monster.isUnique()) {
			InitDeadAnimationFromMonster(Corpses[nd], monster.type());
			Corpses[nd].translationPaletteIndex = ActiveMonsters[i] + 1;
			nd++;

			monster.corpseId = nd;
		}
	}

	assert(static_cast<unsigned>(nd) <= MaxCorpses);
	NdsCorpsesUsed = nd; /* NDS-LUCES2 */
}

int8_t NdsAddUniqueCorpse(Monster &monster) /* NDS-LUCES2: cadaver de un unico que aparece a mitad de nivel */
{
	if (NdsCorpsesUsed >= static_cast<int8_t>(MaxCorpses))
		return monster.type().corpseId; /* sin hueco: cadaver normal de su tipo */
	const int8_t nd = NdsCorpsesUsed;
	InitDeadAnimationFromMonster(Corpses[nd], monster.type());
	Corpses[nd].translationPaletteIndex = static_cast<uint8_t>(monster.getId() + 1);
	NdsCorpsesUsed++;
	return NdsCorpsesUsed;
}

void AddCorpse(Point tilePosition, int8_t dv, Direction ddir)
{
	dCorpse[tilePosition.x][tilePosition.y] = (dv & 0x1F) + (static_cast<int>(ddir) << 5);
}

void MoveLightsToCorpses()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		auto &monster = Monsters[ActiveMonsters[i]];
		if (!monster.isUnique())
			continue;
		MoveLightToCorpse(monster);
	}
}

} // namespace devilution
