// Stubbed implementations of sound functions for the NOSOUND mode.
#include "engine/sound.h"

namespace devilution {

bool gbSndInited;
bool gbMusicOn;
bool gbSoundOn;
_music_id sgnMusicTrack = NUM_MUSIC;

void ClearDuplicateSounds() { }
void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume) { }
std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream) { return nullptr; }
std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool stream) { return nullptr; }
TSnd::~TSnd() { }
void snd_init() { }
void snd_deinit() { }
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
void music_start(_music_id nTrack)
{
	if (nTrack >= NUM_MUSIC)
		return;
	sgnMusicTrack = nTrack;
	if (n64MusicDisabled)
		return;
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
int sound_get_or_set_sound_volume(int volume) { return 0; }
int SoundGetOrSetAudioCuesVolume(int volume) { return 0; }
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
