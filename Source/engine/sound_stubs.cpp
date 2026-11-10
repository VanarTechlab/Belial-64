// Stubbed implementations of sound functions for the NOSOUND mode.
#include "engine/sound.h"

#ifdef __N64__
#include <cmath>

#include <SDL.h>

#include "engine/assets.hpp"
#include "engine/sound_defs.hpp"
#endif

namespace devilution {

bool gbSndInited;
bool gbMusicOn;
bool gbSoundOn;
_music_id sgnMusicTrack = NUM_MUSIC;

#ifdef __N64__
// f2v48: efectos y voces. El WAV se copia de la SD a la memoria del cartucho la primera vez que
// suena (sdl_n64.c) y el mezclador lo lee de alli; en la RAM no se guarda nada del sonido.
extern "C" int n64_sfx_play(const char *path, float left, float right);
extern "C" int n64_sfx_is_playing(const char *path);
extern "C" void n64_sfx_stop(const char *path);
extern "C" void n64_sfx_stop_all(void);
namespace {
int n64SoundVolume = 0;       // VOLUME_MAX
int n64AudioCuesVolume = 0;
float N64Gain(int logVolume)
{
	// centesimas de dB (0 = maximo, -1600 = minimo)
	if (logVolume <= VOLUME_MIN)
		return 0.0F;
	if (logVolume >= 0)
		return 1.0F;
	return std::pow(10.0F, static_cast<float>(logVolume) / 2000.0F);
}
} // namespace
const char *TSnd::N64Sample::Resolve() const
{
	if (state == 0) {
		const AssetRef ref = FindAsset(rel);
		state = ref.ok() ? 1 : -1;
		if (ref.ok())
			path = ref.path;
	}
	return state > 0 ? path.c_str() : nullptr;
}
extern "C" int n64_sfx_preload(const char *path);
void TSnd::N64Sample::Preload() const
{
	if (Resolve() != nullptr)
		n64_sfx_preload(path.c_str());
}
bool TSnd::N64Sample::IsPlaying() const
{
	return state > 0 && n64_sfx_is_playing(path.c_str()) != 0;
}
void TSnd::N64Sample::Stop()
{
	if (state > 0)
		n64_sfx_stop(path.c_str());
}
void TSnd::N64Sample::PlayWithVolumeAndPan(int logVolume, int logUserVolume, int logPan)
{
	if (Resolve() == nullptr)
		return;
	const float g = N64Gain(logVolume) * N64Gain(logUserVolume);
	float left = g;
	float right = g;
	// el panorama atenua el lado contrario (centesimas de dB, como DirectSound)
	if (logPan > 0)
		left *= N64Gain(-logPan);
	else if (logPan < 0)
		right *= N64Gain(logPan);
	n64_sfx_play(path.c_str(), left, right);
}
void ClearDuplicateSounds() { }
void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume)
{
	if (pSnd == nullptr || !gbSoundOn)
		return;
	const uint32_t tc = SDL_GetTicks();
	if (tc - pSnd->start_tc < 80)
		return;
	pSnd->DSB.PlayWithVolumeAndPan(lVolume, userVolume, lPan);
	pSnd->start_tc = tc;
}
std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool stream)
{
	auto snd = std::make_unique<TSnd>();
	snd->start_tc = SDL_GetTicks() - 80 - 1;
	snd->DSB.rel = path;
	return snd;
}
std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream)
{
	return std::move(SoundFileLoadWithStatus(path, stream)).value();
}
TSnd::~TSnd()
{
	DSB.Stop();
}
void snd_init()
{
	gbSndInited = true;
	gbSoundOn = true;
	gbMusicOn = true;
}
void snd_deinit()
{
	n64_sfx_stop_all();
}
#else
void ClearDuplicateSounds() { }
void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume) { }
std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream) { return nullptr; }
std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool stream) { return nullptr; }
TSnd::~TSnd() { }
void snd_init() { }
void snd_deinit() { }
#endif
#ifdef __N64__
// N64 (f2v23): la musica la reproduce la capa sdl_n64.c (WAV del diabdat copiado a la
// memoria del cartucho y mezclado por el RSP). Los efectos de sonido siguen sin hacer.
extern "C" void n64_music_start(const char *path);
extern "C" void n64_music_stop(void);
extern "C" void n64_music_volume(int volume, int muted);
namespace {
const char *const N64MusicTracks[NUM_MUSIC] = {
	"music\\dtowne.wav",
	"music\\dlvla.wav",
	"music\\dlvlb.wav",
	"music\\dlvlc.wav",
	"music\\dlvld.wav",
	"music\\dlvlf.wav",
	"music\\dlvle.wav",
	"music\\dintro.wav",
};
int n64MusicVolume = 0; // VOLUME_MAX
bool n64MusicMuted = false;
bool n64MusicDisabled = false;
} // namespace
void music_stop()
{
	n64_music_stop();
	sgnMusicTrack = NUM_MUSIC;
}
extern bool N64InGame;
void music_start(_music_id nTrack)
{
	if (nTrack >= NUM_MUSIC)
		return;
	sgnMusicTrack = nTrack;
	if (n64MusicDisabled)
		return;
	// N64 (f2v36): sin musica en los menus (copiarla al cartucho tardaba 6-11 s cada vez)
	if (!N64InGame) {
		n64_music_stop();
		return;
	}
	n64_music_volume(n64MusicVolume, n64MusicMuted ? 1 : 0);
	n64_music_start(N64MusicTracks[nTrack]);
}
void sound_disable_music(bool disable)
{
	n64MusicDisabled = disable;
	if (disable) {
		n64_music_stop();
	} else if (sgnMusicTrack != NUM_MUSIC) {
		music_start(sgnMusicTrack);
	}
}
int sound_get_or_set_music_volume(int volume)
{
	if (volume == 1)
		return n64MusicVolume;
	n64MusicVolume = volume;
	n64_music_volume(n64MusicVolume, n64MusicMuted ? 1 : 0);
	return n64MusicVolume;
}
#else
void music_stop() { }
void music_start(_music_id nTrack) { }
void sound_disable_music(bool disable) { }
int sound_get_or_set_music_volume(int volume) { return 0; }
#endif
#ifdef __N64__
int sound_get_or_set_sound_volume(int volume)
{
	if (volume == 1)
		return n64SoundVolume;
	n64SoundVolume = volume;
	gbSoundOn = volume > VOLUME_MIN;
	return n64SoundVolume;
}
int SoundGetOrSetAudioCuesVolume(int volume)
{
	if (volume == 1)
		return n64AudioCuesVolume;
	n64AudioCuesVolume = volume;
	return n64AudioCuesVolume;
}
#else
int sound_get_or_set_sound_volume(int volume) { return 0; }
int SoundGetOrSetAudioCuesVolume(int volume) { return 0; }
#endif
#ifdef __N64__
void music_mute()
{
	n64MusicMuted = true;
	n64_music_volume(n64MusicVolume, 1);
}
void music_unmute()
{
	n64MusicMuted = false;
	n64_music_volume(n64MusicVolume, 0);
}
_music_id GetLevelMusic(dungeon_type dungeonType)
{
	switch (dungeonType) {
	case DTYPE_TOWN: return TMUSIC_TOWN;
	case DTYPE_CATHEDRAL: return TMUSIC_CATHEDRAL;
	case DTYPE_CATACOMBS: return TMUSIC_CATACOMBS;
	case DTYPE_CAVES: return TMUSIC_CAVES;
	case DTYPE_HELL: return TMUSIC_HELL;
	case DTYPE_NEST: return TMUSIC_NEST;
	case DTYPE_CRYPT: return TMUSIC_CRYPT;
	default: return TMUSIC_INTRO;
	}
}
#else
void music_mute() { }
void music_unmute() { }
_music_id GetLevelMusic(dungeon_type dungeonType) { return TMUSIC_TOWN; }
#endif

} // namespace devilution
