#include "p3fes_gameplay.h"
#include "p3fes_backlog.h"
#include "p3fes_gameplay_options.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_optional_patch.h"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace
{
    // Accessed on the EE thread. Keep the selected control mode stable during an encounter.
    bool directParty = false;
    std::unordered_set<uint32_t> commandedUnits;
    using Function = PS2Runtime::RecompiledFunction;
    Function originalBattleInit, originalSetState, originalCameraLookup, originalFusionLookup;

    template <typename T>
    T field(const uint8_t *ram, uint32_t base, uint32_t offset)
    {
        T value{};
        if (base && base <= PS2_RAM_SIZE - sizeof(T) - offset)
            std::memcpy(&value, ram + base + offset, sizeof(T));
        return value;
    }

    uint32_t battleUnit(const uint8_t *ram, uint32_t order)
    {
        return field<uint32_t>(ram, order, 0x30);
    }

    void initBattle(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        directParty = ps2xGameplayOption("P3FES_DIRECT_PARTY");
        commandedUnits.clear();
        originalBattleInit(ram, ctx, runtime);
    }

    void setActionState(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto order = getRegU32(ctx, 4);
        const auto next = static_cast<uint16_t>(getRegU32(ctx, 5));
        if (directParty && (next == 2 || next == 0xC))
        {
            const auto unit = battleUnit(ram, order);
            const auto previous = field<uint16_t>(ram, order, 0x0C);
            // Ready after Command (5) or Target (6) means the player actually
            // chose this action. Rush, ailments and scripted AI use other states.
            if (unit && next == 0xC && (previous == 5 || previous == 6))
                commandedUnits.insert(unit);
            else
                commandedUnits.erase(unit);
        }
        originalSetState(ram, ctx, runtime);
    }

    void cameraLookup(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Boss camera tracks are authored for the protagonist. Preserve the
        // special pointing shot and native cameras outside commanded turns.
        if (directParty && getRegU32(ctx, 31) == 0x2A391Cu && getRegU32(ctx, 4) != 0x1Du)
        {
            const auto order = getRegU32(ctx, 18);
            const auto unit = battleUnit(ram, order);
            const auto state = field<uint16_t>(ram, order, 0x0C);
            if (unit && field<uint8_t>(ram, unit, 0xA2) == 0 && field<uint16_t>(ram, unit, 0xA4) != 1 &&
                (state == 5 || state == 6 || commandedUnits.contains(unit)))
            {
                SET_GPR_U32(ctx, 2, 0);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        originalCameraLookup(ram, ctx, runtime);
    }

    void fusionLookup(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (directParty && getRegU32(ctx, 31) == 0x2020D4u)
        {
            const auto battle = field<uint32_t>(ram, getRegU32(ctx, 28) - 0x4904u, 0);
            if (field<uint32_t>(ram, battle, 0x254) != field<uint32_t>(ram, battle, 0x148))
            {
                SET_GPR_U32(ctx, 2, 0);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
        originalFusionLookup(ram, ctx, runtime);
    }

    void hook(PS2Runtime &runtime, uint32_t pc, Function replacement, Function &original)
    {
        if (auto fn = runtime.lookupFunction(pc); fn && fn != replacement)
        {
            original = fn;
            runtime.replaceFunction(pc, replacement);
        }
    }
}

extern "C" bool ps2xRuntimePatchEnabled(const char *option, uint32_t entry,
                                        const uint8_t *ram, const R5900Context *ctx)
{
    if (std::strcmp(option, "P3FES_DIRECT_PARTY") == 0)
    {
        if (!directParty)
            return false;
        // On a scheduler resume the argument registers may have changed. These
        // three guest functions retain the actor in a callee-saved register.
        switch (entry)
        {
        case 0x28DBF0: // Ready: choose between an individual and a party escape
            return commandedUnits.contains(battleUnit(ram, getRegU32(ctx, ctx->pc == entry ? 4 : 19)));
        case 0x296890: // Escape: resolve the party command
            return commandedUnits.contains(battleUnit(ram, getRegU32(ctx, ctx->pc == entry ? 4 : 20)));
        case 0x2D8110: // Deduct the item from the inventory used to select it
        {
            auto unit = ctx->pc == entry ? battleUnit(ram, field<uint32_t>(ram, getRegU32(ctx, 4), 0))
                                         : getRegU32(ctx, 17);
            return commandedUnits.contains(unit);
        }
        default:
            return true;
        }
    }
    if (std::strcmp(option, "P3FES_FAST_FORWARD") == 0)
        return ps2xGameplayOption(option);
    const char *value = std::getenv(option);
    return value && std::strcmp(value, "1") == 0;
}

void p3fes_gameplay::install(PS2Runtime &runtime)
{
    p3fes_gameplay_options::install();
    p3fes_gameplay_options::installNativeMenu(runtime);
    p3fes_backlog::install(runtime);
    directParty = ps2xGameplayOption("P3FES_DIRECT_PARTY");
    commandedUnits.clear();
    hook(runtime, 0x29B100, initBattle, originalBattleInit);
    hook(runtime, 0x299D60, setActionState, originalSetState);
    hook(runtime, 0x2F8840, cameraLookup, originalCameraLookup);
    hook(runtime, 0x30C0C0, fusionLookup, originalFusionLookup);
}
