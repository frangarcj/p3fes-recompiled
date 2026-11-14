#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    struct IopImportCall
    {
        std::string library;
        uint16_t ordinal = 0;
        uint16_t version = 0;
        // Filled in by the emulator's PS2X_IOP_FAST path (which library family `library` names); -1 = not classified.
        int32_t libraryClass = -1;
    };

    class IopImportRegistry
    {
    public:
        explicit IopImportRegistry(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] std::optional<IopImportCall> decode(uint32_t pc) const;
        // PS2X_IOP_FAST: resolve() memoised until the set of registered export libraries changes.
        void setResolveCache(bool enabled) noexcept { m_resolveCacheEnabled = enabled; }
        [[nodiscard]] bool registerExportTable(uint32_t address);
        [[nodiscard]] bool releaseExportTable(uint32_t address);
        [[nodiscard]] uint32_t findTable(std::string_view library, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] uint32_t resolve(std::string_view library, uint16_t ordinal, std::optional<uint16_t> version = std::nullopt) const;
        [[nodiscard]] int32_t setRebootTimeLibraryHandlingMode(uint32_t address, uint32_t mode);
        void eraseRange(uint32_t base, uint32_t size);

    private:
        struct ExportLibrary
        {
            uint32_t tableAddress = 0;
            uint16_t version = 0;
            std::string name;
            std::vector<uint32_t> functions;
        };

        [[nodiscard]] const ExportLibrary *findLibrary(std::string_view name, std::optional<uint16_t> version) const;
        [[nodiscard]] uint32_t resolveUncached(std::string_view library, uint16_t ordinal, std::optional<uint16_t> version) const;

        struct ResolveCacheEntry
        {
            char name[8]{};
            uint8_t length = 0xFFu; // 0xFF = empty
            uint16_t ordinal = 0;
            int32_t version = -1;
            uint32_t target = 0;
            uint64_t generation = 0;
        };

        IopMemory &m_memory;
        std::map<uint32_t, ExportLibrary> m_libraries;
        bool m_resolveCacheEnabled = false;
        uint64_t m_generation = 1; // bumped by every change to m_libraries
        mutable ResolveCacheEntry m_resolveCache[256];
    };
}
