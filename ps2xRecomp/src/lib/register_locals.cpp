#include "ps2recomp/register_locals.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Register locals: a pass over the text of one generated function.
//
// The translators emit every guest register access against the CPU context
// (GPR_U32(ctx, n), SET_GPR_S32(ctx, n, v)). Each of those is a load or store
// that the host compiler cannot keep in a machine register, because a store to
// guest memory may alias the context. This pass rewrites the accesses to
// function-local variables (ps2_r1..ps2_r31, low 64 bits of each GPR) and
// writes them back to the context only where something else can observe it:
//
//   - before anything that receives the context (calls, syscalls, traps), and
//     reloading afterwards what is still needed;
//   - before every return;
//   - around the few instructions that use the full 128-bit register.
//
// Which registers to store or load at each point comes from two dataflow
// analyses over the lines of the function (each line is one node):
//   may-dirty  (forward):  locals written since the context last had them;
//   live       (backward): locals whose current value may still be read. A
//                          store of a may-dirty local counts as a read, so a
//                          local that is stored at a join point is valid on
//                          every path that reaches it.
// Both analyses over-approximate (every line falls through to the next one,
// besides its gotos), which only costs extra loads and stores.

namespace ps2recomp
{
    namespace
    {
        using Mask = uint32_t; // bit n = guest register n; bit 0 is never set

        struct Line
        {
            std::string text;
            Mask reads = 0;
            Mask writes = 0;
            Mask vecReads = 0;  // GPR_VEC(ctx, n): the context must hold local n first
            Mask vecWrites = 0; // SET_GPR_VEC(ctx, n, ...): local n is reloaded after
            Mask simpleKill = 0;
            bool sync = false;      // the context is passed to other code on this line
            bool hasReturn = false; // contains "return;"
            bool entryCase = false; // "case X: goto label;" of the resume switch
            bool abiReturn = false; // a "jr $ra" return: only results and callee-saved registers matter
            bool functionEnd = false; // the closing brace: falling off the end leaves the function
            std::string label;      // "label_xxx:" defined here
            std::vector<std::string> gotos;
            int reloadAt = -1; // sync lines: index of the line before which locals are reloaded
            Mask dirtyIn = 0;
            Mask liveIn = 0;
            Mask liveOut = 0;
        };

        // n32 ABI. A callee preserves these; with PS2RECOMP_TRUST_CALLEE_SAVED
        // (default on) their locals stay valid across a call instead of being
        // reloaded, and a returning function stores only these and v0/v1.
        constexpr Mask kCalleeSaved = (0xFFu << 16) | (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31);
        constexpr Mask kResults = (1u << 2) | (1u << 3);

        bool trustCalleeSaved()
        {
            static const bool on = []
            {
                const char *value = std::getenv("PS2RECOMP_TRUST_CALLEE_SAVED");
                return value == nullptr || value[0] != '0';
            }();
            return on;
        }

