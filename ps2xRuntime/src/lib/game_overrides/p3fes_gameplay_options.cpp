#include "p3fes_gameplay_options.h"
#include "p3fes_backlog.h"
#include "p3fes_native_movie.h"
#include "ps2_host_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct Option
    {
        const char *key;
        bool defaultValue;
    };
    constexpr Option options[] = {
        {"P3FES_DIRECT_PARTY", false},
        {"P3FES_FAST_FORWARD", false},
        {"P3FES_BACKLOG", true},
    };
    constexpr int optionCount = int(std::size(options));
    constexpr uint16_t selectButton = 0x0001, up = 0x0010, down = 0x0040;

    std::array<std::atomic<bool>, optionCount> values{};
    std::atomic<bool> installed{false}, menuOpen{false}, waitingForRelease{false};
    // The Vita direct GS presenter can draw on its worker thread.
    std::mutex readerMutex;
    uint16_t previous = 0;
    std::filesystem::path settingsDir;
    std::vector<p3fes_backlog::Entry> history;
    struct Line { std::string text; size_t entry; bool speaker; };
    std::vector<Line> lines;
    std::vector<int> entryStarts;
    size_t selected = 0;
    int firstLine = 0;
    int layoutWidth = 0, layoutFont = 0, visibleLines = 1;

    std::filesystem::path userSettingsDir()
    {
#if defined(PLATFORM_VITA)
        return "ux0:data/p3fes/gameplay";
#elif defined(_WIN32)
        if (const wchar_t *dir = _wgetenv(L"LOCALAPPDATA"); dir && *dir)
            return std::filesystem::path(dir) / L"P3FES" / L"gameplay";
#else
        const char *home = std::getenv("HOME");
#if defined(__APPLE__)
        if (home && *home)
            return std::filesystem::path(home) / "Library/Application Support/P3FES/gameplay";
#else
        if (const char *dir = std::getenv("XDG_CONFIG_HOME"); dir && *dir)
            return std::filesystem::path(dir) / "p3fes/gameplay";
        if (home && *home)
            return std::filesystem::path(home) / ".config/p3fes/gameplay";
#endif
#endif
        return {};
    }

    // Each override is a single byte. Updating one setting never rewrites another
    // instance's settings or turns its environment defaults into persistent values.
    bool saveValue(int index, bool enabled)
    {
        if (settingsDir.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(settingsDir, error);
        if (error)
            return false;

        const auto path = settingsDir / options[index].key;
        auto mode = std::ios::binary | std::ios::out;
        if (std::filesystem::exists(path, error))
            mode |= std::ios::in; // replace the single byte in place
        if (error)
            return false;
        std::ofstream file(path, mode);
        file.put(enabled ? '1' : '0');
        file.close();
        return static_cast<bool>(file);
    }

    void openHistory()
    {
        history = p3fes_backlog::entries();
        entryStarts.clear();
        selected = history.empty() ? 0 : history.size() - 1;
        firstLine = 0;
        layoutWidth = 0;
        menuOpen.store(true);
    }

    void closeMenu()
    {
        // Suppress the closing button until released, including in concurrent pad reads.
        waitingForRelease.store(true);
        menuOpen.store(false);
        history.clear();
        lines.clear();
    }

    void wrapHistory(int width, int font)
    {
        if (width == layoutWidth && font == layoutFont)
            return;
        layoutWidth = width;
        layoutFont = font;
        lines.clear();
        entryStarts.clear();
        for (size_t index = 0; index < history.size(); ++index)
        {
            const auto &entry = history[index];
            if (!lines.empty())
                lines.push_back({{}, index, false});
            entryStarts.push_back(static_cast<int>(lines.size()));
            if (!entry.speaker.empty())
                lines.push_back({entry.speaker, index, true});
            std::istringstream paragraphs(entry.text);
            std::string paragraph;
            while (std::getline(paragraphs, paragraph))
            {
                std::istringstream words(paragraph);
                std::string line, word;
                while (words >> word)
                {
                    if (!line.empty() && MeasureText((line + " " + word).c_str(), font) > width)
                    {
                        lines.push_back({std::move(line), index, false});
                        line.clear();
                    }
                    if (!line.empty())
                        line += ' ';
                    if (MeasureText(word.c_str(), font) <= width)
                    {
                        line += word;
                        continue;
                    }
                    // Names and unbroken strings may be wider than the screen.
                    for (size_t pos = 0; pos < word.size();)
                    {
                        size_t next = pos + 1;
                        while (next < word.size() && (static_cast<unsigned char>(word[next]) & 0xc0) == 0x80)
                            ++next;
                        const auto glyph = word.substr(pos, next - pos);
                        if (!line.empty() && MeasureText((line + glyph).c_str(), font) > width)
                        {
                            lines.push_back({std::move(line), index, false});
                            line.clear();
                        }
                        line += glyph;
                        pos = next;
                    }
                }
                lines.push_back({std::move(line), index, false});
            }
        }
        entryStarts.push_back(static_cast<int>(lines.size()));
    }

    bool canOpen()
    {
        if (!values[2].load() || !p3fes_backlog::dialogueActive()) return false;
        const auto movie = p3fes_native_movie::status();
        return movie != p3fes_native_movie::Status::Pending && movie != p3fes_native_movie::Status::Playing;
    }
}

