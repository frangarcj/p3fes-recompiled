#include "p3fes_debug_warp.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

// Every address below was read from the recompiled functions in ps2xRuntime/src/runner (the comments there name
// the MIPS instructions); see the notes at the end of this file for the scene-system facts they rest on.
namespace p3fes_debug_warp
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;

        constexpr uint32_t kKwlnUpdate = 0x001983A0u;      // per-frame function (called once per game frame)
        constexpr uint32_t kKwlnUpdateEnd = 0x001984C0u;
        constexpr uint32_t kAdminiChangeSeq = 0x0027C080u; // (seq id, params*, size, push flag)
        constexpr uint32_t kScrGetIntPara = 0x0035ED20u;   // script argument getter (a0 = index)
        constexpr uint32_t kCallBattleCmd = 0x0027D5E0u;   // btlScrCmd_CALL_BATTLE
        constexpr uint32_t kFieldCreate = 0x001BA5F0u;     // k_field_001ba5f0: creates the field task
        constexpr uint32_t kFieldGlobal = 0x00869F60u;     // K_Field_Get(): word 0 = field task pointer (0 = none)
        constexpr uint32_t kDatSetFlag = 0x0016F1F0u;      // datSetFlag(flag, value)
        constexpr uint32_t kSaveStartCmd = 0x001885A0u;    // script command: starts the save UI task (y_misc_00420340(0,1))
        constexpr uint32_t kSaveWaitCmd = 0x001885F0u;     // script command: 1 once that task has ended (state 3)
        constexpr uint32_t kFlagNoCamp = 0x171u;           // set by the game's opening; blocks TRIANGLE (command menu) at 0x1C8AE4
        constexpr uint32_t kDatGetUnit = 0x0016CD60u;      // datGetUnit(pc id): protagonist (id 1) = 0x836224, others 0x834014 + (id-2)*0x364
        constexpr uint32_t kDatCalcSetHp = 0x002FFD90u;    // datCalcSetHp(unit, hp): stores the u16 at unit+8 (clamps to 999)
        constexpr uint32_t kDatCalcGetMaxHp = 0x002FFDF0u; // datCalcGetMaxHp(unit)
        constexpr uint32_t kProtagonistUnit = 0x00836224u; // datGetUnit(1); current HP = u16 at +8
        constexpr uint32_t kFlagBits = 0x0083A21Cu;        // datGetFlag 0x16F190: bit n of a u32 array here

        constexpr uint32_t kSeqField = 2u;    // K_Seq 0x1B77B0 / 0x1B7850 / 0x1B78A0
        constexpr uint32_t kSeqDungeon = 5u;  // K_Seq_CallDungeon / ExitDungeon / CheckDungeon
        constexpr uint32_t kSeqBattle = 6u;   // adminiSeqCallBtl

        GuestFunction g_changeOriginal = nullptr;
        GuestFunction g_paraOriginal = nullptr;
        GuestFunction g_updateOriginal = nullptr;
        GuestFunction g_fieldCreateOriginal = nullptr;

        struct Config
        {
            bool log = false;
            bool field = false;
            uint32_t fieldWords[5] = {0u, 0u, 0u, 0u, 0u};
            bool tartarus = false;
            uint32_t tartarusWords[2] = {0u, 0u};
            bool battle = false;
            uint32_t encounter = 0u;
            bool raw = false;
            uint32_t rawSeq = 0u;
            std::vector<uint32_t> rawWords;
            double at = 5.0;
            double battleAt = -1.0;
            uint32_t dumpEncounters = 0u;
            std::vector<std::pair<uint32_t, uint32_t>> setFlags; // P3FES_WARP_FLAGS
            std::vector<uint32_t> logFlags;                      // P3FES_WARP_FLAGLOG
            bool logTasks = false;                               // P3FES_WARP_TASKS
            bool menu = false;                                   // P3FES_WARP_MENU
            uint32_t save = 0u;                                  // P3FES_WARP_SAVE (number of save UIs to open)
            bool hpSet = false;                                  // P3FES_WARP_HP
            uint32_t hp = 0u;
        } g_cfg;

        bool g_fieldSeen = false;
        std::chrono::steady_clock::time_point g_fieldSeenAt;
        bool g_fieldDone = false;
        bool g_tartarusDone = false;
        bool g_rawDone = false;
        bool g_battleDone = false;
        bool g_flagsDone = false;
        bool g_hpLogged = false;
        uint32_t g_saveRuns = 0u;
        double g_saveNextAt = 0.0;
        bool g_saveStarted = false;
        bool g_saveDone = false;
        double g_saveStartedAt = 0.0;
        uint32_t g_saveWaitFrames = 0u;
        double g_flagLogNext = 0.0;
        bool g_paraArmed = false;
        uint32_t g_paraValue = 0u;

        inline uint32_t r32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        inline void w32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }

        inline void w16(uint8_t *rdram, uint32_t address, uint16_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }

        bool parseList(const char *text, std::vector<uint32_t> &out)
        {
            out.clear();
            if (!text)
                return false;
            const char *p = text;
            while (*p)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 0);
                if (end == p)
                    return false;
                out.push_back(static_cast<uint32_t>(v));
                p = end;
                if (*p == ',' || *p == ':' || *p == ' ')
                    ++p;
                else if (*p)
                    return false;
            }
            return !out.empty();
        }

        // "n[=v],n[=v],..." (decimal or 0x hex; v defaults to 1)
        bool parseFlagList(const char *text, std::vector<std::pair<uint32_t, uint32_t>> &out)
        {
            out.clear();
            if (!text)
                return false;
            const char *p = text;
            while (*p)
            {
                char *end = nullptr;
                const uint32_t flag = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                if (end == p)
                    return false;
                uint32_t value = 1u;
                p = end;
                if (*p == '=' || *p == ':')
                {
                    ++p;
                    value = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                    if (end == p)
                        return false;
                    p = end;
                }
                out.emplace_back(flag, value);
                if (*p == ',' || *p == ' ')
                    ++p;
                else if (*p)
                    return false;
            }
            return !out.empty();
        }

        inline bool getFlag(const uint8_t *rdram, uint32_t flag)
        {
            return ((r32(rdram, kFlagBits + (flag >> 5) * 4u) >> (flag & 31u)) & 1u) != 0u;
        }

        // kwln task lists (heads at gp-0x4C8C / -0x4C80 / -0x4C74, next at +0x40): name, state nibble (+0x1C), timer,
        // update function and work pointer with its first word (the field-event task keeps its state machine there).
        void logTasks(const uint8_t *rdram, uint32_t gp, double t)
        {
            std::fprintf(stderr, "[p3fes-warp] t=%.1f event-candidate=%d (gp-0x45FC) tasks:", t,
                         static_cast<int16_t>(r32(rdram, gp - 0x45FCu) & 0xFFFFu));
            for (uint32_t head : {gp - 0x4C8Cu, gp - 0x4C80u, gp - 0x4C74u})
            {
                uint32_t task = r32(rdram, head);
                std::fprintf(stderr, " |");
                for (int n = 0; task != 0u && n < 96; ++n)
                {
                    char name[25] = {};
                    std::memcpy(name, rdram + (task & PS2_RAM_MASK), 24);
                    const uint32_t work = r32(rdram, task + 0x3Cu);
                    std::fprintf(stderr, " %s(%u", name, r32(rdram, task + 0x1Cu) & 0xFu);
                    if (work >= 0x100000u && work < 0x2000000u)
                        std::fprintf(stderr, ",w0=%u", r32(rdram, work));
                    std::fprintf(stderr, ")");
                    task = r32(rdram, task + 0x40u);
                }
            }
            std::fprintf(stderr, "\n");
        }

        // Runs `function` (a guest function, at `entry`) on a copy of the caller's context, on a scratch stack
        // frame below the caller's stack pointer. Returns false when the function did not return normally.
        bool callGuest(GuestFunction function, uint8_t *rdram, R5900Context *source, PS2Runtime *runtime, uint32_t a0,
                       uint32_t a1, uint32_t a2, uint32_t a3, uint32_t &v0)
        {
            R5900Context copy = *source;
            const uint32_t returnPc = getRegU32(source, 31);
            const uint32_t sp = getRegU32(source, 29);
            SET_GPR_U32(&copy, 29, sp - 0x400u);
            SET_GPR_U32(&copy, 4, a0);
            SET_GPR_U32(&copy, 5, a1);
            SET_GPR_U32(&copy, 6, a2);
            SET_GPR_U32(&copy, 7, a3);
            SET_GPR_U32(&copy, 31, returnPc);
            function(rdram, &copy, runtime);
            for (uint32_t steps = 0u; copy.pc != returnPc; ++steps)
            {
                // the callee yielded / unwound to the dispatcher: keep dispatching its continuation
                if (steps >= 20000u || !runtime->hasFunction(copy.pc))
                    return false;
                runtime->lookupFunction(copy.pc)(rdram, &copy, runtime);
            }
            v0 = getRegU32(&copy, 2);
            return true;
        }

        // Scratch parameter block lives just under the caller's stack, above the callee frames callGuest creates.
        uint32_t scratchParams(R5900Context *ctx)
        {
            return getRegU32(ctx, 29) - 0x100u;
        }

        void changeSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t seq, const uint32_t *words,
                       uint32_t sizeBytes, const char *what)
        {
            const uint32_t base = scratchParams(ctx);
            for (uint32_t i = 0u; i < 0x40u; i += 4u)
                w32(rdram, base + i, 0u);
            for (uint32_t i = 0u; i < sizeBytes / 4u; ++i)
                w32(rdram, base + i * 4u, words[i]);
            uint32_t v0 = 0u;
            const bool ok = callGuest(g_changeOriginal, rdram, ctx, runtime, seq, base, sizeBytes, 0u, v0);
            std::fprintf(stderr, "[p3fes-warp] %s: adminiChangeSeq(seq=%u size=0x%X) %s\n", what, seq, sizeBytes,
                         ok ? "issued" : "DID NOT RETURN (unwound)");
        }

        void warpField(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // 0x1C-byte parameter block: u16 major, u16 minor, u16 entry, s16 +6, s16 +8 (K_Seq 0x1B77B0 reads them)
            const uint32_t base = scratchParams(ctx);
            for (uint32_t i = 0u; i < 0x40u; i += 4u)
                w32(rdram, base + i, 0u);
            w16(rdram, base + 0u, static_cast<uint16_t>(g_cfg.fieldWords[0]));
            w16(rdram, base + 2u, static_cast<uint16_t>(g_cfg.fieldWords[1]));
            w16(rdram, base + 4u, static_cast<uint16_t>(g_cfg.fieldWords[2]));
            w16(rdram, base + 6u, static_cast<uint16_t>(g_cfg.fieldWords[3]));
            w16(rdram, base + 8u, static_cast<uint16_t>(g_cfg.fieldWords[4]));
            uint32_t v0 = 0u;
            const bool ok = callGuest(g_changeOriginal, rdram, ctx, runtime, kSeqField, base, 0x1Cu, 0u, v0);
            std::fprintf(stderr, "[p3fes-warp] field F%03u_%03u entry=%u: adminiChangeSeq(2) %s\n", g_cfg.fieldWords[0],
                         g_cfg.fieldWords[1], g_cfg.fieldWords[2], ok ? "issued" : "DID NOT RETURN (unwound)");
        }

        // ---- hooks ----------------------------------------------------------------------------------------

        void changeSeqHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_cfg.log)
            {
                const uint32_t seq = getRegU32(ctx, 4);
                const uint32_t params = getRegU32(ctx, 5);
                const uint32_t size = getRegU32(ctx, 6);
                std::fprintf(stderr, "[p3fes-warp] game adminiChangeSeq(seq=%d params=%08X size=0x%X push=%u) ra=%08X",
                             static_cast<int32_t>(seq), params, size, getRegU32(ctx, 7), getRegU32(ctx, 31));
                if (params && size && size <= 0x80u)
                {
                    std::fprintf(stderr, " words:");
                    for (uint32_t i = 0u; i + 4u <= size; i += 4u)
                        std::fprintf(stderr, " %08X", r32(rdram, params + i));
                }
                std::fprintf(stderr, "\n");
            }
            g_changeOriginal(rdram, ctx, runtime);
        }

        void fieldCreateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_cfg.log)
                std::fprintf(stderr,
                             "[p3fes-warp] field create: major=%u minor=%u a3=%u t0=%d t1=%d ra=%08X\n",
                             getRegU32(ctx, 5) & 0xFFFFu, getRegU32(ctx, 6) & 0xFFFFu, getRegU32(ctx, 7) & 0xFFFFu,
                             static_cast<int16_t>(getRegU32(ctx, 8)), static_cast<int16_t>(getRegU32(ctx, 9)),
                             getRegU32(ctx, 31));
            g_fieldCreateOriginal(rdram, ctx, runtime);
        }

        void paraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_paraArmed && getRegU32(ctx, 4) == 0u)
            {
                setReturnU32(ctx, g_paraValue);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_paraOriginal(rdram, ctx, runtime);
        }

        void updateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // A resumed (previously suspended) call re-enters at an internal label: just continue it.
            const uint32_t pc = ctx->pc;
            const bool fresh = !(pc > kKwlnUpdate && pc < kKwlnUpdateEnd);
            if (fresh)
            {
                using Clock = std::chrono::steady_clock;
                const Clock::time_point now = Clock::now();
                if (!g_fieldSeen)
                {
                    if (r32(rdram, kFieldGlobal) != 0u)
                    {
                        g_fieldSeen = true;
                        g_fieldSeenAt = now;
                        std::fprintf(stderr, "[p3fes-warp] first frame with a live field (warp timers start) gp=%08X field task=%08X\n",
                                     getRegU32(ctx, 28), r32(rdram, kFieldGlobal));
                    }
                }
                else
                {
                    const double t = std::chrono::duration<double>(now - g_fieldSeenAt).count();
                    if ((!g_cfg.logFlags.empty() || g_cfg.logTasks) && t >= g_flagLogNext)
                    {
                        g_flagLogNext = t + 2.0;
                        if (g_cfg.logTasks)
                            logTasks(rdram, getRegU32(ctx, 28), t);
                        if (!g_cfg.logFlags.empty())
                        std::fprintf(stderr, "[p3fes-warp] t=%.1f flags:", t);
                        for (uint32_t f : g_cfg.logFlags)
                            std::fprintf(stderr, " %X=%u", f, getFlag(rdram, f) ? 1u : 0u);
                        if (!g_cfg.logFlags.empty())
                            std::fprintf(stderr, "\n");
                    }
                    if (!g_cfg.setFlags.empty() && !g_flagsDone && t >= g_cfg.at)
                    {
                        g_flagsDone = true;
                        for (const auto &fv : g_cfg.setFlags)
                        {
                            uint32_t v0 = 0u;
                            const bool before = getFlag(rdram, fv.first);
                            const bool ok = callGuest(runtime->lookupFunction(kDatSetFlag), rdram, ctx, runtime, fv.first, fv.second, 0u, 0u, v0);
                            std::fprintf(stderr, "[p3fes-warp] datSetFlag(0x%X,%u): %u -> %u %s\n", fv.first, fv.second, before,
                                         getFlag(rdram, fv.first), ok ? "ok" : "DID NOT RETURN (unwound)");
                        }
                    }
                    if (g_cfg.menu && getFlag(rdram, kFlagNoCamp))
                    {
                        // keep the command menu available: the field-event task (0x1C8AE4) refuses TRIANGLE while it is set
                        uint32_t v0 = 0u;
                        callGuest(runtime->lookupFunction(kDatSetFlag), rdram, ctx, runtime, kFlagNoCamp, 0u, 0u, 0u, v0);
                        std::fprintf(stderr, "[p3fes-warp] menu: cleared flag 0x%X (command menu was blocked)\n", kFlagNoCamp);
                    }
                    if (g_saveDone && g_saveRuns < g_cfg.save)
                    {
                        // another save UI after the previous one ended (P3FES_WARP_SAVE=n)
                        g_saveDone = g_saveStarted = false;
                        g_saveWaitFrames = 0u;
                        g_saveNextAt = t + 4.0;
                    }
                    if (g_cfg.save && g_saveRuns < g_cfg.save && !g_saveStarted && t >= std::max(g_cfg.at, g_saveNextAt))
                    {
                        g_saveStarted = true;
                        ++g_saveRuns;
                        g_saveStartedAt = t;
                        uint32_t v0 = 0u;
                        const bool ok = callGuest(runtime->lookupFunction(kSaveStartCmd), rdram, ctx, runtime, 0u, 0u, 0u, 0u, v0);
                        std::fprintf(stderr, "[p3fes-warp] save #%u: script command 0x%X (start save UI) %s v0=%u\n", g_saveRuns, kSaveStartCmd,
                                     ok ? "issued" : "DID NOT RETURN (unwound)", v0);
                    }
                    else if (g_saveStarted && !g_saveDone && (++g_saveWaitFrames % 10u) == 0u)
                    {
                        uint32_t v0 = 0u;
                        const uint32_t saveTask = r32(rdram, getRegU32(ctx, 28) - 0x4CF0u);
                        if (g_cfg.log && (g_saveWaitFrames % 100u) == 0u)
                            std::fprintf(stderr, "[p3fes-warp] save: task=%08X state=%u\n", saveTask,
                                         saveTask ? r32(rdram, saveTask + 0x1Cu) & 0xFu : 99u);
                        // The task is torn down by the save UI itself (state 0, not 3): treat that as the end as well and
                        // clear the script's slot (gp-0x4CF0) the way the wait command does.
                        const bool torn = saveTask != 0u && (r32(rdram, saveTask + 0x1Cu) & 0xFu) == 0u;
                        if (torn)
                            w32(rdram, getRegU32(ctx, 28) - 0x4CF0u, 0u);
                        if (torn || (callGuest(runtime->lookupFunction(kSaveWaitCmd), rdram, ctx, runtime, 0u, 0u, 0u, 0u, v0) && v0 == 1u))
                        {
                            g_saveDone = true;
                            std::fprintf(stderr, "[p3fes-warp] save: save UI task ended after %.1fs\n",
                                         t - g_saveStartedAt);
                        }
                    }
                    if (g_cfg.hpSet && t >= g_cfg.at)
                    {
                        // keep the protagonist's current HP (u16 at unit+8) at the requested value through the game's own
                        // setter (datCalcSetHp); only written when it differs, so the HUD sees a stable value
                        const uint32_t cur = r32(rdram, kProtagonistUnit + 8u) & 0xFFFFu;
                        if (cur != g_cfg.hp)
                        {
                            uint32_t v0 = 0u;
                            const bool ok = callGuest(runtime->lookupFunction(kDatCalcSetHp), rdram, ctx, runtime, kProtagonistUnit, g_cfg.hp, 0u, 0u, v0);
                            if (!g_hpLogged)
                            {
                                g_hpLogged = true;
                                uint32_t maxHp = 0u;
                                callGuest(runtime->lookupFunction(kDatCalcGetMaxHp), rdram, ctx, runtime, kProtagonistUnit, 0u, 0u, 0u, maxHp);
                                std::fprintf(stderr, "[p3fes-warp] t=%.1f hp: datCalcSetHp(protagonist, %u) %s, was %u, now %u / max %u\n", t, g_cfg.hp,
                                             ok ? "ok" : "DID NOT RETURN (unwound)", cur, r32(rdram, kProtagonistUnit + 8u) & 0xFFFFu, maxHp & 0xFFFFu);
                            }
                        }
                    }
                    const bool anyPre = g_cfg.field || g_cfg.tartarus || g_cfg.raw;
                    if (g_cfg.field && !g_fieldDone && t >= g_cfg.at)
                    {
                        g_fieldDone = true;
                        warpField(rdram, ctx, runtime);
                    }
                    else if (g_cfg.tartarus && !g_tartarusDone && t >= g_cfg.at)
                    {
                        g_tartarusDone = true;
                        changeSeq(rdram, ctx, runtime, kSeqDungeon, g_cfg.tartarusWords, 8u, "tartarus");
                    }
                    else if (g_cfg.raw && !g_rawDone && t >= g_cfg.at)
                    {
                        g_rawDone = true;
                        changeSeq(rdram, ctx, runtime, g_cfg.rawSeq, g_cfg.rawWords.data(),
                                  static_cast<uint32_t>(g_cfg.rawWords.size()) * 4u, "raw");
                    }
                    const double battleAt = g_cfg.battleAt >= 0.0 ? g_cfg.battleAt : (anyPre ? g_cfg.at + 25.0 : g_cfg.at);
                    if (g_cfg.battle && !g_battleDone && t >= battleAt)
                    {
                        g_battleDone = true;
                        if (g_cfg.log)
                        {
                            // encounter table: pointer at gp-0x4848, 0x1C bytes per entry (btlScrCmd_CALL_BATTLE)
                            const uint32_t table = r32(rdram, getRegU32(ctx, 28) - 0x4848u);
                            std::fprintf(stderr, "[p3fes-warp] encounter table at %08X\n", table);
                            for (uint32_t id = 0u; table != 0u && id < g_cfg.dumpEncounters; ++id)
                            {
                                std::fprintf(stderr, "[p3fes-warp] enc %3u:", id);
                                for (uint32_t i = 0u; i < 0x1Cu; i += 4u)
                                    std::fprintf(stderr, " %08X", r32(rdram, table + id * 0x1Cu + i));
                                std::fprintf(stderr, "\n");
                            }
                        }
                        g_paraArmed = true;
                        g_paraValue = g_cfg.encounter;
                        uint32_t v0 = 0u;
                        const bool ok = callGuest(runtime->lookupFunction(kCallBattleCmd), rdram, ctx, runtime, 0u, 0u, 0u, 0u, v0);
                        g_paraArmed = false;
                        std::fprintf(stderr, "[p3fes-warp] battle encounter %u: btlScrCmd_CALL_BATTLE %s\n", g_cfg.encounter,
                                     ok ? "issued" : "DID NOT RETURN (unwound)");
                    }
                }
            }
            g_updateOriginal(rdram, ctx, runtime);
        }

        bool installOne(PS2Runtime &runtime, uint32_t address, GuestFunction hook, GuestFunction &original)
        {
            if (!runtime.hasFunction(address))
                return false;
            GuestFunction current = runtime.lookupFunction(address);
            if (!current || current == hook)
                return false;
            original = current;
            return runtime.replaceFunction(address, hook);
        }

        double readSeconds(const char *name, double fallback)
        {
            const char *v = std::getenv(name);
            if (!v || !*v)
                return fallback;
            return std::strtod(v, nullptr);
        }
    }

    bool install(PS2Runtime &runtime)
    {
        g_cfg = Config{};
        std::vector<uint32_t> list;
        if (const char *v = std::getenv("P3FES_WARP_LOG"); v && *v && *v != '0')
            g_cfg.log = true;
        if (const char *v = std::getenv("P3FES_WARP"); v && *v)
        {
            if (parseList(v, list) && list.size() >= 2u && list.size() <= 5u)
            {
                g_cfg.field = true;
                for (size_t i = 0u; i < list.size(); ++i)
                    g_cfg.fieldWords[i] = list[i];
            }
            else
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP='%s' (want major,minor[,entry[,w3[,w4]]])\n", v);
        }
        if (const char *v = std::getenv("P3FES_WARP_TARTARUS"); v && *v)
        {
            if (parseList(v, list) && list.size() <= 2u)
            {
                g_cfg.tartarus = true;
                g_cfg.tartarusWords[0] = list[0];
                g_cfg.tartarusWords[1] = list.size() > 1u ? list[1] : 0u;
            }
            else
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP_TARTARUS='%s' (want floor[,arg])\n", v);
        }
        if (const char *v = std::getenv("P3FES_WARP_BATTLE"); v && *v)
        {
            if (parseList(v, list) && list.size() == 1u)
            {
                g_cfg.battle = true;
                g_cfg.encounter = list[0];
            }
            else
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP_BATTLE='%s' (want encounter id)\n", v);
        }
        if (const char *v = std::getenv("P3FES_WARP_SEQ"); v && *v)
        {
            if (parseList(v, list) && list.size() >= 2u && list.size() <= 16u)
            {
                g_cfg.raw = true;
                g_cfg.rawSeq = list[0];
                g_cfg.rawWords.assign(list.begin() + 1, list.end());
            }
            else
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP_SEQ='%s' (want seq,word[,word...])\n", v);
        }
        if (const char *v = std::getenv("P3FES_WARP_FLAGS"); v && *v)
        {
            if (!parseFlagList(v, g_cfg.setFlags))
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP_FLAGS='%s' (want flag[=value],...)\n", v);
        }
        if (const char *v = std::getenv("P3FES_WARP_FLAGLOG"); v && *v)
        {
            std::vector<std::pair<uint32_t, uint32_t>> tmp;
            if (parseFlagList(v, tmp))
                for (const auto &fv : tmp)
                    g_cfg.logFlags.push_back(fv.first);
        }
        if (const char *v = std::getenv("P3FES_WARP_TASKS"); v && *v && *v != '0')
            g_cfg.logTasks = true;
        if (const char *v = std::getenv("P3FES_WARP_MENU"); v && *v && *v != '0')
            g_cfg.menu = true;
        if (const char *v = std::getenv("P3FES_WARP_SAVE"); v && *v)
            g_cfg.save = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        if (const char *v = std::getenv("P3FES_WARP_HP"); v && *v)
        {
            char *end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 0);
            if (end != v && n >= 1ul && n <= 999ul)
            {
                g_cfg.hpSet = true;
                g_cfg.hp = static_cast<uint32_t>(n);
            }
            else
                std::fprintf(stderr, "[p3fes-warp] ignoring P3FES_WARP_HP='%s' (want 1..999)\n", v);
        }
        g_cfg.at = readSeconds("P3FES_WARP_AT", 5.0);
        g_cfg.battleAt = readSeconds("P3FES_WARP_BATTLE_AT", -1.0);
        g_cfg.dumpEncounters = static_cast<uint32_t>(readSeconds("P3FES_WARP_DUMP_ENC", 0.0));

        const bool wanted = g_cfg.log || !g_cfg.setFlags.empty() || !g_cfg.logFlags.empty() || g_cfg.logTasks || g_cfg.menu || g_cfg.save != 0u || g_cfg.hpSet || g_cfg.field || g_cfg.tartarus || g_cfg.battle || g_cfg.raw;
        if (!wanted)
            return false;

        g_hpLogged = false; g_flagsDone = false; g_flagLogNext = 0.0; g_saveStarted = g_saveDone = false; g_saveWaitFrames = 0u; g_saveRuns = 0u; g_saveNextAt = 0.0;
        g_fieldSeen = g_fieldDone = g_tartarusDone = g_rawDone = g_battleDone = g_paraArmed = false;
        const bool change = installOne(runtime, kAdminiChangeSeq, &changeSeqHook, g_changeOriginal);
        const bool para = installOne(runtime, kScrGetIntPara, &paraHook, g_paraOriginal);
        const bool update = installOne(runtime, kKwlnUpdate, &updateHook, g_updateOriginal);
        const bool create = g_cfg.log && installOne(runtime, kFieldCreate, &fieldCreateHook, g_fieldCreateOriginal);
        std::fprintf(stderr,
                     "[p3fes-warp] installed change=%u para=%u update=%u fieldlog=%u | field=%u(F%u_%u e=%u) tartarus=%u(floor %u) "
                     "battle=%u(enc %u) raw=%u at=%.1fs\n",
                     change, para, update, create, g_cfg.field, g_cfg.fieldWords[0], g_cfg.fieldWords[1], g_cfg.fieldWords[2],
                     g_cfg.tartarus, g_cfg.tartarusWords[0], g_cfg.battle, g_cfg.encounter, g_cfg.raw, g_cfg.at);
        return change && update;
    }
}

