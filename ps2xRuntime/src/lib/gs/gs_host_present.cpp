// host context, frame selection and raylib drawing used by the port.
#include "runtime/gs/gs_host_present.h"
#include "../game_overrides/p3fes_native_present.h"
#include "../game_overrides/p3fes_native_movie.h"
#include "runtime/gs/gs_direct_frame.h"
#include "runtime/gs/gs_host_present_diag.h" // window without extra GPU reads.
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_pcsx2.h"
#include "runtime/ps2_perf.h"
#include "ps2_log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#if defined(__APPLE__) && !defined(PLATFORM_VITA)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#endif

#if !defined(PLATFORM_VITA)
#include <rlgl.h>
#include <string>
#include <vector>
#endif

#if !defined(PLATFORM_VITA)
extern uint64_t g_path3TraceVsync; // the current vsync count (ps2_memory.cpp)
#endif

namespace
{
    // PS2X_GS_RENDERER=pcsx2: the field PCSX2 presented, read back and shown as is.
    struct Pcsx2View
    {
        Texture2D texture{};
        std::vector<uint8_t> rgba;
        uint32_t width = 0u, height = 0u;
        uint64_t serial = 0u;
    } s_pcsx2;

    void releasePcsx2View()
    {
        if (s_pcsx2.texture.id != 0u)
            UnloadTexture(s_pcsx2.texture);
        s_pcsx2 = {};
    }

    constexpr uint32_t kWidth = 640u;
    constexpr uint32_t kHeight = 512u;
    constexpr uint32_t kDisplayHeight = 448u;

    uint64_t steadyNowNs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

#if !defined(PLATFORM_VITA)
    // Desktop reference capture, same settings and file format as the native path
    // (p3fes_native_present.cpp frameDiagFor/capturePath): P3FES_FRAME_DIAG=<s>[,<s>...] seconds after the
    // first presented (non-blank) frame, written as $P3FES_CAPTURE_DIR/frame-<s>.p3fb ("P3FB", u32 width,
    // u32 height, RGBA8 rows top first). It reads the window (the GS picture stretched to it), so its size
    // equals the native path's for the same window.
    struct GsFrameDiag
    {
        std::vector<double> times;
        size_t next = 0u;
        bool parsed = false;
        bool started = false;
        std::chrono::steady_clock::time_point start{};
        uint64_t startVsync = 0u;
        bool fields = false;
    } s_frameDiag;

    // Returns the label (whole seconds) when this frame is to be captured, or -1.
    int gsFrameDiagDue(bool blank)
    {
        GsFrameDiag &diag = s_frameDiag;
        if (!diag.parsed)
        {
            diag.parsed = true;
            const char *value = std::getenv("P3FES_FRAME_DIAG");
            for (const char *cursor = value ? value : ""; *cursor;)
            {
                char *end = nullptr;
                const double seconds = std::strtod(cursor, &end);
                if (end == cursor)
                    break;
                diag.times.push_back(seconds);
                cursor = *end == ',' ? end + 1 : end;
            }
            // P3FES_FRAME_DIAG_CLOCK=fields (GS path only): count emulated time (vsyncs / 59.94) instead of wall
            // time, since this path can run at a fraction of real time. Matches PS2X_PAD_SCRIPT_CLOCK=fields.
            const char *clock = std::getenv("P3FES_FRAME_DIAG_CLOCK");
            diag.fields = clock && std::strcmp(clock, "fields") == 0;
        }
        if (diag.next >= diag.times.size() || blank)
            return -1;
        const auto now = std::chrono::steady_clock::now();
        if (!diag.started)
        {
            diag.started = true;
            diag.start = now;
            diag.startVsync = g_path3TraceVsync;
            if (diag.fields)
                std::fprintf(stderr, "[gs-present] first frame at emulated t=%.2f s\n", g_path3TraceVsync * 1001.0 / 60000.0);
        }
        const double elapsed = diag.fields ? static_cast<double>(g_path3TraceVsync - diag.startVsync) * 1001.0 / 60000.0
                                           : std::chrono::duration<double>(now - diag.start).count();
        if (elapsed < diag.times[diag.next])
            return -1;
        {
            // Fractional times (71.25) are labelled t*100 (7125) so sub-second series do not overwrite each other.
            const double t = diag.times[diag.next++];
            return t == std::floor(t) ? static_cast<int>(t) : static_cast<int>(t * 100.0 + 0.5);
        }
    }

