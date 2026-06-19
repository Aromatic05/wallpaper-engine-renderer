#pragma once

#include "audio/SoundManager.h"

namespace wallpaper 
{

namespace audio { class SoundManager; }
namespace fs { class VFS; }
namespace wpscene { class WPSoundObject; }
class WPSoundParser {
public:
	static audio::SoundHandle Parse(const wpscene::WPSoundObject&, fs::VFS&, audio::SoundManager&);
};
}
