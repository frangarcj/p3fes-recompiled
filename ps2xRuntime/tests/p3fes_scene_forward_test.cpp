// Link with p3fes_scene_forward.cpp. Exercise the host clock gate with the US game's task layout.
#include "../src/lib/game_overrides/p3fes_scene_forward.h"
#include "../src/lib/game_overrides/p3fes_native_movie.h"

#include <cassert>
#include <cstring>
#include <vector>

namespace
{
    bool enabled = true;
    bool historyOpen = false;
    auto movieStatus = p3fes_native_movie::Status::Idle;
}

namespace p3fes_gameplay_options
{
    bool sceneForwardEnabled() { return enabled; }
    bool visible() { return historyOpen; }
}
p3fes_native_movie::Status p3fes_native_movie::status() { return movieStatus; }

int main()
{
    std::vector<uint8_t> ram(32 * 1024 * 1024);
    auto put = [&](uint32_t address, auto value) { std::memcpy(ram.data() + address, &value, sizeof(value)); };
    const uint32_t admin = 0x100000, adminWork = 0x101000, field = 0x102000, fieldWork = 0x103000;
    const uint32_t windowNode = 0x104000, window = 0x105000;
    put(0x7D2CF0 - 0x4C74, admin);
    std::memcpy(ram.data() + admin, "admini", 7);
    put(admin + 0x3C, adminWork);
    put(admin + 0x40, field);
    std::memcpy(ram.data() + field, "field event", 12);
    put(field + 0x3C, fieldWork);
    put(0x959ED0, windowNode);
    put(windowNode + 12, window);
    auto expect = [&](bool value) {
        assert(p3fes_scene_forward::sample(ram.data()) == value);
        assert(p3fes_scene_forward::active.load() == value);
    };

    ram[adminWork + 8] = 3; // story sequence, including camera-only intervals
    expect(false);
    ram[0x7E094C] = 0x10;
    expect(true);
    put(window + 0x54, int16_t(2)); // waiting for a choice with Triangle still down
    expect(false);
    put(window + 0x54, int16_t(-1));
    expect(true);
    ram[0x7E094C] = 0;
    expect(false);

    ram[0x7E094C] = 0x10;
    ram[adminWork + 8] = 2;
    put(fieldWork, 1); // free exploration
    expect(false);
    put(fieldWork, 22); // field conversation
    expect(true);
    std::memcpy(ram.data() + field, "H_CampDraw", 11);
    expect(false);
    std::memcpy(ram.data() + field, "field event", 12);
    ram[adminWork + 8] = 6; // battle
    expect(false);
    ram[adminWork + 8] = 3;
    movieStatus = p3fes_native_movie::Status::Playing; // native movies own their own clock
    expect(false);
    movieStatus = p3fes_native_movie::Status::Idle;
    historyOpen = true;
    expect(false);
    historyOpen = false;
    enabled = false;
    expect(false);
}
