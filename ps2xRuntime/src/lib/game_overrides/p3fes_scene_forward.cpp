#include "p3fes_scene_forward.h"
#include "p3fes_gameplay_options.h"
#include "p3fes_native_movie.h"
#include "runtime/ps2_memory.h"
#include <cstring>

namespace
{
    template<class T>
    T read(const uint8_t *ram, uint32_t address)
    {
        T value;
        std::memcpy(&value, ram + address, sizeof(value));
        return value;
    }

    bool playingScene(const uint8_t *ram)
    {
        // SLUS-21621: the same three kwln lists that kwlnTaskGetTaskByName traverses.
        constexpr uint32_t gp = 0x7D2CF0;
        int sequence = -1;
        bool fieldConversation = false;
        bool timeline = false;
        bool movie = false;
        for (const auto offset : {0x4C8C, 0x4C80, 0x4C74})
        {
            auto task = read<uint32_t>(ram, gp - offset);
            for (unsigned i = 0; task && task <= PS2_RAM_SIZE - 0x44 && i < 256; ++i)
            {
                if (std::memcmp(ram + task, "H_CampDraw", 11) == 0) return false;
                const auto work = read<uint32_t>(ram, task + 0x3C);
                if (work && work <= PS2_RAM_SIZE - 0x200)
                {
                    if (std::memcmp(ram + task, "admini", 7) == 0)
                        sequence = read<int8_t>(ram, work + 8); // adminiGetNowSeqId
                    else if (std::memcmp(ram + task, "field event", 12) == 0)
                        fieldConversation = read<int>(ram, work) == 22; // running the NPC interaction
                    else if (std::memcmp(ram + task, "(ZikkiPlayMode)EventVie", 23) == 0)
                        timeline = true; // event playback task, also used inside a field
                    else if (std::memcmp(ram + task, "H_SfdPlay", 10) == 0)
                    {
                        const auto phase = read<int16_t>(ram, work + 484);
                        movie = phase == 3 || phase == 4;
                    }
                }
                task = read<uint32_t>(ram, task + 0x40);
            }
        }
        if (movie) return true;
        if (sequence != 2 && sequence != 3 && sequence != 5) return false;
        bool dialogue = false;
        // CalcMsgMng uses the same list. Selection windows use +0x54, ordinary
        // messages use +0x34. The event clock waits while a selection is open.
        auto node = read<uint32_t>(ram, 0x959ED0);
        for (unsigned i = 0; node && node <= PS2_RAM_SIZE - 16 && i < 64; ++i)
        {
            const auto window = read<uint32_t>(ram, node + 12);
            if (window && window <= PS2_RAM_SIZE - 0x56)
            {
                const auto choice = read<int16_t>(ram, window + 0x54);
                if (choice >= 1 && choice <= 4) return false;
                dialogue |= read<int8_t>(ram, window + 0x34) == 1;
            }
            node = read<uint32_t>(ram, node + 4);
        }
        return sequence == 3 || timeline || fieldConversation || dialogue;
    }
}

bool p3fes_scene_forward::sample(const uint8_t *ram)
{
    const auto movie = p3fes_native_movie::status();
    const bool forwarding = p3fes_gameplay_options::sceneForwardEnabled() && ram &&
        (read<uint16_t>(ram, 0x7E094C) & 0x10) && !p3fes_gameplay_options::visible() &&
        movie != p3fes_native_movie::Status::Pending && movie != p3fes_native_movie::Status::Playing && playingScene(ram);
    active.store(forwarding, std::memory_order_relaxed);
    return forwarding;
}