extern "C" bool ps2xGameplayOption(const char *key)
{
    for (int i = 0; i < optionCount; ++i)
        if (key && std::strcmp(key, options[i].key) == 0)
            return values[i].load(std::memory_order_relaxed);
    return false;
}

namespace p3fes_gameplay_options
{
    void install()
    {
        settingsDir = userSettingsDir();
        for (int i = 0; i < optionCount; ++i)
        {
            const char *env = std::getenv(options[i].key);
            bool value = env ? std::strcmp(env, "1") == 0 : options[i].defaultValue;
            if (!settingsDir.empty())
            {
                std::ifstream file(settingsDir / options[i].key, std::ios::binary);
                const int saved = file.get();
                if (saved == '1' || saved == '0')
                    value = saved == '1';
            }
            values[i].store(value);
        }
        previous = 0;
        waitingForRelease.store(false);
        menuOpen.store(false);
        installed.store(true);
    }

    void shutdown()
    {
        installed.store(false);
        menuOpen.store(false);
        waitingForRelease.store(false);
    }

    Settings currentSettings()
    {
        Settings result{};
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = values[i].load();
        return result;
    }

    bool applySettings(const Settings &settings)
    {
        bool saved = true;
        for (size_t i = 0; i < settings.size(); ++i)
        {
            if (values[i].load() == settings[i])
                continue;
            if (!saveValue(static_cast<int>(i), settings[i]))
            {
                std::fprintf(stderr, "[gameplay] Could not save %s.\n", options[i].key);
                saved = false;
                continue;
            }
            values[i].store(settings[i]);
        }
        return saved;
    }

    bool sceneForwardEnabled() { return installed.load(std::memory_order_relaxed) && values[1].load(std::memory_order_relaxed); }

    bool visible() { return menuOpen.load(); }

    bool consumesInput(uint16_t pressed)
    {
        return installed.load() && (menuOpen.load() || waitingForRelease.load() ||
               ((pressed & selectButton) && canOpen()));
    }

    void handleInput(uint16_t pressed)
    {
        if (!installed.load())
            return;
        const uint16_t edge = pressed & ~previous;
        previous = pressed;
        if (waitingForRelease.load())
        {
            if (!pressed)
                waitingForRelease.store(false);
            return;
        }
        if (!menuOpen.load())
        {
            if ((edge & selectButton) && canOpen())
            {
                std::lock_guard lock(readerMutex);
                openHistory();
            }
            return;
        }
        std::lock_guard lock(readerMutex);
        if ((edge & selectButton) || !canOpen())
            closeMenu();
        else if (!entryStarts.empty() && !history.empty())
        {
            const int lastOffset = std::max(0, entryStarts[selected + 1] - entryStarts[selected] - visibleLines);
            if (edge & up)
            {
                if (firstLine > 0) firstLine = std::max(0, firstLine - visibleLines);
                else if (selected > 0) --selected;
            }
            else if (edge & down)
            {
                if (firstLine < lastOffset) firstLine = std::min(lastOffset, firstLine + visibleLines);
                else if (selected + 1 < history.size()) { ++selected; firstLine = 0; }
            }
        }
    }

    void drawHistory()
    {
        std::lock_guard lock(readerMutex);
        if (!visible()) return;
        const int width = GetScreenWidth(), height = GetScreenHeight();
        const int font = std::clamp(height / 25, 10, 32);
        const int margin = std::max(font, width / 20), row = font + font / 3;
        const int footerY = height - 2 * font;
        wrapHistory(width - margin * 2, font);
        visibleLines = std::max(1, (footerY - 2 * font) / row);
        DrawRectangle(0, 0, width, height, Color{0, 0, 0, 192});
        if (!history.empty())
        {
            const int start = entryStarts[selected], end = entryStarts[selected + 1];
            const int top = start + firstLine - std::max(0, (visibleLines - (end - start)) / 2);
            for (int i = std::max(0, top); i < std::min(int(lines.size()), top + visibleLines); ++i)
            {
                const auto &line = lines[i];
                const bool current = line.entry == selected;
                const Color color = line.speaker ? (current ? Color{145, 223, 248, 255} : Color{94, 153, 171, 255})
                                                 : (current ? WHITE : Color{169, 173, 180, 255});
                DrawText(line.text.c_str(), margin, font + (i - top) * row, font, color);
            }
        }
        const char *help = "Up/Down  Scroll     SELECT  Back";
        const int small = std::max(10, font * 3 / 4);
        DrawText(help, width - margin - MeasureText(help, small), footerY, small, WHITE);
    }
}
