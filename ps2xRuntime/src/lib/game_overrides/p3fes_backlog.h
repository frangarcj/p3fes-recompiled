#pragma once

#include <string>
#include <vector>

class PS2Runtime;

namespace p3fes_backlog
{
    void install(PS2Runtime &runtime);
    struct Entry
    {
        std::string speaker, text;
        bool operator==(const Entry &) const = default;
    };
    bool dialogueActive();
    // Snapshot of displayed pages in chronological order. A page can contain newlines.
    std::vector<Entry> entries();
}
