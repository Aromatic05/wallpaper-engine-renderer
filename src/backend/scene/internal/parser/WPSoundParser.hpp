#pragma once

#include "audio/SoundManager.h"

namespace wallpaper 
{

namespace audio { class SoundManager; }
namespace fs { class VFS; }
namespace wpscene { class WPSoundObject; }

enum class WPSoundPlaybackMode
{
	Single,
	Random,
	Loop
};

struct WPSoundPlaybackPolicy {
	bool                autoplay { false };
	WPSoundPlaybackMode mode { WPSoundPlaybackMode::Loop };
};

class WPSoundParser {
public:
	static WPSoundPlaybackPolicy ResolvePlaybackPolicy(const wpscene::WPSoundObject&,
	                                                   bool autoplay,
	                                                   bool force_audio_loop);
	static audio::SoundHandle Parse(const wpscene::WPSoundObject&, fs::VFS&, audio::SoundManager&,
	                                bool autoplay, bool force_audio_loop);
};
}