// Scene-system facts used above (all from the recompiled MIPS in ps2xRuntime/src/runner):
//  * The sequence manager ("admini") task owns the current/next sequence ids: task data +8 = current id (s8),
//    +9 = requested id (s8, -1 = none), +0x1C = malloc'd copy of the request parameters (adminiChangeSeq copies
//    `size` bytes from a1). adminiChangeSeq(id, params, size, push) at 0x27C080 only records the request; the next
//    adminiUpdateTask (0x27C3B0) runs the old sequence's Exit, waits for its Check, then the new sequence's Call.
//  * Function table at 0x68F020, 12 bytes per id {call, exit, check}: 1 test, 2 field (0x1B77B0/0x1B7850/0x1B78A0),
//    3 field-with-event (0x1B7900), 4 0x4241E0.., 5 dungeon (0x1B7A50/0x1B7A90/0x1B7AC0), 6 battle (0x27CA70),
//    7 0x27CB10 (fcl shop, 0x3E0780).
//  * Field params (0x1C bytes): u16 major, u16 minor, u16 entry, s16, s16 -> k_field_001ba5f0 (creates the field
//    task, global at 0x869F60 word 0).
//  * Dungeon params (8 bytes): u32 floor, u32 mode -> K_FldDungeon_CreateTask (task pointer at gp-0x4A88).
