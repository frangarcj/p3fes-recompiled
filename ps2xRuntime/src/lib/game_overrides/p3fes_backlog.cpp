#include "p3fes_backlog.h"
#include "p3fes_gameplay_options.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace
{
    std::mutex historyMutex;
    std::deque<p3fes_backlog::Entry> history;
    std::atomic<bool> dialogueOpen{false};
    uint32_t lastWindow = 0; // EE thread only
    constexpr size_t maxPages = 128;
    PS2Runtime::RecompiledFunction originalLayout, originalTaskMain, originalTaskUpdate;

    template <typename T>
    T read(const uint8_t *ram, uint32_t addr)
    {
        T value;
        std::memcpy(&value, ram + addr, sizeof(value));
        return value;
    }

    void appendGlyph(std::string &text, uint16_t code)
    {
        // frFont accepts both single-byte ASCII and a two-byte spelling of
        // the same glyphs at the start of its extended character table.
        if (code >= 0x8080 && code <= 0x80DE)
            code -= 0x8060;
        if (code >= 32 && code <= 126)
        {
            text += static_cast<char>(code);
            return;
        }
        // The stock desktop and Vita menu fonts cover Latin-1. Keep the
        // P3 punctuation outside their range readable in the history.
        switch (code)
        {
        case 0x829A: text += '-'; break;
        case 0x829B: text += '~'; break;
        case 0x829D: text += "..."; break;
        case 0x829E: text += ".."; break;
        case 0x829F: text += '"'; break;
        case 0x82AD: text += "±"; break;
        case 0x82AE: text += "×"; break;
        case 0x82B0: text += "÷"; break;
        case 0x82B8: text += "°"; break;
        case 0x82CF: text += "->"; break;
        case 0x82D0: text += "<-"; break;
        case 0x83F7: text += ' '; break; // space inserted by the US layout routine
        default: text += '?'; break;
        }
    }

    // SLUS-21621 frFont layout. Walk rendered runs so player names and other
    // substitutions come from the same glyph stream as the on-screen text.
    std::string textOf(const uint8_t *ram, uint32_t handle)
    {
        if (!handle || handle > PS2_RAM_SIZE - 0x30)
            return {};
        std::string text;
        uint32_t lastY = 0;
        uint32_t run = read<uint32_t>(ram, handle + 0x2C);
        for (unsigned runs = 0; run && runs < 256; ++runs)
        {
            if (run > PS2_RAM_SIZE - 0x2C)
                return {};
            auto glyph = read<uint32_t>(ram, run + 0x1C);
            const auto y = read<uint32_t>(ram, run + 8);
            if (glyph && !text.empty() && y != lastY)
                text += '\n';
            if (glyph)
                lastY = y;
            while (glyph)
            {
                if (glyph > PS2_RAM_SIZE - 0x2C || text.size() >= 2048)
                    return {};
                appendGlyph(text, read<uint16_t>(ram, glyph));
                glyph = read<uint32_t>(ram, glyph + 0x28);
            }
            run = read<uint32_t>(ram, run + 0x28);
        }
        return run ? std::string{} : text;
    }

    void layoutPage(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Called once after both the name and the message have been laid out on
        // the first page and again on page changes. Choice lists use another path.
        const uint32_t window = getRegU32(ctx, 4);
        if (ps2xGameplayOption("P3FES_BACKLOG") && window && window <= PS2_RAM_SIZE - 0x34)
        {
            auto body = textOf(ram, read<uint32_t>(ram, window + 0x30));
            if (!body.empty())
            {
                auto name = textOf(ram, read<uint32_t>(ram, window + 0x1C));
                std::lock_guard lock(historyMutex);
                if (history.size() == maxPages)
                    history.pop_front();
                history.push_back({std::move(name), std::move(body)});
                lastWindow = window;
            }
        }
        originalLayout(ram, ctx, runtime);
    }

    void taskMain(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // CalcMsgMng stores all window slots in this list. Its print and wait
        // states are 3 and 4, even while the characters are still appearing.
        bool active = false;
        if (lastWindow)
        {
            auto node = read<uint32_t>(ram, 0x959ED0);
            for (unsigned i = 0; node && node <= PS2_RAM_SIZE - 16 && i < 64; ++i)
            {
                if (read<uint32_t>(ram, node + 12) == lastWindow)
                {
                    const auto phase = read<uint32_t>(ram, lastWindow) & 7;
                    active = phase == 3 || phase == 4;
                    break;
                }
                node = read<uint32_t>(ram, node + 4);
            }
        }
        dialogueOpen.store(active, std::memory_order_release);
        originalTaskMain(ram, ctx, runtime);
    }

    void taskUpdate(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (p3fes_gameplay_options::visible())
        {
            const uint32_t task = getRegU32(ctx, 4);
            if (task && task <= PS2_RAM_SIZE - 0x34)
            {
                // kwlnTaskUpdate's own simulation-pause range. Keep rendering,
                // including the portrait and text, but hold scripts and message timers.
                const auto priority = read<uint32_t>(ram, task + 0x20);
                if ((priority >= 0x833 && priority < 0x103E) ||
                    std::memcmp(ram + task, "CalcMsgMng", 11) == 0)
                {
                    SET_GPR_U32(ctx, 2, 1);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
        }
        originalTaskUpdate(ram, ctx, runtime);
    }

}

bool p3fes_backlog::dialogueActive() { return dialogueOpen.load(std::memory_order_acquire); }

std::vector<p3fes_backlog::Entry> p3fes_backlog::entries()
{
    std::lock_guard lock(historyMutex);
    return {history.begin(), history.end()};
}

void p3fes_backlog::install(PS2Runtime &runtime)
{
    std::lock_guard lock(historyMutex);
    history.clear();
    lastWindow = 0;
    dialogueOpen.store(false);
    using Function = PS2Runtime::RecompiledFunction;
    auto hook = [&](uint32_t pc, Function replacement, Function &original) {
        if (auto fn = runtime.lookupFunction(pc); fn && fn != replacement)
        {
            original = fn;
            runtime.replaceFunction(pc, replacement);
        }
    };
    hook(0x3A67E0, layoutPage, originalLayout);
    hook(0x1949E0, taskMain, originalTaskMain);
    hook(0x193EC0, taskUpdate, originalTaskUpdate);
}
