/**
 * @file sound.h
 *
 * Interface of functions setting up the audio pipeline.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "levels/gendung.h"
#include "utils/attributes.h"

#ifndef NOSOUND
#ifdef USE_SDL3
struct MIX_Mixer;
#endif

#include "utils/soundsample.h"
#endif

namespace devilution {

enum _music_id : uint8_t {
	TMUSIC_TOWN,
	TMUSIC_CATHEDRAL,
	TMUSIC_CATACOMBS,
	TMUSIC_CAVES,
	TMUSIC_HELL,
	TMUSIC_NEST,
	TMUSIC_CRYPT,
	TMUSIC_INTRO,
	NUM_MUSIC,
};

struct TSnd {
	uint32_t start_tc;

#ifndef NOSOUND
	SoundSample DSB;

	bool isPlaying()
	{
		return DSB.IsPlaying();
	}
#elif defined(__N64__)
	// f2v48: sonido de la N64 (sdl_n64.c): el WAV se copia a la memoria del cartucho la primera vez
	// que suena y el mezclador de libdragon lo lee de alli. Imita lo que effects.cpp usa de SoundSample.
	struct N64Sample {
		std::string rel;          // ruta dentro del diabdat ("sfx\\misc\\walk1.wav")
		mutable std::string path; // ruta completa en la SD (se busca la primera vez que suena)
		mutable int8_t state = 0; // 0 = sin buscar, 1 = existe, -1 = no existe
		const char *Resolve() const;
		[[nodiscard]] bool IsLoaded() const { return state >= 0; }
		[[nodiscard]] bool IsPlaying() const;
		void Stop();
		void PlayWithVolumeAndPan(int logVolume, int logUserVolume, int logPan);
		[[nodiscard]] int GetLength() const { return 0; }
	} DSB;

	bool isPlaying()
	{
		return DSB.IsPlaying();
	}
#else
	bool isPlaying()
	{
		return false;
	}
#endif

	~TSnd();
};

extern bool gbSndInited;
#ifndef NOSOUND
#ifdef USE_SDL3
extern MIX_Mixer *CurrentMixer;
#endif
#endif

extern _music_id sgnMusicTrack;

void ClearDuplicateSounds();
void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume);
std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream = false);
std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool stream = false);
void snd_init();
void snd_deinit();
_music_id GetLevelMusic(dungeon_type dungeonType);
void music_stop();
void music_start(_music_id nTrack);
void sound_disable_music(bool disable);
int sound_get_or_set_music_volume(int volume);
int sound_get_or_set_sound_volume(int volume);
int SoundGetOrSetAudioCuesVolume(int volume);
void music_mute();
void music_unmute();

/* data */

extern DVL_API_FOR_TEST bool gbMusicOn;
extern DVL_API_FOR_TEST bool gbSoundOn;

} // namespace devilution