        bool isIdentChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        // Parses "(ctx, N" at position pos (which points at '('). Returns the register or -1.
        int parseCtxRegister(std::string_view s, size_t pos, size_t &end)
        {
            static constexpr std::string_view prefix = "(ctx, ";
            if (s.substr(pos, prefix.size()) != prefix)
                return -1;
            size_t i = pos + prefix.size();
            size_t digits = 0;
            int value = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) != 0 && digits < 3)
            {
                value = value * 10 + (s[i] - '0');
                ++i;
                ++digits;
            }
            if (digits == 0 || value > 31 || i >= s.size())
                return -1;
            end = i;
            return value;
        }

        // Rewrites the register accesses of one line and records what it touches.
        void rewriteLine(Line &line)
        {
            const std::string &in = line.text;
            std::string out;
            out.reserve(in.size());
            size_t i = 0;
            int writeCount = 0;
            int lastWrite = -1;
            while (i < in.size())
            {
                struct Pattern
                {
                    std::string_view name;
                    std::string_view replacement;
                    bool write;
                    bool vec;
                };
                static constexpr Pattern patterns[] = {
                    {"SET_GPR_ZE32", "PS2W_ZE32", true, false},
                    {"SET_GPR_U32", "PS2W_S32", true, false}, // SET_GPR_U32 sign-extends, as SET_GPR_S32
                    {"SET_GPR_S32", "PS2W_S32", true, false},
                    {"SET_GPR_U64", "PS2W_U64", true, false},
                    {"SET_GPR_S64", "PS2W_U64", true, false},
                    {"SET_GPR_VEC", "", true, true},
                    {"GPR_U32", "PS2R_U32", false, false},
                    {"GPR_S32", "PS2R_S32", false, false},
                    {"GPR_U64", "PS2R_U64", false, false},
                    {"GPR_S64", "PS2R_S64", false, false},
                    {"GPR_VEC", "", false, true},
                    {"getRegU32", "PS2R_U32", false, false},
                };
                bool matched = false;
                if ((i == 0 || !isIdentChar(in[i - 1])) && (in[i] == 'S' || in[i] == 'G' || in[i] == 'g'))
                {
                    for (const Pattern &p : patterns)
                    {
                        if (in.compare(i, p.name.size(), p.name) != 0)
                            continue;
                        size_t end = 0;
                        const int reg = parseCtxRegister(in, i + p.name.size(), end);
                        if (reg < 0)
                            continue;
                        const char after = in[end];
                        if (p.write ? after != ',' : after != ')')
                            continue;
                        const Mask bit = reg == 0 ? 0u : (1u << reg);
                        if (p.vec)
                        {
                            // Left against the context; the sync code keeps the local in step.
                            (p.write ? line.vecWrites : line.vecReads) |= bit;
                            out.append(in, i, end - i);
                            i = end;
                        }
                        else if (p.write)
                        {
                            line.writes |= bit;
                            ++writeCount;
                            lastWrite = reg;
                            out += p.replacement;
                            out += '(';
                            out += std::to_string(reg);
                            i = end; // keeps ", value)"
                        }
                        else
                        {
                            line.reads |= bit;
                            out += p.replacement;
                            out += '(';
                            out += std::to_string(reg);
                            out += ')';
                            i = end + 1;
                        }
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    out += in[i];
                    ++i;
                }
            }
            line.text = std::move(out);

            // A 128-bit write that is not the whole statement may be conditional
            // (movz/movn: "if (...) SET_GPR_VEC(...)"). The context must then
            // already hold the local, or the reload after the line would bring
            // back a stale value when the write does not happen.
            if (line.vecWrites != 0)
            {
                const size_t start = line.text.find_first_not_of(" \t");
                if (start == std::string::npos || line.text.compare(start, 12, "SET_GPR_VEC(") != 0)
                    line.vecReads |= line.vecWrites;
            }

            // A line that is exactly one register assignment defines that register.
            const size_t first = line.text.find_first_not_of(" \t");
            if (writeCount == 1 && line.vecWrites == 0 && first != std::string::npos &&
                line.text.compare(first, 5, "PS2W_") == 0)
            {
                const size_t last = line.text.find_last_not_of(" \t");
                if (last != std::string::npos && last >= 1 && line.text[last] == ';' && line.text[last - 1] == ')' &&
                    line.text.find(';') == last && lastWrite > 0)
                {
                    line.simpleKill = 1u << lastWrite;
                }
            }
        }

        // True when the line hands the context to other code, which may then
        // read or write any register through it.
        bool passesContext(const std::string &text)
        {
            if (text.find("PS2_CALL_DIRECT(") != std::string::npos || text.find("ctx->r[") != std::string::npos)
                return true;
            size_t pos = 0;
            while ((pos = text.find("ctx", pos)) != std::string::npos)
            {
                const bool startOk = pos == 0 || !isIdentChar(text[pos - 1]);
                size_t end = pos + 3;
                if (startOk && (end >= text.size() || !isIdentChar(text[end])))
                {
                    while (end < text.size() && text[end] == ' ')
                        ++end;
                    const char next = end < text.size() ? text[end] : '\0';
                    if (next == ',' || next == ')')
                    {
                        // "GPR_VEC(ctx, n)" / "SET_GPR_VEC(ctx, n" are handled as vector accesses.
                        const bool vecAccess = pos >= 8 && text.compare(pos - 8, 8, "GPR_VEC(") == 0;
                        if (!vecAccess)
                            return true;
                    }
                }
                pos += 3;
            }
            return false;
        }

        void scanControl(Line &line)
        {
            const std::string &t = line.text;
            const size_t first = t.find_first_not_of(" \t");
            if (first == std::string::npos)
                return;
            if (t.compare(first, 6, "label_") == 0)
            {
                const size_t colon = t.find(':', first);
                if (colon != std::string::npos && colon + 1 >= t.find_last_not_of(" \t") + 1)
                    line.label = t.substr(first, colon - first);
            }
            size_t pos = 0;
            while ((pos = t.find("goto label_", pos)) != std::string::npos)
            {
                const size_t start = pos + 5;
                size_t end = start;
                while (end < t.size() && isIdentChar(t[end]))
                    ++end;
                line.gotos.push_back(t.substr(start, end - start));
                pos = end;
            }
            size_t ret = 0;
            while ((ret = t.find("return;", ret)) != std::string::npos)
            {
                if (ret == 0 || !isIdentChar(t[ret - 1]))
                    line.hasReturn = true;
                ret += 7;
            }
        }

        std::string storeCode(Mask mask)
        {
            std::string code;
            for (int reg = 1; reg < 32; ++reg)
                if ((mask >> reg) & 1u)
                    code += "PS2_ST(" + std::to_string(reg) + "); ";
            return code;
        }

        std::string loadCode(Mask mask)
        {
            std::string code;
            for (int reg = 1; reg < 32; ++reg)
                if ((mask >> reg) & 1u)
                    code += "PS2_LD(" + std::to_string(reg) + "); ";
            return code;
        }

        std::string indentOf(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t");
            return text.substr(0, first == std::string::npos ? 0 : first);
        }
    }

    std::string applyRegisterLocals(const std::string &functionText)
    {
        // Split into lines; find the function body.
        std::vector<Line> lines;
        {
            size_t start = 0;
            while (start <= functionText.size())
            {
                const size_t end = functionText.find('\n', start);
                Line line;
                line.text = functionText.substr(start, end == std::string::npos ? std::string::npos : end - start);
                lines.push_back(std::move(line));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
        // The strict-return diagnostic alternative ("#if ... #else ... #endif"
        // around a return) is two versions of the same code; the analysis
        // treats lines as one sequence, so keep only the default version.
        {
            std::vector<Line> kept;
            kept.reserve(lines.size());
            bool skipping = false;
            int openDiagnostics = 0;
            for (Line &line : lines)
            {
                const size_t first = line.text.find_first_not_of(" \t");
                const std::string_view trimmed =
                    first == std::string::npos ? std::string_view{} : std::string_view(line.text).substr(first);
                if (trimmed.rfind("#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS)", 0) == 0)
                {
                    skipping = true;
                    ++openDiagnostics;
                    continue;
                }
                if (openDiagnostics > 0 && trimmed.rfind("#else", 0) == 0)
                {
                    skipping = false;
                    continue;
                }
                if (openDiagnostics > 0 && trimmed.rfind("#endif", 0) == 0)
                {
                    skipping = false;
                    --openDiagnostics;
                    continue;
                }
                if (!skipping)
                    kept.push_back(std::move(line));
            }
            lines = std::move(kept);
        }
        int declLine = -1;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].text.find("uint32_t ps2_cur_pc = ") != std::string::npos)
            {
                declLine = static_cast<int>(i);
                break;
            }
        }
        if (declLine < 0)
            return functionText;
        const int n = static_cast<int>(lines.size());
        const int bodyStart = declLine + 1;

        // The resume switch, if any: "switch (ctx->pc) {" ... "    }".
        int switchStart = -1, switchEnd = -1;
        for (int i = bodyStart; i < n; ++i)
        {
            const std::string &t = lines[i].text;
            if (t.find_first_not_of(" \t") == std::string::npos)
                continue;
            if (t.find("switch (ctx->pc) {") != std::string::npos)
            {
                switchStart = i;
                for (int j = i + 1; j < n; ++j)
                {
                    const size_t first = lines[j].text.find_first_not_of(" \t");
                    if (first != std::string::npos && lines[j].text[first] == '}')
                    {
                        switchEnd = j;
                        break;
                    }
                }
            }
            break;
        }
        if (switchStart >= 0 && switchEnd < 0)
            return functionText;
        const int codeStart = switchEnd >= 0 ? switchEnd + 1 : bodyStart;

        std::unordered_map<std::string, int> labelLine;
        bool inReturnBlock = false;
        for (int i = bodyStart; i < n; ++i)
        {
            Line &line = lines[i];
            const size_t first = line.text.find_first_not_of(" \t");
            const bool comment = first != std::string::npos && line.text.compare(first, 2, "//") == 0;
            const bool preprocessor = first != std::string::npos && line.text[first] == '#';
            if (comment || preprocessor)
                continue;
            scanControl(line);
            if (i > switchStart && i < switchEnd)
            {
                line.entryCase = !line.gotos.empty();
                continue;
            }
            if (i < codeStart)
                continue;
            rewriteLine(line);
            line.sync = passesContext(line.text);
            if (!line.label.empty())
                labelLine[line.label] = i;
            // "jr $ra": the emitter opens a block with the jump target taken
            // from register 31; returns inside that block are ABI returns.
            if (line.text.find("const uint32_t jumpTarget = PS2R_U32(31);") != std::string::npos)
                inReturnBlock = true;
            else if (line.text == "    }")
                inReturnBlock = false;
            if (inReturnBlock && line.hasReturn && !line.sync)
                line.abiReturn = true;
        }
        // Falling off the closing brace also leaves the function.
        for (int i = n - 1; i >= codeStart; --i)
        {
            if (lines[i].text == "}")
            {
                lines[i].functionEnd = true;
                break;
            }
        }
        const Mask preserved = trustCalleeSaved() ? kCalleeSaved : 0u;
        auto returnStoreMask = [&](const Line &line) -> Mask
        {
            const Mask dirty = line.dirtyIn | line.writes;
            return (line.abiReturn && trustCalleeSaved()) ? (dirty & (kCalleeSaved | kResults)) : dirty;
        };

        // Successors: the next line, plus goto targets.
        std::vector<std::vector<int>> succ(n);
        for (int i = codeStart; i < n; ++i)
        {
            if (i + 1 < n)
                succ[i].push_back(i + 1);
            for (const std::string &target : lines[i].gotos)
            {
                const auto found = labelLine.find(target);
                if (found == labelLine.end())
                    return functionText; // unknown label: leave the function untouched
                succ[i].push_back(found->second);
            }
        }

        // A block opened by a conditional may be skipped: add the edge from
        // its opening line to its closing line. Without it the analyses would
        // take assignments inside "if (...) { }" as always executed and leave
        // a register unloaded on the path that skips the block.
        {
            std::vector<int> open;
            for (int i = codeStart; i < n; ++i)
            {
                const std::string &t = lines[i].text;
                const size_t first = t.find_first_not_of(" \t");
                if (first == std::string::npos || t.compare(first, 2, "//") == 0)
                    continue;
                for (char c : t)
                {
                    if (c == '{')
                    {
                        open.push_back(i);
                    }
                    else if (c == '}' && !open.empty())
                    {
                        const int opener = open.back();
                        open.pop_back();
                        if (opener == i)
                            continue;
                        const std::string &o = lines[opener].text;
                        size_t start = o.find_first_not_of(" \t");
                        if (start != std::string::npos && o[start] == '}')
                            start = o.find_first_not_of(" \t", start + 1);
                        const std::string_view head =
                            start == std::string::npos ? std::string_view{} : std::string_view(o).substr(start);
                        const bool conditional = head.rfind("if", 0) == 0 || head.rfind("else", 0) == 0 ||
                                                 head.rfind("switch", 0) == 0 || head.rfind("for", 0) == 0 ||
                                                 head.rfind("while", 0) == 0;
                        if (conditional)
                            succ[opener].push_back(i);
                    }
                }
            }
        }

        // Where a sync line's reload goes: after the statement, or after the
        // block it opens ("if (!dispatch(...)) {" ... "}").
        for (int i = codeStart; i < n; ++i)
        {
            Line &line = lines[i];
            if (!line.sync)
                continue;
            const size_t last = line.text.find_last_not_of(" \t");
            if (last != std::string::npos && line.text[last] == '{')
            {
                int depth = 0;
                int close = -1;
                for (int j = i; j < n && close < 0; ++j)
                {
                    for (char c : lines[j].text)
                    {
                        if (c == '{')
                            ++depth;
                        else if (c == '}' && --depth == 0)
                        {
                            close = j;
                            break;
                        }
                    }
                }
                if (close < 0)
                    return functionText;
                line.reloadAt = close + 1;
            }
            else
            {
                line.reloadAt = i + 1;
            }
        }

        // may-dirty, forward.
        std::vector<Mask> dirtyOut(n, 0);
        auto transferDirty = [&](int i, Mask in) -> Mask
        {
            const Line &line = lines[i];
            if (line.sync)
                return 0; // stored before the line; its own writes are stored right after it
            Mask out = in & ~line.vecReads; // stored before the line
            out |= line.writes;
            out &= ~line.vecWrites; // reloaded from the context after the line
            return out;
        };
        for (bool changed = true; changed;)
        {
            changed = false;
            for (int i = codeStart; i < n; ++i)
            {
                const Mask out = transferDirty(i, lines[i].dirtyIn);
                if (out != dirtyOut[i])
                {
                    dirtyOut[i] = out;
                    changed = true;
                }
                for (int s : succ[i])
                {
                    const Mask merged = lines[s].dirtyIn | out;
                    if (merged != lines[s].dirtyIn)
                    {
                        lines[s].dirtyIn = merged;
                        changed = true;
                    }
                }
            }
        }

        // live, backward. Stores of may-dirty locals are uses.
        for (bool changed = true; changed;)
        {
            changed = false;
            for (int i = n - 1; i >= codeStart; --i)
            {
                Line &line = lines[i];
                Mask out = 0;
                for (int s : succ[i])
                    out |= lines[s].liveIn;
                Mask in;
                if (line.sync)
                {
                    // Before the line the dirty locals are stored and its own
                    // reads happen. Afterwards the caller-saved locals that are
                    // still needed are reloaded; callee-saved ones stay valid
                    // across the call, so they must be valid before it.
                    const Mask need = line.reloadAt < n ? lines[line.reloadAt].liveIn : 0u;
                    in = line.dirtyIn | line.reads | line.vecReads | (need & preserved & ~line.writes);
                }
                else
                {
                    in = (out & ~(line.simpleKill | line.vecWrites)) | line.reads | line.vecReads;
                    if (line.hasReturn)
                        in |= returnStoreMask(line);
                    if (line.functionEnd)
                        in |= line.dirtyIn;
                }
                if (in != line.liveIn || out != line.liveOut)
                {
                    line.liveIn = in;
                    line.liveOut = out;
                    changed = true;
                }
            }
        }

        // Emit.
        std::vector<std::string> before(n + 1), after(n + 1);
        for (int i = codeStart; i < n; ++i)
        {
            Line &line = lines[i];
            const std::string indent = indentOf(line.text);
            if (line.sync)
            {
                const std::string store = storeCode(line.dirtyIn);
                if (!store.empty())
                    before[i] += indent + store + "\n";
                // Registers the line itself assigned must reach the context
                // before the reload overwrites the locals.
                const std::string own = storeCode(line.writes);
                const int at = std::min(line.reloadAt, n);
                const Mask need = at < n ? lines[at].liveIn : 0;
                std::string reload = loadCode(need & ~line.writes & ~preserved);
                if (line.reloadAt == i + 1)
                {
                    if (!own.empty() || !reload.empty())
                        after[i] += indent + own + reload + "\n";
                }
                else
                {
                    if (!own.empty())
                        return functionText; // a block-opening call that also assigns: not expected
                    if (!reload.empty())
                        before[at] += indentOf(lines[i].text) + reload + "\n";
                }
            }
            else
            {
                const std::string vecStore = storeCode(line.vecReads);
                if (!vecStore.empty())
                    before[i] += indent + vecStore + "\n";
                const std::string vecLoad = loadCode(line.vecWrites);
                if (!vecLoad.empty())
                    after[i] += indent + vecLoad + "\n";
                if (line.functionEnd)
                {
                    const std::string store = storeCode(line.dirtyIn);
                    if (!store.empty())
                        before[i] += "    " + store + "\n";
                }
                if (line.hasReturn)
                {
                    const std::string store = storeCode(returnStoreMask(line));
                    if (!store.empty())
                    {
                        // Only whole-statement returns are expected outside sync lines.
                        std::string &t = line.text;
                        size_t pos = 0;
                        while ((pos = t.find("return;", pos)) != std::string::npos)
                        {
                            const std::string replacement = "{ " + store + "return; }";
                            t.replace(pos, 7, replacement);
                            pos += replacement.size();
                        }
                    }
                }
            }
        }
        // Entry points.
        for (int i = switchStart + 1; i < switchEnd; ++i)
        {
            Line &line = lines[i];
            if (!line.entryCase)
                continue;
            const auto found = labelLine.find(line.gotos.front());
            if (found == labelLine.end())
                return functionText;
            const std::string load = loadCode(lines[found->second].liveIn);
            const size_t pos = line.text.find("goto label_");
            line.text.insert(pos, load);
        }
        std::string startLoad;
        if (codeStart < n)
            startLoad = loadCode(lines[codeStart].liveIn);

        std::string result;
        result.reserve(functionText.size() + functionText.size() / 4);
        for (int i = 0; i < n; ++i)
        {
            result += before[i];
            result += lines[i].text;
            if (i + 1 < n)
                result += '\n';
            if (i == declLine)
                result += "    PS2_REG_LOCALS;\n";
            if (i == codeStart - 1 && !startLoad.empty())
                result += "    " + startLoad + "\n";
            result += after[i];
        }
        return result;
    }
}