    void gsCaptureWindow(int label)
    {
        rlDrawRenderBatchActive();
        const int width = std::max(GetRenderWidth(), 1), height = std::max(GetRenderHeight(), 1);
        unsigned char *pixels = rlReadScreenPixels(width, height);
        if (!pixels)
            return;
        const char *directory = std::getenv("P3FES_CAPTURE_DIR");
        char path[512];
        std::snprintf(path, sizeof(path), "%s%sframe-%d.p3fb", directory ? directory : "", directory ? "/" : "", label);
        if (FILE *file = std::fopen(path, "wb"))
        {
            const uint32_t header[2] = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            std::fwrite("P3FB", 1, 4, file);
            std::fwrite(header, sizeof(uint32_t), 2, file);
            std::fwrite(pixels, 4u, static_cast<size_t>(width) * height, file);
            std::fclose(file);
            std::fprintf(stderr, "[gs-present] captured %s (%dx%d)\n", path, width, height);
        }
        RL_FREE(pixels);
    }
#endif

    void waitDirectFrame(uint64_t fence)
    {
#if defined(__APPLE__) && !defined(PLATFORM_VITA)
        if (fence != 0u)
            glWaitSync(reinterpret_cast<GLsync>(static_cast<uintptr_t>(fence)), 0u, GL_TIMEOUT_IGNORED);
#else
        (void)fence;
#endif
    }
}

void GSHostPresentation::captureHostContext()
{
    if (started)
        return;
    started = true;
    const char *setting = std::getenv("PS2X_GS_DIRECT_PRESENT");
    captureRequested = setting && *setting == '1';
    if (captureRequested)
        GSSharedPresent::CaptureHostContext();
}

void GSHostPresentation::restoreHostContext()
{
    if (captureRequested)
        GSSharedPresent::RestoreHostContext();
}

