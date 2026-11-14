#include "p3fes_native_movie.h"

#if defined(PLATFORM_VITA)
#include <raylib.h>
#include <vitaGL.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void ps2x_vita_pause_game_audio(int paused);
std::recursive_mutex &ps2xVitaGlMutex();

namespace p3fes_native_movie
{
namespace
{
constexpr unsigned kBuffers = 5;
std::atomic<Status> g_status{Status::Idle};
std::atomic<bool> g_cancel{false}, g_audioRunning{false};
std::mutex g_requestMutex, g_decoderMutex;
std::mutex g_frameMutex;
std::unordered_map<void *, SceUID> g_frameBlocks;
unsigned g_frameAllocations = 0, g_frameFrees = 0;
std::string g_path;
SceAvPlayerHandle g_player = 0;
std::thread g_audioThread;
std::array<GLuint, kBuffers> g_textures{};
std::array<SceGxmTexture *, kBuffers> g_descriptors{};
std::array<void *, kBuffers> g_ownedPixels{};
unsigned g_textureIndex = 0;
uint64_t g_started = 0, g_firstFrame = 0, g_draws = 0;
uint32_t g_previousButtons = 0;

void *allocate(void *, uint32_t alignment, uint32_t size)
{
    return size ? memalign(std::max(8u, alignment), size) : nullptr;
}
void deallocate(void *, void *memory) { std::free(memory); }

// AVPlayer's decoder needs physically contiguous frame memory. The host shim
// leaves a separate reserve for it instead of giving all PHYCONT to vitaGL.
void *allocateFrame(void *, uint32_t requestedAlignment, uint32_t size)
{
    constexpr uint32_t alignment = 1024u * 1024u;
    if (!size || size > 16u * alignment || requestedAlignment > alignment)
    {
        std::fprintf(stderr, "[native-movie] frame alloc rejected size=%u align=%u\n", size, requestedAlignment);
        return nullptr;
    }
    const uint32_t bytes = (size + alignment - 1u) & ~(alignment - 1u);
    const SceUID block = sceKernelAllocMemBlock("p3fes-video", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, bytes, nullptr);
    if (block < 0)
    {
        std::fprintf(stderr, "[native-movie] frame alloc failed size=%u align=%u bytes=%u error=0x%08X\n",
                     size, requestedAlignment, bytes, static_cast<unsigned>(block));
        return nullptr;
    }
    void *memory = nullptr;
    int mapped = 0;
    if (sceKernelGetMemBlockBase(block, &memory) < 0 ||
        (mapped = sceGxmMapMemory(memory, bytes, SCE_GXM_MEMORY_ATTRIB_RW)) < 0)
    {
        std::fprintf(stderr, "[native-movie] frame map failed size=%u bytes=%u error=0x%08X\n",
                     size, bytes, static_cast<unsigned>(mapped));
        sceKernelFreeMemBlock(block);
        return nullptr;
    }
    std::fprintf(stderr, "[native-movie] frame alloc size=%u align=%u bytes=%u\n", size, requestedAlignment, bytes);
    { std::lock_guard lock(g_frameMutex); g_frameBlocks.emplace(memory, block); ++g_frameAllocations; }
    return memory;
}
void deallocateFrame(void *, void *memory)
{
    if (!memory)
        return;
    SceUID block;
    {
        std::lock_guard lock(g_frameMutex);
        const auto found = g_frameBlocks.find(memory);
        if (found == g_frameBlocks.end())
            return;
        block = found->second;
        g_frameBlocks.erase(found);
        ++g_frameFrees;
    }
    sceGxmUnmapMemory(memory);
    sceKernelFreeMemBlock(block);
}

void audioLoop()
{
    int port = -1, configuredFrames = 0, configuredRate = 0, configuredChannels = 0;
    bool attemptedPort = false;
    uint64_t deadline = 0, blocks = 0;
    while (g_audioRunning.load(std::memory_order_acquire))
    {
        SceAvPlayerFrameInfo frame{};
        bool got = false;
        {
            std::lock_guard lock(g_decoderMutex);
            got = sceAvPlayerIsActive(g_player) && sceAvPlayerGetAudioData(g_player, &frame);
        }
        if (!got || !frame.pData)
        {
            sceKernelDelayThread(1000);
            continue;
        }
        const auto &audio = frame.details.audio;
        if ((audio.channelCount != 1 && audio.channelCount != 2) ||
            !audio.sampleRate || !audio.size || audio.size % (2u * audio.channelCount))
            continue;
        const unsigned frames = audio.size / (2u * audio.channelCount);
        if (!frames || frames > 16384u || audio.sampleRate > 48000u)
            continue;
        const unsigned outputFrames = (frames + 63u) & ~63u;
        std::vector<int16_t> padded;
        const void *pcm = frame.pData;
        if (outputFrames != frames)
        {
            padded.resize(outputFrames * audio.channelCount);
            std::memcpy(padded.data(), frame.pData, audio.size);
            pcm = padded.data();
        }
        if (!attemptedPort)
        {
            attemptedPort = true;
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, outputFrames, audio.sampleRate,
                                      audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            std::fprintf(stderr, "[native-movie] audio port=%d rate=%u channels=%u samples=%u\n",
                         port, audio.sampleRate, audio.channelCount, frames);
        }
        ++blocks;
        if (port >= 0)
        {
            if (configuredFrames != static_cast<int>(outputFrames) || configuredRate != static_cast<int>(audio.sampleRate) ||
                configuredChannels != audio.channelCount)
            {
                sceAudioOutSetConfig(port, outputFrames, audio.sampleRate,
                                     audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
                configuredFrames = outputFrames; configuredRate = audio.sampleRate; configuredChannels = audio.channelCount;
            }
            if (sceAudioOutOutput(port, pcm) >= 0)
                continue;
        }
        // Keep draining at the original sample rate even without an audio port.
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (!deadline || now > deadline + 100000u)
            deadline = now;
        deadline += static_cast<uint64_t>(frames) * 1000000u / audio.sampleRate;
        if (deadline > now)
            sceKernelDelayThread(static_cast<unsigned>(deadline - now));
    }
    if (port >= 0)
    {
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
    }
    std::fprintf(stderr, "[native-movie] audio blocks=%llu\n", static_cast<unsigned long long>(blocks));
}

void closePlayer(Status result)
{
    g_audioRunning.store(false, std::memory_order_release);
    if (g_audioThread.joinable())
        g_audioThread.join();
    std::lock_guard glLock(ps2xVitaGlMutex());
    glFinish();
    // Restore vitaGL-owned storage before it deletes the texture descriptors;
    // AVPlayer owns the borrowed NV12 frames and frees them in Close.
    for (unsigned i = 0; i < kBuffers; ++i)
        if (g_descriptors[i] && g_ownedPixels[i])
            sceGxmTextureInitLinear(g_descriptors[i], g_ownedPixels[i], SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 8, 8, 0);
    glDeleteTextures(kBuffers, g_textures.data());
    g_textures = {}; g_descriptors = {}; g_ownedPixels = {};
    if (g_player)
    {
        sceAvPlayerStop(g_player);
        sceAvPlayerClose(g_player);
        g_player = 0;
    }
    // Some AVPlayer implementations omit texture deallocation on early stop.
    // Close has ended all decoder access, and glFinish above ended GPU access.
    // Only allocations not already returned through the callback remain here.
    std::vector<void *> remaining;
    {
        std::lock_guard lock(g_frameMutex);
        for (const auto &[memory, block] : g_frameBlocks)
            remaining.push_back(memory);
    }
    for (void *memory : remaining)
        deallocateFrame(nullptr, memory);
    if (!remaining.empty())
        std::fprintf(stderr, "[native-movie] reclaimed frame allocations=%u\n", static_cast<unsigned>(remaining.size()));
    {
        std::lock_guard lock(g_frameMutex);
        std::fprintf(stderr, "[native-movie] frame memory allocated=%u freed=%u outstanding=%u\n",
                     g_frameAllocations, g_frameFrees, static_cast<unsigned>(g_frameBlocks.size()));
    }
    ps2x_vita_pause_game_audio(0);
    const double elapsed = (sceKernelGetProcessTimeWide() - g_started) / 1000000.0;
    std::fprintf(stderr, "[native-movie] closed status=%d seconds=%.3f draws=%llu\n", static_cast<int>(result), elapsed,
                 static_cast<unsigned long long>(g_draws));
    g_status.store(result, std::memory_order_release);
}

void playerEvent(void *, int32_t event, int32_t source, void *)
{
    std::fprintf(stderr, "[native-movie] event=0x%X source=%d\n", static_cast<unsigned>(event), source);
}

bool openPlayer()
{
    const int loaded = sceSysmoduleIsLoaded(SCE_SYSMODULE_AVPLAYER);
    if (loaded < 0 && sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0)
        return false;
    SceAvPlayerInitData init{};
    init.memoryReplacement.allocate = allocate;
    init.memoryReplacement.deallocate = deallocate;
    init.memoryReplacement.allocateTexture = allocateFrame;
    init.memoryReplacement.deallocateTexture = deallocateFrame;
    init.eventReplacement.eventCallback = playerEvent;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = kBuffers;
    init.autoStart = SCE_TRUE;
    g_player = sceAvPlayerInit(&init);
    if (!g_player || (static_cast<uint32_t>(g_player) & 0xFFFF0000u) == 0x806A0000u)
    {
        g_player = 0;
        return false;
    }
    {
        std::lock_guard glLock(ps2xVitaGlMutex());
        glGenTextures(kBuffers, g_textures.data());
        for (unsigned i = 0; i < kBuffers; ++i)
        {
            glBindTexture(GL_TEXTURE_2D, g_textures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g_descriptors[i] = vglGetGxmTexture(GL_TEXTURE_2D);
            g_ownedPixels[i] = vglGetTexDataPointer(GL_TEXTURE_2D);
            if (!g_descriptors[i] || !g_ownedPixels[i])
                return false;
        }
    }
    std::string path;
    { std::lock_guard lock(g_requestMutex); path = g_path; }
    const int added = sceAvPlayerAddSource(g_player, path.c_str());
    std::fprintf(stderr, "[native-movie] source=%s add=0x%08X\n", path.c_str(), added);
    if (added < 0)
        return false;
    g_textureIndex = 0; g_firstFrame = 0; g_draws = 0;
    g_started = sceKernelGetProcessTimeWide();
    SceCtrlData pad{};
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    g_previousButtons = pad.buttons;
    ps2x_vita_pause_game_audio(1);
    g_audioRunning.store(true, std::memory_order_release);
    g_audioThread = std::thread(audioLoop);
    g_status.store(Status::Playing, std::memory_order_release);
    return true;
}
}

bool request(const std::string &path)
{
    if (g_status.load(std::memory_order_acquire) == Status::Pending ||
        g_status.load(std::memory_order_acquire) == Status::Playing)
        return false;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fclose(file);
    { std::lock_guard lock(g_requestMutex); g_path = path; }
    g_cancel.store(false, std::memory_order_release);
    g_status.store(Status::Pending, std::memory_order_release);
    return true;
}
Status status() { return g_status.load(std::memory_order_acquire); }
void cancel() { g_cancel.store(true, std::memory_order_release); }
void reset() { if (status() == Status::Finished || status() == Status::Failed) g_status.store(Status::Idle); }

bool renderHostFrame()
{
    if (status() == Status::Pending && !openPlayer())
    {
        closePlayer(Status::Failed);
        return true;
    }
    if (status() != Status::Playing)
        return false;
    SceCtrlData pad{};
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    const uint32_t pressed = pad.buttons & ~g_previousButtons;
    g_previousButtons = pad.buttons;
    if (g_cancel.load(std::memory_order_acquire) ||
        (pressed & (SCE_CTRL_START | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)))
    {
        std::fprintf(stderr, "[native-movie] skipped cancel=%u buttons=0x%X\n",
                     g_cancel.load(std::memory_order_relaxed) ? 1u : 0u, pressed);
        closePlayer(Status::Finished);
        return true;
    }
    SceAvPlayerFrameInfo frame{};
    bool active, got;
    {
        std::lock_guard lock(g_decoderMutex);
        active = sceAvPlayerIsActive(g_player);
        got = active && sceAvPlayerGetVideoData(g_player, &frame);
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    // Optional harness aid for repeated renderer checks. Disabled in normal
    // playback and full boot benchmarks; it exercises the ordinary skip/EOF
    // handoff without waiting through the same opening on every iteration.
    static const double testSkipSeconds = [] {
        const char *value = std::getenv("P3FES_MOVIE_TEST_SKIP_SECONDS");
        return value ? std::clamp(std::atof(value), 0.0, 60.0) : 0.0;
    }();
    if (g_firstFrame && testSkipSeconds > 0.0 &&
        now - g_firstFrame >= static_cast<uint64_t>(testSkipSeconds * 1000000.0))
    {
        std::fprintf(stderr, "[native-movie] test harness skip\n");
        closePlayer(Status::Finished);
        return true;
    }
    if (!active && g_firstFrame)
    {
        closePlayer(Status::Finished);
        return true;
    }
    if ((!g_firstFrame && now - g_started > 10000000u) ||
        (g_firstFrame && now - g_firstFrame > 300000000u))
    {
        closePlayer(Status::Failed);
        return true;
    }
    if (!got || !frame.pData)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }
    const unsigned width = frame.details.video.width, height = frame.details.video.height;
    if (!width || !height || width > 1920 || height > 1088 || (width & 1u) || (height & 1u))
    {
        closePlayer(Status::Failed);
        return true;
    }
    BeginDrawing();
    ClearBackground(BLACK);
    g_textureIndex = (g_textureIndex + 1u) % kBuffers;
    sceGxmTextureInitLinear(g_descriptors[g_textureIndex], frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, width, height, 0);
    sceGxmTextureSetMinFilter(g_descriptors[g_textureIndex], SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(g_descriptors[g_textureIndex], SCE_GXM_TEXTURE_FILTER_LINEAR);
    glBindTexture(GL_TEXTURE_2D, g_textures[g_textureIndex]);
    glEnable(GL_TEXTURE_2D); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_FOG);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    const float scale = std::min(960.0f / width, 544.0f / height);
    const float w = width * scale, h = height * scale, x = (960 - w) * 0.5f, y = (544 - h) * 0.5f;
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + w, y);
    glTexCoord2f(0, 1); glVertex2f(x, y + h);
    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
    glEnd();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
    EndDrawing();
    ++g_draws;
    if (!g_firstFrame)
    {
        g_firstFrame = now;
        std::fprintf(stderr, "[native-movie] first frame %ux%u timestamp=%llu\n", width, height,
                     static_cast<unsigned long long>(frame.timeStamp));
    }
    return true;
}
void shutdownHost()
{
    if (g_player || g_audioThread.joinable()) closePlayer(Status::Finished);
}
}
#else
namespace p3fes_native_movie
{
bool request(const std::string &) { return false; }
Status status() { return Status::Idle; }
void cancel() {}
void reset() {}
bool renderHostFrame() { return false; }
void shutdownHost() {}
}
#endif
