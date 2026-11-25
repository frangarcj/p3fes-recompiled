// Link with p3fes_backlog.cpp and the runtime. The fake layout function
// models the point where the game has finished substituting the speaker/text.
#include "game_overrides/p3fes_backlog.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t layoutPc = 0x3A67E0;
    bool recording = true;
    unsigned layouts = 0;

    void originalLayout(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++layouts;
        ctx->pc = getRegU32(ctx, 31);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "%s\n", message);
            std::exit(1);
        }
    }

    template <typename T>
    void store(std::vector<uint8_t> &ram, uint32_t address, T value)
    {
        std::memcpy(ram.data() + address, &value, sizeof(value));
    }
}

extern "C" bool ps2xGameplayOption(const char *key)
{
    return recording && std::strcmp(key, "P3FES_BACKLOG") == 0;
}

int main()
{
    constexpr uint32_t task = 0x193EC0, update = 0x1949E0;
    std::vector<PS2Runtime::RecompiledFunction> table((layoutPc - task) / 4 + 1);
    table.front() = [](uint8_t *, R5900Context *c, PS2Runtime *) { c->pc = getRegU32(c, 31); };
    table[(update - task) / 4] = table.front();
    table.back() = originalLayout;
    ps2SetRecompiledFunctionTable({table.data(), task, layoutPc + 4, static_cast<uint32_t>(table.size())});
    PS2Runtime runtime;
    p3fes_backlog::install(runtime);

    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    constexpr uint32_t window = 0x100, font = 0x200, run = 0x300, first = 0x400, second = 0x500;
    store(ram, window + 0x30, font);
    store(ram, font + 0x2C, run);
    store(ram, run + 0x1C, first);
    store(ram, first, uint16_t(0x80A1)); // double-byte A
    store(ram, first + 0x28, second);
    store(ram, second, uint16_t(0x829D)); // ellipsis

    auto page = [&] {
        R5900Context ctx{};
        ctx.pc = layoutPc;
        ctx.r[4] = _mm_cvtsi32_si128(window);
        runtime.lookupFunction(layoutPc)(ram.data(), &ctx, &runtime);
    };
    page();
    require(p3fes_backlog::entries() == std::vector<p3fes_backlog::Entry>{{"", "A..."}}, "Extended glyph decoding failed");

    constexpr uint32_t node = 0x1000;
    store(ram, 0x959ED0, node);
    store(ram, node + 12, window);
    for (unsigned phase : {0, 2, 3, 4, 1, 0})
    {
        store(ram, window, phase);
        R5900Context ctx{};
        ctx.pc = update;
        runtime.lookupFunction(update)(ram.data(), &ctx, &runtime);
        require(p3fes_backlog::dialogueActive() == (phase == 3 || phase == 4), "History must follow the live message window");
    }
    recording = false;
    page();
    recording = true;
    store(ram, second + 0x28, first); // malformed cyclic glyph chain
    page();
    require(layouts == 3 && p3fes_backlog::entries().size() == 1, "Ignored pages must still call the original layout");

    store(ram, first + 0x28, 0u);
    store(ram, first, uint16_t('B'));
    for (unsigned i = 0; i < 128; ++i)
        page();
    const auto history = p3fes_backlog::entries();
    require(history.size() == 128 && history.front().text == "B" && history.back().text == "B", "Oldest page was not evicted");
    p3fes_backlog::install(runtime);
    require(p3fes_backlog::entries().empty(), "History carried over to the next session");
}
