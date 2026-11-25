#include "p3fes_gameplay_options.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace
{
    using namespace p3fes_gameplay_options;
    using Function = PS2Runtime::RecompiledFunction;
    // US FES shares Config's work layout and sprite drawing between the title and camp tasks.
    constexpr uint32_t titleConfig = 0x15B430, campSystem = 0x14F7D0, configLayer = 0x155830;
    constexpr uint32_t inputReady = 0x30, selection = 0x154;
    constexpr uint16_t accept = 0x40, back = 0x20, shoulders = 0xC;
    constexpr uint16_t up = 0x1000, down = 0x4000, left = 0x8000, right = 0x2000;
    constexpr int confirm = 7;
    constexpr std::array labels{"Direct party control", "Scene fast forward", "Record dialogue history"};
    constexpr std::array descriptions{"Command allies from the next battle.", "Hold Triangle to speed up scenes.",
                                      "Press SELECT during dialogue to read."};
    constexpr int optionCount = static_cast<int>(labels.size());
    Function titleUpdate, campUpdate, originalLayer;
    enum class Page { Original, Gameplay };
    struct Menu
    {
        uint32_t work = 0;
        Page page = Page::Original;
        Settings pending{};
        int originalRow = 0;
        bool saveFailed = false;
    } menu;

    template <typename T>
    T read(const uint8_t *ram, uint32_t address)
    {
        T result;
        std::memcpy(&result, ram + address, sizeof(result));
        return result;
    }

    void put(uint8_t *ram, uint32_t address, int value)
    {
        std::memcpy(ram + address, &value, sizeof(value));
    }

    // Run the game's ordinary camp sprite, font and sound routines using the current
    // EE thread's gp and stack. The caller's live registers are never borrowed.
    void call(uint8_t *ram, PS2Runtime *runtime, R5900Context &local, uint32_t entry)
    {
        constexpr uint32_t done = 0xFFFFFFFC;
        SET_GPR_U32(&local, 31, done);
        local.pc = entry;
        for (unsigned resumes = 0; local.pc != done && !runtime->isStopRequested() && resumes < 20000; ++resumes)
        {
            const auto fn = runtime->lookupFunction(local.pc);
            if (!fn) break;
            fn(ram, &local, runtime);
        }
    }

    void text(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, std::string_view string,
              int x, int y, float depth, uint32_t color)
    {
        if (string.empty()) return;
        const auto sp = getRegU32(ctx, 29);
        if (sp < 0x1000 || sp > PS2_RAM_SIZE || string.size() >= 96) return;
        // Keep the NUL-terminated string above the outgoing arguments and the callee's frame.
        R5900Context local = *ctx;
        const auto buffer = sp - 96;
        std::memcpy(ram + buffer, string.data(), string.size());
        ram[buffer + string.size()] = 0;
        SET_GPR_U32(&local, 29, buffer - 32);
        SET_GPR_U32(&local, 4, x);
        SET_GPR_U32(&local, 5, y);
        SET_GPR_U32(&local, 6, color);
        SET_GPR_U32(&local, 7, 6);  // same font and alignment as the System screens
        SET_GPR_U32(&local, 8, 1);
        SET_GPR_U32(&local, 9, buffer);
        SET_GPR_U32(&local, 10, 0x10);
        SET_GPR_U32(&local, 11, 0);
        local.f[12] = depth;
        call(ram, runtime, local, 0x3B2CB0);
    }

    void sound(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, int effect)
    {
        R5900Context local = *ctx;
        for (int i = 4; i <= 6; ++i) SET_GPR_U32(&local, i, 0);
        SET_GPR_U32(&local, 7, effect);
        call(ram, runtime, local, 0x10A4E0);
    }

    void input(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t work = menu.work;
        const int row = read<int>(ram, work + selection);
        const auto edge = read<uint16_t>(ram, 0x7E094E);
        const auto direction = edge | read<uint16_t>(ram, 0x7E0952) | read<uint16_t>(ram, 0x7E095A);
        const bool gameplay = menu.page == Page::Gameplay;
        if (edge & shoulders)
        {
            menu.page = gameplay ? Page::Original : Page::Gameplay;
            if (!gameplay) menu.originalRow = row;
            put(ram, work + selection, gameplay ? menu.originalRow : 0);
            sound(ram, ctx, runtime, 0);
        }
        else if ((edge & accept) && row == confirm)
        {
            menu.saveFailed = !applySettings(menu.pending);
            if (!menu.saveFailed) return; // FES commits its own settings and dismisses Config.
        }
        else if (!gameplay || (edge & back))
            return; // The stock Cancel path discards both sets of unconfirmed edits.
        else if (direction & (up | down))
        {
            constexpr std::array selectable{0, 1, 2, confirm};
            const auto position = std::find(selectable.begin(), selectable.end(), row) - selectable.begin();
            const size_t offset = direction & down ? 1 : selectable.size() - 1;
            const int next = selectable[(position + offset) % selectable.size()];
            if (row != next) sound(ram, ctx, runtime, 0);
            put(ram, work + selection, next);
        }
        else if (row >= 0 && row < optionCount && (direction & (left | right)))
        {
            const bool enabled = (direction & left) != 0; // ON is on the left of the game's switch.
            if (menu.pending[row] != enabled) sound(ram, ctx, runtime, 5);
            menu.pending[row] = enabled;
        }
        // Skip only the original input block. The task still animates/draws all layers,
        // then recomputes inputReady. No controller state or stock option is overwritten.
        put(ram, work + inputReady, 0);
    }

    void update(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, Function original)
    {
        const uint32_t task = getRegU32(ctx, 4);
        if (task && task <= PS2_RAM_SIZE - 0x40)
        {
            const auto work = read<uint32_t>(ram, task + 0x3C);
            if (work && work <= PS2_RAM_SIZE - 0x17C)
            {
                const auto phase = read<int>(ram, work);
                if (phase == 7 || (phase == 8 && menu.work != work))
                {
                    menu = {};
                    menu.work = work;
                    menu.pending = currentSettings();
                }
                if (phase == 8 && read<int>(ram, work + inputReady)) input(ram, ctx, runtime);
            }
        }
        original(ram, ctx, runtime);
    }

    void title(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { update(ram, ctx, runtime, titleUpdate); }
    void camp(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { update(ram, ctx, runtime, campUpdate); }

    void draw(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int layer = getRegU32(ctx, 6);
        const uint32_t node = getRegU32(ctx, 4);
        const int row = getRegU32(ctx, 7);
        const bool original = menu.page == Page::Original;
        if (!menu.work || getRegU32(ctx, 8) != menu.work + 0x158 || node > PS2_RAM_SIZE-68 ||
            (original && layer != 6) || layer < 2)
        {
            originalLayer(ram, ctx, runtime);
            return;
        }
        const float depth = read<float>(ram, node + 36);
        const int x = int(read<float>(ram, node + 56)), y = int(read<float>(ram, node + 60));
        const auto fade = read<uint8_t>(ram, node + 64);
        auto label = [&](std::string_view s, int px, int py, bool active = true) {
            text(ram, ctx, runtime, s, px, py, depth, (active ? 0xFFFFFF00u : 0x197DC200u) | (255 - fade));
        };
        const auto sprites = read<uint32_t>(ram, getRegU32(ctx, 5));
        auto sprite = [&](int id, int px, int py, uint32_t atlas) {
            R5900Context local = *ctx;
            SET_GPR_U32(&local, 5, atlas);
            SET_GPR_U32(&local, 6, id);
            SET_GPR_U32(&local, 7, fade);
            local.f[12] = float(px); local.f[13] = float(py); local.f[14] = depth;
            call(ram, runtime, local, 0x1159F0);
        };
        if (original)
        {
            label("R: Gameplay", x + 295, y - 28);
            if (menu.saveFailed) label("Could not save settings.", x, y + 260);
            originalLayer(ram, ctx, runtime);
            return;
        }
        // Confirm is row 7 in the original state machine. Place it directly below
        // the gameplay options while preserving its original action and artwork.
        constexpr int confirmY = optionCount * 33;
        if (layer == 2)
        {
            for (int i = 0; i <= optionCount; ++i)
            {
                sprite(14, x, y + 33 * i, sprites);
                sprite(14, x + 292, y + 33 * i, sprites);
            }
        }
        else if (layer == 3)
            sprite(153, x, y + (row == confirm ? confirmY : row * 33), sprites);
        else if (layer == 6)
        {
            label("L: Config", x + 295, y - 28);
            for (int i = 0; i < optionCount; ++i)
                label(labels[i], x, y + i * 33 + 10, row == i);
            sprite(row == confirm ? 152 : 143, x, y + confirmY, sprites);
        }
        else if (layer == 4 || layer == 5)
        {
            for (int i = 0; i < optionCount; ++i)
            {
                const bool on = menu.pending[i];
                sprite(layer == 4 ? 154 : on ? 156 : 157, x + (layer == 5 && !on ? 44 : 0), y + 33 * i, sprites);
            }
            if (layer == 5) sprite(row == confirm ? 166 : 165, x, y + confirmY, sprites);
        }
        else if (layer == 99)
        {
            const char *help = menu.saveFailed ? "Could not save settings. Confirm to retry." :
                               row >= 0 && row < optionCount ? descriptions[row] : "Apply settings and return.";
            label(help, x - 345, y + 4);
            // Same button legend and sprite bank as the original Config footer.
            const auto buttons = read<uint32_t>(ram, 0x833BA0);
            sprite(0, x + 102, y + 30, buttons);
            sprite(1, x + 169, y + 30, buttons);
        }
        ctx->pc = getRegU32(ctx, 31);
    }
}

void p3fes_gameplay_options::installNativeMenu(PS2Runtime &runtime)
{
    menu = {};
    auto bind = [&](uint32_t address, Function hook, Function &original) {
        if (auto fn = runtime.lookupFunction(address); fn && fn != hook)
        {
            original = fn;
            runtime.replaceFunction(address, hook);
        }
    };
    bind(titleConfig, title, titleUpdate);
    bind(campSystem, camp, campUpdate);
    bind(configLayer, draw, originalLayer);
}
