#pragma once

#include <string>

namespace p3fes_native_bgm
{
    // Start a native 48 kHz stereo PCM WAV stream. Desktop builds return false
    // so callers can keep using the game's original audio path.
    bool start(const std::string &path);
    void stop();
    void fadeOut(float seconds);
    bool playing();
    void setVolume(float volume);
    void pause(bool paused);
}
