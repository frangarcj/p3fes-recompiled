#include "p3fes_native_frame_cuts.h"
#include "p3fes_native_dialogue.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

// All addresses below were re-derived from the generated disassembly in
// ps2xRuntime/src/runner (the comments name the instruction addresses).
namespace p3fes_native_frame_cuts
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;

        enum class Mode : uint8_t { Off, On, Verify, Log };

        constexpr uint32_t kShadowMapTask = 0x0019C4B0u;
        constexpr uint32_t kAdxtExecServer = 0x00549EA0u;
        constexpr uint32_t kRwCalloc = 0x00104040u;
        constexpr uint32_t kRwMalloc = 0x00104140u;
        constexpr uint32_t kRwFree = 0x00104280u;
        constexpr uint32_t kDbprtMain = 0x00104420u;

        GuestFunction g_shadowOriginal = nullptr;
        GuestFunction g_adxOriginal = nullptr;
        GuestFunction g_callocOriginal = nullptr;
        GuestFunction g_mallocOriginal = nullptr;
        GuestFunction g_freeOriginal = nullptr;
        GuestFunction g_dbprtOriginal = nullptr;

        Mode g_shadowMode = Mode::Off;
        Mode g_rwAllocMode = Mode::Off;
        Mode g_adxMode = Mode::Off;

        // Hooks run on the single EE worker thread; plain counters are enough.
        struct Counters
        {
            uint64_t shadowCalls = 0u;
            uint64_t shadowPasses = 0u; // state == 1 calls that skipped a full pass
            uint64_t adxHit = 0u;
            uint64_t adxValve = 0u;
            uint64_t adxMiss = 0u;
            uint64_t adxMissGuard = 0u;
            uint64_t adxMissAdxt = 0u;
            uint64_t adxMissRna = 0u;
            uint64_t adxMissDtr = 0u;
            uint64_t adxMissSjd = 0u;
            uint64_t adxMissDtx = 0u;
            uint64_t adxMissAdxtLive = 0u; // subset of adxMissAdxt: state 1..4 / unknown
            uint64_t adxMissAdxtAct = 0u;  // subset: stopped/ended handle whose tick would act
            uint64_t adxtTermHit = 0u;     // subset of adxHit: ticks judged idle only thanks to the TermSupply no-op rule
            uint64_t adxVerifyOk = 0u;
            uint64_t adxVerifyBad = 0u;
            uint64_t adxVerifyUnwound = 0u;  // original returned with pc != ra (guest unwound/preempted): not comparable
            uint64_t adxtVerifyOk = 0u;      // ADXT table unchanged by a tick whose ADXT part was judged idle
            uint64_t adxtVerifyBad = 0u;
            uint64_t adxtVerifyUnwound = 0u;
            uint64_t allocHit = 0u;
            uint64_t allocFall = 0u;
            uint64_t freeHit = 0u;
            uint64_t freeFall = 0u;
            uint64_t callocHit = 0u;
            uint64_t callocFall = 0u;
            uint64_t verifyOk = 0u;
            uint64_t verifyBad = 0u;
            uint64_t dbprtCalls = 0u;
        } g_counters;

        bool g_diag = false;

        // ---- guest memory helpers -------------------------------------------------

        inline uint32_t r32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        inline uint16_t r16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        inline uint8_t r8(const uint8_t *rdram, uint32_t address)
        {
            return rdram[address & PS2_RAM_MASK];
        }

        inline void w32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }

        inline void w16(uint8_t *rdram, uint32_t address, uint16_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }

        inline void w8(uint8_t *rdram, uint32_t address, uint8_t value)
        {
            rdram[address & PS2_RAM_MASK] = value;
        }

        inline void returnToCaller(R5900Context *ctx)
        {
            ctx->pc = getRegU32(ctx, 31);
        }

        Mode readMode(const char *name, bool allowVerify)
        {
            const char *value = std::getenv(name);
            if (!value || !*value)
                return Mode::On;
            if (value[0] == '0' && value[1] == '\0')
                return Mode::Off;
            if (allowVerify && std::strcmp(value, "verify") == 0)
                return Mode::Verify;
            if (allowVerify && std::strcmp(value, "log") == 0)
                return Mode::Log; // hook installed, only logs; the original always runs
            return Mode::On;
        }

        void logTrace();

        void maybeLog()
        {
            if (!g_diag)
                return;
            static auto previous = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - previous < std::chrono::seconds(5))
                return;
            previous = now;
            logCounters();
            logTrace();
        }

        // ---- 1. K_FldShadow_UpdateShadowMapTask (0x19C4B0) ---------------------------
        //
        // a0+0x3C -> state block; state word at [block]:
        //   0x19C504..0x19C538  state 0     -> [block] = 1, v0 = 0 (no pass)
        //   0x19C50C            state 2     -> v0 = -1
        //   0x19C518 -> 0x19C540 state 1    -> the render pass, then v0 = 0 (0x19D234); state stays 1
        //   0x19C528            other       -> v0 = 0
        //
        // Net guest-state effect of the state 1 pass (proved from the disassembly):
        //   * the pass walks two caster lists (MT_Scene_GetResListHead(1) at 0x19C4E8 and (3) at
        //     0x19C4F8; node->next at +0xF8) and handles a caster only when
        //       [c+0x28] & 2, sd = [[c+listOffset]+0x3C] (listOffset 0x1E4 for list 1, 0x1F0 for
        //       list 3), hu[sd+0xA] == 1 and (int)[sd] > 0 (0x19C5AC..0x19C5DC / 0x19CC48..0x19CC78)
        //     (and RwCameraBeginUpdate succeeding; the failure branch only asserts).
        //   * per handled caster, model = [c+0x128]:
        //       - bit 0 of byte [model + 0x3B4 + 12*i], i = 0..4: cleared (0x19C91C) before the model draw
        //         and set again (0x19C9E4 / 0x19CFD8) after it  -> net: set
        //       - hu[model+0xD8] bit 2: set before the draw, cleared after (0x19C9A4/0x19C9C0,
        //         0x19CFA0/0x19CFBC)                               -> net: cleared
        //       - [gp-0x4B98]: 1 before the draw, 0 after (0x19C940/0x19C9D4, 0x19CF98/0x19CFC8) -> net: 0
        //       - [gp-0x6B04] (list 1 only): 0 or untouched before the draw, 1 after (0x19C9D0) -> net: 1
        //   * directional/second/ambient light enable bytes (+2 of the three kwlnGet*Light objects) are
        //     read at 0x19C548/0x19C568/0x19C588, zeroed, and restored at 0x19D1E4..0x19D214 -> net zero.
        //   * model colour (+0xD0..D3) is read (mdlGetColor), overwritten and restored (0x19CA1C) -> net zero.
        // Everything else the pass touches is private to the shadow camera (frames, matrices, view window,
        // clip planes, raster clear), the GIF packet stream the host never presents (the render target is
        // not implemented), RenderWare render states that every later pass sets for itself, and stack.

        constexpr uint32_t kGpSceneRoot = 0x57B0u;   // lw $v0,-0x57B0($gp) in MT_Scene_GetResListHead
        constexpr uint32_t kGpShadowDrawFlag = 0x4B98u;
        constexpr uint32_t kGpShadowModelFlag = 0x6B04u;
        constexpr uint32_t kMaxCasters = 4096u;

        uint32_t resListHead(const uint8_t *rdram, uint32_t gp, uint32_t index)
        {
            const uint32_t root = r32(rdram, gp - kGpSceneRoot);
            const uint32_t table = r32(rdram, root + 8u);
            if (!table || index >= 0x16u)
                return 0u;
            return r32(rdram, table + index * 4u);
        }

        // Returns the caster's model when the pass would draw it, else 0.
        uint32_t casterModel(const uint8_t *rdram, uint32_t caster, uint32_t listOffset)
        {
            if ((r32(rdram, caster + 0x28u) & 2u) == 0u)
                return 0u;
            const uint32_t owner = r32(rdram, caster + listOffset);
            const uint32_t shadow = r32(rdram, owner + 0x3Cu);
            if (r16(rdram, shadow + 0xAu) != 1u)
                return 0u;
            if (static_cast<int32_t>(r32(rdram, shadow)) <= 0)
                return 0u;
            return r32(rdram, caster + 0x128u);
        }

        void applyShadowNetEffects(uint8_t *rdram, uint32_t gp)
        {
            struct List { uint32_t index, listOffset; bool list1; };
            static constexpr List kLists[2] = {{1u, 0x1E4u, true}, {3u, 0x1F0u, false}};
            for (const List &list : kLists)
            {
                uint32_t caster = resListHead(rdram, gp, list.index);
                for (uint32_t guard = 0u; caster && guard < kMaxCasters; ++guard, caster = r32(rdram, caster + 0xF8u))
                {
                    const uint32_t model = casterModel(rdram, caster, list.listOffset);
                    if (!model)
                        continue;
                    for (uint32_t i = 0u; i < 5u; ++i)
                    {
                        const uint32_t flags = model + 0x3B4u + 12u * i;
                        w8(rdram, flags, static_cast<uint8_t>(r8(rdram, flags) | 1u));
                    }
                    w16(rdram, model + 0xD8u, static_cast<uint16_t>(r16(rdram, model + 0xD8u) & 0xFFFBu));
                    w32(rdram, gp - kGpShadowDrawFlag, 0u);
                    if (list.list1)
                        w32(rdram, gp - kGpShadowModelFlag, 1u);
                }
            }
        }

        // Diagnostic: run the original pass and print every guest word it changed outside the stack.
        void verifyShadowPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::vector<uint8_t> before;
            static uint32_t passes = 0u;
            static uint32_t populated = 0u;
            static uint32_t suspended = 0u;
            std::vector<std::vector<uint32_t>> lines;
            const uint32_t sp = getRegU32(ctx, 29);
            before.resize(PS2_RAM_SIZE);
            std::memcpy(before.data(), rdram, PS2_RAM_SIZE);
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t listModels[2] = {resListHead(rdram, gp, 1), resListHead(rdram, gp, 3)};
            const uint32_t lightPtr[3] = {r32(rdram, gp - 0x4C44u), r32(rdram, gp - 0x4C40u), r32(rdram, gp - 0x4C48u)};
            const uint8_t lightBefore[3] = {r8(rdram, lightPtr[0] + 2u), r8(rdram, lightPtr[1] + 2u), r8(rdram, lightPtr[2] + 2u)};
            const uint32_t returnPc = getRegU32(ctx, 31);
            g_shadowOriginal(rdram, ctx, runtime);
            ++passes;
            // The pass usually yields to the EE scheduler (time slice) at some call and resumes later
            // through an internal label; the comparison then only covers the part that ran inside this
            // call.
            const bool partial = ctx->pc != returnPc;
            if (partial)
                ++suspended;
            if (passes % 500u == 1u)
                std::fprintf(stderr, "[p3fes-cuts] lights %08X/%08X/%08X enable bytes before %u/%u/%u after %u/%u/%u\n",
                             lightPtr[0], lightPtr[1], lightPtr[2], lightBefore[0], lightBefore[1], lightBefore[2],
                             r8(rdram, lightPtr[0] + 2u), r8(rdram, lightPtr[1] + 2u), r8(rdram, lightPtr[2] + 2u));
            const uint32_t stackLow = (sp - 0x6000u) & PS2_RAM_MASK;
            const uint32_t stackHigh = PS2_RAM_SIZE; // the stack is the top of RAM
            uint32_t ranges = 0u, words = 0u;
            const uint32_t *a = reinterpret_cast<const uint32_t *>(before.data());
            const uint32_t *b = reinterpret_cast<const uint32_t *>(rdram);
            std::fprintf(stderr, "[p3fes-cuts] shadow verify pass %u: sp=%08X heads=%08X/%08X gp=%08X\n",
                         passes, sp, listModels[0], listModels[1], gp);
            for (uint32_t i = 0u; i < PS2_RAM_SIZE / 4u; ++i)
            {
                if (a[i] == b[i])
                    continue;
                const uint32_t address = i * 4u;
                if (address >= stackLow && address < stackHigh)
                    continue;
                uint32_t end = i + 1u;
                while (end < PS2_RAM_SIZE / 4u && end - i < 64u && a[end] != b[end])
                    ++end;
                words += end - i;
                if (ranges < 600u)
                {
                    std::vector<uint32_t> line{address, end - i};
                    for (uint32_t k = i; k < end && k < i + 6u; ++k)
                    {
                        line.push_back(a[k]);
                        line.push_back(b[k]);
                    }
                    lines.push_back(std::move(line));
                }
                ++ranges;
                i = end - 1u;
            }
            if (words && ++populated <= 4u)
            {
                for (const auto &line : lines)
                {
                    std::fprintf(stderr, "  diff %08X +%u words:", line[0], line[1]);
                    for (size_t k = 2u; k + 1u < line.size(); k += 2u)
                        std::fprintf(stderr, " %08X>%08X", line[k], line[k + 1u]);
                    std::fprintf(stderr, "\n");
                }
            }
            std::fprintf(stderr, "[p3fes-cuts] shadow verify pass %u%s: %u ranges, %u words outside the stack\n",
                         passes, partial ? " (partial, yielded)" : "", ranges, words);
        }

        void shadowMapHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            ++g_counters.shadowCalls;
            const uint32_t block = r32(rdram, getRegU32(ctx, 4) + 0x3Cu);
            const uint32_t state = r32(rdram, block);
            if (g_shadowMode == Mode::Verify && state == 1u && g_shadowOriginal)
            {
                verifyShadowPass(rdram, ctx, runtime);
                return;
            }
            int32_t result = 0;
            if (state == 0u)
            {
                w32(rdram, block, 1u);
            }
            else if (state == 2u)
            {
                result = -1;
            }
            else if (state == 1u)
            {
                applyShadowNetEffects(rdram, getRegU32(ctx, 28));
                ++g_counters.shadowPasses;
            }
            setReturnS32(ctx, result);
            returnToCaller(ctx);
        }

        // ---- 1b. libmc2 free-space count (_sceMcFatGetFreeClustSize, 0x515A40) -------
        //
        // The memory-card screens call sceMc2GetInfo in a loop, and every call counts the card's
        // free clusters again: one _sceMcFatReadFatData per cluster of the card (about 8000), each
        // FAT page fetched through the real card driver running in the IOP interpreter. That is
        // 83 % of the game thread's time in the save/load menu (5 fps on the console).
        //
        // The count only changes when the card is written, so it is remembered per socket and
        // replayed until something is written, erased, formatted or re-mounted (the entry hooks
        // below bump g_mcGeneration). The value is always the one the original function computed:
        // it is captured when the original returns in line, or, when it was suspended on the way
        // (it waits for the IOP), at the caller's return address 0x516094, whose dispatch slot is
        // hooked for that purpose. P3FES_CUT_MCFREE=0 disables it.
        bool installOne(PS2Runtime &runtime, uint32_t address, GuestFunction hook, GuestFunction &original);
        constexpr uint32_t kMcFreeClustSize = 0x00515A40u;
        constexpr uint32_t kMcFreeClustReturn = 0x00516094u; // in _sceMcFatGetInfo, after the jal
        constexpr uint32_t kMcInvalidators[] = {
            0x005157E0u, // _sceMcFatWriteFatData
            0x0051B8F0u, // _sceMcCoreWritePage
            0x0051BF40u, // _sceMcCoreWritePageFast
            0x0051BA88u, // _sceMcCoreWriteSameData
            0x0051BBD8u, // _sceMcCoreBlockErase
            0x00516118u, // _sceMcFatFormat
            0x00516A48u, // _sceMcFatUnFormat
            0x005153A0u, // _sceMcFatInit
            0x00515400u, // _sceMcFatInit2
        };
        constexpr size_t kMcInvalidatorCount = sizeof(kMcInvalidators) / sizeof(kMcInvalidators[0]);
        constexpr uint32_t kMcSockets = 8u;

        GuestFunction g_mcFreeOriginal = nullptr;
        GuestFunction g_mcReturnOriginal = nullptr;
        GuestFunction g_mcInvalidatorOriginal[kMcInvalidatorCount] = {};
        uint64_t g_mcGeneration = 1u;
        struct McFreeEntry
        {
            uint64_t generation = 0u; // 0: nothing remembered
            int32_t freeClusters = 0;
        };
        McFreeEntry g_mcFree[kMcSockets];
        // The one count in flight (the library serialises card requests).
        struct McFreePending
        {
            bool active = false;
            uint32_t socket = 0u, errorAddress = 0u;
            uint64_t generation = 0u;
        } g_mcPending;
        uint64_t g_mcFreeHits = 0u, g_mcFreeCounts = 0u;

        void mcRemember(const uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_mcPending.active)
                return;
            g_mcPending.active = false;
            if (g_mcPending.generation != g_mcGeneration || g_mcPending.socket >= kMcSockets ||
                r32(rdram, g_mcPending.errorAddress) != 0u)
                return;
            g_mcFree[g_mcPending.socket] = {g_mcGeneration, static_cast<int32_t>(getRegU32(ctx, 2))};
        }

        void mcFreeClustSizeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t socket = getRegU32(ctx, 4);
            const uint32_t errorAddress = getRegU32(ctx, 5);
            if (socket < kMcSockets && g_mcFree[socket].generation == g_mcGeneration)
            {
                ++g_mcFreeHits;
                w32(rdram, errorAddress, 0u);
                setReturnS32(ctx, g_mcFree[socket].freeClusters);
                returnToCaller(ctx);
                return;
            }
            ++g_mcFreeCounts;
            g_mcPending = {true, socket, errorAddress, g_mcGeneration};
            const uint32_t returnPc = getRegU32(ctx, 31);
            g_mcFreeOriginal(rdram, ctx, runtime); // may unwind (suspended): then mcReturnHook captures
            if (ctx->pc == returnPc)
                mcRemember(rdram, ctx);
        }

        void mcReturnHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx && ctx->pc == kMcFreeClustReturn)
                mcRemember(rdram, ctx);
            g_mcReturnOriginal(rdram, ctx, runtime);
        }

        template <size_t Index>
        void mcInvalidateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_mcGeneration;
            g_mcPending.active = false;
            g_mcInvalidatorOriginal[Index](rdram, ctx, runtime);
        }

        template <size_t... Index>
        bool installMcInvalidators(PS2Runtime &runtime, std::index_sequence<Index...>)
        {
            return (installOne(runtime, kMcInvalidators[Index], &mcInvalidateHook<Index>, g_mcInvalidatorOriginal[Index]) && ...);
        }

        // ---- 1c. Sofdec scratchpad swap (0x585468 -> 0x581108 -> 0x580CB0) -----------
        //
        // The CRI servers bracket their work with a pair of Sofdec callbacks that swap the 16 KiB
        // scratchpad with a private copy (two 16 KiB SPR DMA transfers in, two out: 0x580CB0 and
        // 0x580D40) so the MPEG decoder finds its work area there. They run on every server round,
        // movie or not: 3 % of the game thread in the field. The swap-in is skipped while no Sofdec
        // task has ticked for two seconds; the swap-out then finds its state word (0x7B4530) at 0
        // and does nothing by itself. A swap that is already in (state 1) is never skipped.
        // P3FES_CUT_SFD_SPR=0 disables it.
        constexpr uint32_t kSfdSwapIn = 0x00585468u;
        constexpr uint32_t kSfdSwapState = 0x007B4530u;
        constexpr uint32_t kSfdUpdateTask = 0x0010A860u; // H_SfdPlay_UpdateTask
        constexpr uint64_t kSfdIdleTicks = 120u;
        GuestFunction g_sfdSwapOriginal = nullptr;
        GuestFunction g_sfdTaskOriginal = nullptr;
        uint64_t g_sfdLastTaskTick = 0u;
        bool g_sfdTaskSeen = false;
        uint64_t g_sfdSwapSkipped = 0u, g_sfdSwapRun = 0u;

        void sfdTaskHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (runtime)
            {
                g_sfdLastTaskTick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                g_sfdTaskSeen = true;
            }
            g_sfdTaskOriginal(rdram, ctx, runtime);
        }

        void sfdSwapInHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx && runtime && r32(rdram, kSfdSwapState) == 0u)
            {
                const uint64_t now = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                if (!g_sfdTaskSeen || now - g_sfdLastTaskTick > kSfdIdleTicks)
                {
                    ++g_sfdSwapSkipped;
                    returnToCaller(ctx);
                    return;
                }
            }
            ++g_sfdSwapRun;
            g_sfdSwapOriginal(rdram, ctx, runtime);
        }

        // ---- 1d. redundant _rwSkySetRenderState calls (0x4D6B90) ---------------------
        //
        // The game sets about 620 render states per frame through the device's SetRenderState, and
        // 69 % of the calls repeat the value that state already has (ZTESTENABLE alone 160 times a
        // frame). The function is 3.4 KiB of GS-register bookkeeping: 4 % of the game thread.
        // P3FES_CUT_RS=verify runs every call and reports any repeat call that still changed the
        // driver's state block (gp-20304 .. gp-16900), which is the evidence needed before repeats
        // may be dropped; P3FES_CUT_RS=1 drops repeats of the states listed in g_rsDroppable.
        constexpr uint32_t kSkySetRenderState = 0x004D6B90u;
        constexpr uint32_t kRwGpAddress = 0x007D2CF0u;
        constexpr uint32_t kRsBlockStart = kRwGpAddress - 20304u, kRsBlockSize = 3404u;
        GuestFunction g_rsOriginal = nullptr;
        Mode g_rsMode = Mode::Off;
        uint64_t g_rsCalls[64] = {}, g_rsSame[64] = {}, g_rsSameChanged[64] = {}, g_rsDropped = 0u;
        uint32_t g_rsLast[64] = {};
        bool g_rsKnown[64] = {};
        uint64_t g_rsDroppable = 0u; // bit per state, filled from the verify results (see install)
        unsigned g_rsChangeLogs = 0u;
        void renderStateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t state = getRegU32(ctx, 4), value = getRegU32(ctx, 5);
            if (state >= 64u)
            {
                g_rsOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_rsCalls[state];
            const bool same = g_rsKnown[state] && g_rsLast[state] == value;
            g_rsKnown[state] = true;
            g_rsLast[state] = value;
            if (!same)
            {
                g_rsOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_rsSame[state];
            if (g_rsMode == Mode::On && ((g_rsDroppable >> state) & 1u) != 0u)
            {
                ++g_rsDropped;
                setReturnS32(ctx, 1);
                returnToCaller(ctx);
                return;
            }
            if (g_rsMode != Mode::Verify)
            {
                g_rsOriginal(rdram, ctx, runtime);
                return;
            }
            static uint8_t before[kRsBlockSize];
            std::memcpy(before, rdram + kRsBlockStart, kRsBlockSize);
            const uint32_t returnPc = getRegU32(ctx, 31);
            g_rsOriginal(rdram, ctx, runtime);
            if (ctx->pc != returnPc)
                return;
            // The GIF packet cursors (gp-17184, gp-17148) advance when the call emits a packet. The
            // native renderer never rasterises those packets, so that alone is not a state change.
            // The same goes for the whole packet bookkeeping block gp-17216 .. gp-17120 (current packet,
            // its start and end, the cursors): a repeat that is the first emission after a flush opens a
            // new packet there.
            {
                const uint32_t first = kRwGpAddress - 17216u - kRsBlockStart, last = kRwGpAddress - 17120u - kRsBlockStart;
                std::memcpy(before + first, rdram + kRsBlockStart + first, last - first);
            }
            if (std::memcmp(before, rdram + kRsBlockStart, kRsBlockSize) == 0)
                return;
            ++g_rsSameChanged[state];
            if (g_rsChangeLogs < 40u)
            {
                ++g_rsChangeLogs;
                std::fprintf(stderr, "[p3fes-cuts] render-state repeat changed state: state=%u value=%08X:", state, value);
                unsigned shown = 0u;
                for (uint32_t k = 0u; k + 4u <= kRsBlockSize && shown < 6u; k += 4u)
                    if (std::memcmp(before + k, rdram + kRsBlockStart + k, 4u) != 0)
                    {
                        uint32_t was = 0u;
                        std::memcpy(&was, before + k, 4u);
                        std::fprintf(stderr, " gp%+d %08X>%08X", static_cast<int>(kRsBlockStart + k) - static_cast<int>(kRwGpAddress),
                                     was, r32(rdram, kRsBlockStart + k));
                        ++shown;
                    }
                std::fprintf(stderr, "\n");
            }
        }

        // ---- 2. ADXT_ExecServer idle gate (0x549EA0) ---------------------------------
        //
        // 0x549EA0: jal 0x539848 (jr ra), jal 0x549EC8, tail j 0x539850 (jr ra).
        // 0x549EC8 body: SVM_Lock; if [0x793B18] != 0 return (re-entrancy guard); [0x793B18] = 1; unlock;
        // lock; 0x542F00 (16 SJD objects at 0x78FF98, stride 0xA8); [0x793B18] = 2; 16 ADXT objects at
        // 0x787570 (stride 0xC8) -> 0x54C9D8 -> adxt_ExecHndl 0x54CA08; [0x793B18] = 3; 0x541B28 (RNA
        // ExecServer 0x561138: RNA objects at 0x7AB850 stride 0x64 -> 0x560F78, DTR 0x559140: 32 slots at
        // 0x7A0938 stride 0x40, then SJX 0x559D18: 16 DTX objects at 0x7A1160 stride 0x44); if
        // [0x793B20] < 0x33 0x541B28 again; [0x793B18] = 0; unlock.
        // The lock pair is balanced (SVM_Lock 0x564888 / SVM_Unlock 0x5648E8 only toggle a recursion
        // counter and call an optional callback), so with every sub-step a no-op the net state change is
        // only the DTX serial counter at 0x7A1158 (incremented once per allocated DTX object at
        // 0x559938/0x559958), which the gate reproduces.
        //
        // Why each step is a no-op under the gate conditions:
        //   adxt_ExecHndl 0x54CA08 (per allocated object, [obj+0]==1): dispatch on signed byte [obj+1]:
        //     3 -> 0x54C288, 1 -> 0x54BCD8, 2 -> 0x54C100, 4 -> 0x54C370, 5 -> 0x54C578 (jr ra only),
        //     0 / 6 / anything else -> no handler. Then 0x54C940(obj) and tail 0x54C8C0(obj).
        //   0x54C940: [obj+8] (stm) == 0 -> return. v0 = 0x548768(stm) = lock 0x539848 (jr ra),
        //     0x5487A0(stm) = (a0 != 0) signed byte [stm+1], unlock 0x539850 (jr ra) -> pure read.
        //     v0 == 0 -> return. Else jump table 0x7C30B0 on signed byte [obj+2]
        //     (0,1 -> 0x54C998: 0x546368(stm) = 0x545C38 (jr ra), 0x5463A0 = lb [stm+1], 0x545C40 (jr ra);
        //     == 3 -> 0x5420A0([obj+4]); 2 -> 0x54C9C0: 0x5420A0([obj+4]); 3,4 / out of range -> return).
        //   0x54C8C0: stm != 0 and 0x546368(stm) == 4 -> writes [obj+0x60] = -1 (u16), [obj+1] = 6;
        //     [obj+0x94] (lsc) != 0 and 0x55D790(lsc) (signed byte [lsc+1], pure read) == 3 -> same writes.
        //   So for a handle in state 0/5/6 the whole tick is a pure read unless one of those conditions holds;
        //   adxtObjState() reproduces them from RAM reads only (conservatively: any case that would call
        //   0x5420A0 or write is "busy"), except for the 0x5420A0 call itself, see sjdTermSupplyIsNoop().
        //   State 5 is ADXT PLAYEND (handler 0x54C578 = adxt_stat_playend, a bare jr ra): the stream has finished and
        //   the handle waits for ADXT_Stop. A music stream that ran out in Tartarus stays there for thousands of ticks: stm
        //   state 3 (READEND), sub-state [obj+2] == 0, lsc state 0, so 0x54C940 reaches 0x5420A0([obj+4]) every
        //   tick. That call is two optional callbacks ([adxb+0xB4] / [adxb+0xC0], adxb = [[obj+4]+4]); with both words
        //   0 it executes four loads and two not-taken branches. The gate therefore treats "0x5420A0 would be called,
        //   both words 0" as idle and re-evaluates it every tick (a callback appearing, stm state 4 or lsc state 3
        //   still send the tick down the original path). Verified with P3FES_CUT_ADXT_IDLE=verify: the whole ADXT /
        //   SJD / ADXB table range is bit-identical across such ticks. P3FES_CUT_ADXT_TERM=0 disables only this rule.
        //   RNA 0x560F78: returns at 0x560FA0 unless byte [obj+0x58] == 1.
        //   DTR 0x559140: slot [+1]==1 && [+2]==1 -> 0x503500 (sceSifDmaStat) whose result can return
        //     early (skipping SJX); slot [+0]==1 -> 0x558DD0, which returns immediately unless [+1]==1.
        //   SJD 0x542BC8 (not in the original notes): acts when [obj+0xA0] > 0, [obj+0xA4] > 0 or state
        //     byte [obj+1] is 1 or 2.
        //   DTX 0x559938 (not in the original notes): see adxServerBusy(); the first version of this gate
        //     assumed "acts only when [obj+1] == 1", which verify mode disproved (state 0 sends).

        constexpr uint32_t kAdxGuard = 0x00793B18u;
        constexpr uint32_t kAdxtBase = 0x00787570u, kAdxtStride = 0xC8u;
        constexpr uint32_t kSjdBase = 0x0078FF98u, kSjdStride = 0xA8u;
        constexpr uint32_t kRnaBase = 0x007AB850u, kRnaStride = 0x64u;
        constexpr uint32_t kDtrBase = 0x007A0938u, kDtrStride = 0x40u;
        constexpr uint32_t kDtxBase = 0x007A1160u, kDtxStride = 0x44u;
        constexpr uint32_t kDtxCounter = 0x007A1158u;

        uint32_t g_lastBusyAdxt = 0u;

        enum class AdxBusy : uint8_t { Idle, Guard, Adxt, Rna, Dtr, Sjd, Dtx };
        enum class AdxtObj : uint8_t { Idle, Live, Acts };

        // The 0x5420A0([obj+4]) call (SJD "stream ended" notification of a stopped/ended handle whose stream
        // is in state 3 = READEND; state 5 is PLAYEND, 0x54C578 = adxt_stat_playend, a bare jr ra):
        //   0x5420A0(sjd): 0x536080([sjd+4]) then tail 0x535E28([sjd+4]); with adxb = [sjd+4]
        //   0x536080(adxb): v = [adxb+0xB4]; if (v != 0) (*[0x7836C0])(v);
        //   0x535E28(adxb): v = [adxb+0xC0]; if (v != 0) (*[0x7836AC])(v);
        // (both are leaf-like: only the return address goes to the dead stack). When both words are 0 the whole
        // call is four loads and two not-taken branches: no guest state changes, tick after tick. Observed in
        // Tartarus: obj 1 sits in state 5 with stm state 3 for thousands of ticks and both words 0 (adxb 0x7839F4).
        // Anything else (a callback installed, null pointers) keeps the original path.
        // Switch: P3FES_CUT_ADXT_TERM=0.
        bool g_adxtTermNoop = true;
        bool g_adxtTermUsed = false; // set by adxtObjState when this rule made an object idle (reset per tick)

        bool sjdTermSupplyIsNoop(const uint8_t *rdram, uint32_t o)
        {
            if (!g_adxtTermNoop)
                return false;
            const uint32_t sjd = r32(rdram, o + 4u);
            if (sjd == 0u)
                return false;
            const uint32_t adxb = r32(rdram, sjd + 4u);
            if (adxb == 0u)
                return false;
            return r32(rdram, adxb + 0xB4u) == 0u && r32(rdram, adxb + 0xC0u) == 0u;
        }

        // Decides whether one allocated ADXT object's tick (adxt_ExecHndl) is a guest-state no-op.
        AdxtObj adxtObjState(const uint8_t *rdram, uint32_t o)
        {
            const int8_t state = static_cast<int8_t>(r8(rdram, o + 1u));
            if (state != 0 && state != 5 && state != 6)
                return AdxtObj::Live;
            const uint32_t stm = r32(rdram, o + 8u);
            const uint32_t lsc = r32(rdram, o + 0x94u);
            bool termNoop = false;
            if (stm != 0u)
            {
                const int8_t stat = static_cast<int8_t>(r8(rdram, stm + 1u));
                if (stat != 0)
                {
                    const int8_t sub = static_cast<int8_t>(r8(rdram, o + 2u));
                    if (sub == 2 || ((sub == 0 || sub == 1) && stat == 3))
                    {
                        // 0x54C940 would call 0x5420A0([obj+4]): a no-op unless a decoder callback is installed
                        if (!sjdTermSupplyIsNoop(rdram, o))
                            return AdxtObj::Acts;
                        termNoop = true;
                    }
                }
                if (stat == 4)
                    return AdxtObj::Acts; // 0x54C8C0 would force state 6
            }
            if (lsc != 0u && static_cast<int8_t>(r8(rdram, lsc + 1u)) == 3)
                return AdxtObj::Acts; // 0x54C8C0 would force state 6
            if (termNoop)
                g_adxtTermUsed = true;
            return AdxtObj::Idle;
        }

        // ---- DTX / SJX empty exchange ---------------------------------------------------
        //
        // One DTX_ExecHndl (0x559938) round of the (only) allocated channel is a stop-and-wait ping-pong with the
        // IOP side: [+1] == 1 (block sent, waiting) -> once the reply counter [[+0x14] + 0x3C] has overtaken [+8] the
        // receive callback sjx_rcvcbf (0x564360) consumes the reply and the send callback sjx_sndcbf (0x564400) builds
        // the next block, which sceSifSetDma (0x506088) hands to the HLE; the HLE answers inside that call with
        // counter + 1. Both directions are empty when
        //   * the reply waiting in the EE buffer has entry count [buf+0] == 0 (sjx_rcvcbf: blezl at 0x5643A0 -> only
        //     lock/unlock), and
        //   * every active SJX slot (32 x 0x14 bytes at 0x7B1AD0, byte [+0] == 1) holds a local SJUNI whose chunk list
        //     [sj + 0x18 + 4*id] is empty: SJUNI GetChunk (0x5634D0 -> 0x563530) then only reads that head and returns a
        //     zero length (0x563660), so sjx_sndcbf stores entry count 0 (0x5644E0).
        // Such a round changes guest memory only by: [obj+8] and [peer+0x3C] (+2 per round, always compared with
        // each other: the HLE answers sent+1), [obj+0x40] (DMA id), the serial counter 0x7A1158 (write-only: only
        // 0x559958/0x559968 touch it), the EE buffer (count 0 either way) and the lock-callback bookkeeping. Skipping the
        // round leaves state [+1] == 1 with an unconsumed empty reply, which the next executed round consumes.
        constexpr uint32_t kSjxSlots = 0x007B1AD0u, kSjxStride = 0x14u, kSjxSlotCount = 32u;
        constexpr uint32_t kSjxRcvCbf = 0x00564360u, kSjxSndCbf = 0x00564400u, kSjuniGetChunk = 0x005634D0u;
        constexpr uint32_t kDtxSendEnable = 0x007A114Cu;

        bool sjxSendEmpty(const uint8_t *rdram)
        {
            for (uint32_t i = 0u; i < kSjxSlotCount; ++i)
            {
                const uint32_t slot = kSjxSlots + kSjxStride * i;
                if (static_cast<int8_t>(r8(rdram, slot)) != 1)
                    continue;
                const uint32_t sj = r32(rdram, slot + 4u);
                if (sj == 0u || r8(rdram, sj + 4u) == 0u)
                    return false; // GetChunk would take its error path
                if (r32(rdram, r32(rdram, sj) + 0x18u) != kSjuniGetChunk)
                    return false; // not a SJUNI: unknown GetChunk
                const uint32_t id = r32(rdram, slot + 0xCu);
                if (id >= 4u)
                    return false; // error callback path
                if (r32(rdram, sj + 0x18u + 4u * id) != 0u)
                    return false; // data pending
            }
            return true;
        }

        constexpr uint32_t kRnaRcvCbf = 0x0055FBA0u, kRnaSndCbf = 0x0055FC00u;

        // ps2rna_sndcbf (0x55FC00, channel 1): for every allocated RNA object (byte [+0] == 1, 16 x 0x64 at 0x7AB850) it
        // emits a command per "desired != sent shadow" pair and refreshes the shadow:
        //   global word 0x7A9424 vs 0x7A9428 (cmd 0xB, tested once per allocated object, 0x55FC60..0x55FC6C),
        //   [+0x5B] vs [+0x5A] (cmd 8), hu[+0x50] == 0x100 (cmd 2), [+0x54] vs [+0x55] (cmd 0xA),
        //   [+0x56] vs [+0x57] (cmd 0xC), [+0x52] vs [+0x53] (cmd 3), [+0x30] vs [+0x34] (cmd 4),
        //   [+0x38] vs [+0x3C] (cmd 5), words [+0x40 + 4k] vs [+0x48 + 4k] for k < (int8)[+0x52] (cmd 9),
        //   [+0x5C] vs [+0x60] (cmd 0xD), hu[+0x50] == 1 (cmd 2).
        // With none of them true it only stores entry count 0 (0x55FF50). ps2rna_rcvcbf (0x55FBA0) just spins `count`
        // times (nops) and returns, so an empty reply is a no-op.
        bool rnaSendEmpty(const uint8_t *rdram)
        {
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kRnaBase + kRnaStride * i;
                if (static_cast<int8_t>(r8(rdram, o)) != 1)
                    continue;
                if (r32(rdram, 0x7A9424u) != r32(rdram, 0x7A9428u))
                    return false;
                if (r8(rdram, o + 0x5Bu) != r8(rdram, o + 0x5Au) || r8(rdram, o + 0x54u) != r8(rdram, o + 0x55u) ||
                    r8(rdram, o + 0x56u) != r8(rdram, o + 0x57u) || r8(rdram, o + 0x52u) != r8(rdram, o + 0x53u))
                    return false;
                const uint16_t f50 = r16(rdram, o + 0x50u);
                if (f50 == 0x100u || f50 == 1u)
                    return false;
                if (r32(rdram, o + 0x30u) != r32(rdram, o + 0x34u) || r32(rdram, o + 0x38u) != r32(rdram, o + 0x3Cu) ||
                    r32(rdram, o + 0x5Cu) != r32(rdram, o + 0x60u))
                    return false;
                const int8_t channels = static_cast<int8_t>(r8(rdram, o + 0x52u));
                for (int32_t k = 0; k < channels; ++k)
                    if (r32(rdram, o + 0x40u + 4u * static_cast<uint32_t>(k)) != r32(rdram, o + 0x48u + 4u * static_cast<uint32_t>(k)))
                        return false;
            }
            return true;
        }

        // True when one DTX_ExecHndl round of object `o` would move nothing in either direction (see above).
        bool dtxExchangeEmpty(const uint8_t *rdram, uint32_t o)
        {
            const bool sjx = r32(rdram, o + 0x20u) == kSjxRcvCbf && r32(rdram, o + 0x28u) == kSjxSndCbf;
            const bool rna = r32(rdram, o + 0x20u) == kRnaRcvCbf && r32(rdram, o + 0x28u) == kRnaSndCbf;
            if (!sjx && !rna)
                return false; // unknown channel: not analysed
            if (r32(rdram, kDtxSendEnable) != 1u || r8(rdram, o + 2u) != 1u)
                return false; // the round would only receive (state -> 0): leave it to the guest
            if (r8(rdram, o + 1u) != 1u)
                return false; // state 0: the round would send (first block / transitional)
            const uint32_t peer = r32(rdram, o + 0x14u);
            if (static_cast<int32_t>(r32(rdram, o + 8u)) >= static_cast<int32_t>(r32(rdram, peer + 0x3Cu)))
                return true; // still waiting for the reply: the round is a no-op (0x559984 / 0x5599D4 not taken)
            if (r32(rdram, r32(rdram, o + 0xCu)) != 0u)
                return false; // reply carries entries
            return sjx ? sjxSendEmpty(rdram) : rnaSendEmpty(rdram);
        }

        constexpr uint32_t kDtxValvePeriod = 32u;
        uint32_t g_dtxSkipRun = 0u;
        bool g_lastAdxtLive = false;

        AdxBusy adxServerBusy(const uint8_t *rdram, uint32_t &dtxAllocated)
        {
            dtxAllocated = 0u;
            g_lastAdxtLive = false;
            g_adxtTermUsed = false;
            if (r32(rdram, kAdxGuard) != 0u)
                return AdxBusy::Guard;
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kAdxtBase + kAdxtStride * i;
                if (r8(rdram, o) != 1u)
                    continue;
                const AdxtObj obj = adxtObjState(rdram, o);
                if (obj != AdxtObj::Idle)
                {
                    g_lastAdxtLive = obj == AdxtObj::Live;
                    const uint32_t summary = (i << 24) | (static_cast<uint32_t>(r8(rdram, o + 1u)) << 16) |
                                             (r32(rdram, o + 8u) ? 0x100u : 0u) | (r32(rdram, o + 0x94u) ? 1u : 0u);
                    static uint32_t transitions = 0u;
                    if (summary != g_lastBusyAdxt && g_diag && ++transitions <= 40u)
                        std::fprintf(stderr, "[p3fes-cuts] adxt busy obj=%u state=%u [8]=%u [0x94]=%u (%s)\n", i,
                                     (summary >> 16) & 0xFFu, (summary >> 8) & 1u, summary & 1u,
                                     obj == AdxtObj::Live ? "live" : "acts");
                    g_lastBusyAdxt = summary;
                    return AdxBusy::Adxt;
                }
            }
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                if (r8(rdram, kRnaBase + kRnaStride * i + 0x58u) == 1u)
                    return AdxBusy::Rna;
            }
            for (uint32_t i = 0u; i < 32u; ++i)
            {
                const uint32_t o = kDtrBase + kDtrStride * i;
                // 0x558DD0 (DTR_ExecHndl) returns at once unless signed byte [+1] == 1; the 0x559140
                // pre-loop only acts (sceSifDmaStat) when [+1] == 1 && [+2] == 1.
                if (static_cast<int8_t>(r8(rdram, o + 1u)) == 1 &&
                    (static_cast<int8_t>(r8(rdram, o)) == 1 || static_cast<int8_t>(r8(rdram, o + 2u)) == 1))
                {
                    const uint32_t sig = (i << 24) | (r8(rdram, o) << 16) | (r8(rdram, o + 1u) << 8) | r8(rdram, o + 2u);
                    static uint32_t lastSig = 0xFFFFFFFFu, transitions = 0u;
                    if (sig != lastSig && g_diag && ++transitions <= 40u)
                        std::fprintf(stderr, "[p3fes-cuts] dtr busy slot=%u [0]=%u [1]=%u [2]=%u\n", i, r8(rdram, o),
                                     r8(rdram, o + 1u), r8(rdram, o + 2u));
                    lastSig = sig;
                    return AdxBusy::Dtr;
                }
            }
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kSjdBase + kSjdStride * i;
                if (r8(rdram, o) != 1u)
                    continue;
                const uint8_t state = r8(rdram, o + 1u);
                if (state == 1u || state == 2u || static_cast<int32_t>(r32(rdram, o + 0xA0u)) > 0 ||
                    static_cast<int32_t>(r32(rdram, o + 0xA4u)) > 0)
                    return AdxBusy::Sjd;
            }
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kDtxBase + kDtxStride * i;
                if (r8(rdram, o) != 1u)
                    continue;
                ++dtxAllocated;
                if (!dtxExchangeEmpty(rdram, o))
                {
                    static uint32_t lastSig = 0xFFFFFFFFu, transitions = 0u;
                    const uint32_t peer = r32(rdram, o + 0x14u);
                    const uint32_t sig = i ^ r32(rdram, o) ^ (static_cast<uint32_t>(r8(rdram, o + 1u)) << 9) ^ (r32(rdram, r32(rdram, o + 0xCu)) << 13);
                    if (sig != lastSig && g_diag && ++transitions <= 24u)
                        std::fprintf(stderr, "[p3fes-cuts] dtx busy slot=%u [0]=%08X [8]=%d peer=%08X peer[0x3C]=%d [0x114C]=%u rcv=%08X(%08X) snd=%08X(%08X) bufcount=%u sjxEmpty=%u\n", i,
                                     r32(rdram, o), static_cast<int32_t>(r32(rdram, o + 8u)), peer,
                                     static_cast<int32_t>(r32(rdram, peer + 0x3Cu)), r32(rdram, 0x7A114Cu),
                                     r32(rdram, o + 0x20u), r32(rdram, o + 0x24u), r32(rdram, o + 0x28u),
                                     r32(rdram, o + 0x2Cu), r32(rdram, r32(rdram, o + 0xCu)), sjxSendEmpty(rdram) ? 1u : 0u);
                    lastSig = sig;
                    return AdxBusy::Dtx;
                }
            }
            return AdxBusy::Idle;
        }

        // Verify mode: guest memory the server tick can touch.
        struct AdxRange { uint32_t base, size; };
        constexpr AdxRange kAdxRanges[] = {
            {kAdxtBase, kAdxtStride * 16u},
            {kSjdBase, kSjdStride * 16u},
            {0x00783680u, kAdxtBase - 0x00783680u},   // ADXB/AMP/BAHX objects and the callback words 0x7836AC/0x7836C0
            {kAdxGuard, 0x68u},                       // 0x793B18..0x793B80
            {kDtrBase, kDtrStride * 32u},
            {kDtxCounter, (kDtxBase - kDtxCounter) + kDtxStride * 16u},
            {kRnaBase, kRnaStride * 16u},
            {kSjxSlots, kSjxStride * kSjxSlotCount},  // SJX slots
            {0x007B0CD8u, 0x30u * 64u},               // SJUNI objects (0x562FE8)
            {0x007B1D80u, 0x880u},                    // DTX EE buffer (SJX channel)
            {0x007ABEC0u, 0x880u},                    // DTX EE buffer (RNA channel)
            {0x007A9420u, 0x10u},                     // RNA command shadow words (0x7A9424 / 0x7A9428)
        };

        void adxSnapshot(const uint8_t *rdram, std::vector<uint8_t> &out)
        {
            out.clear();
            for (const AdxRange &range : kAdxRanges)
            {
                const size_t at = out.size();
                out.resize(at + range.size);
                std::memcpy(out.data() + at, rdram + (range.base & PS2_RAM_MASK), range.size);
            }
        }

        // Runs the original server and, when the recompiled chain unwinds to the dispatcher (an indirect
        // jump into code without its own function entry, e.g. the 0x54C940 jump table, or an EE scheduler
        // checkpoint), keeps dispatching the guest continuation until it returns to `entryRa`. Verify mode
        // only: it bypasses the scheduler for the duration of one tick. Returns false if it gave up.
        bool adxRunOriginalToReturnWith(GuestFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                        uint32_t entryRa)
        {
            original(rdram, ctx, runtime);
            for (uint32_t steps = 0u; ctx->pc != entryRa; ++steps)
            {
                if (steps >= 20000u || !runtime->hasFunction(ctx->pc))
                    return false;
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            }
            return true;
        }

        bool adxRunOriginalToReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t entryRa)
        {
            return adxRunOriginalToReturnWith(g_adxOriginal, rdram, ctx, runtime, entryRa);
        }

        // Words a skipped (empty) tick may legitimately differ in: the DTX round counters.
        bool adxVerifyIgnored(uint32_t address)
        {
            if (address == kDtxCounter)
                return true; // serial counter, write-only
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kDtxBase + kDtxStride * i;
                if (address == o + 8u || address == o + 0x40u)
                    return true; // expected counter, DMA id
            }
            // reply counter at the end of the EE buffers
            return address == 0x007B1D80u + 0x87Cu || address == 0x007ABEC0u + 0x87Cu;
        }

        void adxVerifyIdle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::vector<uint8_t> before, after;
            const uint32_t entryRa = getRegU32(ctx, 31);
            adxSnapshot(rdram, before);
            const bool completed = adxRunOriginalToReturn(rdram, ctx, runtime, entryRa);
            adxSnapshot(rdram, after);

            if (!completed)
            {
                // The recompiled chain unwound to the dispatcher (indirect jump into code without its own
                // function entry, or an EE scheduler checkpoint): the rest of the tick runs later, so the
                // tables are mid-tick and cannot be compared.
                static uint32_t printed = 0u;
                if (printed++ < 6u)
                    std::fprintf(stderr, "[p3fes-cuts] adx verify: original unwound entry ra=0x%08X pc after=0x%08X guard=%u\n",
                                 entryRa, ctx->pc, r32(rdram, kAdxGuard));
                ++g_counters.adxVerifyUnwound;
                return;
            }

            uint32_t mismatches = 0u;
            size_t off = 0u;
            for (const AdxRange &range : kAdxRanges)
            {
                for (uint32_t i = 0u; i + 4u <= range.size; i += 4u)
                {
                    uint32_t b, a;
                    std::memcpy(&b, before.data() + off + i, 4u);
                    std::memcpy(&a, after.data() + off + i, 4u);
                    if (a == b || adxVerifyIgnored(range.base + i))
                        continue;
                    ++mismatches;
                    static uint32_t printed = 0u;
                    if (printed++ < 12u)
                        std::fprintf(stderr, "[p3fes-cuts] adx verify mismatch @0x%08X old=0x%08X new=0x%08X (entry ra=0x%08X, pc after=0x%08X)\n",
                                     range.base + i, b, a, entryRa, ctx->pc);
                }
                off += range.size;
            }
            // The original tick must also leave a consistent protocol state: every DTX object waiting for its reply.
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t o = kDtxBase + kDtxStride * i;
                if (r8(rdram, o) == 1u && r8(rdram, o + 1u) != 1u)
                    ++mismatches;
            }
            if (mismatches)
                ++g_counters.adxVerifyBad;
            else
                ++g_counters.adxVerifyOk;
        }

        void adxServerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            uint32_t dtxAllocated = 0u;
            if (g_diag)
            {
                // ADXT state changes, for comparing runs with the gate on/off.
                static int last[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
                static uint32_t printed = 0u, tick = 0u;
                ++tick;
                for (uint32_t i = 0u; i < 16u; ++i)
                {
                    const uint32_t o = kAdxtBase + kAdxtStride * i;
                    const int now = r8(rdram, o) == 1u ? static_cast<int>(r8(rdram, o + 1u)) : -2;
                    if (now != last[i] && printed < 200u)
                    {
                        ++printed;
                        std::fprintf(stderr, "[p3fes-cuts] adxt state obj=%u %d->%d (tick %u)\n", i, last[i], now, tick);
                    }
                    last[i] = now;
                }
            }
            const AdxBusy busy = adxServerBusy(rdram, dtxAllocated);
            maybeLog();
            if (g_adxMode == Mode::Log)
            {
                g_adxOriginal(rdram, ctx, runtime);
                return;
            }
            {
                static uint32_t calls = 0u;
                if (g_diag && (++calls == 3000u || calls == 7000u))
                {
                    std::fprintf(stderr, "[p3fes-cuts] === state dump at call %u\n", calls);
                    for (uint32_t i = 0u; i < 16u; ++i)
                    {
                        const uint32_t o = kDtxBase + kDtxStride * i;
                        if (r8(rdram, o) != 1u) continue;
                        std::fprintf(stderr, "[p3fes-cuts] DTX[%u]:", i);
                        for (uint32_t k = 0u; k < kDtxStride; k += 4u) std::fprintf(stderr, " %08X", r32(rdram, o + k));
                        std::fprintf(stderr, " ser=%08X [114C]=%08X\n", r32(rdram, kDtxCounter), r32(rdram, 0x7A114Cu));
                    }
                    for (uint32_t i = 0u; i < 32u; ++i)
                    {
                        const uint32_t o = 0x7B1AD0u + 0x14u * i;
                        if (r8(rdram, o) != 1u) continue;
                        const uint32_t sj = r32(rdram, o + 4u);
                        const uint32_t vt = r32(rdram, sj);
                        std::fprintf(stderr, "[p3fes-cuts] SJX[%u] @%08X: %08X %08X %08X %08X %08X sj=%08X vt=%08X getchunk=%08X lists=%08X/%08X/%08X/%08X free=%08X\n", i, o,
                                     r32(rdram, o), r32(rdram, o + 4u), r32(rdram, o + 8u), r32(rdram, o + 0xCu), r32(rdram, o + 0x10u), sj, vt,
                                     r32(rdram, vt + 0x18u), r32(rdram, sj + 0x18u), r32(rdram, sj + 0x1Cu), r32(rdram, sj + 0x20u), r32(rdram, sj + 0x24u), r32(rdram, sj + 0x14u));
                    }
                    for (uint32_t i = 0u; i < 32u; ++i)
                    {
                        const uint32_t e = 0x997658u + 0x10u * i;
                        if (!r32(rdram, e)) continue;
                        const uint32_t obj = r32(rdram, e);
                        std::fprintf(stderr, "[p3fes-cuts] CVFS[%u]: %08X %08X %08X %08X obj: %08X %08X %08X %08X fn=%08X\n", i, r32(rdram, e), r32(rdram, e + 4u),
                                     r32(rdram, e + 8u), r32(rdram, e + 12u), r32(rdram, obj), r32(rdram, obj + 4u), r32(rdram, obj + 8u), r32(rdram, obj + 12u), r32(rdram, r32(rdram, obj)));
                    }
                    std::fprintf(stderr, "[p3fes-cuts] ACS: init=%08X work=%08X [work+4C]=%08X handles:", r32(rdram, 0x9970FCu), r32(rdram, 0x997098u), r32(rdram, r32(rdram, 0x997098u) + 0x4Cu));
                    for (uint32_t i = 0u; i < 16u; ++i) std::fprintf(stderr, " %08X", r32(rdram, 0x997058u + 4u * i));
                    std::fprintf(stderr, "\n");
                    for (uint32_t i = 0u; i < 16u; ++i)
                    {
                        const uint32_t h = r32(rdram, 0x997058u + 4u * i);
                        if (!h) continue;
                        std::fprintf(stderr, "[p3fes-cuts] ACSH[%u] @%08X:", i, h);
                        for (uint32_t k = 0u; k < 0x60u; k += 4u) std::fprintf(stderr, " %08X", r32(rdram, h + k));
                        std::fprintf(stderr, "\n");
                    }
                    {
                        const uint32_t rofs = r32(rdram, 0x7B26F8u);
                        std::fprintf(stderr, "[p3fes-cuts] ROFS ptr=%08X fn=%08X words:", rofs, r32(rdram, rofs + 0x30u));
                        for (uint32_t k = 0u; k < 0x40u; k += 4u) std::fprintf(stderr, " %08X", r32(rdram, rofs + k));
                        std::fprintf(stderr, "\n[p3fes-cuts] dvci guard=%08X alloc:", r32(rdram, 0x7A1D80u));
                        for (uint32_t i = 0u; i < 40u; ++i) if (r8(rdram, 0x7A1DF0u + 0x48u * i)) std::fprintf(stderr, " %u", i);
                        std::fprintf(stderr, " | cvfs 792BCC=%08X alloc:", r32(rdram, 0x792BCCu));
                        for (uint32_t i = 0u; i < 40u; ++i) if (r8(rdram, 0x792BD8u + 0x60u * i)) std::fprintf(stderr, " %u:%u", i, r8(rdram, 0x792BD8u + 0x60u * i));
                        std::fprintf(stderr, " | fs53D 784A60 alloc:");
                        for (uint32_t i = 0u; i < 16u; ++i) if (r8(rdram, 0x784A60u + 0x44u * i)) std::fprintf(stderr, " %u:%u", i, r8(rdram, 0x784A60u + 0x44u * i));
                        std::fprintf(stderr, "\n");
                    }
                    std::fprintf(stderr, "[p3fes-cuts] STM0:");
                    for (uint32_t k = 0u; k < 0x60u; ++k) std::fprintf(stderr, " %02X", r8(rdram, 0x792BD8u + k));
                    std::fprintf(stderr, "\n[p3fes-cuts] 7A08B0:");
                    for (uint32_t k = 0u; k < 0x50u; k += 4u) std::fprintf(stderr, " %08X", r32(rdram, 0x7A08B0u + k));
                    std::fprintf(stderr, "\n");
                    for (uint32_t i = 0u; i < 16u; ++i)
                    {
                        const uint32_t o = kRnaBase + kRnaStride * i;
                        std::fprintf(stderr, "[p3fes-cuts] RNA[%u]: %08X %08X %08X %08X ... [58]=%08X\n", i, r32(rdram, o), r32(rdram, o + 4u), r32(rdram, o + 8u), r32(rdram, o + 0xCu), r32(rdram, o + 0x58u));
                        if (i == 3u) break;
                    }
                    std::fprintf(stderr, "[p3fes-cuts] MWSFD init [7B2F4C]=%08X sftim handles:", r32(rdram, 0x7B2F4Cu));
                    for (uint32_t i = 0u; i < 8u; ++i) std::fprintf(stderr, " %08X", r32(rdram, 0x7B4774u + 4u * i));
                    std::fprintf(stderr, " AHX:");
                    for (uint32_t i = 0u; i < 4u; ++i) std::fprintf(stderr, " %02X", r8(rdram, 0x799978u + 0x26Cu * i));
                    std::fprintf(stderr, " AIX[+0]:");
                    for (uint32_t i = 0u; i < 4u; ++i) std::fprintf(stderr, " %08X", r32(rdram, 0x79A330u + 0x195Cu * i));
                    std::fprintf(stderr, " [997058+A4]=%08X [787538]=%08X\n", r32(rdram, 0x997058u + 0xA4u), r32(rdram, 0x787538u));
                }
                if (g_diag && calls == 3000u)
                {
                    for (uint32_t cls = 0u; cls < 8u; ++cls)
                        for (uint32_t slot = 0u; slot < 6u; ++slot)
                        {
                            const uint32_t e = 0x999E20u + cls * 0x48u + slot * 12u;
                            if (r32(rdram, e))
                            {
                                char name[33] = {};
                                const uint32_t np = r32(rdram, e + 8u);
                                for (uint32_t k = 0u; k < 32u; ++k) { name[k] = static_cast<char>(r8(rdram, np + k)); if (!name[k]) break; }
                                std::fprintf(stderr, "[p3fes-cuts] SVM cls=%u slot=%u fn=%08X arg=%08X name=%s\n", cls, slot, r32(rdram, e), r32(rdram, e + 4u), name);
                            }
                        }
                    for (uint32_t cls = 0u; cls < 8u; ++cls)
                        std::fprintf(stderr, "[p3fes-cuts] SVM cls=%u calls=%u running=%u\n", cls, r32(rdram, 0x7B2638u + cls * 4u), r32(rdram, 0x7B2658u + cls * 4u));
                    for (uint32_t a = 0x788640u; a < 0x788770u; a += 16u)
                        std::fprintf(stderr, "[p3fes-cuts] M %08X: %08X %08X %08X %08X\n", a, r32(rdram, a), r32(rdram, a + 4u), r32(rdram, a + 8u), r32(rdram, a + 12u));
                    const uint32_t hooks[] = {0x99A0C0u, 0x99A0C8u, 0x997040u};
                    for (uint32_t h : hooks) std::fprintf(stderr, "[p3fes-cuts] hook %08X fn=%08X arg=%08X\n", h, r32(rdram, h), r32(rdram, h + 4u));
                }
            }
            if (busy == AdxBusy::Idle)
            {
                // Safety valve: the HLE hands consumed PCM space back only inside an exchange. Everything that
                // could owe space (an ADXT/RNA that played) is covered by the predicates, but one real round every
                // kDtxValvePeriod ticks bounds the delay of anything this reasoning missed.
                if (g_adxMode != Mode::Verify && dtxAllocated != 0u && ++g_dtxSkipRun >= kDtxValvePeriod)
                {
                    g_dtxSkipRun = 0u;
                    ++g_counters.adxValve;
                    g_adxOriginal(rdram, ctx, runtime);
                    return;
                }
                ++g_counters.adxHit;
                if (g_adxtTermUsed)
                    ++g_counters.adxtTermHit;
                if (g_adxMode == Mode::Verify && g_adxOriginal)
                {
                    adxVerifyIdle(rdram, ctx, runtime);
                    return;
                }
                returnToCaller(ctx);
                return;
            }
            g_dtxSkipRun = 0u;
            ++g_counters.adxMiss;
            if (g_adxMode == Mode::Verify && g_adxOriginal && busy != AdxBusy::Guard && busy != AdxBusy::Adxt)
            {
                // The gate judged every allocated ADXT object idle; only another subsystem keeps the server
                // busy. Check the ADXT-specific claim in isolation: the tick must not modify the ADXT table.
                static std::vector<uint8_t> before;
                const uint32_t entryRa = getRegU32(ctx, 31);
                before.resize(kAdxtStride * 16u);
                std::memcpy(before.data(), rdram + (kAdxtBase & PS2_RAM_MASK), before.size());
                const bool completed = adxRunOriginalToReturn(rdram, ctx, runtime, entryRa);
                if (!completed)
                    ++g_counters.adxtVerifyUnwound;
                else if (std::memcmp(before.data(), rdram + (kAdxtBase & PS2_RAM_MASK), before.size()) == 0)
                    ++g_counters.adxtVerifyOk;
                else
                {
                    ++g_counters.adxtVerifyBad;
                    static uint32_t printed = 0u;
                    for (uint32_t i = 0u; i + 4u <= before.size() && printed < 12u; i += 4u)
                    {
                        uint32_t b, a;
                        std::memcpy(&b, before.data() + i, 4u);
                        a = r32(rdram, kAdxtBase + i);
                        if (a != b)
                        {
                            ++printed;
                            std::fprintf(stderr, "[p3fes-cuts] adxt verify mismatch @0x%08X old=0x%08X new=0x%08X\n",
                                         kAdxtBase + i, b, a);
                        }
                    }
                }
                switch (busy)
                {
                case AdxBusy::Rna: ++g_counters.adxMissRna; break;
                case AdxBusy::Dtr: ++g_counters.adxMissDtr; break;
                case AdxBusy::Sjd: ++g_counters.adxMissSjd; break;
                default: ++g_counters.adxMissDtx; break;
                }
                return;
            }
            switch (busy)
            {
            case AdxBusy::Guard: ++g_counters.adxMissGuard; break;
            case AdxBusy::Adxt:
                ++g_counters.adxMissAdxt;
                if (g_lastAdxtLive)
                    ++g_counters.adxMissAdxtLive;
                else
                    ++g_counters.adxMissAdxtAct;
                break;
            case AdxBusy::Rna: ++g_counters.adxMissRna; break;
            case AdxBusy::Dtr: ++g_counters.adxMissDtr; break;
            case AdxBusy::Sjd: ++g_counters.adxMissSjd; break;
            default: ++g_counters.adxMissDtx; break;
            }
            g_adxOriginal(rdram, ctx, runtime);
        }

        // ---- 2b. CRI server classes 2 and 4 (SVM_ExecSvrFunc 0x565260) and the vblank thread wake-up -----
        //
        // SVM_ExecSvrFunc(cls) walks 6 slots {fn, arg, name} (12 bytes, 0x48 per class, table 0x999E20; filled by
        // SVM_SetCbSvr 0x565058), calls every non-null fn(arg), ORs the results, then ++[0x7B2638 + 4*cls]
        // (0x565304..0x565318); [0x7B2658 + 4*cls] is set to 1 around each call (0x5652C4/0x5652D0), so it ends 0.
        // The ADXM runtime (0x540D80, vint every vblank, framework mode 2) wakes two worker threads with 0x53FFF8
        // (iReferThreadStatus + iWakeupThread when the thread sleeps): thread [0x7886E0] (body 0x53FBF0) runs class 2,
        // thread [0x7886E4] (body 0x53FCF8) runs class 4, then both SleepThread again. Registered servers in this game
        // (dumped from the table at run time):
        //   class 2: 0x552658 adxcs_dec_svr, 0x53EA50 adxt_exec_tsvr (= ADXT_ExecServer 0x549EA0), 0x585BA8
        //            MWSFSVR_VsyncThrdProc, 0x5351C0 adxcs_exec_svr (arg 0x997058; returns 1 when arg != 0)
        //   class 4: 0x53EA70 adxt_exec_fssvr (-> 0x53E0B8 -> 0x53E0E0)
        //   class 5 (SVM_ExecSvrMain, 30 Hz, main-thread handshake): 0x53EA30, 0x585BC0, 0x552680;  class 6: 0x585C08
        //   (not gated).
        constexpr uint32_t kSvmTable = 0x00999E20u;
        constexpr uint32_t kSvmCallCount = 0x007B2638u, kSvmRunning = 0x007B2658u;
        constexpr uint32_t kWakeHelper = 0x0053FFF8u;
        constexpr uint32_t kSvmClsWrapper2 = 0x005653F0u, kSvmClsWrapper4 = 0x00565420u;
        constexpr uint32_t kThreadCls2 = 0x007886E0u, kThreadCls4 = 0x007886E4u, kThreadCls6 = 0x007886ECu;
        constexpr uint32_t kLoopCountCls2 = 0x007886B0u, kLoopCountCls4 = 0x007886B8u; // 64-bit, written only by the loops

        GuestFunction g_svmOriginal = nullptr;
        GuestFunction g_wakeOriginal = nullptr;
        Mode g_criMode = Mode::Off;

        struct CriCounters
        {
            uint64_t classHit[8] = {};
            uint64_t classMiss[8] = {};
            uint64_t classVerifyOk[8] = {};
            uint64_t classVerifyBad[8] = {};
            uint64_t classVerifyUnwound[8] = {};
            uint64_t wakeHit[2] = {};   // thread 5 (class 2) / thread 6 (class 4): wake-up replaced by bookkeeping
            uint64_t wakeMissBusy[2] = {};
            uint64_t wakeMissState[2] = {}; // thread not asleep / handshake pending / unexpected flags
            uint64_t wakeVerifyOk[2] = {};
            uint64_t wakeVerifyBad[2] = {};
        } g_cri;

        uint32_t g_idleWhy = 0u;
        uint64_t g_whyCount[160] = {};
        inline bool why(uint32_t code)
        {
            g_idleWhy = code;
            return false;
        }

        uint32_t adxDtxAllocatedCount(const uint8_t *rdram)
        {
            uint32_t n = 0u;
            for (uint32_t i = 0u; i < 16u; ++i)
                n += r8(rdram, kDtxBase + kDtxStride * i) == 1u ? 1u : 0u;
            return n;
        }

        bool svmSlotIs(const uint8_t *rdram, uint32_t cls, uint32_t slot, uint32_t fn, uint32_t arg)
        {
            const uint32_t e = kSvmTable + cls * 0x48u + slot * 12u;
            return r32(rdram, e) == fn && (fn == 0u || r32(rdram, e + 4u) == arg);
        }

        // Class 4: ADXT_ExecFsServer 0x53EA70 -> 0x53E0B8 (lock no-op 0x539848) -> 0x53E0E0:
        //   [0x787538] != 0 -> plain return (0x53E0FC/0x53E10C);  otherwise [0x787538] = 1..8 around the calls
        //   558500, 546E38, 546D90, 53D708, 546D90, 546E38, 558500 and finally 0. Each sub-call is a table walk:
        //   558500 (0x7A08B0 object): TestAndSet 0x5655E8 on [0x7A08EC] (returns old != 1, stores 1); works only when it
        //     returned 1 and signed byte [0x7A08B1] is 1 (0x558138) or 2 (0x558320); then stores [0x7A08EC] = 0.
        //   546E38 -> 546E60 -> cvFsExecServer 0x556608: 32 entries (0x997658, stride 0x10): calls [[e]] only when [e] != 0
        //     and [[e]] != 0.
        //   546D90 -> 546DB8: TestAndSet on [0x792BCC]; when it returned 1, 40 objects (0x792BD8, stride 0x60) with
        //     signed byte [+0] == 1 -> 546BB0; then [0x792BCC] = 0.
        //   53D708 -> 53D730: 16 objects (0x784A60, stride 0x44) with signed byte [+0] == 1 -> 53D640.
        //   The lock pairs (0x539818/0x539830/0x545C38/0x545C40/...) only toggle the SVM lock counter / thread priority.
        // Devices registered in the cvFs table (0x997658, [e] = device, [[e]] = its exec function, 0 = none), as seen in
        // this game: 0x55E320 mfCiExecServer (40 objects at 0x7A7A18, each only calls the empty 0x55E318: always idle),
        // 0x559F48 dvCi (TestAndSet [0x7A1D80]; when it returned 1, 40 objects at 0x7A1DF0 stride 0x48 with signed
        // byte [+0] == 1 -> 0x559E80, which returns at once while word [+0x1C] == 0; then [0x7A1D80] = 0) and 0x56C8C8 ROFS_ExecServer 0x566778 -> [[0x7B26F8] + 0x30]
        // = 0x568938: lock; ctx = [0x7B2700]; returns unless ctx != 0 && [ctx] != 0 && [0x7B2708] == 0 (then sets it
        // 1), walks [ctx+4] entries of 0x3C bytes from ctx + 0x108C acting on halfword [+0x36] == 1 (0x568710), and
        // finally ++[ctx+0x14] and [0x7B2708] = 0.  Class 4 runs cvFs twice per call (546E38 appears twice in 0x53E0E0),
        // so the ROFS counter advances by 2.
        struct FsEffects { uint32_t rofsCounter = 0u; };

        bool fsServerIdle(const uint8_t *rdram, FsEffects &fx)
        {
            fx = {};
            if (!svmSlotIs(rdram, 4u, 0u, 0x0053EA70u, 0u))
                return why(1);
            for (uint32_t slot = 1u; slot < 6u; ++slot)
                if (!svmSlotIs(rdram, 4u, slot, 0u, 0u))
                    return why(2);
            if (r32(rdram, 0x787538u) != 0u)
                return true; // re-entrant call: the server returns at once
            const uint32_t guard558 = r32(rdram, 0x7A08ECu);
            if (guard558 > 1u)
                return why(3);
            if (guard558 == 0u)
            {
                const int8_t st = static_cast<int8_t>(r8(rdram, 0x7A08B1u));
                if (st == 1 || st == 2)
                    return why(4);
            }
            for (uint32_t i = 0u; i < 32u; ++i)
            {
                const uint32_t entry = r32(rdram, 0x997658u + 0x10u * i);
                if (entry == 0u)
                    continue;
                const uint32_t fn = r32(rdram, entry);
                if (fn == 0u || fn == 0x0055E320u)
                    continue;
                if (fn == 0x00559F48u)
                {
                    const uint32_t guard = r32(rdram, 0x7A1D80u);
                    if (guard > 1u)
                        return why(5);
                    if (guard == 0u)
                        for (uint32_t k = 0u; k < 40u; ++k)
                            if (static_cast<int8_t>(r8(rdram, 0x7A1DF0u + 0x48u * k)) == 1 &&
                                r32(rdram, 0x7A1DF0u + 0x48u * k + 0x1Cu) != 0u) // 0x559E94: [+0x1C] == 0 -> return
                                return why(6);
                    continue;
                }
                if (fn == 0x0056C8C8u)
                {
                    const uint32_t holder = r32(rdram, 0x7B26F8u);
                    if (holder == 0u)
                        continue; // 0x566784: nothing registered
                    if (r32(rdram, holder + 0x30u) != 0x00568938u)
                        return why(7);
                    const uint32_t ctx = r32(rdram, 0x7B2700u);
                    if (ctx == 0u || r32(rdram, ctx) == 0u || r32(rdram, 0x7B2708u) != 0u)
                        continue;
                    const int32_t count = static_cast<int32_t>(r32(rdram, ctx + 4u));
                    if (count > 256)
                        return why(7);
                    for (int32_t k = 0; k < count; ++k)
                        if (static_cast<int16_t>(r16(rdram, ctx + 0x108Cu + 0x3Cu * static_cast<uint32_t>(k) + 0x36u)) == 1)
                            return why(7);
                    fx.rofsCounter = ctx + 0x14u;
                    continue;
                }
                return why(5); // unknown device
            }
            const uint32_t guard546 = r32(rdram, 0x792BCCu);
            if (guard546 > 1u)
                return why(8);
            if (guard546 == 0u)
                for (uint32_t i = 0u; i < 40u; ++i)
                {
                    const uint32_t o = 0x792BD8u + 0x60u * i;
                    if (static_cast<int8_t>(r8(rdram, o)) != 1)
                        continue;
                    // adxstmf_stat_exec 0x546BB0, object with signed byte [+0] == 1:
                    //   [+2] != 0: jumps to the tail 0x546D50, which only calls 0x546788 when [+1] == 2 && [+0x49] == 1;
                    //   [+2] == 0: acts (writes / file calls) when [+0x48] == 1 (0x546BDC), [+0x46] == 1 (0x546BFC),
                    //     [+0x45] == 1 (0x546C30) or [+0x47] == 1 (0x546D48, stores 0), otherwise lock/unlock
                    //     (0x545BF0 / 0x545C08) and the same tail.
                    const bool tail = static_cast<int8_t>(r8(rdram, o + 1u)) == 2 && static_cast<int8_t>(r8(rdram, o + 0x49u)) == 1;
                    if (tail)
                        return why(9);
                    if (static_cast<int8_t>(r8(rdram, o + 2u)) == 0 &&
                        (static_cast<int8_t>(r8(rdram, o + 0x48u)) == 1 || static_cast<int8_t>(r8(rdram, o + 0x46u)) == 1 ||
                         static_cast<int8_t>(r8(rdram, o + 0x45u)) == 1 || static_cast<int8_t>(r8(rdram, o + 0x47u)) == 1))
                        return why(9);
                }
            for (uint32_t i = 0u; i < 16u; ++i)
                if (static_cast<int8_t>(r8(rdram, 0x784A60u + 0x44u * i)) == 1)
                    return why(10);
            return true;
        }

        // Class 2 members that are not plain table walks. Facts (generated code, addresses as in the map):
        //   0x552658: 0x553FA8 (4 AHX objects, 0x799978 stride 0x26C, signed byte [+0] == 1 -> 0x553B70) and 0x555658
        //     (4 AIX objects, 0x79A330 stride 0x195C -> 0x555570: SVM lock, aixsjd_ExecHndl 0x5555A0 returns at once unless
        //     signed byte [+0] == 1, unlock).
        //   0x585BA8 -> 0x585C70 -> 0x585C90 (Sofdec vsync): ++[0x7B4540], ++[0x7B4544]; when the lib is initialised
        //     ([0x7B2F4C] == 1): TestAndSet on [0x7B2FAC] (0x585578 -> 0x5655E8; libwork 0x7B2F50 + 0x5C); when it
        //     returned 1: 0x599B80 = ++[0x7B4720] and, for each of the 8 handles at 0x7B4774, 0x58C060 (-1 for a null handle
        //     or [h+0x48] == 0, else 0x599C08); then [0x7B2FAC] = 0.
        struct Cls2Extra { uint32_t incSfd4540 = 0u; uint32_t incSfdPly = 0u; };
        bool decSvrIdle(const uint8_t *rdram);
        bool mwsfdVsyncIdle(const uint8_t *rdram, Cls2Extra &extra);

        // ACS server 0x5351C0(arg 0x997058) -> ACSSVR 0x535050 (a0 = 0x997058, work = [0x997098] = 0x99709C):
        //   acts only when [0x9970FC] > 0 (0x5351B0). Then
        //   (a) when [work + 0x4C] != 0 (0x535348): ACSSND/fader tick 0x5353A8(work): for the 16 entries [[work] + 4i] that
        //       are non-null with [e+0x50] != 0 (0x532E08) and signed byte [e+0x8C] == 2 (0x533230) it runs 0x5331A8 and
        //       a state switch (busy); then switch on signed byte [work+0x50]: 0 -> tail 0x531C00(ACSFDR_Process, [work+4]),
        //       1/2/3 -> volume/fade work (busy). ACSFDR_Process returns at once (0x531C1C) while signed byte [fader+0] == 0.
        //   (b) for each of the 16 handles h = [0x997058 + 4i] != 0 with [h+0x50] != 0 (0x5350D0): 0x533CC0 stores
        //       [h+0xC4] = [h+0xC8] = 0; when [work+0x4C] != 0 && signed byte [work+0x50] != 0 && signed byte [h+0x8C] == 1
        //       it adds a fade step (0x533CA8: [h+0xC8] = 1, [h+0xC4] += ...); then ACSSND_ApplyAllVol 0x533CD0(h):
        //       [h+0x60] == 1 -> return; signed byte [h+0xA4] != 1 -> returns unless [h+0xC8] == 1; [h+0xA4] == 1 -> fade
        //       state machine (busy).
        // At rest that is: both words zeroed per live handle, nothing else.
        struct AcsEffects { uint32_t handles[16]; uint32_t count = 0u; };

        bool acsIdle(const uint8_t *rdram, AcsEffects &fx)
        {
            fx.count = 0u;
            if (static_cast<int32_t>(r32(rdram, 0x9970FCu)) <= 0)
                return true;
            const uint32_t work = r32(rdram, 0x997098u);
            if (work == 0u)
                return why(15);
            const bool tick = r32(rdram, work + 0x4Cu) != 0u;
            const bool stateSet = tick && static_cast<int8_t>(r8(rdram, work + 0x50u)) != 0;
            if (tick)
            {
                const uint32_t array = r32(rdram, work);
                for (uint32_t i = 0u; i < 16u; ++i)
                {
                    const uint32_t e = r32(rdram, array + 4u * i);
                    if (e != 0u && r32(rdram, e + 0x50u) != 0u && static_cast<int8_t>(r8(rdram, e + 0x8Cu)) == 2)
                        return why(15);
                }
                if (stateSet) // 0x535460..: states 1..3 (and any other non-zero value) leave the tail
                    return why(16);
                const uint32_t fader = r32(rdram, work + 4u);
                if (fader == 0u || static_cast<int8_t>(r8(rdram, fader)) != 0)
                    return why(16);
            }
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t h = r32(rdram, 0x997058u + 4u * i);
                if (h == 0u || r32(rdram, h + 0x50u) == 0u)
                    continue;
                if (stateSet && static_cast<int8_t>(r8(rdram, h + 0x8Cu)) == 1)
                    return why(17);
                if (r32(rdram, h + 0x60u) != 1u && static_cast<int8_t>(r8(rdram, h + 0xA4u)) == 1)
                    return why(17);
                fx.handles[fx.count++] = h;
            }
            return true;
        }

        // MWSFSVR 0x585E70 (class 5 slot 1 when [0x7B2F60] == 1 via 0x585BC0, class 6 otherwise via 0x585C08):
        //   returns 0 at once unless [0x7B2F4C] == 1; TestAndSet on [0x7B2FA8] (libwork 0x7B2F50 + 0x58) returned 0 when it
        //   was 1; otherwise: user callback [0x7B2F90](+[0x7B2F94]) (0x585DA8), 8 player handles at 0x7B2FBC stride 0x2AC
        //   (0x585F38 -> 0x585F50 returns at once unless word [h+0] == 1), [0x7B2FA8] = 0 (0x5860E8), 0x595BE8 (the 8 sftim
        //   handles at 0x7B4774: only valid ones are processed), user callback [0x7B2F98] (0x585DD8), and when
        //   [0x7B2F74] != 1 (0x586200) callback [0x7B2FA0] (0x585E40). Result 0 when nothing is active.
        bool mwsfdIdleProcIdle(const uint8_t *rdram)
        {
            if (r32(rdram, 0x7B2F4Cu) != 1u)
                return true;
            const uint32_t guard = r32(rdram, 0x7B2FA8u);
            if (guard == 1u)
                return true;
            if (guard != 0u)
                return why(18);
            if (r32(rdram, 0x7B2F90u) != 0u || r32(rdram, 0x7B2F98u) != 0u)
                return why(18);
            if (r32(rdram, 0x7B2F74u) != 1u && r32(rdram, 0x7B2FA0u) != 0u)
                return why(18);
            for (uint32_t i = 0u; i < 8u; ++i)
            {
                if (r32(rdram, 0x7B2FBCu + 0x2ACu * i) == 1u)
                    return why(19);
                const uint32_t h = r32(rdram, 0x7B4774u + 4u * i);
                if (h != 0u && r32(rdram, h + 0x48u) != 0u)
                    return why(19);
            }
            return true;
        }

        struct LscIdleTag {};
        bool lscIdle(const uint8_t *rdram)
        {
            // LSC_ExecServer 0x55D730: 32 objects (0x7A32A0, stride 0x238), signed byte [+0] == 1 -> 0x55E0C0, which returns
            // at once when signed byte [+4] == 1, [+1] != 2 (0x55E0EC) or word [+0x24] <= 0 (0x55E0F8); else 0x55DEF0 /
            // 0x55DFB8 / 0x55DDC8 state machine.
            for (uint32_t i = 0u; i < 32u; ++i)
            {
                const uint32_t o = 0x7A32A0u + 0x238u * i;
                if (static_cast<int8_t>(r8(rdram, o)) != 1)
                    continue;
                if (static_cast<int8_t>(r8(rdram, o + 4u)) == 1 || static_cast<int8_t>(r8(rdram, o + 1u)) != 2 ||
                    static_cast<int32_t>(r32(rdram, o + 0x24u)) <= 0)
                    continue;
                return false;
            }
            return true;
        }

        FsEffects g_fsEffects;
        AcsEffects g_acsEffects;

        bool classIdle(const uint8_t *rdram, uint32_t cls, Cls2Extra &extra)
        {
            if (cls == 2u)
            {
                if (g_adxMode == Mode::Off || g_adxMode == Mode::Log)
                    return false;
                if (!svmSlotIs(rdram, 2u, 0u, 0x00552658u, 0u) || !svmSlotIs(rdram, 2u, 1u, 0x0053EA50u, 0u) ||
                    !svmSlotIs(rdram, 2u, 2u, 0x00585BA8u, 0u) || !svmSlotIs(rdram, 2u, 3u, 0u, 0u) ||
                    !svmSlotIs(rdram, 2u, 4u, 0x005351C0u, 0x00997058u) || !svmSlotIs(rdram, 2u, 5u, 0u, 0u))
                    return why(11);
                if (!decSvrIdle(rdram))
                    return why(12);
                if (!mwsfdVsyncIdle(rdram, extra))
                    return why(13);
                if (!acsIdle(rdram, g_acsEffects))
                    return false;
                uint32_t dtxAllocated = 0u;
                if (adxServerBusy(rdram, dtxAllocated) != AdxBusy::Idle)
                    return why(18);
                // the valve of the ADXT gate: a real exchange every kDtxValvePeriod ticks
                return (dtxAllocated == 0u || g_adxMode == Mode::Verify || g_dtxSkipRun + 1u < kDtxValvePeriod) ? true : why(14);
            }
            if (cls == 4u)
                return fsServerIdle(rdram, g_fsEffects);
            if (cls == 5u)
            {
                if (!svmSlotIs(rdram, 5u, 0u, 0x0053EA30u, 0u) || !svmSlotIs(rdram, 5u, 1u, 0x00585BC0u, 0u) ||
                    !svmSlotIs(rdram, 5u, 2u, 0x00552680u, 0u) || !svmSlotIs(rdram, 5u, 3u, 0u, 0u) ||
                    !svmSlotIs(rdram, 5u, 4u, 0u, 0u) || !svmSlotIs(rdram, 5u, 5u, 0u, 0u))
                    return why(11);
                if (!lscIdle(rdram))
                    return why(12);
                return r32(rdram, 0x7B2F60u) != 1u || mwsfdIdleProcIdle(rdram);
            }
            if (cls == 6u)
            {
                for (uint32_t slot = 1u; slot < 6u; ++slot)
                    if (!svmSlotIs(rdram, 6u, slot, 0u, 0u))
                        return why(11);
                if (!svmSlotIs(rdram, 6u, 0u, 0x00585C08u, 0u))
                    return why(11);
                return r32(rdram, 0x7B2F60u) == 1u || mwsfdIdleProcIdle(rdram);
            }
            return false;
        }

        // Replays what the skipped ExecSvrFunc(cls) leaves behind.
        void classApplyEffects(uint8_t *rdram, uint32_t cls, const Cls2Extra &extra)
        {
            w32(rdram, kSvmCallCount + 4u * cls, r32(rdram, kSvmCallCount + 4u * cls) + 1u);
            w32(rdram, kSvmRunning + 4u * cls, 0u);
            if (cls == 4u && g_fsEffects.rofsCounter != 0u)
                w32(rdram, g_fsEffects.rofsCounter, r32(rdram, g_fsEffects.rofsCounter) + 2u);
            if (cls == 2u)
            {
                w32(rdram, 0x7B4540u, r32(rdram, 0x7B4540u) + extra.incSfd4540);
                w32(rdram, 0x7B4544u, r32(rdram, 0x7B4544u) + extra.incSfd4540);
                w32(rdram, 0x7B4720u, r32(rdram, 0x7B4720u) + extra.incSfdPly);
                for (uint32_t i = 0u; i < g_acsEffects.count; ++i)
                {
                    w32(rdram, g_acsEffects.handles[i] + 0xC4u, 0u);
                    w32(rdram, g_acsEffects.handles[i] + 0xC8u, 0u);
                }
                if (adxDtxAllocatedCount(rdram) != 0u)
                    ++g_dtxSkipRun; // the ADXT gate counts skipped ticks for its valve
            }
        }

        struct VerifyRange { uint32_t base, size; };

        // Guest memory a class tick can touch (compared in verify mode); live ACS handles are added per call.
        void classVerifyRanges(const uint8_t *rdram, std::vector<VerifyRange> &out)
        {
            static const VerifyRange fixed[] = {
                {0x00787538u, 4u}, {0x007A08B0u, 0x80u}, {0x007A08ECu, 0x20u}, {0x00997658u, 0x200u}, {0x00792BCCu, 0x10u},
                {0x00792BD8u, 0x60u * 40u}, {0x00784A60u, 0x44u * 16u}, {0x00799978u, 0x26Cu * 4u}, {0x0079A330u, 0x195Cu * 4u},
                {0x007B2F4Cu, 0x1600u}, {0x007B4540u, 0x10u}, {0x007B4720u, 0x10u}, {0x007B4774u, 0x20u},
                {0x00997058u, 0xB0u}, {0x0099709Cu, 0x60u}, {0x007B2638u, 0x40u}, {0x007886B0u, 0x30u}, {0x007A1D80u, 0x10u},
                {0x007A1DF0u, 0x48u * 40u}, {0x007C7230u, 0x40u}, {0x007B26F8u, 0x20u}, {0x007C7260u, 0x20u},
                {0x007A7A18u, 0x38u * 40u}, {0x007A32A0u, 0x238u * 32u}};
            out.assign(std::begin(fixed), std::end(fixed));
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t h = r32(rdram, 0x997058u + 4u * i);
                if (h != 0u)
                    out.push_back({h, 0x100u});
            }
            const uint32_t work = r32(rdram, 0x997098u);
            if (work != 0u)
            {
                if (const uint32_t fader = r32(rdram, work + 4u))
                    out.push_back({fader, 0x40u});
                if (const uint32_t array = r32(rdram, work))
                    out.push_back({array, 0x40u});
            }
        }

        void snapshotRanges(const uint8_t *rdram, const std::vector<VerifyRange> &ranges, std::vector<uint8_t> &out)
        {
            out.clear();
            for (const VerifyRange &range : ranges)
            {
                const size_t at = out.size();
                out.resize(at + range.size);
                std::memcpy(out.data() + at, rdram + (range.base & PS2_RAM_MASK), range.size);
            }
        }

        void restoreRanges(uint8_t *rdram, const std::vector<VerifyRange> &ranges, const std::vector<uint8_t> &in)
        {
            size_t at = 0u;
            for (const VerifyRange &range : ranges)
            {
                std::memcpy(rdram + (range.base & PS2_RAM_MASK), in.data() + at, range.size);
                at += range.size;
            }
        }

        // The class wrappers 0x5653F0 (2), 0x565420 (4), 0x565438 (5), 0x565450 (6) load a0 and tail-jump to 0x565260;
        // the thread bodies reach them with jal, which goes through the function table (0x565260 itself is reached by a
        // direct C++ call from the wrappers and cannot be replaced).
        void svmHookCommon(GuestFunction original, uint32_t cls, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_criMode == Mode::Off)
            {
                original(rdram, ctx, runtime);
                return;
            }
            Cls2Extra extra;
            g_idleWhy = 0u;
            if (!classIdle(rdram, cls, extra))
            {
                ++g_cri.classMiss[cls];
                ++g_whyCount[cls * 20u + (g_idleWhy % 20u)];
                original(rdram, ctx, runtime);
                return;
            }
            if (g_criMode == Mode::Verify)
            {
                static std::vector<uint8_t> before, expected, after;
                std::vector<VerifyRange> ranges;
                classVerifyRanges(rdram, ranges);
                const uint32_t entryRa = getRegU32(ctx, 31);
                snapshotRanges(rdram, ranges, before);
                // expected image: the gate's own effects applied to the "before" state
                classApplyEffects(rdram, cls, extra);
                snapshotRanges(rdram, ranges, expected);
                restoreRanges(rdram, ranges, before);
                const bool completed = adxRunOriginalToReturnWith(original, rdram, ctx, runtime, entryRa);
                if (!completed)
                {
                    ++g_cri.classVerifyUnwound[cls];
                    return;
                }
                // the run may have changed which ranges exist (handles); compare the ones captured before
                snapshotRanges(rdram, ranges, after);
                uint32_t mismatches = 0u;
                size_t off = 0u;
                for (const VerifyRange &range : ranges)
                {
                    for (uint32_t i = 0u; i + 4u <= range.size; i += 4u)
                    {
                        uint32_t e, a;
                        std::memcpy(&e, expected.data() + off + i, 4u);
                        std::memcpy(&a, after.data() + off + i, 4u);
                        if (a == e)
                            continue;
                        ++mismatches;
                        static uint32_t printed = 0u;
                        if (printed++ < 16u)
                            std::fprintf(stderr, "[p3fes-cuts] cri class %u verify mismatch @0x%08X expected=0x%08X got=0x%08X\n", cls,
                                         range.base + i, e, a);
                    }
                    off += range.size;
                }
                if (mismatches)
                    ++g_cri.classVerifyBad[cls];
                else
                    ++g_cri.classVerifyOk[cls];
                return;
            }
            ++g_cri.classHit[cls];
            classApplyEffects(rdram, cls, extra);
            setReturnU32(ctx, cls == 2u ? 1u : 0u);
            returnToCaller(ctx);
        }

        // Class-2 members that are gated on their own (the class as a whole stays busy because of the ACS handle, see the
        // report): 0x552658 (AHX/AIX decoders: nothing allocated -> only lock pairs) and 0x585BA8 (Sofdec vsync).
        constexpr uint32_t kDecSvr = 0x00552658u, kMwsfdVsync = 0x00585BA8u;
        GuestFunction g_decOriginal = nullptr;
        GuestFunction g_mwsfdOriginal = nullptr;
        uint64_t g_srvHit[2] = {}, g_srvMiss[2] = {}, g_srvVerifyOk[2] = {}, g_srvVerifyBad[2] = {}, g_srvVerifyUnwound[2] = {};

        bool decSvrIdle(const uint8_t *rdram)
        {
            if (!svmSlotIs(rdram, 2u, 0u, kDecSvr, 0u))
                return false;
            for (uint32_t i = 0u; i < 4u; ++i)
                if (static_cast<int8_t>(r8(rdram, 0x799978u + 0x26Cu * i)) == 1 ||
                    static_cast<int8_t>(r8(rdram, 0x79A330u + 0x195Cu * i)) == 1)
                    return false;
            return true;
        }

        // Sofdec vsync 0x585BA8 -> 0x585C70 -> 0x585C90 (see cls2OthersIdle for the facts); returns the counters to advance.
        bool mwsfdVsyncIdle(const uint8_t *rdram, Cls2Extra &extra)
        {
            if (!svmSlotIs(rdram, 2u, 2u, kMwsfdVsync, 0u))
                return false;
            extra.incSfd4540 = 1u;
            extra.incSfdPly = 0u;
            if (r32(rdram, 0x7B2F4Cu) == 1u)
            {
                const uint32_t guard = r32(rdram, 0x7B2FACu);
                if (guard > 1u)
                    return false;
                if (guard == 0u)
                {
                    for (uint32_t i = 0u; i < 8u; ++i)
                    {
                        const uint32_t h = r32(rdram, 0x7B4774u + 4u * i);
                        if (h != 0u && r32(rdram, h + 0x48u) != 0u)
                            return false;
                    }
                    extra.incSfdPly = 1u;
                }
            }
            return true;
        }

        void serverGateCommon(int which, GuestFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            Cls2Extra extra;
            const bool idle = g_criMode != Mode::Off && (which == 0 ? decSvrIdle(rdram) : mwsfdVsyncIdle(rdram, extra));
            if (!idle)
            {
                ++g_srvMiss[which];
                original(rdram, ctx, runtime);
                return;
            }
            if (g_criMode == Mode::Verify)
            {
                static const VerifyRange ranges[] = {{0x00799978u, 0x26Cu * 4u}, {0x0079A330u, 0x195Cu * 4u}, {0x007B2F4Cu, 0x80u},
                                                     {0x007B4540u, 0x10u}, {0x007B4720u, 0x10u}, {0x007B4774u, 0x20u}};
                std::vector<uint8_t> before, after;
                const uint32_t entryRa = getRegU32(ctx, 31);
                for (const VerifyRange &r : ranges)
                    before.insert(before.end(), rdram + (r.base & PS2_RAM_MASK), rdram + (r.base & PS2_RAM_MASK) + r.size);
                const bool completed = adxRunOriginalToReturnWith(original, rdram, ctx, runtime, entryRa);
                if (!completed)
                {
                    ++g_srvVerifyUnwound[which];
                    return;
                }
                for (const VerifyRange &r : ranges)
                    after.insert(after.end(), rdram + (r.base & PS2_RAM_MASK), rdram + (r.base & PS2_RAM_MASK) + r.size);
                uint32_t bad = 0u;
                size_t off = 0u;
                for (const VerifyRange &r : ranges)
                {
                    for (uint32_t i = 0u; i + 4u <= r.size; i += 4u)
                    {
                        uint32_t b, a;
                        std::memcpy(&b, before.data() + off + i, 4u);
                        std::memcpy(&a, after.data() + off + i, 4u);
                        uint32_t e = b;
                        if (which == 1)
                        {
                            const uint32_t address = r.base + i;
                            if (address == 0x7B4540u || address == 0x7B4544u)
                                e += extra.incSfd4540;
                            if (address == 0x7B4720u)
                                e += extra.incSfdPly;
                        }
                        if (a != e)
                        {
                            ++bad;
                            static uint32_t printed = 0u;
                            if (printed++ < 8u)
                                std::fprintf(stderr, "[p3fes-cuts] cri server %d verify mismatch @0x%08X expected=%08X got=%08X\n", which,
                                             r.base + i, e, a);
                        }
                    }
                    off += r.size;
                }
                ++(bad ? g_srvVerifyBad[which] : g_srvVerifyOk[which]);
                return;
            }
            ++g_srvHit[which];
            if (which == 1)
            {
                w32(rdram, 0x7B4540u, r32(rdram, 0x7B4540u) + extra.incSfd4540);
                w32(rdram, 0x7B4544u, r32(rdram, 0x7B4544u) + extra.incSfd4540);
                w32(rdram, 0x7B4720u, r32(rdram, 0x7B4720u) + extra.incSfdPly);
            }
            setReturnU32(ctx, 0u);
            returnToCaller(ctx);
        }
        void decSvrHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { serverGateCommon(0, g_decOriginal, rdram, ctx, runtime); }
        void mwsfdVsyncHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { serverGateCommon(1, g_mwsfdOriginal, rdram, ctx, runtime); }

        GuestFunction g_svmOriginal4 = nullptr;
        void svmHook4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { svmHookCommon(g_svmOriginal4, 4u, rdram, ctx, runtime); }
        GuestFunction g_svmOriginal2 = nullptr, g_svmOriginal5 = nullptr, g_svmOriginal6 = nullptr;
        void svmHook2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { svmHookCommon(g_svmOriginal2, 2u, rdram, ctx, runtime); }
        void svmHook5(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { svmHookCommon(g_svmOriginal5, 5u, rdram, ctx, runtime); }
        void svmHook6(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { svmHookCommon(g_svmOriginal6, 6u, rdram, ctx, runtime); }

        // 0x540ED8 (called by the main-thread handshake 0x541008 between the "pre" and "post" Sofdec callbacks):
        // ResumeThread([0x7886EC]) when it is suspended (0x5400B0) and WakeupThread when it sleeps (0x540048), so that
        // the idle worker (body 0x53FDE8, priority 0x19, below the main thread's 0x18) runs while the main thread
        // sleeps in 0x540FE0; in mode [0x788648] == 1 the same for thread [0x7886F0]. One worker cycle is:
        //   ++[0x7886C0]; ExecSvrFunc(6) with flag [0x788694] 1 -> 0; [0x788684] == 1 -> cleared and SleepThread
        //   (0x53FE70); a non-zero class result loops again (0x53FE80); mode == 1 resumes/wakes [0x7886F0];
        //   [0x997040].fn callback (0x541510) unless [0x788684] == 1; [0x788768] = 1; SleepThread.
        // The next class-2 cycle (0x53FBF0) suspends it again. Class 6 idle (result 0), [0x788684] == 0, no callback and
        // the thread asleep + suspended (the state the previous cycle left it in) make the whole round bookkeeping.
        GuestFunction g_handshakeOriginal = nullptr;
        constexpr uint32_t kHandshakeResume = 0x00540ED8u;
        uint64_t g_t7Hit = 0u, g_t7Miss = 0u, g_t7VerifyOk = 0u, g_t7VerifyBad = 0u;
        struct T7Expect { bool armed = false; uint64_t loop = 0u; uint32_t classCount = 0u; } g_t7Expect;

        void handshakeResumeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_criMode == Mode::Off)
            {
                g_handshakeOriginal(rdram, ctx, runtime);
                return;
            }
            if (g_criMode == Mode::Verify && g_t7Expect.armed)
            {
                g_t7Expect.armed = false;
                uint64_t loop;
                std::memcpy(&loop, rdram + (0x7886C0u & PS2_RAM_MASK), sizeof(loop));
                // the cycle ran during the previous handshake's sleep: counters advanced by exactly one round
                if (loop == g_t7Expect.loop + 1u && r32(rdram, kSvmCallCount + 24u) == g_t7Expect.classCount + 1u)
                    ++g_t7VerifyOk;
                else
                    ++g_t7VerifyBad;
            }
            const uint32_t id = r32(rdram, kThreadCls6);
            bool ok = id != 0u && r32(rdram, 0x788648u) != 1u && r32(rdram, 0x788684u) == 0u && r32(rdram, 0x788694u) == 0u &&
                      r32(rdram, 0x997040u) == 0u && r32(rdram, 0x788738u) == 0u;
            if (ok)
            {
                const GuestThread *thread = runtime->eeScheduler().thread(static_cast<int>(id));
                ok = thread != nullptr && thread->status == EeThreadStatus::WaitingSuspended &&
                     thread->wait.reason == EeWaitReason::Sleep && thread->wakeupCount == 0u;
            }
            Cls2Extra extra;
            if (ok)
            {
                g_idleWhy = 0u;
                ok = classIdle(rdram, 6u, extra);
            }
            if (!ok)
            {
                ++g_t7Miss;
                g_handshakeOriginal(rdram, ctx, runtime);
                return;
            }
            if (g_criMode == Mode::Verify)
            {
                std::memcpy(&g_t7Expect.loop, rdram + (0x7886C0u & PS2_RAM_MASK), sizeof(uint64_t));
                g_t7Expect.classCount = r32(rdram, kSvmCallCount + 24u);
                g_t7Expect.armed = true;
                g_handshakeOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_t7Hit;
            uint64_t loop;
            std::memcpy(&loop, rdram + (0x7886C0u & PS2_RAM_MASK), sizeof(loop));
            ++loop;
            std::memcpy(rdram + (0x7886C0u & PS2_RAM_MASK), &loop, sizeof(loop));
            classApplyEffects(rdram, 6u, extra);
            w32(rdram, 0x788768u, 1u);
            returnToCaller(ctx);
        }

        // 0x53FFF8(thread): called by the vint for the class-2 and class-4 workers. Replaces the wake-up (and the whole
        // thread cycle that would follow) by its bookkeeping when the cycle is provably a no-op:
        //   thread body 0x53FCF8 (class 4): ++[0x7886B8] (64-bit, write-only statistic); when hu[0x788764] != 0 it
        //     stores to GS register 0x120000E0 around the call (-> keep the cycle); ExecSvrFunc(4); flag [0x788690]
        //     1 -> 0; SleepThread.
        //   thread body 0x53FBF0 (class 2): ++[0x7886B0]; SuspendThread([0x7886EC]) unless it is already suspended
        //     (0x540118); [0x997040].fn callback (0x541510) when [0x788768] == 0, then [0x788768] = 0 (-> require both
        //     zero); mode [0x788648] == 1 would suspend [0x7886F0] too; 0x540F60 acts when [0x78869C] == 1 (the main
        //     thread sleeps in 0x540FE0 waiting for exactly this cycle: never skipped); ExecSvrFunc(2); flag
        //     [0x788688] 1 -> 0; SleepThread.
        // The thread loop counters have no reader (all references found by a scan of the generated code); the class
        // counters at 0x7B2638 are only read by the ADXM/MWSFD finish paths.
        bool threadAsleep(PS2Runtime *runtime, uint32_t id)
        {
            const GuestThread *thread = runtime->eeScheduler().thread(static_cast<int>(id));
            return thread != nullptr && thread->wakeupCount == 0u && thread->suspendCount == 0 &&
                   thread->status == EeThreadStatus::Waiting && thread->wait.reason == EeWaitReason::Sleep;
        }

        bool threadParked(PS2Runtime *runtime, uint32_t id)
        {
            if (id == 0u)
                return true; // 0x540118 returns at once for a null id
            const GuestThread *thread = runtime->eeScheduler().thread(static_cast<int>(id));
            // 0x540118 leaves SUSPEND (8) and WAIT_SUSPEND (0xC) alone; a dormant thread rejects SuspendThread
            return thread == nullptr || thread->status == EeThreadStatus::Suspended ||
                   thread->status == EeThreadStatus::WaitingSuspended || thread->status == EeThreadStatus::Dormant;
        }

        struct WakeExpect
        {
            bool armed = false;
            uint64_t loopCount = 0u;
            uint32_t classCount = 0u;
        };
        WakeExpect g_wakeExpect[2];

        void wakeVerifyCheck(uint8_t *rdram, PS2Runtime *runtime, int which, uint32_t cls, uint32_t loopAddress)
        {
            WakeExpect &expect = g_wakeExpect[which];
            if (!expect.armed)
                return;
            expect.armed = false;
            uint64_t loop;
            std::memcpy(&loop, rdram + (loopAddress & PS2_RAM_MASK), sizeof(loop));
            const bool ok = loop == expect.loopCount + 1u && r32(rdram, kSvmCallCount + 4u * cls) == expect.classCount + 1u &&
                            r32(rdram, kSvmRunning + 4u * cls) == 0u &&
                            threadAsleep(runtime, r32(rdram, which == 0 ? kThreadCls2 : kThreadCls4));
            if (ok)
                ++g_cri.wakeVerifyOk[which];
            else
            {
                ++g_cri.wakeVerifyBad[which];
                static uint32_t printed = 0u;
                if (printed++ < 8u)
                    std::fprintf(stderr, "[p3fes-cuts] cri wake verify mismatch thread=%d loop=%llu expected=%llu class=%u expected=%u running=%u hs=%u flag=%u\n",
                                 which == 0 ? 5 : 6, static_cast<unsigned long long>(loop),
                                 static_cast<unsigned long long>(expect.loopCount + 1u), r32(rdram, kSvmCallCount + 4u * cls),
                                 expect.classCount + 1u, r32(rdram, kSvmRunning + 4u * cls), r32(rdram, 0x78869Cu),
                                 r32(rdram, 0x788768u));
            }
        }

        void wakeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_criMode == Mode::Off)
            {
                g_wakeOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t id = getRegU32(ctx, 4);
            int which = -1;
            if (id != 0u && id == r32(rdram, kThreadCls2))
                which = 0;
            else if (id != 0u && id == r32(rdram, kThreadCls4))
                which = 1;
            if (which < 0)
            {
                g_wakeOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t cls = which == 0 ? 2u : 4u;
            const uint32_t loopAddress = which == 0 ? kLoopCountCls2 : kLoopCountCls4;
            if (g_criMode == Mode::Verify)
                wakeVerifyCheck(rdram, runtime, which, cls, loopAddress);

            bool stateOk = r32(rdram, 0x788728u) == 0u && r32(rdram, 0x788718u) == 0u && threadAsleep(runtime, id);
            if (stateOk && which == 1)
                stateOk = r16(rdram, 0x788764u) == 0u;
            if (stateOk && which == 0)
                stateOk = r32(rdram, 0x78869Cu) == 0u && r32(rdram, 0x788768u) == 0u && r32(rdram, 0x788648u) != 1u &&
                          r32(rdram, 0x997040u) == 0u && threadParked(runtime, r32(rdram, kThreadCls6));
            if (!stateOk)
            {
                ++g_cri.wakeMissState[which];
                g_wakeOriginal(rdram, ctx, runtime);
                return;
            }
            Cls2Extra extra;
            g_idleWhy = 0u;
            if (!classIdle(rdram, cls, extra))
            {
                ++g_cri.wakeMissBusy[which];
                ++g_whyCount[cls * 20u + (g_idleWhy % 20u)];
                g_wakeOriginal(rdram, ctx, runtime);
                return;
            }
            if (g_criMode == Mode::Verify)
            {
                // let the thread run its cycle; the next vblank checks its net effect
                WakeExpect &expect = g_wakeExpect[which];
                std::memcpy(&expect.loopCount, rdram + (loopAddress & PS2_RAM_MASK), sizeof(uint64_t));
                expect.classCount = r32(rdram, kSvmCallCount + 4u * cls);
                expect.armed = true;
                g_wakeOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_cri.wakeHit[which];
            uint64_t loop;
            std::memcpy(&loop, rdram + (loopAddress & PS2_RAM_MASK), sizeof(loop));
            ++loop;
            std::memcpy(rdram + (loopAddress & PS2_RAM_MASK), &loop, sizeof(loop));
            classApplyEffects(rdram, cls, extra);
            returnToCaller(ctx);
        }

        // ---- 3. RenderWare allocator (0x104140 / 0x104280 / 0x104040) ---------------
        //
        // RwMalloc(a0 = size, a1 unused apart from debug name compares):
        //   0x104140..0x1041C0: debug-only logging (0x195450 returns the updating task, strcmp/printf on
        //     matches) -> skipped, no guest-visible state; s0 = DIntr()
        //   0x1041C8: inner = 0x103DA0(size + 0x14):
        //       [gp-0x4E44] += n                       (0x103DD0..0x103DD8, n = size + 0x14)
        //       m = _malloc_r(reent, n + 0x10)         (0x103DE8: 0x520728 -> _malloc_r host stub -> guestMalloc)
        //       m == 0: counter stays incremented, debug prints, 0x16F190(0x141A), K_Assert path, returns 0
        //       [m+0] = n; [m+8] = ++[gp-0x4E38]; [m+0xC] = [gp-0x4E34]; returns raw = m + 0x10
        //   0x104200: [raw-0xC] = 0x10;  a0 = raw + 4;  user = a0 + 0x10 - (a0 & 0xF);  [user-4] = raw
        //   EIntr() if DIntr returned nonzero (net interrupt state unchanged) and a debug strcmp; returns user.
        // RwFree(user) (0x104280): user == 0 -> nothing; else 0x103D00([user-4]):
        //   raw == 0 -> nothing; size = [raw-0x10]; flag = [raw-4]; [gp-0x4E44] -= size;
        //   flag != 0 -> memset(raw-0x10, 0, size) (0x521408); _free_r(reent, raw-0x10) (0x520748 -> guestFree)
        // RwCalloc(count, size, a2) (0x104040): total = count * size (32-bit mult); user = RwMalloc(total, a2);
        //   user != 0 -> memset(user, 0, total), [gp-0x4E3C]++ ; returns user.
        // RwRealloc (0x103F50) is left alone; it calls 0x104140 / 0x104280 through the dispatch table and so
        // uses these implementations, and reads the same header ([user-4] = raw).

        constexpr uint32_t kGpBytesInUse = 0x4E44u;
        constexpr uint32_t kGpSerial = 0x4E38u;
        constexpr uint32_t kGpTag = 0x4E34u;
        constexpr uint32_t kGpCallocCount = 0x4E3Cu;

        struct NativeBlock
        {
            uint32_t user = 0u;
            uint32_t raw = 0u;
            uint32_t innerSize = 0u;
        };

        bool nativeAlloc(uint8_t *rdram, PS2Runtime *runtime, uint32_t gp, uint32_t size, NativeBlock &out)
        {
            const uint32_t n = size + 0x14u;
            const uint32_t request = n + 0x10u;
            if (n < size || request < n)
                return false;
            const uint32_t block = runtime->guestMalloc(request);
            if (!block)
                return false; // leave failure (counters, diagnostics, assert) to the guest implementation
            const uint32_t raw = block + 0x10u;
            w32(rdram, gp - kGpBytesInUse, r32(rdram, gp - kGpBytesInUse) + n);
            const uint32_t serial = r32(rdram, gp - kGpSerial) + 1u;
            w32(rdram, gp - kGpSerial, serial);
            w32(rdram, block + 0x0u, n);
            w32(rdram, block + 0x8u, serial);
            w32(rdram, block + 0xCu, r32(rdram, gp - kGpTag));
            w32(rdram, raw - 0xCu, 0x10u);
            const uint32_t a0 = raw + 4u;
            const uint32_t user = a0 + (0x10u - (a0 & 0xFu));
            w32(rdram, user - 4u, raw);
            out.user = user;
            out.raw = raw;
            out.innerSize = n;
            return true;
        }

        void maybeLogAlloc()
        {
            static uint32_t calls = 0u;
            if ((++calls & 0x3FFu) == 0u)
                maybeLog();
        }

        void callGuest(GuestFunction function, uint32_t entry, uint8_t *rdram, R5900Context *source,
                       PS2Runtime *runtime, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t &v0)
        {
            R5900Context copy = *source;
            copy.pc = entry;
            copy.r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(a0));
            copy.r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(a1));
            copy.r[6] = _mm_cvtsi32_si128(static_cast<int32_t>(a2));
            function(rdram, &copy, runtime);
            v0 = getRegU32(&copy, 2);
        }

        // P3FES_CUT_RWALLOC=verify: after a native allocation, ask the guest function for the same
        // request, compare the header it writes, then release it again and restore the counters.
        void verifyAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t gp, uint32_t size,
                         const NativeBlock &native)
        {
            if (!g_mallocOriginal || !g_freeOriginal)
                return;
            const uint32_t bytesAfterNative = r32(rdram, gp - kGpBytesInUse);
            const uint32_t serialAfterNative = r32(rdram, gp - kGpSerial);
            uint32_t user2 = 0u;
            callGuest(g_mallocOriginal, kRwMalloc, rdram, ctx, runtime, size, getRegU32(ctx, 5), 0u, user2);
            bool ok = user2 != 0u;
            if (ok)
            {
                const uint32_t raw2 = r32(rdram, user2 - 4u);
                ok = (user2 - raw2) == (native.user - native.raw) &&
                     r32(rdram, raw2 - 0x10u) == r32(rdram, native.raw - 0x10u) &&
                     r32(rdram, raw2 - 0xCu) == r32(rdram, native.raw - 0xCu) &&
                     r32(rdram, raw2 - 0x8u) == serialAfterNative + 1u &&
                     r32(rdram, raw2 - 0x4u) == r32(rdram, native.raw - 0x4u) &&
                     r32(rdram, gp - kGpBytesInUse) == bytesAfterNative + native.innerSize &&
                     (user2 & 0xFu) == 0u;
                uint32_t ignored = 0u;
                callGuest(g_freeOriginal, kRwFree, rdram, ctx, runtime, user2, 0u, 0u, ignored);
            }
            w32(rdram, gp - kGpSerial, serialAfterNative);
            w32(rdram, gp - kGpBytesInUse, bytesAfterNative);
            if (ok)
            {
                ++g_counters.verifyOk;
            }
            else if (++g_counters.verifyBad <= 8u)
            {
                std::fprintf(stderr, "[p3fes-cuts] rwalloc verify MISMATCH size=%u native user=%08X raw=%08X guest user=%08X\n",
                             size, native.user, native.raw, user2);
            }
        }

        void rwMallocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            maybeLogAlloc();
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t size = getRegU32(ctx, 4);
            NativeBlock block;
            if (!nativeAlloc(rdram, runtime, gp, size, block))
            {
                ++g_counters.allocFall;
                g_mallocOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_counters.allocHit;
            if (g_rwAllocMode == Mode::Verify)
                verifyAlloc(rdram, ctx, runtime, gp, size, block);
            setReturnU32(ctx, block.user);
            returnToCaller(ctx);
        }

        void rwFreeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            maybeLogAlloc();
            const uint32_t user = getRegU32(ctx, 4);
            if (!user)
            {
                ++g_counters.freeHit;
                returnToCaller(ctx);
                return;
            }
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t raw = r32(rdram, user - 4u);
            if (raw)
            {
                const uint32_t header = raw - 0x10u;
                const uint32_t size = r32(rdram, header);
                const uint32_t flag = r32(rdram, raw - 4u);
                const uint32_t physical = header & PS2_RAM_MASK;
                if (flag && (size > PS2_RAM_SIZE || physical + size > PS2_RAM_SIZE))
                {
                    ++g_counters.freeFall; // implausible header: let the guest code deal with it
                    g_freeOriginal(rdram, ctx, runtime);
                    return;
                }
                w32(rdram, gp - kGpBytesInUse, r32(rdram, gp - kGpBytesInUse) - size);
                if (flag)
                    std::memset(rdram + physical, 0, size);
                runtime->guestFree(header);
            }
            ++g_counters.freeHit;
            returnToCaller(ctx);
        }

        void rwCallocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            maybeLogAlloc();
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t total = getRegU32(ctx, 4) * getRegU32(ctx, 5);
            NativeBlock block;
            if (!nativeAlloc(rdram, runtime, gp, total, block) ||
                (block.user & PS2_RAM_MASK) + total > PS2_RAM_SIZE)
            {
                // nativeAlloc may have succeeded only to fail the bound check: release it untouched.
                if (block.user)
                {
                    w32(rdram, gp - kGpBytesInUse, r32(rdram, gp - kGpBytesInUse) - block.innerSize);
                    w32(rdram, gp - kGpSerial, r32(rdram, gp - kGpSerial) - 1u);
                    runtime->guestFree(block.raw - 0x10u);
                }
                ++g_counters.callocFall;
                g_callocOriginal(rdram, ctx, runtime);
                return;
            }
            std::memset(rdram + (block.user & PS2_RAM_MASK), 0, total);
            w32(rdram, gp - kGpCallocCount, r32(rdram, gp - kGpCallocCount) + 1u);
            ++g_counters.callocHit;
            setReturnU32(ctx, block.user);
            returnToCaller(ctx);
        }

        // ---- 4. H_Dbprt_Main (0x104420) ----------------------------------------------
        //
        // Sole caller: kwlnRootUpdateEtcDrawTask 0x198F90 = H_Fade_Main (0x107060), H_Dbprt_Main,
        // H_Dbprt_Flush (0x1043B0), return 0. H_Dbprt_Main only reads the text buffers (0x7E1E30 rows,
        // list [gp-0x4E28], 0x1053F0 rows) and draws them through the Im2D callback ([0x9600A0]); its
        // only stores go to its own stack. Buffer clearing is done by H_Dbprt_Flush, which stays. The
        // render states it leaves behind (0x196770 common states, 6/7/8/9/0xC, texture raster
        // [gp-0x4E20]) are re-established by every later drawing pass (H_Fade_Main sets its own at
        // 0x107074..0x1070F4 before drawing).
        void dbprtMainHook(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!ctx)
                return;
            ++g_counters.dbprtCalls;
            returnToCaller(ctx);
        }

        // ---- diagnostic call counters (P3FES_CUT_CRI_TRACE=1): how often each CRI entry point runs ----
        constexpr uint32_t kTraceAddrs[] = {
            0x540D80u, 0x53FFF8u, 0x541008u, 0x540FE0u, 0x540F60u, 0x540ED8u, 0x565260u, 0x564978u, 0x564990u,
            0x549EA0u, 0x559938u, 0x506088u, 0x53E0B8u, 0x535050u, 0x585C90u, 0x585E70u, 0x53F618u, 0x552658u,
            0x53F8B8u, 0x53F940u, 0x5400B0u, 0x540048u, 0x540098u, 0x540118u, 0x53FBF0u, 0x53FCF8u, 0x53FDE8u,
            0x539818u, 0x5617C0u, 0x564888u, 0x5400B0u};
        constexpr size_t kTraceCount = sizeof(kTraceAddrs) / sizeof(kTraceAddrs[0]);
        GuestFunction g_traceOriginal[kTraceCount] = {};
        uint64_t g_traceCalls[kTraceCount] = {};
        uint64_t g_traceExecClass[8] = {};
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_traceRa;

        template <size_t I>
        void traceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_traceCalls[I];
            if (kTraceAddrs[I] == 0x565260u)
                ++g_traceExecClass[getRegU32(ctx, 4) & 7u];
            if (kTraceAddrs[I] == 0x539818u || kTraceAddrs[I] == 0x5617C0u || kTraceAddrs[I] == 0x564888u || kTraceAddrs[I] == 0x5400B0u)
                ++g_traceRa[{kTraceAddrs[I], getRegU32(ctx, 31)}];
            g_traceOriginal[I](rdram, ctx, runtime);
        }

        template <size_t... Is>
        void installTraceHooks(PS2Runtime &runtime, std::index_sequence<Is...>)
        {
            static const GuestFunction hooks[] = {&traceHook<Is>...};
            for (size_t i = 0u; i < kTraceCount; ++i)
            {
                if (!runtime.hasFunction(kTraceAddrs[i]))
                    continue;
                GuestFunction current = runtime.lookupFunction(kTraceAddrs[i]);
                if (!current || current == hooks[i])
                    continue;
                g_traceOriginal[i] = current;
                runtime.replaceFunction(kTraceAddrs[i], hooks[i]);
            }
        }

        void logTrace()
        {
            static uint64_t previous[kTraceCount] = {};
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - last).count();
            last = now;
            std::fprintf(stderr, "[p3fes-cri-trace] %.1fs per second:", seconds);
            for (size_t i = 0u; i < kTraceCount; ++i)
            {
                if (g_traceOriginal[i] == nullptr)
                    continue;
                std::fprintf(stderr, " %X=%.1f", kTraceAddrs[i], static_cast<double>(g_traceCalls[i] - previous[i]) / seconds);
                previous[i] = g_traceCalls[i];
            }
            std::fprintf(stderr, " cls:");
            for (uint32_t c = 0u; c < 8u; ++c)
                std::fprintf(stderr, " %llu", static_cast<unsigned long long>(g_traceExecClass[c]));
            std::fprintf(stderr, "\n");
            std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> ras;
            for (const auto &e : g_traceRa)
                ras.push_back({e.second, e.first});
            std::sort(ras.begin(), ras.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
            std::fprintf(stderr, "[p3fes-cri-trace] callers (cumulative):");
            for (size_t i = 0u; i < ras.size() && i < 14u; ++i)
                std::fprintf(stderr, " %X<-%X=%llu", ras[i].second.first, ras[i].second.second, static_cast<unsigned long long>(ras[i].first));
            std::fprintf(stderr, "\n");
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
    }

    bool install(PS2Runtime &runtime)
    {
        const char *diag = std::getenv("P3FES_RESOURCE_DIAG");
        g_diag = diag && *diag == '1';
        g_counters = {};
        g_cri = {};
        g_t7Hit = g_t7Miss = g_t7VerifyOk = g_t7VerifyBad = 0u;
        for (int i = 0; i < 2; ++i) g_srvHit[i] = g_srvMiss[i] = g_srvVerifyOk[i] = g_srvVerifyBad[i] = g_srvVerifyUnwound[i] = 0u;
        g_dtxSkipRun = 0u;
        bool any = false;

        g_shadowMode = readMode("P3FES_CUT_SHADOWMAP", true);
        // The pass feeds the native shadow render target (P3FES_SHADOWS, default on), so it is only
        // cut on request or when native shadows are disabled.
        if (const char *cut = std::getenv("P3FES_CUT_SHADOWMAP"); !cut || !*cut)
        {
            const char *shadows = std::getenv("P3FES_SHADOWS");
            g_shadowMode = shadows && *shadows == '0' ? Mode::On : Mode::Off;
        }
        bool shadow = false;
        if (g_shadowMode != Mode::Off)
            shadow = installOne(runtime, kShadowMapTask, &shadowMapHook, g_shadowOriginal);
        if (g_shadowMode == Mode::Verify && !shadow)
            g_shadowMode = Mode::On;
        std::fprintf(stderr, "[p3fes-native] cut shadow-map pass hook=%u mode=%s\n", shadow ? 1u : 0u,
                     g_shadowMode == Mode::Verify ? "verify" : (g_shadowMode == Mode::On ? "on" : "off"));
        any |= shadow;

        if (const char *value = std::getenv("P3FES_CUT_RS"); !value || *value != '0')
        {
            // On by default for the states whose repeats were verified to change nothing but GS packets
            // (lounge, Tartarus floors 2 and 5, a battle, school, monorail: verify mode, 2026-11-14):
            // TEXTUREADDRESS 2, ZTEST 6, SHADEMODE 7, ZWRITE 8, TEXTUREFILTER 9, VERTEXALPHA 12, FOG 14-16,
            // CULLMODE 20. Not the texture raster (1), ADDRESSU/V (3, 4) or the blend pair (10, 11): their
            // repeats do change driver state.
            g_rsMode = !value || !*value ? Mode::On
                     : std::strcmp(value, "verify") == 0 ? Mode::Verify : (std::strcmp(value, "log") == 0 ? Mode::Log : Mode::On);
            const char *list = std::getenv("P3FES_CUT_RS_STATES");
            if (!list)
                list = "2,6,7,8,9,12,14,15,16,20";
            for (const char *cursor = list; *cursor;)
            {
                char *end = nullptr;
                const unsigned long state = std::strtoul(cursor, &end, 10);
                if (end == cursor) break;
                if (state < 64u) g_rsDroppable |= 1ull << state;
                cursor = *end == ',' ? end + 1 : end;
            }
            const bool installed = installOne(runtime, kSkySetRenderState, &renderStateHook, g_rsOriginal);
            std::fprintf(stderr, "[p3fes-native] render-state repeat gate hook=%u mode=%s droppable=%llX\n", installed ? 1u : 0u,
                         value && *value ? value : "on", static_cast<unsigned long long>(g_rsDroppable));
        }
        if (const char *value = std::getenv("P3FES_CUT_SFD_SPR"); !value || *value != '0')
        {
            // The task hook first: without it the swap hook would never see a movie start.
            const bool installed = installOne(runtime, kSfdUpdateTask, &sfdTaskHook, g_sfdTaskOriginal) &&
                                   installOne(runtime, kSfdSwapIn, &sfdSwapInHook, g_sfdSwapOriginal);
            std::fprintf(stderr, "[p3fes-native] Sofdec scratchpad swap gate hook=%u\n", installed ? 1u : 0u);
            any |= installed;
        }

        if (const char *value = std::getenv("P3FES_CUT_MCFREE"); !value || *value != '0')
        {
            // All or nothing: without every invalidator the remembered count could go stale.
            bool installed = installMcInvalidators(runtime, std::make_index_sequence<kMcInvalidatorCount>{});
            installed = installed && installOne(runtime, kMcFreeClustReturn, &mcReturnHook, g_mcReturnOriginal) &&
                        installOne(runtime, kMcFreeClustSize, &mcFreeClustSizeHook, g_mcFreeOriginal);
            std::fprintf(stderr, "[p3fes-native] memory-card free-space memo hook=%u\n", installed ? 1u : 0u);
            any |= installed;
        }

        if (const char *trace = std::getenv("P3FES_CUT_CRI_TRACE"); trace && *trace == '1')
            installTraceHooks(runtime, std::make_index_sequence<kTraceCount>{});

        if (const char *term = std::getenv("P3FES_CUT_ADXT_TERM"); term && *term == '0')
            g_adxtTermNoop = false;
        const Mode adxMode = readMode("P3FES_CUT_ADXT_IDLE", true);
        const bool adx = adxMode != Mode::Off && installOne(runtime, kAdxtExecServer, &adxServerHook, g_adxOriginal);
        g_adxMode = adx ? adxMode : Mode::Off;
        std::fprintf(stderr, "[p3fes-native] cut ADXT idle gate hook=%u mode=%s\n", adx ? 1u : 0u,
                     g_adxMode == Mode::Verify ? "verify" : (g_adxMode == Mode::On ? "on" : "off"));
        any |= adx;

        {
            Mode criMode = readMode("P3FES_CUT_CRI_IDLE", true);
            if (criMode == Mode::Log)
                criMode = Mode::Off;
            const bool svm = criMode != Mode::Off && installOne(runtime, kSvmClsWrapper4, &svmHook4, g_svmOriginal4) &&
                             installOne(runtime, 0x005653F0u, &svmHook2, g_svmOriginal2) &&
                             installOne(runtime, 0x00565438u, &svmHook5, g_svmOriginal5) &&
                             installOne(runtime, 0x00565450u, &svmHook6, g_svmOriginal6);
            const bool wake = svm && installOne(runtime, kWakeHelper, &wakeHook, g_wakeOriginal);
            const bool t7 = svm && installOne(runtime, kHandshakeResume, &handshakeResumeHook, g_handshakeOriginal);
            (void)t7;
            const bool srvs = svm && installOne(runtime, kDecSvr, &decSvrHook, g_decOriginal) &&
                              installOne(runtime, kMwsfdVsync, &mwsfdVsyncHook, g_mwsfdOriginal);
            (void)srvs;
            g_criMode = svm ? criMode : Mode::Off;
            std::fprintf(stderr, "[p3fes-native] cut CRI server gates svm=%u wake=%u mode=%s\n", svm ? 1u : 0u, wake ? 1u : 0u,
                         g_criMode == Mode::Verify ? "verify" : (g_criMode == Mode::On ? "on" : "off"));
            any |= svm;
        }

        g_rwAllocMode = readMode("P3FES_CUT_RWALLOC", true);
        bool alloc = false;
        if (g_rwAllocMode != Mode::Off)
        {
            // All three or none: they share the header layout and the counters.
            GuestFunction mallocOriginal = nullptr, freeOriginal = nullptr, callocOriginal = nullptr;
            if (runtime.hasFunction(kRwMalloc) && runtime.hasFunction(kRwFree) && runtime.hasFunction(kRwCalloc))
            {
                alloc = installOne(runtime, kRwMalloc, &rwMallocHook, mallocOriginal) &&
                        installOne(runtime, kRwFree, &rwFreeHook, freeOriginal) &&
                        installOne(runtime, kRwCalloc, &rwCallocHook, callocOriginal);
                g_mallocOriginal = mallocOriginal;
                g_freeOriginal = freeOriginal;
                g_callocOriginal = callocOriginal;
                if (!alloc)
                {
                    if (mallocOriginal) runtime.replaceFunction(kRwMalloc, mallocOriginal);
                    if (freeOriginal) runtime.replaceFunction(kRwFree, freeOriginal);
                    if (callocOriginal) runtime.replaceFunction(kRwCalloc, callocOriginal);
                }
            }
        }
        if (g_rwAllocMode == Mode::Verify && !alloc)
            g_rwAllocMode = Mode::On;
        std::fprintf(stderr, "[p3fes-native] cut RenderWare allocator hooks=%u mode=%s\n", alloc ? 1u : 0u,
                     g_rwAllocMode == Mode::Verify ? "verify" : (g_rwAllocMode == Mode::On ? "on" : "off"));
        any |= alloc;

        const Mode dbprtMode = readMode("P3FES_CUT_DBPRT", false);
        const bool dbprt = dbprtMode != Mode::Off && installOne(runtime, kDbprtMain, &dbprtMainHook, g_dbprtOriginal);
        std::fprintf(stderr, "[p3fes-native] cut debug text overlay hook=%u\n", dbprt ? 1u : 0u);
        any |= dbprt;
        any |= p3fes_native_dialogue::install(runtime);
        return any;
    }

    static_assert(kRenderStateCacheBytes == sizeof(g_rsLast) + sizeof(g_rsKnown), "render-state cache layout");

    void saveRenderStateCache(void *buffer)
    {
        std::memcpy(buffer, g_rsLast, sizeof(g_rsLast));
        std::memcpy(static_cast<uint8_t *>(buffer) + sizeof(g_rsLast), g_rsKnown, sizeof(g_rsKnown));
    }

    void restoreRenderStateCache(const void *buffer)
    {
        std::memcpy(g_rsLast, buffer, sizeof(g_rsLast));
        std::memcpy(g_rsKnown, static_cast<const uint8_t *>(buffer) + sizeof(g_rsLast), sizeof(g_rsKnown));
    }

    void logCounters()
    {
        p3fes_native_dialogue::logCounters();
        const Counters &c = g_counters;
        if (g_rsOriginal)
        {
            std::fprintf(stderr, "[p3fes-cuts] render-state dropped=%llu calls(repeat/repeat-that-changed):",
                         static_cast<unsigned long long>(g_rsDropped));
            for (unsigned i = 0; i < 64u; ++i)
                if (g_rsCalls[i])
                    std::fprintf(stderr, " %u:%llu(%llu/%llu)", i, static_cast<unsigned long long>(g_rsCalls[i]),
                                 static_cast<unsigned long long>(g_rsSame[i]), static_cast<unsigned long long>(g_rsSameChanged[i]));
            std::fprintf(stderr, "\n");
            for (unsigned i = 0; i < 64u; ++i) g_rsCalls[i] = g_rsSame[i] = g_rsSameChanged[i] = 0u;
        }
        std::fprintf(stderr, "[p3fes-cuts] sfd-spr skipped=%llu run=%llu | mc-free hits=%llu counts=%llu\n",
                     static_cast<unsigned long long>(g_sfdSwapSkipped), static_cast<unsigned long long>(g_sfdSwapRun),
                     static_cast<unsigned long long>(g_mcFreeHits), static_cast<unsigned long long>(g_mcFreeCounts));
        {
            const CriCounters &k = g_cri;
            std::fprintf(stderr, "[p3fes-cuts] cri-thread7 (0x540ED8) hit=%llu miss=%llu verify ok=%llu bad=%llu\n",
                         static_cast<unsigned long long>(g_t7Hit), static_cast<unsigned long long>(g_t7Miss),
                         static_cast<unsigned long long>(g_t7VerifyOk), static_cast<unsigned long long>(g_t7VerifyBad));
            std::fprintf(stderr, "[p3fes-cuts] cri-cls56 svm5 hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu | svm6 hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu\n",
                         static_cast<unsigned long long>(g_cri.classHit[5]), static_cast<unsigned long long>(g_cri.classMiss[5]),
                         static_cast<unsigned long long>(g_cri.classVerifyOk[5]), static_cast<unsigned long long>(g_cri.classVerifyBad[5]),
                         static_cast<unsigned long long>(g_cri.classVerifyUnwound[5]), static_cast<unsigned long long>(g_cri.classHit[6]),
                         static_cast<unsigned long long>(g_cri.classMiss[6]), static_cast<unsigned long long>(g_cri.classVerifyOk[6]),
                         static_cast<unsigned long long>(g_cri.classVerifyBad[6]), static_cast<unsigned long long>(g_cri.classVerifyUnwound[6]));
            std::fprintf(stderr, "[p3fes-cuts] cri-servers decSvr(0x552658) hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu | mwsfdVsync(0x585BA8) hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu\n",
                         static_cast<unsigned long long>(g_srvHit[0]), static_cast<unsigned long long>(g_srvMiss[0]),
                         static_cast<unsigned long long>(g_srvVerifyOk[0]), static_cast<unsigned long long>(g_srvVerifyBad[0]),
                         static_cast<unsigned long long>(g_srvVerifyUnwound[0]), static_cast<unsigned long long>(g_srvHit[1]),
                         static_cast<unsigned long long>(g_srvMiss[1]), static_cast<unsigned long long>(g_srvVerifyOk[1]),
                         static_cast<unsigned long long>(g_srvVerifyBad[1]), static_cast<unsigned long long>(g_srvVerifyUnwound[1]));
            for (uint32_t cls = 2u; cls <= 6u; ++cls)
            {
                if (cls == 3u)
                    continue;
                std::fprintf(stderr, "[p3fes-cuts] cri-why cls%u:", cls);
                for (uint32_t i = 0u; i < 20u; ++i)
                    if (g_whyCount[cls * 20u + i])
                        std::fprintf(stderr, " %u=%llu", i, static_cast<unsigned long long>(g_whyCount[cls * 20u + i]));
                std::fprintf(stderr, "\n");
            }
            std::fprintf(stderr,
                         "[p3fes-cuts] cri-idle svm2 hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu | svm4 hit=%llu miss=%llu verify ok=%llu bad=%llu unwound=%llu | wake5 hit=%llu busy=%llu state=%llu verify ok=%llu bad=%llu | wake6 hit=%llu busy=%llu state=%llu verify ok=%llu bad=%llu\n",
                         static_cast<unsigned long long>(k.classHit[2]), static_cast<unsigned long long>(k.classMiss[2]),
                         static_cast<unsigned long long>(k.classVerifyOk[2]), static_cast<unsigned long long>(k.classVerifyBad[2]),
                         static_cast<unsigned long long>(k.classVerifyUnwound[2]),
                         static_cast<unsigned long long>(k.classHit[4]), static_cast<unsigned long long>(k.classMiss[4]),
                         static_cast<unsigned long long>(k.classVerifyOk[4]), static_cast<unsigned long long>(k.classVerifyBad[4]),
                         static_cast<unsigned long long>(k.classVerifyUnwound[4]),
                         static_cast<unsigned long long>(k.wakeHit[0]), static_cast<unsigned long long>(k.wakeMissBusy[0]),
                         static_cast<unsigned long long>(k.wakeMissState[0]), static_cast<unsigned long long>(k.wakeVerifyOk[0]),
                         static_cast<unsigned long long>(k.wakeVerifyBad[0]),
                         static_cast<unsigned long long>(k.wakeHit[1]), static_cast<unsigned long long>(k.wakeMissBusy[1]),
                         static_cast<unsigned long long>(k.wakeMissState[1]), static_cast<unsigned long long>(k.wakeVerifyOk[1]),
                         static_cast<unsigned long long>(k.wakeVerifyBad[1]));
        }
        std::fprintf(stderr,
                     "[p3fes-cuts] adx hit=%llu valve=%llu miss=%llu (guard=%llu adxt=%llu[live=%llu act=%llu] rna=%llu dtr=%llu sjd=%llu dtx=%llu) adx-verify ok=%llu bad=%llu unwound=%llu adxt-verify ok=%llu bad=%llu unwound=%llu term-hit=%llu "
                     "alloc hit=%llu fall=%llu free hit=%llu fall=%llu calloc hit=%llu fall=%llu verify ok=%llu bad=%llu "
                     "shadow calls=%llu passes-skipped=%llu dbprt=%llu last-busy-adxt(obj/state/[8]/[0x94])=%02X/%u/%u/%u\n",
                     static_cast<unsigned long long>(c.adxHit), static_cast<unsigned long long>(c.adxValve), static_cast<unsigned long long>(c.adxMiss),
                     static_cast<unsigned long long>(c.adxMissGuard), static_cast<unsigned long long>(c.adxMissAdxt),
                     static_cast<unsigned long long>(c.adxMissAdxtLive), static_cast<unsigned long long>(c.adxMissAdxtAct),
                     static_cast<unsigned long long>(c.adxMissRna), static_cast<unsigned long long>(c.adxMissDtr),
                     static_cast<unsigned long long>(c.adxMissSjd), static_cast<unsigned long long>(c.adxMissDtx),
                     static_cast<unsigned long long>(c.adxVerifyOk), static_cast<unsigned long long>(c.adxVerifyBad),
                     static_cast<unsigned long long>(c.adxVerifyUnwound), static_cast<unsigned long long>(c.adxtVerifyOk),
                     static_cast<unsigned long long>(c.adxtVerifyBad), static_cast<unsigned long long>(c.adxtVerifyUnwound),
                     static_cast<unsigned long long>(c.adxtTermHit),
                     static_cast<unsigned long long>(c.allocHit), static_cast<unsigned long long>(c.allocFall),
                     static_cast<unsigned long long>(c.freeHit), static_cast<unsigned long long>(c.freeFall),
                     static_cast<unsigned long long>(c.callocHit), static_cast<unsigned long long>(c.callocFall),
                     static_cast<unsigned long long>(c.verifyOk), static_cast<unsigned long long>(c.verifyBad),
                     static_cast<unsigned long long>(c.shadowCalls), static_cast<unsigned long long>(c.shadowPasses),
                     static_cast<unsigned long long>(c.dbprtCalls), g_lastBusyAdxt >> 24, (g_lastBusyAdxt >> 16) & 0xFFu,
                     (g_lastBusyAdxt >> 8) & 1u, g_lastBusyAdxt & 1u);
    }
}

// Unity include while p3fes_native_dialogue.cpp is not in ps2xRuntime/CMakeLists.txt.
