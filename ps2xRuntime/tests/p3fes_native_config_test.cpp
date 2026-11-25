// Link with p3fes_native_config.cpp and the runtime. Both original Config tasks
// share this controller and work layout in SLUS-21621.
#include "game_overrides/p3fes_gameplay_options.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace p3fes_gameplay_options;

namespace
{
    constexpr uint32_t sound = 0x10A4E0, camp = 0x14F7D0, title = 0x15B430;
    constexpr uint32_t task = 0x1000, work = 0x2000;
    Settings saved{};
    bool canSave = true;
    unsigned accepts = 0, cancels = 0;

    void check(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "%s\n", message);
            std::exit(1);
        }
    }

    void setWord(uint8_t *ram, uint32_t addr, uint32_t value) { std::memcpy(ram + addr, &value, 4); }
    uint32_t word(uint8_t *ram, uint32_t addr)
    {
        uint32_t value;
        std::memcpy(&value, ram + addr, 4);
        return value;
    }

    void guestReturn(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = getRegU32(ctx, 31); }
    void stock(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (word(ram, work) == 8 && word(ram, work + 0x30))
        {
            const auto edge = word(ram, 0x7E094E) & 0xFFFF;
            if (edge == 0x40 && word(ram, work + 0x154) == 7) ++accepts;
            if (edge == 0x20) ++cancels;
        }
        setWord(ram, work + 0x30, 1);
        guestReturn(ram, ctx, runtime);
    }
}

Settings p3fes_gameplay_options::currentSettings() { return saved; }
bool p3fes_gameplay_options::applySettings(const Settings &settings)
{
    if (canSave) saved = settings;
    return canSave;
}

int main()
{
    std::vector<PS2Runtime::RecompiledFunction> functions((title - sound) / 4 + 1, nullptr);
    functions[0] = guestReturn;
    functions[(0x155830 - sound) / 4] = guestReturn;
    functions[(camp - sound) / 4] = functions.back() = stock;
    ps2SetRecompiledFunctionTable({functions.data(), sound, title + 4, static_cast<uint32_t>(functions.size())});
    PS2Runtime runtime;
    installNativeMenu(runtime);
    std::vector<uint8_t> mem(PS2_RAM_SIZE);
    auto ram = mem.data();
    setWord(ram, task + 0x3C, work);
    for (const auto entry : {title, camp})
    {
        saved = {false, false, true};
        accepts = cancels = 0;
        auto tick = [&](uint32_t phase, uint16_t press) {
            R5900Context ctx{};
            ctx.pc = entry;
            ctx.r[4] = _mm_cvtsi32_si128(task);
            setWord(ram, work, phase);
            std::memcpy(ram + 0x7E094E, &press, sizeof(press));
            runtime.lookupFunction(entry)(ram, &ctx, &runtime);
        };
        setWord(ram, work + 0x154, 2);
        setWord(ram, work + 0x158, 1);
        tick(7, 0);
        tick(8, 8);
        tick(8, 0x8000);
        tick(8, 4);
        check(word(ram, work + 0x154) == 2, "Changing pages lost the original cursor");
        tick(8, 0x20);
        check(cancels == 1 && !saved[0], "Circle must cancel pending changes");

        tick(7, 0);
        tick(8, 8);
        tick(8, 0x8000);
        for (int expected : {1, 2, 7, 0})
        {
            tick(8, 0x4000);
            check(word(ram, work + 0x154) == uint32_t(expected), "Gameplay has three switches followed by Confirm");
        }
        tick(8, 0x1000);
        check(word(ram, work + 0x154) == 7, "Up from first gameplay row must select Confirm");
        canSave = false;
        tick(8, 0x40);
        check(accepts == 0 && !saved[0], "A failed save must keep Config open");
        canSave = true;
        tick(8, 0x40);
        check(accepts == 1 && saved == Settings{true, false, true}, "Confirm must apply pending options");
        check(word(ram, work + 0x158) == 1, "The port overwrote an original game setting");
    }
}