void GSHostPresentation::presentDirectTexture(uint32_t texture, uint32_t width, uint32_t height)
{
#if defined(PLATFORM_VITA)
    if (texture == 0u || width == 0u || height == 0u)
    {
        workerDirectWidth.store(0u, std::memory_order_relaxed);
        workerDirectHeight.store(0u, std::memory_order_relaxed);
        workerDirectLastNs.store(0u, std::memory_order_relaxed);
        workerDirectActive.store(false, std::memory_order_release);
        return;
    }

    // Reused from Celosia's Vita presentation model: the GS worker already
    // owns vitaGL here, so present the GPU texture before handing GL back to
    // the host loop. The high bit tells the Vita raylib shim this is a raw GL
    // texture name rather than one of its managed textures.
    BeginDrawing();
    ClearBackground(BLACK);
    const float sourceWidth = static_cast<float>(width);
    const float sourceHeight = static_cast<float>(height);
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    const float aspect = sourceWidth / sourceHeight;
    const float destinationHeight = std::min(screenHeight, screenWidth / aspect);
    const float destinationWidth = destinationHeight * aspect;
    const Rectangle source{0.0f, 0.0f, sourceWidth, sourceHeight};
    const Rectangle destination{(screenWidth - destinationWidth) * 0.5f,
                                (screenHeight - destinationHeight) * 0.5f,
                                destinationWidth, destinationHeight};
    DrawTexturePro(Texture2D{texture | 0x80000000u, static_cast<int>(width), static_cast<int>(height),
                             1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8},
                   source, destination, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    EndDrawing();
    workerDirectWidth.store(width, std::memory_order_relaxed);
    workerDirectHeight.store(height, std::memory_order_relaxed);
    workerDirectLastNs.store(steadyNowNs(), std::memory_order_relaxed);
    workerDirectActive.store(true, std::memory_order_release);
#else
    (void)texture;
    (void)width;
    (void)height;
#endif
}

void GSHostPresentation::createFallback()
{
    if (fallback.id != 0u)
        return;
    Image blank = GenImageColor(kWidth, kHeight, BLANK);
    fallback = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(fallback, TEXTURE_FILTER_POINT); // Matches the original RAM path.
    upload.resize(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
}

void GSHostPresentation::blankFallback()
{
    if (!uploadedBlank)
    {
        for (size_t offset = 0u; offset < upload.size(); offset += 4u)
        {
            upload[offset] = MAGENTA.r;
            upload[offset + 1u] = MAGENTA.g;
            upload[offset + 2u] = MAGENTA.b;
            upload[offset + 3u] = MAGENTA.a;
        }
        UpdateTexture(fallback, upload.data());
        uploadedBlank = true;
    }
    current.texture = fallback;
    current.width = kWidth;
    current.height = kDisplayHeight;
    current.borrowed = false;
    current.blank = true;
    uploaded = true;
}

GSHostPresentation::View GSHostPresentation::prepare(GS &gs, uint64_t vsyncTick)
{
    createFallback();
    if (ps2_pcsx2gs::Active())
    {
        uint32_t w = 0u, h = 0u;
        if (ps2_pcsx2gs::CopyLatestFrame(s_pcsx2.rgba, w, h, s_pcsx2.serial))
        {
            if (s_pcsx2.texture.id == 0u || w != s_pcsx2.width || h != s_pcsx2.height)
            {
                if (s_pcsx2.texture.id != 0u)
                    UnloadTexture(s_pcsx2.texture);
                Image image{s_pcsx2.rgba.data(), static_cast<int>(w), static_cast<int>(h), 1,
                            PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                s_pcsx2.texture = LoadTextureFromImage(image);
                SetTextureFilter(s_pcsx2.texture, TEXTURE_FILTER_BILINEAR);
                s_pcsx2.width = w;
                s_pcsx2.height = h;
            }
            else
            {
                UpdateTexture(s_pcsx2.texture, s_pcsx2.rgba.data());
            }
        }
        if (s_pcsx2.texture.id != 0u)
        {
            current = {};
            current.texture = s_pcsx2.texture;
            current.width = s_pcsx2.width;
            current.height = s_pcsx2.height;
            current.borrowed = true; // owned here, not by `fallback`
            current.blank = false;
            return current;
        }
        blankFallback();
        return current;
    }
    const bool shared = gs.usesSharedPresentation() && GSSharedPresent::Active();
    if (!modeKnown || shared != sharedMode)
    {
        std::fprintf(stderr, "[gs-present] host mode: %s\n", shared ? "shared texture (opt-in)" : "RAM frames");
        std::fflush(stderr);
        modeKnown = true;
        sharedMode = shared;
        hasLatched = uploaded = uploadedBlank = false;
        lastSharedSequence = 0u;
        current = {};
    }
    const bool needsLatch = !hasLatched || vsyncTick != lastTick;
    if (needsLatch)
    {
        gs.latchHostPresentationFrame();
        lastTick = vsyncTick;
        hasLatched = true;
    }
#if defined(PLATFORM_VITA)
    if (workerDirectActive.load(std::memory_order_acquire))
    {
        // Celosia gives the worker a grace window: if hardware presentation
        // stops for several seconds, let the host resume drawing rather than
        // leaving the last GPU frame frozen forever.
        constexpr uint64_t kDirectWorkerGraceNs = 3000000000ull;
        const uint64_t last = workerDirectLastNs.load(std::memory_order_relaxed);
        const uint64_t now = steadyNowNs();
        if (last != 0u && now - last <= kDirectWorkerGraceNs)
        {
        // The worker has already presented the GPU texture. Do not interpret
        // GSDirectFrame's tiny transport record as RGBA pixels or touch the
        // fallback texture; renderFrame() will only poll input for this tick.
            current = {};
            current.width = workerDirectWidth.load(std::memory_order_relaxed);
            current.height = workerDirectHeight.load(std::memory_order_relaxed);
            current.borrowed = true;
            current.blank = false;
            return current;
        }
        workerDirectWidth.store(0u, std::memory_order_relaxed);
        workerDirectHeight.store(0u, std::memory_order_relaxed);
        workerDirectLastNs.store(0u, std::memory_order_relaxed);
        workerDirectActive.store(false, std::memory_order_release);
    }
#endif
    current.shared = shared;
    if (shared)
    {
        // The previous call already finished EndDrawing, including the batch. Acquire may keep the previous slot; the
        // normal path downloads no pixels.
        GSSharedPresent::Frame frame;
        if (GSSharedPresent::Acquire(frame))
        {
            current.frame = frame;
            if (frame.sequence != lastSharedSequence)
            {
                // observe the acquired texture without another read or barrier.
                static const bool diagnostic = [] {
                    const char *v = std::getenv("PS2X_GS_PRESENT_DIAG");
                    return v && std::strcmp(v, "1") == 0;
                }();
                // shift the log without raising its quota or changing presentation.
                static const uint64_t diagnosticFromTick = GSHostPresentDiag::parseFromTick(
                    std::getenv("PS2X_GS_PRESENT_DIAG_FROM_TICK"));
                if (diagnostic && GSHostPresentDiag::consume(vsyncTick, diagnosticFromTick, diagnosticSharedFrames))
                {
                    // sourceFbp is DISPFB pages or TEX0 blocks depending on the acquired source.
                    std::fprintf(stderr, "[gs-present:shared] seq=%llu render=%llu hostTick=%llu frameTick=%llu displayFbp=%u sourceFbp=%u preferred=%u smode2=%llu width=%u height=%u hashValid=%u hash=%016llx blank=%u\n",
                        (unsigned long long)frame.sequence, (unsigned long long)frame.renderSequence,
                        (unsigned long long)vsyncTick, (unsigned long long)frame.vsyncTick,
                        frame.displayFbp, frame.sourceFbp, unsigned(frame.usedPreferred), (unsigned long long)frame.smode2,
                        frame.width, frame.height,
                        unsigned(frame.hashValid), (unsigned long long)frame.contentHash, unsigned(frame.blank));
                }
                gs.recordAcquiredSharedFrame(frame);
                lastSharedSequence = frame.sequence;
                // explicit separate capture; uses this same reservation/acquired texture.
                acquiredCapture.capture(frame, vsyncTick);
            }
            if (!frame.blank && frame.texture != 0u && frame.width != 0u && frame.height != 0u)
            {
                current.texture = Texture2D{frame.texture, static_cast<int>(frame.width), static_cast<int>(frame.height),
                                            1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                current.width = frame.width;
                current.height = frame.height;
                current.borrowed = true;
                current.blank = false;
                return current;
            }
        }
        blankFallback(); // PMODE apagado conserva el placeholder magenta de RAM.
        return current;
    }
    if (!needsLatch && uploaded)
        return current;

    scratch.clear();
    uint32_t width = 0u, height = 0u, displayFbp = 0u, sourceFbp = 0u;
    bool preferred = false;
    if (!gs.copyLatchedHostPresentationFrame(scratch, width, height, &displayFbp, &sourceFbp, &preferred))
    {
        blankFallback();
        lastWidth = current.width;
        lastHeight = current.height;
        return current;
    }
    PS2_IF_AGRESSIVE_LOGS({
        if (uploadDebugCount < 128u || displayFbp != lastDisplayFbp || sourceFbp != lastSourceFbp ||
            preferred != lastPreferred || width != lastWidth || height != lastHeight)
            std::cout << "[frame:upload] idx=" << uploadDebugCount << " tick=" << vsyncTick
                      << " displayFbp=" << displayFbp << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(preferred ? 1u : 0u) << std::endl;
        ++uploadDebugCount;
    });
    lastDisplayFbp = displayFbp;
    lastSourceFbp = sourceFbp;
    lastPreferred = preferred;
    lastWidth = width;
    lastHeight = height;

    GSDirectFrame direct{};
    if (GSDirectFrame::load(scratch, direct))
    {
        waitDirectFrame(direct.fence);
        current = {};
#if defined(PLATFORM_VITA)
        const uint32_t directTextureId = direct.texture | 0x80000000u;
#else
        const uint32_t directTextureId = direct.texture;
#endif
        current.texture = Texture2D{directTextureId, static_cast<int>(width), static_cast<int>(height),
                                    1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        current.width = width;
        current.height = height;
        current.borrowed = true;
        current.blank = false;
        uploaded = true;
        uploadedBlank = false;
        return current;
    }

    std::fill(upload.begin(), upload.end(), 0u);
    const uint32_t copyWidth = std::min(width, kWidth);
    const uint32_t copyHeight = std::min(height, kHeight);
    const size_t sourceRow = static_cast<size_t>(width) * 4u;
    const size_t destinationRow = static_cast<size_t>(kWidth) * 4u;
    const size_t copyRow = static_cast<size_t>(copyWidth) * 4u;
    for (uint32_t y = 0u; y < copyHeight; ++y)
    {
        const size_t sourceOffset = static_cast<size_t>(y) * sourceRow;
        const size_t destinationOffset = static_cast<size_t>(y) * destinationRow;
        if (sourceOffset + copyRow > scratch.size() || destinationOffset + copyRow > upload.size())
            break;
        std::memcpy(upload.data() + destinationOffset, scratch.data() + sourceOffset, copyRow);
    }
    UpdateTexture(fallback, upload.data());
    uploaded = true;
    uploadedBlank = false;
    current.texture = fallback;
    current.width = width;
    current.height = height;
    current.borrowed = current.blank = false;
    current.frame = {};
    GSSharedPresent::Frame captured{};
    captured.texture = fallback.id;
    captured.width = width;
    captured.height = height;
    captured.sequence = vsyncTick != 0u ? vsyncTick : 1u;
    captured.displayFbp = displayFbp;
    captured.sourceFbp = sourceFbp;
    captured.usedPreferred = preferred;
    captured.vsyncTick = vsyncTick;
    acquiredCapture.capture(captured, vsyncTick, "ram_upload");
    return current;
}

GSHostPresentation::View GSHostPresentation::renderFrame(GS &gs, uint64_t vsyncTick, const std::function<void()> &overlay)
{
    if (p3fes_native_movie::renderHostFrame())
        return current;
    if (p3fes_native_present::active())
    {
        // Game-thread hooks only record CPU-side draw commands. Execute every
        // graphics API call here, on the host thread that owns the raylib/GL
        // context, and never paint an interlaced GS fallback over the native
        // progressive frame.
        if (!p3fes_native_present::renderHostFrame())
        {
            PollInputEvents();
            // No swap means no vitaGL frame limiter either (including during
            // Sofdec's direct display path). Do not spin on input and contend
            // with the game thread while waiting for its next complete frame.
            ps2_perf::Scope wait(ps2_perf::Bucket::HostWait);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return current;
    }
    View selected;
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::Upload);
        selected = prepare(gs, vsyncTick);
    }
#if defined(PLATFORM_VITA)
    if (workerDirectActive.load(std::memory_order_acquire))
    {
        // The GS worker already swapped this picture. Keep pad/window events
        // moving without drawing another host frame over it or monopolizing
        // the one vitaGL context between game frames.
        PollInputEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        return selected;
    }
#endif
    BeginDrawing();
    ClearBackground(BLACK);
    const float sourceWidth = static_cast<float>(std::max<uint32_t>(1u, selected.width));
    const float sourceHeight = static_cast<float>(std::max<uint32_t>(1u, selected.height));
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    // PCSX2's read-back field is in GS pixels (320 or 512 wide modes are common); the TV showed it at 4:3.
    const float aspect = ps2_pcsx2gs::Active() ? 4.0f / 3.0f : sourceWidth / sourceHeight;
    const float destinationHeight = std::min(screenHeight, screenWidth / aspect);
    const float destinationWidth = destinationHeight * aspect;
    const Rectangle source{0.0f, 0.0f, sourceWidth, sourceHeight};
    const Rectangle destination{(screenWidth - destinationWidth) * 0.5f,
                                (screenHeight - destinationHeight) * 0.5f,
                                destinationWidth, destinationHeight};
    // PBO texture with rows in order; do not flip source.height.
    DrawTexturePro(selected.texture, source, destination, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
#if !defined(PLATFORM_VITA)
    if (const int label = gsFrameDiagDue(selected.blank); label >= 0)
        gsCaptureWindow(label);
#endif
    if (overlay)
        overlay();
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::HostWait);
        EndDrawing();
    }
    return selected;
}

void GSHostPresentation::shutdown(GS &gs)
{
    ps2_pcsx2gs::Shutdown(); // stops feeding PCSX2 before the GS back end goes away
    releasePcsx2View();
    if (!started && fallback.id == 0u)
        return;
    // nullptr creates the CPU backend; drains and destroys the worker/shared context.
    gs.setRasterBackend(nullptr);
    restoreHostContext();
    if (fallback.id != 0u)
        UnloadTexture(fallback); // Never unload the producer's borrowed Texture2D.
    fallback = {};
    if (captureRequested)
        GSSharedPresent::ShutdownHost();
    workerDirectWidth.store(0u, std::memory_order_relaxed);
    workerDirectHeight.store(0u, std::memory_order_relaxed);
    workerDirectLastNs.store(0u, std::memory_order_relaxed);
    workerDirectActive.store(false, std::memory_order_release);
    p3fes_native_present::shutdownHost();
    started = captureRequested = hasLatched = uploaded = uploadedBlank = modeKnown = sharedMode = false;
    lastTick = lastSharedSequence = 0u;
    current = {};
    scratch.clear();
    upload.clear();
}
