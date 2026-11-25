#pragma once

#include <array>
#include <cstdint>

class PS2Runtime;

extern "C" bool ps2xGameplayOption(const char *key);

namespace p3fes_gameplay_options
{
    using Settings = std::array<bool, 3>;
    Settings currentSettings();
    bool applySettings(const Settings &settings);
    void installNativeMenu(PS2Runtime &runtime);
    void install();
    void shutdown();
    bool visible();
    bool sceneForwardEnabled();
    // Called on the window thread once per host input sample.
    void handleInput(uint16_t pressed);
    // The pad reader may run on a different thread.
    bool consumesInput(uint16_t pressed);
    void drawHistory();
}
