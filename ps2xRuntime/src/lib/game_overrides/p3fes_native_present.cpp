#include "p3fes_native_present.h"
#include "p3fes_native_movie.h"
#include "../gs/gs_draw_control.h"
#include "runtime/ps2_perf.h"
#if defined(PS2X_P3FES_FRAME_TRACE)
#include "p3fes_frame_trace.h"
#endif

#if !defined(__ANDROID__)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <cstring>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <string>

#if defined(PLATFORM_VITA)
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#endif

namespace p3fes_native_present
{
using namespace p3fes::native_renderer;

namespace
{
struct MeshDraw
{
    MeshId id = 0;
    Matrix4 model = kIdentityMatrix;
    MeshDrawState state{};

    MeshDraw() = default;
    // Builds the state in place from the caller's, instead of default-constructing it and
    // move-assigning over it (which also destroys the empty vectors it replaces).
    MeshDraw(MeshId meshId, const Matrix4 &meshModel, MeshDrawState &&meshState)
        : id(meshId), model(meshModel), state(std::move(meshState)) {}
};

// A draw of a retained record (drawMeshShared): the immutable state belongs to the producer's static-atomic cache, which
// keeps the other reference; nobody on either side mutates it. A small alternative of its own: the draw of a cached
// atomic writes ~130 bytes instead of a whole MeshDraw with its default-constructed state.
struct SharedMeshDraw
{
    MeshId id = 0;
    Matrix4 model = kIdentityMatrix;
    std::shared_ptr<const MeshDrawState> state;
    // The draw's own material colours (RGBA bytes packed as they lie in guest memory) when they are not the ones the
    // retained state holds: the host copies the state and patches the colours (colorCount <= 8).
    uint32_t colorOverride[8];
    uint8_t colorCount = 0;

    SharedMeshDraw(MeshId meshId, const Matrix4 &meshModel, const std::shared_ptr<const MeshDrawState> &retained, const uint32_t *colors,
                   uint32_t count)
        : id(meshId), model(meshModel), state(retained), colorCount(static_cast<uint8_t>(count))
    {
        for (uint32_t i = 0; i < count; ++i) colorOverride[i] = colors[i];
    }
};

struct TransientMeshDraw
{
    MeshData mesh;
    Matrix4 model = kIdentityMatrix;
    MeshDrawState state{};
};

struct WorldLineDraw
{
    std::vector<Vertex> vertices;
    Matrix4 model = kIdentityMatrix;
    std::string textureName;
    MeshDrawState state{};
};

struct ScreenDraw
{
    std::vector<ScreenVertex> vertices;
    float sourceWidth = 640.0f;
    float sourceHeight = 448.0f;
    std::string textureName;
    ScreenDrawState state{};
    bool lines = false;

    ScreenDraw() = default;
    // built in place in the frame's draw list (one move of the vertices and the name instead of two)
    ScreenDraw(std::vector<ScreenVertex> &&v, float width, float height, std::string &&name, const ScreenDrawState &drawState, bool asLines)
        : vertices(std::move(v)), sourceWidth(width), sourceHeight(height), textureName(std::move(name)), state(drawState), lines(asLines)
    {
    }
};

struct ClearDraw
{
    ClearColor color{};
    bool colorBuffer = false;
    bool depthBuffer = false;
};

struct CameraDraw
{
    Camera camera{};
};

// Brackets the draws of a render-to-texture pass. The host renders every bracketed run before the
// frame's own draws (see renderHostFrame), so a pass can be sampled by any draw of the same frame.
struct TargetDraw
{
    std::string name;
    uint32_t width = 0, height = 0;
    bool begin = false;
};

struct TextureDefinition
{
    TextureData texture;
    uint64_t revision = 0;
    // Host thread only: g_uploadEpoch when this definition's revision was last found uploaded in the renderer, so a
    // later frame referencing the same definition skips the name lookups (see ensureTexture).
    mutable uint64_t readyEpoch = 0;
};

using Draw = std::variant<MeshDraw, TransientMeshDraw, WorldLineDraw, ScreenDraw, ClearDraw, CameraDraw, TargetDraw, SharedMeshDraw>;

struct Frame
{
    Camera camera{};
    Camera recordedCamera{};
    ClearColor clear{};
    std::vector<Draw> draws;
    bool hasTargets = false;
    // Definitions this frame references, kept alive until it is consumed. The producer appends each
    // texture at most once per frame (see retainTexture); the host builds a name index when it
    // consumes the frame.
    std::vector<std::shared_ptr<const TextureDefinition>> textures;
    uint64_t sequence = 0;
#if defined(PS2X_P3FES_FRAME_TRACE)
    p3fes_frame_trace::Handle trace;
    std::vector<p3fes_frame_trace::Origin> drawOrigins;
#endif
};

// Threading. The guest game thread is the only producer (beginFrame ... endFrame, defineMesh,
// defineTexture, retireTexture); the presentation thread only swaps in the finished frame. So the
// frame under construction and the texture table are producer-private and are touched without a
// lock. g_mutex guards what both sides read or write: g_pending, g_meshDefinitions (written by the
// producer, copied by the host once per mesh), g_retiredTextures and g_statePool.
std::atomic<bool> g_active{false};
std::atomic<bool> g_hostPresentationPaused{false};
std::atomic<int> g_widescreen{0};
// Bumped by setActive(false) and shutdownHost; the producer drops its private state on the next call.
std::atomic<uint32_t> g_resetEpoch{0};
std::mutex g_mutex;
std::atomic<unsigned> g_submittedFrames{0u}, g_supersededFrames{0u};
std::optional<Frame> g_pending;
// Shared so that neither side copies a mesh (vertex/index arrays) while holding g_mutex: the producer
// builds the shared copy before taking the lock, the host copies the pointer.
std::unordered_map<MeshId, std::shared_ptr<const MeshData>> g_meshDefinitions;
std::unordered_map<std::string, std::weak_ptr<const TextureDefinition>> g_retiredTextures;
std::vector<MeshDrawState> g_statePool;
std::atomic<uint32_t> g_textureDefinitionCount{0};
constexpr size_t kStatePoolLimit = 512;

// Producer-private state.
struct TextureEntry
{
    std::shared_ptr<const TextureDefinition> definition; // null once retired and not redefined
    uint64_t retainedSequence = 0; // frame sequence for which `slot` is valid
    uint32_t slot = 0;             // index into the building frame's `textures`
};
Frame g_building;
bool g_buildingOpen = false;
// Render-to-texture bracket being recorded: the display camera to restore at its end, and a flag
// that makes the next setCamera record even when it equals the display camera.
bool g_targetOpen = false;
bool g_forceCameraRecord = false;
Camera g_cameraBeforeTarget{};
uint64_t g_cameraEpoch = 1; // see cameraEpoch()
float g_targetWidth = 0.0f, g_targetHeight = 0.0f;
uint64_t g_sequence = 0;
uint32_t g_producerEpoch = 0;
size_t g_lastFrameDrawCount = 0;
std::unordered_map<std::string, TextureEntry> g_textureDefinitions;
std::vector<std::string> g_retiredEntryNames;
// Direct-mapped front for g_textureDefinitions keyed by the name's bytes (names of up to 15 chars, which
// is every name prepareNativeRasterTexture generates in practice): skips hashing and the bucket walk.
// Entries point at map nodes, which are stable until erased; every erase clears this cache.
struct NameCacheSlot
{
    uint64_t key0 = 0, key1 = 0;
    TextureEntry *entry = nullptr;
};
NameCacheSlot g_nameCache[256];
// Bumped whenever TextureEntry nodes may have been erased (clearNameCache is called before every erase or clear of
// g_textureDefinitions): retained-draw plans that hold entry pointers are valid only for the generation they were made in.
uint64_t g_textureTableGen = 1;
void clearNameCache()
{
    for (NameCacheSlot &slot : g_nameCache) slot = {};
    ++g_textureTableGen;
}
std::vector<MeshDrawState> g_stateStock;
// stockDrawState() bookkeeping: the state handed out is the back of g_stateStock (g_stockLent) or g_lentScratch.
// A producer reset that arrives while the stock's back is lent clears the stock when the loan ends.
MeshDrawState g_lentScratch;
bool g_stockLent = false;
bool g_stockClearPending = false;
// Direct-mapped MeshId -> material count of its definition, so a draw does not walk g_meshDefinitions
// (hash, bucket, node, vector header: four dependent loads) just to learn there is nothing to retain.
// Definitions are never replaced or erased while the producer state lives; syncProducer clears it.
struct MaterialCountSlot
{
    MeshId id = 0;
    uint32_t count = 0;
    bool valid = false;
};
MaterialCountSlot g_materialCountCache[256];

// Host-thread-only state.
std::unique_ptr<Renderer> g_renderer;
std::unordered_set<MeshId> g_uploaded;
std::unordered_map<std::string, uint64_t> g_uploadedTextures;
uint64_t g_uploadEpoch = 1; // bumped whenever an uploaded texture is removed (invalidates readyEpoch)
std::optional<Frame> g_hostFrame;
// Open-addressed name -> definition index over g_hostFrame->textures (later duplicates win).
std::vector<const TextureDefinition *> g_hostTextureIndex;
std::vector<const TextureDefinition *> g_hostTextureList;
uint32_t g_errors = 0;
bool g_textureUploadLogged = false;
bool g_textureUpdateLogged = false;
uint64_t g_presentedFrames = 0, g_textureUploads = 0, g_textureDeletes = 0;
uint32_t g_lastFrameDraws = 0, g_lastFrameMeshes = 0, g_lastFrameColorClearsAfterMesh = 0;
Camera g_lastFrameCamera{};

bool resourceDiagOn()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_RESOURCE_DIAG");
        return value && *value == '1';
    }();
    return enabled;
}

// Host-thread timing of renderHostFrame, microseconds summed over the presented frames of one report window
// (P3FES_RESOURCE_DIAG=1; the clock is steady_clock, 1 us on the Vita).
struct HostTiming
{
    uint64_t frames = 0, total = 0;
    uint64_t prepass = 0;      // layer analysis for widescreen placement
    uint64_t preload = 0;      // texture index + texture uploads
    uint64_t targets = 0;      // render-to-texture passes
    uint64_t beginFrame = 0;   // backend beginFrame (acquire, clear)
    uint64_t meshes = 0;       // mesh / transient mesh / world line replays, ensure + draw
    uint64_t screenEnsure = 0, screenPlace = 0, screenDraw = 0; // screen draws: texture check, placement, backend call
    uint64_t other = 0;        // clear, camera
    uint64_t endFrame = 0;     // backend endFrame (swap)
    uint64_t post = 0;         // hand-back, retire, report
    uint64_t postFree = 0;     // of which dropping the frame (draw vectors and names)
    uint32_t screenDraws = 0, meshDraws = 0, screenVertices = 0;
};
HostTiming g_timing;
inline uint64_t nowUs()
{
#if defined(PLATFORM_VITA)
    return sceKernelGetProcessTimeWide();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
// Producer-side lock that reports how long it waited for g_mutex to the frame histogram
// (PS2X_FRAME_HIST=1, EeScheduler.cpp); a plain lock_guard otherwise.
extern "C" void ps2xFrameHistAddLock(uint64_t us);
extern "C" int ps2xFrameHistEnabled();
struct ProducerLock
{
    std::unique_lock<std::mutex> lock;
    ProducerLock()
    {
        static const bool timed = ps2xFrameHistEnabled() != 0;
        if (!timed) { lock = std::unique_lock<std::mutex>(g_mutex); return; }
        const uint64_t start = nowUs();
        lock = std::unique_lock<std::mutex>(g_mutex);
        ps2xFrameHistAddLock(nowUs() - start);
    }
};
void clockCostProbe()
{
    static bool done = false;
    if (done) return;
    done = true;
    volatile uint64_t sink = 0;
    const uint64_t a = nowUs();
    for (int i = 0; i < 2000; ++i) sink = sink + nowUs();
    const uint64_t b = nowUs();
    for (int i = 0; i < 2000; ++i) sink = sink + static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t c = nowUs();
    std::fprintf(stderr, "[native-host] clock cost: nowUs=%.3f us, steady_clock=%.3f us\n", (b - a) / 2000.0, (c - b) / 2000.0);
}

void reportResources()
{
    if (!resourceDiagOn()) return;
    using Clock = std::chrono::steady_clock;
    static auto last = Clock::now();
    static uint64_t frames = 0, uploads = 0;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    if (seconds < 5.0) return;
    // The lock only covers reading the size: printing under it blocked the game thread for the length of the log
    // write (10-40 ms to ux0:) every five seconds.
    size_t retiredCount = 0;
    {
        std::lock_guard lock(g_mutex);
        retiredCount = g_retiredTextures.size();
    }
    std::fprintf(stderr, "[native-present] fps=%.2f uploads=%llu deletes=%llu textures=%u pending-retire=%u\n",
        (g_presentedFrames - frames) / seconds,
        static_cast<unsigned long long>(g_textureUploads - uploads),
        static_cast<unsigned long long>(g_textureDeletes), static_cast<unsigned>(g_textureDefinitionCount.load(std::memory_order_relaxed)), static_cast<unsigned>(retiredCount));
    std::fprintf(stderr,
        "[native-frame] draws=%u meshes=%u color-clears-after-mesh=%u matrices=%u pos=%.3f,%.3f,%.3f z=%.6f,%.6f,%.6f\n",
        g_lastFrameDraws, g_lastFrameMeshes, g_lastFrameColorClearsAfterMesh,
        g_lastFrameCamera.useMatrices ? 1u : 0u,
        g_lastFrameCamera.position[0], g_lastFrameCamera.position[1], g_lastFrameCamera.position[2],
        g_lastFrameCamera.projection[10], g_lastFrameCamera.projection[11], g_lastFrameCamera.projection[14]);
    frames = g_presentedFrames; uploads = g_textureUploads; last = now;
    const HostTiming &t = g_timing;
    if (t.frames)
    {
        const double n = static_cast<double>(t.frames);
        const double sd = std::max<double>(t.screenDraws, 1.0);
        std::fprintf(stderr,
            "[native-host] frames=%llu total=%.2fms prepass=%.2f preload=%.2f targets=%.2f begin=%.2f meshes=%.2f(%.1f draws) "
            "screen=%.2f(%.1f draws, %.0f verts) other=%.2f end=%.2f post=%.2f(free %.2f) | per screen draw us: ensure=%.1f place=%.1f backend=%.1f\n",
            static_cast<unsigned long long>(t.frames), t.total / n / 1000.0, t.prepass / n / 1000.0, t.preload / n / 1000.0,
            t.targets / n / 1000.0, t.beginFrame / n / 1000.0, t.meshes / n / 1000.0, t.meshDraws / n,
            (t.screenEnsure + t.screenPlace + t.screenDraw) / n / 1000.0, t.screenDraws / n, t.screenVertices / n,
            t.other / n / 1000.0, t.endFrame / n / 1000.0, (t.post + t.postFree) / n / 1000.0, t.postFree / n / 1000.0,
            t.screenEnsure / sd, t.screenPlace / sd, t.screenDraw / sd);
    }
    if (g_renderer)
    {
        const std::string backend = g_renderer->drainDiagReport();
        if (!backend.empty()) std::fputs(backend.c_str(), stderr);
    }
    g_timing = {};
}

// Producer side, while a frame is being recorded. A resource may be destroyed by the game before
// this frame reaches the graphics thread, so the frame keeps its own reference to every definition
// it uses. A name already retained in this frame costs one hash lookup and no allocation; a
// redefinition of the name since then replaces the retained definition (latest wins).
void retainTexture(const std::string &name)
{
    if (name.empty()) return;
    TextureEntry *cachedEntry = nullptr;
    NameCacheSlot *slot = nullptr;
    uint64_t key[2] = {0, 0};
    if (name.size() <= 15)
    {
        // A string of up to 15 chars owns at least 16 readable bytes (inline buffer, or a heap block
        // whose capacity exceeds 15), so two fixed 8-byte loads are safe; mask off what is past the end.
        const size_t size = name.size();
        std::memcpy(&key[0], name.data(), 8);
        std::memcpy(&key[1], name.data() + 8, 8);
        if (size < 8)
        {
            key[0] &= (1ull << (8 * size)) - 1;
            key[1] = 0;
        }
        else
            key[1] &= size == 8 ? 0ull : (1ull << (8 * (size - 8))) - 1;
        slot = &g_nameCache[((key[0] * 0x9E3779B97F4A7C15ull) ^ (key[1] * 0xC2B2AE3D27D4EB4Full)) >> 56];
        if (slot->entry && slot->key0 == key[0] && slot->key1 == key[1])
            cachedEntry = slot->entry;
    }
    if (!cachedEntry)
    {
        const auto found = g_textureDefinitions.find(name);
        if (found == g_textureDefinitions.end()) return;
        cachedEntry = &found->second;
        if (slot) *slot = {key[0], key[1], cachedEntry};
    }
    if (!cachedEntry->definition) return;
    TextureEntry &entry = *cachedEntry;
    if (entry.retainedSequence == g_building.sequence)
    {
        auto &held = g_building.textures[entry.slot];
        if (held != entry.definition) held = entry.definition;
        return;
    }
    entry.retainedSequence = g_building.sequence;
    entry.slot = static_cast<uint32_t>(g_building.textures.size());
    g_building.textures.push_back(entry.definition);
}

// The entry retainTexture resolves `name` to, or null (no entry for it yet). Same lookups as retainTexture.
TextureEntry *findTextureEntry(const std::string &name)
{
    if (name.empty()) return nullptr;
    if (name.size() <= 15)
    {
        uint64_t key[2] = {0, 0};
        const size_t size = name.size();
        std::memcpy(&key[0], name.data(), 8);
        std::memcpy(&key[1], name.data() + 8, 8);
        if (size < 8)
        {
            key[0] &= (1ull << (8 * size)) - 1;
            key[1] = 0;
        }
        else
            key[1] &= size == 8 ? 0ull : (1ull << (8 * (size - 8))) - 1;
        NameCacheSlot &slot = g_nameCache[((key[0] * 0x9E3779B97F4A7C15ull) ^ (key[1] * 0xC2B2AE3D27D4EB4Full)) >> 56];
        if (slot.entry && slot.key0 == key[0] && slot.key1 == key[1]) return slot.entry;
        const auto found = g_textureDefinitions.find(name);
        if (found == g_textureDefinitions.end()) return nullptr;
        slot = {key[0], key[1], &found->second};
        return &found->second;
    }
    const auto found = g_textureDefinitions.find(name);
    return found == g_textureDefinitions.end() ? nullptr : &found->second;
}

// retainTexture for an entry that is already known (the body that follows the lookup there).
inline void retainEntry(TextureEntry &entry)
{
    if (!entry.definition) return;
    if (entry.retainedSequence == g_building.sequence)
    {
        auto &held = g_building.textures[entry.slot];
        if (held != entry.definition) held = entry.definition;
        return;
    }
    entry.retainedSequence = g_building.sequence;
    entry.slot = static_cast<uint32_t>(g_building.textures.size());
    g_building.textures.push_back(entry.definition);
}

// A draw state kept by the producer (makeRetainedState) with the texture entries its names resolve to, so that
// re-submitting it each frame does not hash the names again.
struct RetainedState : MeshDrawState
{
    explicit RetainedState(const MeshDrawState &state) : MeshDrawState(state) {}
    struct Item
    {
        TextureEntry *entry;
        const std::string *name;
    };
    mutable uint64_t planGen = 0;     // g_textureTableGen the plan was made for (0: none yet)
    mutable bool planUsable = false;  // false: the plain retainPasses path is used
    mutable std::vector<Item> plan;
};

void retainMaterials(const MeshData &mesh, size_t first = 0)
{
    for (size_t i = first; i < mesh.materials.size(); ++i)
        retainTexture(mesh.materials[i].textureName);
}

void retainPasses(const MeshDrawState &state)
{
    for (const auto &name : state.materialTextures) retainTexture(name);
    for (const auto &pass : state.materialDualPasses)
        if (pass.enabled) retainTexture(pass.textureName);
    for (const auto &pass : state.materialEnvPasses)
        if (pass.enabled) retainTexture(pass.textureName);
}

// Drops private state left over from a previous activation (setActive(false) / shutdownHost).
void syncProducer()
{
    const uint32_t epoch = g_resetEpoch.load(std::memory_order_acquire);
    if (epoch == g_producerEpoch) return;
    g_producerEpoch = epoch;
    g_building = {};
    g_buildingOpen = false;
    g_targetOpen = g_forceCameraRecord = false;
    ++g_cameraEpoch;
    clearNameCache();
    g_textureDefinitions.clear();
    g_retiredEntryNames.clear();
    if (g_stockLent) g_stockClearPending = true; else g_stateStock.clear();
    for (MaterialCountSlot &slot : g_materialCountCache) slot = {};
    g_textureDefinitionCount.store(0, std::memory_order_relaxed);
    // The tables are emptied under the lock but destroyed after it (freeing the meshes takes a while).
    std::unordered_map<MeshId, std::shared_ptr<const MeshData>> oldMeshes;
    std::unordered_map<std::string, std::weak_ptr<const TextureDefinition>> oldRetired;
    std::vector<MeshDrawState> oldPool;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        oldMeshes.swap(g_meshDefinitions);
        oldRetired.swap(g_retiredTextures);
        oldPool.swap(g_statePool);
    }
}

// P3FES_CUT_STATEPOOL=0: only MeshDraw states go back to the producer's pool (the previous behaviour). By default the
// states of transient meshes (particle tanks, dynamic geometry) are recycled too: without it every such draw built
// a fresh state, 5 vector allocations each.
static bool g_cutsEnabled = true; // setCutsEnabled(); producer thread only
bool recycleTransientStates()
{
    static const bool on = [] { const char *v = std::getenv("P3FES_CUT_STATEPOOL"); return !v || *v != '0'; }();
    return on && g_cutsEnabled;
}

// Resets a drawn state for reuse while keeping its vectors' storage.
void resetForReuse(MeshDrawState &state)
{
#define P3FES_KEEP_VECTORS(X) \
    X(materialSamplers) X(materialTextures) X(materialColors) X(materialUvTransforms) \
    X(materialDualPasses) X(materialEnvPasses) X(materialBlends) X(directionalLights) \
    X(localLights) X(skinPalette)
#define X(name) auto name = std::move(state.name);
    P3FES_KEEP_VECTORS(X)
#undef X
    state = MeshDrawState{};
#define X(name) name.clear(); state.name = std::move(name);
    P3FES_KEEP_VECTORS(X)
#undef X
#undef P3FES_KEEP_VECTORS
}

#if defined(PS2X_P3FES_FRAME_TRACE)
void recordDrawOrigin()
{
    if (g_building.trace)
        g_building.drawOrigins.push_back(p3fes_frame_trace::origin());
}

const std::string *firstTexture(const MeshDrawState &state)
{
    for (const auto &name : state.materialTextures)
        if (!name.empty()) return &name;
    for (const auto &pass : state.materialDualPasses)
        if (pass.enabled && !pass.textureName.empty()) return &pass.textureName;
    for (const auto &pass : state.materialEnvPasses)
        if (pass.enabled && !pass.textureName.empty()) return &pass.textureName;
    return nullptr;
}

p3fes_frame_trace::DrawInfo drawInfo(const Frame &frame, const Draw &draw, uint32_t index)
{
    p3fes_frame_trace::DrawInfo info;
    info.index = index;
    if (index < frame.drawOrigins.size()) info.origin = frame.drawOrigins[index];
    const std::string *textureName = nullptr;
    if (const auto *mesh = std::get_if<MeshDraw>(&draw))
    {
        info.kind = "mesh";
        textureName = firstTexture(mesh->state);
    }
    else if (const auto *shared = std::get_if<SharedMeshDraw>(&draw))
    {
        info.kind = "mesh";
        textureName = firstTexture(*shared->state);
    }
    else if (const auto *transient = std::get_if<TransientMeshDraw>(&draw))
    {
        info.kind = "transient-mesh";
        textureName = firstTexture(transient->state);
        if (!textureName)
            for (const auto &material : transient->mesh.materials)
                if (!material.textureName.empty()) { textureName = &material.textureName; break; }
    }
    else if (const auto *lines = std::get_if<WorldLineDraw>(&draw))
    {
        info.kind = "world-lines";
        if (!lines->textureName.empty()) textureName = &lines->textureName;
    }
    else if (const auto *screen = std::get_if<ScreenDraw>(&draw))
    {
        info.kind = screen->lines ? "screen-lines" : "screen-triangles";
        if (!screen->textureName.empty()) textureName = &screen->textureName;
        info.sourceWidth = screen->sourceWidth;
        info.sourceHeight = screen->sourceHeight;
        info.vertices = screen->vertices;
        info.state = screen->state;
    }
    else if (std::holds_alternative<ClearDraw>(draw)) info.kind = "clear";
    else if (std::holds_alternative<TargetDraw>(draw)) info.kind = "target";
    else info.kind = "camera";

    if (textureName)
    {
        info.texture = *textureName;
        for (auto it = frame.textures.rbegin(); it != frame.textures.rend(); ++it)
            if ((*it)->texture.name == *textureName)
            {
                info.textureWidth = (*it)->texture.width;
                info.textureHeight = (*it)->texture.height;
                info.textureRevision = (*it)->revision;
                break;
            }
    }
    return info;
}
#endif

void collectRetiredTextures()
{
    std::vector<std::string> released;
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_retiredTextures.begin(); it != g_retiredTextures.end();)
        {
            if (it->second.expired())
            {
                released.push_back(it->first);
                it = g_retiredTextures.erase(it);
            }
            else ++it;
        }
    }
    for (const auto &name : released)
    {
        if (g_renderer) g_renderer->removeTexture(name);
        g_uploadedTextures.erase(name);
        ++g_uploadEpoch;
        ++g_textureDeletes;
    }
}

// ---- Frame diagnostics -------------------------------------------------------------------------
// P3FES_FRAME_DIAG=<seconds>[,<seconds>...]: that many seconds after the first presented frame,
// log the frame's screen-space draws ("[p3fes-frame]" lines) and save the finished picture as
// frame-<seconds>.p3fb next to the log (ux0:data/p3fes/ on the Vita, where it is copied from the
// displayed framebuffer, so it shows what the hardware produced).
struct FrameDiag
{
    bool dump = false, capture = false;
};
std::vector<double> g_diagTimes;
size_t g_diagNext = 0;
int g_diagLabel = 0;
std::chrono::steady_clock::time_point g_diagStart{};

// On demand: create the (empty) file capture.req next to the log, e.g. over FTP while the game runs.
// It is looked for once a second; the frame is saved as frame-<n>.p3fb (n counts up from 1000) and
// the request file is removed.
bool captureRequested(uint64_t presentedFrames)
{
    if (presentedFrames % 30u != 0u)
        return false;
#if defined(PLATFORM_VITA)
    const char *request = "ux0:data/p3fes/capture.req";
#else
    const char *request = "capture.req";
#endif
    FILE *file = std::fopen(request, "rb");
    if (!file)
        return false;
    std::fclose(file);
    std::remove(request);
    static int label = 1000;
    g_diagLabel = label++;
    return true;
}

FrameDiag frameDiagFor(uint64_t presentedFrames)
{
    if (captureRequested(presentedFrames))
        return {true, true};
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_FRAME_DIAG");
        if (!value || !*value) return false;
        for (const char *cursor = value; *cursor;)
        {
            char *end = nullptr;
            const double seconds = std::strtod(cursor, &end);
            if (end == cursor) break;
            g_diagTimes.push_back(seconds);
            cursor = *end == ',' ? end + 1 : end;
        }
        return !g_diagTimes.empty();
    }();
    if (!enabled || g_diagNext >= g_diagTimes.size())
        return {};
    const auto now = std::chrono::steady_clock::now();
    if (presentedFrames == 0u || g_diagStart.time_since_epoch().count() == 0)
        g_diagStart = now;
    if (std::chrono::duration<double>(now - g_diagStart).count() < g_diagTimes[g_diagNext])
        return {};
    g_diagLabel = static_cast<int>(g_diagTimes[g_diagNext]);
    ++g_diagNext;
    return {true, true};
}

std::string capturePath()
{
    char path[512];
#if defined(PLATFORM_VITA)
    std::snprintf(path, sizeof(path), "ux0:data/p3fes/frame-%d.p3fb", g_diagLabel);
#else
    // P3FES_CAPTURE_DIR: where desktop runs put their captures (several benches can run at once).
    const char *directory = std::getenv("P3FES_CAPTURE_DIR");
    std::snprintf(path, sizeof(path), "%s%sframe-%d.p3fb", directory ? directory : "", directory ? "/" : "", g_diagLabel);
#endif
    return path;
}

void dumpFrame(const Frame &frame, const std::function<std::string(const ScreenDraw &, size_t)> &placementOf)
{
    std::fprintf(stderr, "[p3fes-frame] t=%d sequence=%llu draws=%u targets=%d\n", g_diagLabel,
                 static_cast<unsigned long long>(frame.sequence), static_cast<unsigned>(frame.draws.size()), frame.hasTargets ? 1 : 0);
    uint32_t index = 0;
    size_t slot = static_cast<size_t>(-1);
    for (const Draw &draw : frame.draws)
    {
        ++slot;
        if (const auto *screen = std::get_if<ScreenDraw>(&draw))
        {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            float z0 = 1e9f, z1 = -1e9f;
            for (const ScreenVertex &v : screen->vertices)
            {
                x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
                u0 = std::min(u0, v.u); u1 = std::max(u1, v.u); v0 = std::min(v0, v.v); v1 = std::max(v1, v.v);
                z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
            }
            uint32_t tw = 0, th = 0;
            for (auto it = frame.textures.rbegin(); it != frame.textures.rend(); ++it)
                if ((*it)->texture.name == screen->textureName)
                {
                    tw = (*it)->texture.width;
                    th = (*it)->texture.height;
                    break;
                }
            const ScreenDrawState &s = screen->state;
            const auto &c = screen->vertices.empty() ? std::array<std::uint8_t, 4>{} : screen->vertices[0].color;
            std::fprintf(stderr,
                "[p3fes-frame] %u screen%s n=%u src=%gx%g xy=%.1f,%.1f..%.1f,%.1f z=%.5f..%.5f uv=%.4f,%.4f..%.4f,%.4f "
                "tex=%s %ux%u blend=%u depth=%d/%d/%u atest=%d/%u/%u afail=%u clamp=%d%d linear=%d fog=%d rgba=%u,%u,%u,%u place=%s\n",
                index, screen->lines ? "-lines" : "", static_cast<unsigned>(screen->vertices.size()), screen->sourceWidth, screen->sourceHeight,
                x0, y0, x1, y1, z0, z1, u0, v0, u1, v1, screen->textureName.c_str(), tw, th,
                static_cast<unsigned>(s.blend), s.depthTest ? 1 : 0, s.depthWrite ? 1 : 0,
                static_cast<unsigned>(s.depthFunction), s.alphaTest.enabled ? 1 : 0, s.alphaTest.function,
                s.alphaTest.reference, static_cast<unsigned>(s.alphaFailAction), s.clampU ? 1 : 0, s.clampV ? 1 : 0,
                s.linearFilter ? 1 : 0, s.fog.enabled ? 1 : 0, c[0], c[1], c[2], c[3],
                (placementOf ? placementOf(*screen, slot) : std::string()).c_str());
        }
        else if (const auto *target = std::get_if<TargetDraw>(&draw))
            std::fprintf(stderr, "[p3fes-frame] %u target %s %s %ux%u\n", index, target->begin ? "begin" : "end",
                         target->name.c_str(), target->width, target->height);
        else if (const auto *mesh = std::get_if<MeshDraw>(&draw))
        {
            const std::string *name = nullptr;
            for (const auto &texture : mesh->state.materialTextures)
                if (!texture.empty()) { name = &texture; break; }
            {
                // Per material: colour alpha, blend pair and the alpha range of its texture.
                const MeshDrawState &st = mesh->state;
                std::fprintf(stderr, "[p3fes-frame] %u mesh-materials atest=%d/%u/%u cull=%u ambient=%.2f,%.2f,%.2f dir=%u local=%u pos=%.0f,%.0f,%.0f:", index,
                             st.alphaTest.enabled ? 1 : 0, st.alphaTest.function, st.alphaTest.reference,
                             static_cast<unsigned>(st.cullMode), st.ambientLight[0], st.ambientLight[1], st.ambientLight[2],
                             static_cast<unsigned>(st.directionalLights.size()), static_cast<unsigned>(st.localLights.size()),
                             mesh->model[12], mesh->model[13], mesh->model[14]);
                for (size_t m = 0; m < st.materialTextures.size() && m < 6u; ++m)
                {
                    unsigned lo = 255u, hi = 0u, w = 0u, h = 0u;
                    for (auto it = frame.textures.rbegin(); it != frame.textures.rend(); ++it)
                        if ((*it)->texture.name == st.materialTextures[m])
                        {
                            const auto &rgba = (*it)->texture.rgba;
                            for (size_t i = 3; i < rgba.size(); i += 4)
                            {
                                lo = std::min<unsigned>(lo, rgba[i]);
                                hi = std::max<unsigned>(hi, rgba[i]);
                            }
                            w = (*it)->texture.width;
                            h = (*it)->texture.height;
                            break;
                        }
                    std::fprintf(stderr, " [%s %ux%u a=%u..%u col-a=%u blend=%u/%u]", st.materialTextures[m].c_str(), w, h, lo, hi,
                                 m < st.materialColors.size() ? st.materialColors[m][3] : 999u,
                                 m < st.materialBlends.size() && st.materialBlends[m].enabled ? st.materialBlends[m].src : 0u,
                                 m < st.materialBlends.size() && st.materialBlends[m].enabled ? st.materialBlends[m].dst : 0u);
                }
                std::fprintf(stderr, "\n");
            }
            std::fprintf(stderr, "[p3fes-frame] %u mesh %llX tex=%s mats=%u lit=%d blend=%d depth=%d/%d skin=%u\n", index,
                         static_cast<unsigned long long>(mesh->id), name ? name->c_str() : "-",
                         static_cast<unsigned>(mesh->state.materialTextures.size()), mesh->state.lightingEnabled ? 1 : 0,
                         mesh->state.blendEnabled ? 1 : 0, mesh->state.depthTest ? 1 : 0,
                         mesh->state.depthWrite ? 1 : 0, static_cast<unsigned>(mesh->state.skinPalette.size()));
        }
        else if (const auto *shared = std::get_if<SharedMeshDraw>(&draw))
        {
            const MeshDrawState &st = *shared->state;
            const std::string *name = nullptr;
            for (const auto &texture : st.materialTextures)
                if (!texture.empty()) { name = &texture; break; }
            std::fprintf(stderr, "[p3fes-frame] %u mesh %llX (retained) tex=%s mats=%u lit=%d blend=%d depth=%d/%d colors-override=%u pos=%.0f,%.0f,%.0f\n", index,
                         static_cast<unsigned long long>(shared->id), name ? name->c_str() : "-", static_cast<unsigned>(st.materialTextures.size()),
                         st.lightingEnabled ? 1 : 0, st.blendEnabled ? 1 : 0, st.depthTest ? 1 : 0, st.depthWrite ? 1 : 0,
                         static_cast<unsigned>(shared->colorCount), shared->model[12], shared->model[13], shared->model[14]);
        }
        else if (const auto *transient = std::get_if<TransientMeshDraw>(&draw))
        {
            const std::string *name = nullptr;
            for (const auto &texture : transient->state.materialTextures)
                if (!texture.empty()) { name = &texture; break; }
            if (!name)
                for (const auto &material : transient->mesh.materials)
                    if (!material.textureName.empty()) { name = &material.textureName; break; }
            std::fprintf(stderr, "[p3fes-frame] %u transient n=%u tex=%s blend=%d depth=%d/%d\n", index,
                         static_cast<unsigned>(transient->mesh.vertices.size()), name ? name->c_str() : "-",
                         transient->state.blendEnabled ? 1 : 0, transient->state.depthTest ? 1 : 0,
                         transient->state.depthWrite ? 1 : 0);
        }
        else if (const auto *clear = std::get_if<ClearDraw>(&draw))
            std::fprintf(stderr, "[p3fes-frame] %u clear color=%d depth=%d rgba=%u,%u,%u,%u\n", index,
                         clear->colorBuffer ? 1 : 0, clear->depthBuffer ? 1 : 0, clear->color.r, clear->color.g,
                         clear->color.b, clear->color.a);
        else if (std::holds_alternative<CameraDraw>(draw))
            std::fprintf(stderr, "[p3fes-frame] %u camera\n", index);
        else
            std::fprintf(stderr, "[p3fes-frame] %u world-lines\n", index);
        ++index;
    }
}

bool g_rendererCapture = false;
void requestCapture()
{
    g_rendererCapture = g_renderer->captureNextFrame(capturePath());
}

void finishCapture()
{
#if defined(PLATFORM_VITA)
    if (g_rendererCapture)
        return;
    // Copy what the display is showing: this is the picture the hardware produced, independent of
    // the GL layer.
    sceDisplayWaitVblankStartMulti(2);
    SceDisplayFrameBuf shown{};
    shown.size = sizeof(shown);
    if (sceDisplayGetFrameBuf(&shown, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !shown.base)
    {
        std::fprintf(stderr, "[p3fes-frame] capture: no display framebuffer\n");
        return;
    }
    const std::string path = capturePath();
    FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        std::fprintf(stderr, "[p3fes-frame] capture: cannot create %s\n", path.c_str());
    else
    {
        const uint32_t header[2] = {static_cast<uint32_t>(shown.width), static_cast<uint32_t>(shown.height)};
        std::fwrite("P3FB", 1, 4, file);
        std::fwrite(header, sizeof(uint32_t), 2, file);
        for (uint32_t y = 0; y < shown.height; ++y)
            std::fwrite(static_cast<const uint8_t *>(shown.base) + static_cast<size_t>(y) * shown.pitch * 4u, 4u,
                        shown.width, file);
        std::fclose(file);
        std::fprintf(stderr, "[p3fes-frame] capture %s %ux%u format=%u\n", path.c_str(), shown.width, shown.height,
                     shown.pixelformat);
    }
#endif
}

bool ensureRenderer()
{
    if (!g_renderer)
        g_renderer = createPlatformRenderer();
    if (!g_renderer)
        return false;
    std::string error;
    if (!g_renderer->open(960, 544, "Persona 3 FES native", error))
    {
        if (g_errors++ < 4u)
            std::fprintf(stderr, "[p3fes-native] host renderer open failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool ensureMesh(MeshId id)
{
    if (g_uploaded.find(id) != g_uploaded.end())
        return true;

    std::shared_ptr<const MeshData> meshPtr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_meshDefinitions.find(id);
        if (found == g_meshDefinitions.end())
            return false;
        meshPtr = found->second;
    }
    const MeshData &mesh = *meshPtr;
    std::string error;
    if (!g_renderer->uploadMesh(id, mesh, error))
    {
        if (g_errors++ < 8u)
            std::fprintf(stderr, "[p3fes-native] host mesh upload failed: %s\n", error.c_str());
        return false;
    }
    g_uploaded.insert(id);
    return true;
}

const TextureDefinition *findHostTexture(std::string_view name)
{
    if (g_hostTextureIndex.empty()) return nullptr;
    const size_t mask = g_hostTextureIndex.size() - 1;
    for (size_t i = std::hash<std::string_view>{}(name) & mask;; i = (i + 1) & mask)
    {
        const TextureDefinition *entry = g_hostTextureIndex[i];
        if (!entry) return nullptr;
        if (entry->texture.name == name) return entry;
    }
}

// Indexes g_hostFrame->textures by name; when a name appears twice the later (newer) definition wins.
void buildHostTextureIndex()
{
    size_t capacity = 16;
    while (capacity < g_hostFrame->textures.size() * 2) capacity *= 2;
    g_hostTextureIndex.assign(capacity, nullptr);
    g_hostTextureList.clear();
    const size_t mask = capacity - 1;
    for (const auto &definition : g_hostFrame->textures)
    {
        const std::string_view name = definition->texture.name;
        size_t i = std::hash<std::string_view>{}(name) & mask;
        while (g_hostTextureIndex[i] && g_hostTextureIndex[i]->texture.name != name)
            i = (i + 1) & mask;
        if (!g_hostTextureIndex[i]) g_hostTextureList.push_back(definition.get());
        else
            for (auto &listed : g_hostTextureList)
                if (listed == g_hostTextureIndex[i]) listed = definition.get();
        g_hostTextureIndex[i] = definition.get();
    }
}

bool ensureTexture(const std::string &name)
{
    if (name.empty())
        return true;

    const TextureDefinition *definition = nullptr;
    bool updating = false;
    {
        if (!g_hostFrame) return false;
        definition = findHostTexture(name);
        if (!definition)
            return false;
        if (definition->readyEpoch == g_uploadEpoch)
            return true;
        const auto uploaded = g_uploadedTextures.find(name);
        if (uploaded != g_uploadedTextures.end() && uploaded->second == definition->revision)
        {
            definition->readyEpoch = g_uploadEpoch;
            return true;
        }
        updating = uploaded != g_uploadedTextures.end();
    }

    std::string error;
    if (!g_renderer->uploadTexture(definition->texture, error))
    {
        if (g_errors++ < 8u)
            std::fprintf(stderr, "[p3fes-native] host texture upload failed: %s\n", error.c_str());
        return false;
    }
    g_uploadedTextures[name] = definition->revision;
    definition->readyEpoch = g_uploadEpoch;
    ++g_textureUploads;
    if (!g_textureUploadLogged)
    {
        g_textureUploadLogged = true;
        std::fprintf(stderr, "[p3fes-native] host texture upload active: %s %ux%u\n",
                     name.c_str(), definition->texture.width, definition->texture.height);
    }
    if (updating && !g_textureUpdateLogged)
    {
        g_textureUpdateLogged = true;
        std::fprintf(stderr, "[p3fes-native] host texture in-place update active: %s %ux%u\n",
                     name.c_str(), definition->texture.width, definition->texture.height);
    }
    return true;
}
} // namespace

void setActive(bool active)
{
    g_active.store(active, std::memory_order_release);
    ps2x::gs::setPrimitiveRasterSuppressed(active);
    if (!active)
    {
        // The producer drops its private frame/texture state at its next call.
        g_resetEpoch.fetch_add(1u, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.reset();
    }
}

bool active()
{
    return g_active.load(std::memory_order_acquire);
}

void setHostPresentationPaused(bool paused)
{
    const bool wasPaused = g_hostPresentationPaused.exchange(paused, std::memory_order_acq_rel);
    if (paused != wasPaused)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Drop a frame recorded on the other side of the presentation mode
        // switch. During Sofdec the producer keeps ending frames so its command
        // memory stays bounded, but those frames must never be replayed after
        // the software-display path hands control back to vitaGL.
        g_pending.reset();
    }
}

MeshDrawState acquireDrawState()
{
    if (!active())
        return {};
    syncProducer();
    if (g_stateStock.empty())
        return {};
    MeshDrawState state = std::move(g_stateStock.back());
    g_stateStock.pop_back();
    return state;
}

MeshDrawState &stockDrawState()
{
    if (active())
    {
        syncProducer();
        if (!g_stateStock.empty())
        {
            g_stockLent = true;
            return g_stateStock.back();
        }
    }
    g_stockLent = false;
    return g_lentScratch;
}

// Ends the loan of stockDrawState(): the state was moved from (or is abandoned).
static void finishLentState()
{
    if (g_stockLent)
    {
        g_stateStock.pop_back();
        g_stockLent = false;
    }
    else
        g_lentScratch = MeshDrawState{};
    if (g_stockClearPending)
    {
        g_stockClearPending = false;
        g_stateStock.clear();
    }
}

void dropStockDrawState()
{
    finishLentState();
}

void beginFrame(const Camera &camera, ClearColor clear)
{
    if (!active())
        return;
    syncProducer();
    if (g_buildingOpen)
        return;
    // Forget textures that were retired and not redefined (their entries only existed so a frame
    // that retained them could still be addressed by name).
    for (const std::string &name : g_retiredEntryNames)
    {
        const auto found = g_textureDefinitions.find(name);
        if (found != g_textureDefinitions.end() && !found->second.definition)
        {
            clearNameCache();
            g_textureDefinitions.erase(found);
        }
    }
    g_retiredEntryNames.clear();
    // Reuse the drawn states the host handed back: one lock per frame, none per draw.
    if (g_stateStock.size() < 64)
    {
        std::vector<MeshDrawState> taken;
        {
            ProducerLock lock;
            taken.swap(g_statePool);
        }
        for (MeshDrawState &state : taken)
            g_stateStock.push_back(std::move(state));
    }
    g_building = {};
    g_building.draws.reserve(g_lastFrameDrawCount + 16u);
    g_building.camera = camera;
    g_building.recordedCamera = camera;
    g_building.clear = clear;
    g_building.sequence = ++g_sequence;
    g_targetOpen = g_forceCameraRecord = false;
    ++g_cameraEpoch;
#if defined(PS2X_P3FES_FRAME_TRACE)
    g_building.trace = p3fes_frame_trace::beginFrame(g_building.sequence);
#endif
    g_buildingOpen = true;
}

// Camera::operator== is a chain of ~40 float compares; this runs for every draw and the answer is
// almost always "same". Bitwise-identical data is equal (barring NaN, where recording one camera
// draw less changes nothing), so test that with a few block compares and only fall back to the
// member-wise comparison, which also treats -0.0 and 0.0 as equal, when the bits differ.
// P3FES_CUT_CAMERA=0 restores the memcmp calls (newlib's memcmp on the Vita compares byte by byte for the odd 41-byte
// head: ~1000 instructions per call of this function, once per atomic) and the per-draw setCamera call.
static bool cutCameraEnabled()
{
    static const bool on = [] { const char *v = std::getenv("P3FES_CUT_CAMERA"); return !v || *v != '0'; }();
    return on && g_cutsEnabled;
}
// Equal bytes, compared a 32-bit word at a time (no alignment assumed).
static inline bool bytesEqualWords(const void *x, const void *y, size_t bytes)
{
    const unsigned char *a = static_cast<const unsigned char *>(x), *b = static_cast<const unsigned char *>(y);
    size_t i = 0;
    for (; i + 4 <= bytes; i += 4)
    {
        uint32_t wa, wb;
        std::memcpy(&wa, a + i, 4);
        std::memcpy(&wb, b + i, 4);
        if (wa != wb) return false;
    }
    for (; i < bytes; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
bool sameCamera(const Camera &a, const Camera &b)
{
    static_assert(offsetof(Camera, view) == offsetof(Camera, useMatrices) + 4u &&
                      offsetof(Camera, projection) == offsetof(Camera, view) + 64u &&
                      sizeof(Camera) == offsetof(Camera, projection) + 64u,
                  "Camera layout assumed by sameCamera");
    if (cutCameraEnabled())
    {
        if (bytesEqualWords(&a, &b, offsetof(Camera, useMatrices) + 1u) &&
            bytesEqualWords(a.view.data(), b.view.data(), 128u))
            return true;
    }
    else if (std::memcmp(&a, &b, offsetof(Camera, useMatrices) + 1u) == 0 &&
             std::memcmp(a.view.data(), b.view.data(), 128u) == 0)
        return true;
    return a == b;
}

uint64_t cameraEpoch()
{
    return g_cameraEpoch;
}

void setCutsEnabled(bool enabled)
{
    g_cutsEnabled = enabled;
}

bool setCameraWouldBeNoop(const Camera &camera)
{
    if (!active())
        return true;
    return !g_buildingOpen || (!g_forceCameraRecord && sameCamera(g_building.recordedCamera, camera));
}

void setCamera(const Camera &camera)
{
    if (!active())
        return;
    syncProducer();
    if (!g_buildingOpen || (!g_forceCameraRecord && sameCamera(g_building.recordedCamera, camera)))
        return;
    g_forceCameraRecord = false;
#if defined(PS2X_P3FES_FRAME_TRACE)
    recordDrawOrigin();
#endif
    g_building.draws.emplace_back(CameraDraw{camera});
    g_building.recordedCamera = camera;
    ++g_cameraEpoch;
}

void clear(ClearColor clear, bool colorBuffer, bool depthBuffer)
{
    if (!active() || (!colorBuffer && !depthBuffer))
        return;
    syncProducer();
    if (g_buildingOpen)
    {
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        g_building.draws.emplace_back(ClearDraw{clear, colorBuffer, depthBuffer});
    }
}

bool beginTextureTarget(const std::string &name, uint32_t width, uint32_t height)
{
    if (!active() || name.empty() || !width || !height)
        return false;
    syncProducer();
    if (!g_buildingOpen || g_targetOpen)
        return false;
#if defined(PS2X_P3FES_FRAME_TRACE)
    recordDrawOrigin();
#endif
    g_building.draws.emplace_back(TargetDraw{name, width, height, true});
    g_targetWidth = static_cast<float>(width);
    g_targetHeight = static_cast<float>(height);
    g_building.hasTargets = true;
    g_cameraBeforeTarget = g_building.recordedCamera;
    g_forceCameraRecord = true;
    ++g_cameraEpoch;
    g_targetOpen = true;
    return true;
}

void endTextureTarget()
{
    if (!active())
        return;
    syncProducer();
    if (!g_buildingOpen || !g_targetOpen)
        return;
#if defined(PS2X_P3FES_FRAME_TRACE)
    recordDrawOrigin();
#endif
    g_building.draws.emplace_back(TargetDraw{{}, 0u, 0u, false});
    // The host skips the bracket when it replays the display pass, so the display camera state is
    // the one recorded before it.
    g_building.recordedCamera = g_cameraBeforeTarget;
    g_forceCameraRecord = false;
    ++g_cameraEpoch;
    g_targetOpen = false;
}

bool inTextureTarget()
{
    return g_targetOpen;
}

void setWidescreen(int mode)
{
    g_widescreen.store(mode, std::memory_order_relaxed);
}

void defineMesh(MeshId id, const MeshData &mesh)
{
    if (!active())
        return;
    syncProducer();
    // Producer is the only writer, so existence can be checked without the lock; the copy is made outside it.
    if (g_meshDefinitions.find(id) != g_meshDefinitions.end())
        return;
    auto shared = std::make_shared<const MeshData>(mesh);
    ProducerLock lock;
    g_meshDefinitions.try_emplace(id, std::move(shared));
}

void defineTexture(const TextureData &texture, uint64_t revision)
{
    if (!active() || texture.name.empty())
        return;
    syncProducer();
    const auto found = g_textureDefinitions.find(texture.name);
    if (found != g_textureDefinitions.end() && found->second.definition &&
        found->second.definition->revision == revision)
        return;
    auto definition = std::make_shared<TextureDefinition>(TextureDefinition{texture, revision});
    if (found != g_textureDefinitions.end())
        found->second.definition = std::move(definition);
    else
    {
        g_textureDefinitions[texture.name].definition = std::move(definition);
        g_textureDefinitionCount.store(static_cast<uint32_t>(g_textureDefinitions.size()),
                                       std::memory_order_relaxed);
    }
}

void retireTexture(const std::string &name)
{
    syncProducer();
    const auto found = g_textureDefinitions.find(name);
    if (found == g_textureDefinitions.end() || !found->second.definition) return;
    {
        ProducerLock lock;
        g_retiredTextures[name] = found->second.definition;
    }
    // Keep the entry (without a definition) until the next frame starts: a frame being recorded may
    // already hold a slot for this name, and a redefinition must be able to replace it in place.
    found->second.definition.reset();
    g_retiredEntryNames.push_back(name);
}

void drawMesh(MeshId id, const Matrix4 &model, MeshDrawState state)
{
    if (!active())
        return;
    syncProducer();
    if (g_buildingOpen)
    {
        // Producer-side read of the mesh table: only this thread inserts into it, the host only reads.
        // The host binds state.materialTextures[i] for every material it covers (an empty entry
        // included) and the mesh's own texture name only past the end of that vector, so only those
        // materials need their definition retained.
        MaterialCountSlot &slot = g_materialCountCache[((id * 0x9E3779B97F4A7C15ull) >> 56) & 255u];
        if (!slot.valid || slot.id != id)
        {
            const auto mesh = g_meshDefinitions.find(id);
            slot = mesh != g_meshDefinitions.end()
                ? MaterialCountSlot{id, static_cast<uint32_t>(mesh->second->materials.size()), true}
                : MaterialCountSlot{};
        }
        if (slot.valid && state.materialTextures.size() < slot.count)
        {
            const auto mesh = g_meshDefinitions.find(id);
            if (mesh != g_meshDefinitions.end()) retainMaterials(*mesh->second, state.materialTextures.size());
        }
        retainPasses(state);
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        // Build in place: avoids materialising a MeshDraw and moving it into the variant.
        g_building.draws.emplace_back(std::in_place_type<MeshDraw>, id, model, std::move(state));
    }
}

void drawMeshFromStock(MeshId id, const Matrix4 &model)
{
    MeshDrawState &state = g_stockLent ? g_stateStock.back() : g_lentScratch;
    if (active())
    {
        syncProducer();
        if (g_buildingOpen)
        {
            // Same recording as drawMesh.
            MaterialCountSlot &slot = g_materialCountCache[((id * 0x9E3779B97F4A7C15ull) >> 56) & 255u];
            if (!slot.valid || slot.id != id)
            {
                const auto mesh = g_meshDefinitions.find(id);
                slot = mesh != g_meshDefinitions.end()
                    ? MaterialCountSlot{id, static_cast<uint32_t>(mesh->second->materials.size()), true}
                    : MaterialCountSlot{};
            }
            if (slot.valid && state.materialTextures.size() < slot.count)
            {
                const auto mesh = g_meshDefinitions.find(id);
                if (mesh != g_meshDefinitions.end()) retainMaterials(*mesh->second, state.materialTextures.size());
            }
            retainPasses(state);
#if defined(PS2X_P3FES_FRAME_TRACE)
            recordDrawOrigin();
#endif
            g_building.draws.emplace_back(std::in_place_type<MeshDraw>, id, model, std::move(state));
        }
    }
    finishLentState();
}

std::shared_ptr<const MeshDrawState> makeRetainedState(const MeshDrawState &state)
{
    return std::make_shared<const RetainedState>(state);
}

void drawMeshShared(MeshId id, const Matrix4 &model, const std::shared_ptr<const MeshDrawState> &state, const uint32_t *colors, uint32_t colorCount)
{
    if (!active() || !state)
        return;
    syncProducer();
    if (g_buildingOpen)
    {
        // Same recording as drawMesh, with the retained state: its textures are retained for this frame as for any
        // other draw (the frame keeps a reference to the definition each name resolves to now).
        const RetainedState &retained = static_cast<const RetainedState &>(*state); // only makeRetainedState makes these
        if (retained.planGen != g_textureTableGen)
        {
            retained.planGen = g_textureTableGen;
            retained.plan.clear();
            retained.planUsable = false;
            const auto mesh = g_meshDefinitions.find(id);
            if (mesh != g_meshDefinitions.end() && retained.materialTextures.size() >= mesh->second->materials.size())
            {
                retained.planUsable = true;
                for (const auto &name : retained.materialTextures)
                    if (!name.empty()) retained.plan.push_back({findTextureEntry(name), &name});
                for (const auto &pass : retained.materialDualPasses)
                    if (pass.enabled && !pass.textureName.empty()) retained.plan.push_back({findTextureEntry(pass.textureName), &pass.textureName});
                for (const auto &pass : retained.materialEnvPasses)
                    if (pass.enabled && !pass.textureName.empty()) retained.plan.push_back({findTextureEntry(pass.textureName), &pass.textureName});
            }
        }
        if (retained.planUsable)
        {
            for (RetainedState::Item &item : retained.plan)
            {
                if (!item.entry)
                {
                    // no entry when the plan was made: look again (the texture may be defined by now)
                    item.entry = findTextureEntry(*item.name);
                    if (!item.entry) continue;
                }
                retainEntry(*item.entry);
            }
        }
        else
        {
            MaterialCountSlot &slot = g_materialCountCache[((id * 0x9E3779B97F4A7C15ull) >> 56) & 255u];
            if (!slot.valid || slot.id != id)
            {
                const auto mesh = g_meshDefinitions.find(id);
                slot = mesh != g_meshDefinitions.end()
                    ? MaterialCountSlot{id, static_cast<uint32_t>(mesh->second->materials.size()), true}
                    : MaterialCountSlot{};
            }
            if (slot.valid && state->materialTextures.size() < slot.count)
            {
                const auto mesh = g_meshDefinitions.find(id);
                if (mesh != g_meshDefinitions.end()) retainMaterials(*mesh->second, state->materialTextures.size());
            }
            retainPasses(*state);
        }
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        g_building.draws.emplace_back(std::in_place_type<SharedMeshDraw>, id, model, state, colors, colorCount);
    }
}

void drawTransientMesh(MeshData mesh, const Matrix4 &model, MeshDrawState state)
{
    if (!active() || mesh.vertices.empty())
        return;
    syncProducer();
    if (g_buildingOpen)
    {
        retainMaterials(mesh);
        retainPasses(state);
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        g_building.draws.emplace_back(TransientMeshDraw{std::move(mesh), model, std::move(state)});
    }
}

void drawWorldLines(std::vector<Vertex> vertices, const Matrix4 &model,
                    std::string textureName, MeshDrawState state)
{
    if (!active() || vertices.size() < 2u)
        return;
    syncProducer();
    if (g_buildingOpen)
    {
        retainTexture(textureName);
        retainPasses(state);
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        g_building.draws.emplace_back(WorldLineDraw{std::move(vertices), model, std::move(textureName), std::move(state)});
    }
}

void drawScreenTriangles(std::vector<ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                         std::string textureName, ScreenDrawState state)
{
    if (!active() || vertices.empty())
        return;
#if defined(PS2X_P3FES_FRAME_TRACE)
    p3fes_frame_trace::NativeScope traceScope(p3fes_frame_trace::Stage::CommandQueue);
#endif
    syncProducer();
    if (g_buildingOpen)
    {
        retainTexture(textureName);
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        // Screen-space draws of a render-target pass address the target's own pixels.
        if (g_targetOpen)
        {
            sourceWidth = g_targetWidth;
            sourceHeight = g_targetHeight;
        }
        g_building.draws.emplace_back(std::in_place_type<ScreenDraw>, std::move(vertices), sourceWidth, sourceHeight,
                                      std::move(textureName), state, false);
    }
}

void drawScreenLines(std::vector<ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                     std::string textureName, ScreenDrawState state)
{
    if (!active() || vertices.empty())
        return;
#if defined(PS2X_P3FES_FRAME_TRACE)
    p3fes_frame_trace::NativeScope traceScope(p3fes_frame_trace::Stage::CommandQueue);
#endif
    syncProducer();
    if (g_buildingOpen)
    {
        retainTexture(textureName);
#if defined(PS2X_P3FES_FRAME_TRACE)
        recordDrawOrigin();
#endif
        // Screen-space draws of a render-target pass address the target's own pixels.
        if (g_targetOpen)
        {
            sourceWidth = g_targetWidth;
            sourceHeight = g_targetHeight;
        }
        g_building.draws.emplace_back(std::in_place_type<ScreenDraw>, std::move(vertices), sourceWidth, sourceHeight,
                                      std::move(textureName), state, true);
    }
}

void endFrame()
{
    if (!active())
        return;
    syncProducer();
    if (!g_buildingOpen)
        return;
#if defined(PS2X_P3FES_FRAME_TRACE)
    uint32_t meshCount = 0;
    for (const Draw &draw : g_building.draws)
        if (std::holds_alternative<MeshDraw>(draw) || std::holds_alternative<SharedMeshDraw>(draw) ||
            std::holds_alternative<TransientMeshDraw>(draw))
            ++meshCount;
    p3fes_frame_trace::endProducer(g_building.trace,
        static_cast<uint32_t>(g_building.draws.size()), meshCount,
        static_cast<uint32_t>(g_textureDefinitions.size()));
#endif
    if (g_targetOpen)
        endTextureTarget();
    g_lastFrameDrawCount = g_building.draws.size();
    // Latest complete frame wins. The host may present at 60 Hz while the game
    // submits at 30 Hz, so keeping only one pending frame bounds memory/latency.
    // The superseded frame (if any) is destroyed outside the lock.
    std::optional<Frame> superseded;
    {
        ProducerLock lock;
#if defined(PS2X_P3FES_FRAME_TRACE)
        // Keep the one selected diagnostic frame until the presentation thread
        // consumes it; later frames must not evict the requested capture.
        if (!g_pending || !p3fes_frame_trace::rendering(g_pending->trace))
#endif
        {
            superseded = std::move(g_pending);
            g_pending = std::move(g_building);
        }
    }
    g_submittedFrames.fetch_add(1u, std::memory_order_relaxed);
    if (superseded)
        g_supersededFrames.fetch_add(1u, std::memory_order_relaxed);
    g_building = {};
    g_buildingOpen = false;
    ++g_cameraEpoch;
    // A frame the host never presented still holds drawn states with their vectors' storage; keep
    // that storage for the producer instead of freeing it and re-allocating on the next frames.
    if (superseded)
    {
        for (Draw &draw : superseded->draws)
            if (auto *mesh = std::get_if<MeshDraw>(&draw))
            {
                if (g_stateStock.size() >= kStatePoolLimit) break;
                resetForReuse(mesh->state);
                g_stateStock.push_back(std::move(mesh->state));
            }
            else if (auto *transient = std::get_if<TransientMeshDraw>(&draw); transient && recycleTransientStates())
            {
                if (g_stateStock.size() >= kStatePoolLimit) break;
                resetForReuse(transient->state);
                g_stateStock.push_back(std::move(transient->state));
            }
    }
}

bool renderHostFrame()
{
    if (!active() || g_hostPresentationPaused.load(std::memory_order_acquire))
        return false;
    bool haveNewFrame = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending)
        {
            g_hostFrame = std::move(g_pending);
            g_pending.reset();
            haveNewFrame = true;
        }
    }
    // The display keeps showing the last swapped buffer until another one is
    // submitted. Replaying g_hostFrame here would swap the same picture at the
    // host thread's rate even when the game produced no new frame, needlessly
    // filling Vita's/Vita3K's GXM display queue.
    if (!haveNewFrame || !g_hostFrame || !ensureRenderer())
        return false;

    const Frame &frame = *g_hostFrame;
#if defined(PS2X_P3FES_FRAME_TRACE)
    const bool traceFrame = frame.trace && p3fes_frame_trace::rendering(frame.trace);
    const p3fes_frame_trace::Handle traceHandle = frame.trace;
    if (traceFrame) p3fes_frame_trace::beginRender(traceHandle);
    if (traceFrame) p3fes_frame_trace::setFrameState(traceHandle, frame.camera, frame.clear);
    const uint64_t traceUploadsBefore = g_textureUploads;
    const uint64_t traceDeletesBefore = g_textureDeletes;
#endif
    ps2_perf::Scope profile(ps2_perf::Bucket::Gs);
    const bool timing = resourceDiagOn();
    if (timing) clockCostProbe();
    const uint64_t timingStart = timing ? nowUs() : 0u;
    uint64_t timingMark = timingStart;
    const auto lap = [&](uint64_t &bucket) {
        if (!timing) return;
        const uint64_t now = nowUs();
        bucket += now - timingMark;
        timingMark = now;
    };
    const int widescreen = g_widescreen.load(std::memory_order_relaxed);
    g_renderer->setWidescreen(widescreen);
    // Widescreen with square-pixel 2-D: a HUD element is several draws at one Z (panel, glyphs,
    // icons), so placement is decided per Z layer from the layer's horizontal extent. A layer that
    // reaches only the left (right) edge of the 4:3 source is pinned to that edge of the screen, one
    // that reaches both (fades, vignettes, backdrops) is stretched, the rest stays centred.
    //
    // Menus cannot be placed per layer: a menu puts all of its content in one Z layer, with elements that
    // belong together on both sides of the layer's centre, and a bar or a title that touches an edge says
    // nothing about the list next to it. A layer that touches an edge and is not a compact corner element or a
    // dialogue panel (the "mixed" layers) is therefore placed draw by draw, see classifyMixedDraws: backdrops
    // and strips are stretched, art touching an edge is pinned to it, flat bars touching an edge are extended
    // to it, and everything else stays in one centred block, so that groups keep their layout.
    struct ScreenLayer
    {
        float z, x0, x1, width;
        bool hasStrip = false; // some texture's draws span most of the width (a dialogue box, a wide panel)
        bool mixed = false;    // placed draw by draw (see above)
        float untexturedArea = 0.0f, untexturedSolidArea = 0.0f, untexturedSolidLargest = 0.0f, untexturedX0 = 1e9f, untexturedX1 = -1e9f; // draws without texture
    };
    // Horizontal extent of the draws of one texture inside one layer.
    struct LayerTexture
    {
        float z, x0, x1;
        const std::string *name;
        float area = 0.0f;       // sum of the draws' boxes clipped to the source area (backdrop detection)
        float opaqueArea = 0.0f; // the same for draws that blend normally (not the fades and effects: additive, mask passes)
        float opaqueLargest = 0.0f; // the largest of those draws (overlapping effect quads add up to a screen too)
    };
    static std::vector<ScreenLayer> layers;
    static std::vector<LayerTexture> layerTextures;
    layers.clear();
    layerTextures.clear();
    // A panel: one texture whose draws run from near the left edge to near the right edge without
    // hanging over either (a portrait covers 60 % of the width too, but overflows the right edge).
    const auto isStrip = [](const LayerTexture &texture, float width) {
        return texture.x0 >= -0.5f && texture.x0 <= 0.15f * width && texture.x1 >= 0.85f * width && texture.x1 <= width + 0.5f;
    };
    // layerTextures are found by (z, name) through a small open-addressed table of indices.
    static std::vector<int32_t> layerTextureTable;
    static std::vector<int32_t> drawLayer, drawLayerTexture; // per frame.draws slot: index into layers / layerTextures (-1: none)
    // Per slot the placement picked by classifyMixedDraws (kMenuNone: the layer rules apply), the box of the draw.
    constexpr uint8_t kMenuNone = 0xFFu, kMenuBleedLeft = 0x20u, kMenuBleedRight = 0x40u, kMenuPlacementMask = 0x0Fu;
    static std::vector<uint8_t> drawMenu;
    static std::vector<std::array<float, 4>> drawBox; // x0, x1, y0, y1 of the draw's vertices
    // Panels of a strip texture: the connected groups of its draws (the pieces of a 9-slice box). A panel that
    // runs across the screen is the dialogue box, any other (the choice box) is an ordinary element.
    struct StripPanel
    {
        float z, x0, x1, y0, y1;
        bool spanning;
    };
    static std::vector<StripPanel> stripPanels;
    static std::vector<int32_t> drawStripPanel; // per slot: index into stripPanels (-1: not a piece of one)
    static std::vector<int32_t> panelRoot, panelSlots;
    layerTextureTable.assign(1024, -1);
    const auto layerTextureSlot = [&](float z, const std::string &name) {
        uint32_t zBits;
        std::memcpy(&zBits, &z, sizeof(zBits));
        return static_cast<size_t>((std::hash<std::string_view>{}(name) ^ (zBits * 2654435761u)) & (layerTextureTable.size() - 1));
    };
    const auto findLayerTextureIndex = [&](float z, const std::string &name) -> int32_t {
        for (size_t i = layerTextureSlot(z, name);; i = (i + 1) & (layerTextureTable.size() - 1))
        {
            const int32_t index = layerTextureTable[i];
            if (index < 0) return -1;
            if (layerTextures[static_cast<size_t>(index)].z == z && *layerTextures[static_cast<size_t>(index)].name == name)
                return index;
        }
    };
    bool anyMixed = false, polygonBackdrop = false, menuFrame = false;
    if (widescreen == 2)
    {
        drawLayer.assign(frame.draws.size(), -1);
        drawLayerTexture.assign(frame.draws.size(), -1);
        drawBox.assign(frame.draws.size(), {0.0f, 0.0f, 0.0f, 0.0f});
        bool inTarget = false, previousDepthClear = false, depthClearPair = false;
        for (size_t slot = 0; slot < frame.draws.size(); ++slot)
        {
            const Draw &draw = frame.draws[slot];
            if (const auto *target = std::get_if<TargetDraw>(&draw))
                inTarget = target->begin;
            // The menu pass of the game starts with two depth clears in a row, and its backdrop made of polygons
            // is drawn with a camera of its own after them (the world's camera was set before).
            if (const auto *clearDraw = std::get_if<ClearDraw>(&draw))
            {
                const bool depthOnly = clearDraw->depthBuffer && !clearDraw->colorBuffer;
                depthClearPair = depthClearPair || (depthOnly && previousDepthClear && !inTarget);
                previousDepthClear = depthOnly;
                continue;
            }
            previousDepthClear = false;
            if (depthClearPair && !inTarget && std::holds_alternative<CameraDraw>(draw))
                polygonBackdrop = true;
            const auto *screen = inTarget ? nullptr : std::get_if<ScreenDraw>(&draw);
            if (!screen || screen->vertices.empty())
                continue;
            const float z = screen->vertices[0].z;
            int32_t layerIndex = -1;
            for (size_t i = 0; i < layers.size(); ++i)
                if (layers[i].z == z)
                {
                    layerIndex = static_cast<int32_t>(i);
                    break;
                }
            if (layerIndex < 0)
            {
                layers.push_back({z, 1e9f, -1e9f, screen->sourceWidth});
                layerIndex = static_cast<int32_t>(layers.size() - 1);
            }
            ScreenLayer *layer = &layers[static_cast<size_t>(layerIndex)];
            int32_t textureIndex = -1;
            if (!screen->textureName.empty())
            {
                textureIndex = findLayerTextureIndex(z, screen->textureName);
                if (textureIndex < 0)
                {
                    layerTextures.push_back({z, 1e9f, -1e9f, &screen->textureName});
                    textureIndex = static_cast<int32_t>(layerTextures.size() - 1);
                    for (size_t i = layerTextureSlot(z, screen->textureName);; i = (i + 1) & (layerTextureTable.size() - 1))
                        if (layerTextureTable[i] < 0)
                        {
                            layerTextureTable[i] = textureIndex;
                            break;
                        }
                }
            }
            LayerTexture *texture = textureIndex >= 0 ? &layerTextures[static_cast<size_t>(textureIndex)] : nullptr;
            float bx0 = screen->vertices[0].x, bx1 = bx0, by0 = screen->vertices[0].y, by1 = by0;
            for (const ScreenVertex &v : screen->vertices)
            {
                bx0 = std::min(bx0, v.x); bx1 = std::max(bx1, v.x);
                by0 = std::min(by0, v.y); by1 = std::max(by1, v.y);
            }
            layer->x0 = std::min(layer->x0, bx0);
            layer->x1 = std::max(layer->x1, bx1);
            if (texture)
            {
                texture->x0 = std::min(texture->x0, bx0);
                texture->x1 = std::max(texture->x1, bx1);
            }
            drawBox[slot] = {bx0, bx1, by0, by1};
            // Coverage of the source area, for the menu backdrop test (boxes, so a tiled backdrop adds up to 100 %).
            const float cw = std::min(bx1, screen->sourceWidth) - std::max(bx0, 0.0f);
            const float ch = std::min(by1, screen->sourceHeight) - std::max(by0, 0.0f);
            if (cw > 0.0f && ch > 0.0f)
            {
                const bool normalBlend = (screen->state.blend == ScreenBlend::Alpha || screen->state.blend == ScreenBlend::Opaque) &&
                                         !screen->state.alphaOnlyWrite;
                if (texture)
                {
                    texture->area += cw * ch;
                    if (normalBlend) { texture->opaqueArea += cw * ch; texture->opaqueLargest = std::max(texture->opaqueLargest, cw * ch); }
                }
                else
                {
                    layer->untexturedArea += cw * ch;
                    if (normalBlend && screen->vertices[0].color[3] >= 0xF0u)
                    {
                        layer->untexturedSolidArea += cw * ch;
                        layer->untexturedSolidLargest = std::max(layer->untexturedSolidLargest, cw * ch);
                    }
                    layer->untexturedX0 = std::min(layer->untexturedX0, bx0);
                    layer->untexturedX1 = std::max(layer->untexturedX1, bx1);
                }
            }
            drawLayer[slot] = layerIndex;
            drawLayerTexture[slot] = textureIndex;
        }
        stripPanels.clear();
        drawStripPanel.assign(frame.draws.size(), -1);
        for (size_t textureIndex = 0; textureIndex < layerTextures.size(); ++textureIndex)
        {
            if (!isStrip(layerTextures[textureIndex], 640.0f))
                continue;
            // the draws of this texture, joined when their boxes touch
            panelSlots.clear();
            for (size_t slot = 0; slot < frame.draws.size(); ++slot)
                if (drawLayerTexture[slot] == static_cast<int32_t>(textureIndex))
                    panelSlots.push_back(static_cast<int32_t>(slot));
            if (panelSlots.size() > 192u)
                continue;
            panelRoot.resize(panelSlots.size());
            for (size_t i = 0; i < panelRoot.size(); ++i)
                panelRoot[i] = static_cast<int32_t>(i);
            const auto find = [&](int32_t i) {
                while (panelRoot[static_cast<size_t>(i)] != i)
                    i = panelRoot[static_cast<size_t>(i)] = panelRoot[static_cast<size_t>(panelRoot[static_cast<size_t>(i)])];
                return i;
            };
            // Pieces of one box lie edge to edge (they touch on one axis and overlap on the other); a box drawn on
            // top of another one, or its shadow offset by a few units, overlaps on both and is another panel.
            constexpr float kTouch = 1.5f;
            for (size_t i = 0; i < panelSlots.size(); ++i)
                for (size_t j = i + 1; j < panelSlots.size(); ++j)
                {
                    const auto &a = drawBox[static_cast<size_t>(panelSlots[i])], &b = drawBox[static_cast<size_t>(panelSlots[j])];
                    const float overlapX = std::min(a[1], b[1]) - std::max(a[0], b[0]);
                    const float overlapY = std::min(a[3], b[3]) - std::max(a[2], b[2]);
                    if (overlapX >= -kTouch && overlapY >= -kTouch && (overlapX <= kTouch || overlapY <= kTouch))
                        panelRoot[static_cast<size_t>(find(static_cast<int32_t>(i)))] = find(static_cast<int32_t>(j));
                }
            static std::vector<int32_t> rootPanel;
            rootPanel.assign(panelSlots.size(), -1);
            for (size_t i = 0; i < panelSlots.size(); ++i)
            {
                const int32_t root = find(static_cast<int32_t>(i));
                const auto &box = drawBox[static_cast<size_t>(panelSlots[i])];
                int32_t &panel = rootPanel[static_cast<size_t>(root)];
                if (panel < 0)
                {
                    stripPanels.push_back({layerTextures[textureIndex].z, box[0], box[1], box[2], box[3], false});
                    panel = static_cast<int32_t>(stripPanels.size() - 1);
                }
                StripPanel &group = stripPanels[static_cast<size_t>(panel)];
                group.x0 = std::min(group.x0, box[0]); group.x1 = std::max(group.x1, box[1]);
                group.y0 = std::min(group.y0, box[2]); group.y1 = std::max(group.y1, box[3]);
                drawStripPanel[static_cast<size_t>(panelSlots[i])] = panel;
            }
            for (size_t i = 0; i < rootPanel.size(); ++i)
                if (rootPanel[i] >= 0)
                {
                    StripPanel &group = stripPanels[static_cast<size_t>(rootPanel[i])];
                    group.spanning = group.x0 >= -0.5f && group.x0 <= 0.15f * 640.0f && group.x1 >= 0.85f * 640.0f && group.x1 <= 640.5f;
                }
        }
        for (const StripPanel &panel : stripPanels)
            if (panel.spanning)
                for (ScreenLayer &layer : layers)
                    if (layer.z == panel.z)
                        layer.hasStrip = true;
        // A menu frame: no world shows behind the content. Either the backdrop is made of polygons (see above) or
        // a textured screen draw (several, tiled) covers the whole screen and blends normally. The fades and
        // masks that cover the screen in the field and in battle are not that.
        menuFrame = polygonBackdrop;
        for (const LayerTexture &texture : layerTextures)
            menuFrame = menuFrame || (texture.opaqueArea >= 0.7f * 640.0f * 448.0f && texture.opaqueLargest >= 0.3f * 640.0f * 448.0f && texture.x0 <= 0.15f * 640.0f && texture.x1 >= 0.85f * 640.0f);
        for (const ScreenLayer &layer : layers) // a flat solid backdrop (the save screen); a fade is not solid until it is complete
            menuFrame = menuFrame || (layer.untexturedSolidArea >= 0.7f * 640.0f * 448.0f && layer.untexturedSolidLargest >= 0.3f * 640.0f * 448.0f && layer.untexturedX0 <= 0.15f * 640.0f && layer.untexturedX1 >= 0.85f * 640.0f);
        for (ScreenLayer &layer : layers)
        {
            const bool left = layer.x0 <= 0.5f, right = layer.x1 >= layer.width - 0.5f;
            layer.mixed = menuFrame || (left && right);
            anyMixed = anyMixed || layer.mixed;
        }
        drawMenu.assign(frame.draws.size(), kMenuNone);
    }
    // Mixed layers, draw by draw (after the texture index exists: the flat-edge test reads texels).
    const auto classifyMixedDraws = [&] {
        using Placement = Renderer::ScreenPlacement;
        // Is the column of texels at the quad's left (right) side, where it meets the edge of the source area (or
        // its own end when that lies inside), one flat colour? Then the quad can be extended sideways to the
        // edge of the screen by repeating that column (a bar or a panel flush with the edge); character art and
        // lettering are not flat and are pinned to the edge instead.
        const auto flatEdge = [&](const ScreenDraw &screen, bool left) {
            const auto &v = screen.vertices;
            if (v.size() != 6u || screen.lines) return false;
            const TextureDefinition *definition = screen.textureName.empty() ? nullptr : findHostTexture(screen.textureName);
            if (!screen.textureName.empty() && (!definition || definition->texture.rgba.size() < 4u)) return false;
            float xl = v[0].x, xr = v[0].x, yt = v[0].y, yb = v[0].y;
            for (const ScreenVertex &p : v)
            {
                xl = std::min(xl, p.x); xr = std::max(xr, p.x);
                yt = std::min(yt, p.y); yb = std::max(yb, p.y);
            }
            if (xr - xl < 1.0f) return false;
            float uL = 0.0f, uR = 0.0f, vTop = 0.0f, vBottom = 0.0f;
            for (const ScreenVertex &p : v)
            {
                if (std::fabs(p.x - xl) > 0.01f && std::fabs(p.x - xr) > 0.01f) return false; // not an axis-aligned quad
                if (std::fabs(p.y - yt) > 0.01f && std::fabs(p.y - yb) > 0.01f) return false;
                if (std::fabs(p.x - xl) <= 0.01f) uL = p.u; else uR = p.u;
                if (std::fabs(p.x - (left ? xl : xr)) <= 0.01f)
                {
                    if (std::fabs(p.y - yt) <= 0.01f) vTop = p.v; else vBottom = p.v;
                }
            }
            if (!definition) return true; // untextured: vertex colour only
            const float edge = left ? std::max(xl, 0.0f) : std::min(xr, screen.sourceWidth);
            const float edgeU = uL + (edge - xl) / (xr - xl) * (uR - uL);
            const TextureData &tex = definition->texture;
            const int w = static_cast<int>(tex.width), h = static_cast<int>(tex.height);
            if (w <= 0 || h <= 0 || tex.rgba.size() < static_cast<size_t>(w) * h * 4u) return false;
            // Repeating the edge column is invisible when the texels next to it, further inside, are the same
            // (a bar's rows may differ from each other: its top and bottom edges are soft).
            const int px = std::clamp(static_cast<int>(edgeU * w + (left ? 0.5f : -0.5f)), 0, w - 1);
            static const bool debugFlat = std::getenv("P3FES_WS_DEBUG") != nullptr;
            const float uSign = uR >= uL ? 1.0f : -1.0f;
            const int inward = (left ? 1 : -1) * (uSign > 0.0f ? 1 : -1);
            // (a quad that maps one column of texels over its whole width is a bar the game itself stretches that way)
            const float texelsPerUnit = std::fabs(uR - uL) * static_cast<float>(w) / (xr - xl);
            const int step = std::max(1, static_cast<int>(std::lround(4.0f * texelsPerUnit)));
            int worst = texelsPerUnit * (xr - xl) <= 1.0f ? -1 : 0;
            for (int s = 0; s < 8 && worst >= 0; ++s)
            {
                const float t = (s + 0.5f) / 8.0f;
                const int py = std::clamp(static_cast<int>((vTop + (vBottom - vTop) * t) * h), 0, h - 1);
                const uint8_t *edgeTexel = &tex.rgba[(static_cast<size_t>(py) * w + px) * 4u];
                const int pxIn = std::clamp(px + inward * step, 0, w - 1);
                const uint8_t *inner = &tex.rgba[(static_cast<size_t>(py) * w + pxIn) * 4u];
                if (edgeTexel[3] < 24 && inner[3] < 24) continue; // two transparent texels are the same whatever their colour
                for (int c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<int>(edgeTexel[c]) - static_cast<int>(inner[c])));
            }
            const bool flat = worst <= 24;
            if (debugFlat)
                std::fprintf(stderr, "[p3fes-ws] flatEdge %s tex=%s %dx%d edgeU=%.4f px=%d v=%.3f..%.3f xl=%.1f xr=%.1f worst=%d -> %d\n", left ? "left" : "right",
                             screen.textureName.c_str(), w, h, edgeU, px, vTop, vBottom, xl, xr, worst, flat ? 1 : 0);
            return flat;
        };
        // Spinning art made of several pieces (the shop menu's disc: four rotated quads of one texture around one
        // centre). The box of a rotated piece changes with the angle, so the per-draw rules below (a backdrop
        // by extent, a piece touching an edge pinned to it) gave different pieces of the same disc different
        // placements, and the placement changed again as the disc turned: the disc tore along the seams of its
        // pieces, the viewports of the placements cut it with straight edges, and it flickered. The pieces
        // overlapping each other are placed as one: pinned to the edge their joint box touches (both: stretched),
        // centred otherwise. A cluster is the same on every frame of the spin: the box of the pieces
        // always reaches the same edges, as the turn is about the centre.
        static const bool placeSpinningPieces = [] { const char *v = std::getenv("P3FES_WS_SPINNING"); return !v || *v != '0'; }(); // =0: per-draw rules only
        if (menuFrame && placeSpinningPieces)
        {
            constexpr size_t kMaxPieces = 48u;
            constexpr float kJoin = 6.0f;
            static std::vector<int32_t> pieces, pieceRoot;
            pieces.clear();
            for (size_t slot = 0; slot < frame.draws.size() && pieces.size() <= kMaxPieces; ++slot)
            {
                const auto *screen = std::get_if<ScreenDraw>(&frame.draws[slot]);
                if (!screen || screen->lines || screen->vertices.size() != 6u || screen->textureName.empty() || drawLayer[slot] < 0 ||
                    !layers[static_cast<size_t>(drawLayer[slot])].mixed)
                    continue;
                const auto &v = screen->vertices;
                float xl = v[0].x, xr = xl, yt = v[0].y, yb = yt;
                for (const ScreenVertex &p : v)
                {
                    xl = std::min(xl, p.x); xr = std::max(xr, p.x);
                    yt = std::min(yt, p.y); yb = std::max(yb, p.y);
                }
                bool rotated = false;
                for (const ScreenVertex &p : v)
                    rotated = rotated || (std::fabs(p.x - xl) > 0.01f && std::fabs(p.x - xr) > 0.01f) ||
                              (std::fabs(p.y - yt) > 0.01f && std::fabs(p.y - yb) > 0.01f);
                if (rotated)
                    pieces.push_back(static_cast<int32_t>(slot));
            }
            if (pieces.size() >= 2u && pieces.size() <= kMaxPieces)
            {
                pieceRoot.resize(pieces.size());
                for (size_t i = 0; i < pieces.size(); ++i)
                    pieceRoot[i] = static_cast<int32_t>(i);
                const auto root = [&](int32_t i) {
                    while (pieceRoot[static_cast<size_t>(i)] != i)
                        i = pieceRoot[static_cast<size_t>(i)];
                    return i;
                };
                for (size_t i = 0; i < pieces.size(); ++i)
                    for (size_t j = i + 1; j < pieces.size(); ++j)
                    {
                        const size_t sa = static_cast<size_t>(pieces[i]), sb = static_cast<size_t>(pieces[j]);
                        const auto &a = drawBox[sa], &b = drawBox[sb];
                        if (drawLayer[sa] == drawLayer[sb] &&
                            std::get<ScreenDraw>(frame.draws[sa]).textureName == std::get<ScreenDraw>(frame.draws[sb]).textureName &&
                            a[0] <= b[1] + kJoin && b[0] <= a[1] + kJoin && a[2] <= b[3] + kJoin && b[2] <= a[3] + kJoin)
                            pieceRoot[static_cast<size_t>(root(static_cast<int32_t>(i)))] = root(static_cast<int32_t>(j));
                    }
                for (size_t i = 0; i < pieces.size(); ++i)
                {
                    const int32_t r = root(static_cast<int32_t>(i));
                    float x0 = 1e9f, x1 = -1e9f;
                    size_t members = 0u;
                    for (size_t j = 0; j < pieces.size(); ++j)
                        if (root(static_cast<int32_t>(j)) == r)
                        {
                            x0 = std::min(x0, drawBox[static_cast<size_t>(pieces[j])][0]);
                            x1 = std::max(x1, drawBox[static_cast<size_t>(pieces[j])][1]);
                            ++members;
                        }
                    if (members < 2u)
                        continue;
                    const float sw = std::get<ScreenDraw>(frame.draws[static_cast<size_t>(pieces[i])]).sourceWidth;
                    const bool touchLeft = x0 <= 0.5f, touchRight = x1 >= sw - 0.5f;
                    drawMenu[static_cast<size_t>(pieces[i])] = static_cast<uint8_t>(
                        touchLeft && touchRight ? Placement::Stretch : touchLeft ? Placement::Left : touchRight ? Placement::Right : Placement::Centre);
                }
            }
        }
        for (size_t slot = 0; slot < frame.draws.size(); ++slot)
        {
            const auto *screen = std::get_if<ScreenDraw>(&frame.draws[slot]);
            if (!screen || screen->vertices.empty() || drawLayer[slot] < 0)
                continue;
            const ScreenLayer &layer = layers[static_cast<size_t>(drawLayer[slot])];
            if (!layer.mixed || drawMenu[slot] != kMenuNone)
                continue;
            const LayerTexture *texture = drawLayerTexture[slot] >= 0 ? &layerTextures[static_cast<size_t>(drawLayerTexture[slot])] : nullptr;
            const float sw = screen->sourceWidth, sh = screen->sourceHeight;
            const float x0 = drawBox[slot][0], x1 = drawBox[slot][1];
            Placement placement = Placement::Centre;
            uint8_t flags = 0;
            // A backdrop: draws of one texture (or all untextured ones of the layer) that cover the screen
            // between them, edge to edge. Flat, gradient or abstract: stretching it is the right thing.
            const float coverage = texture ? texture->area : layer.untexturedArea;
            const float gx0 = texture ? texture->x0 : layer.untexturedX0, gx1 = texture ? texture->x1 : layer.untexturedX1;
            if (coverage >= 0.7f * sw * sh && gx0 <= 0.15f * sw && gx1 >= 0.85f * sw &&
                (!menuFrame || (x1 - x0) * (drawBox[slot][3] - drawBox[slot][2]) >= 0.1f * sw * sh)) // in a menu not the small pieces drawn at the same Z
                placement = Placement::Stretch;
            else if (x0 <= 0.02f * sw && x1 >= 0.98f * sw) // a strip across the whole screen, edge to edge (its end caps may sit a few units in)
                placement = Placement::Stretch;
            else if (!menuFrame)
            {
                // HUD: placed by cluster below, except the sprites that follow the 3-D world (light and glow
                // quads: a small texture magnified several times, or a render target), whose positions were
                // projected with the widened camera: they move with the stretched picture.
                bool worldSprite = screen->textureName.compare(0, 3, "rt.") == 0;
                if (!worldSprite && !screen->textureName.empty())
                    if (const TextureDefinition *definition = findHostTexture(screen->textureName))
                        if (definition->texture.width >= 2u && definition->texture.height >= 2u)
                            worldSprite = (x1 - x0) >= 4.0f * static_cast<float>(definition->texture.width) &&
                                          (drawBox[slot][3] - drawBox[slot][2]) >= 4.0f * static_cast<float>(definition->texture.height);
                if (worldSprite)
                    drawMenu[slot] = static_cast<uint8_t>(Placement::Stretch);
                continue;
            }
            else if (x1 > 0.5f && x0 < sw - 0.5f)
            {
                if (x0 <= 0.5f) // touches or overhangs the left edge
                {
                    if (flatEdge(*screen, true)) flags = kMenuBleedLeft; // stays centred (clipped to the source area)
                    else placement = Placement::Left;
                }
                else if (x1 >= sw - 0.5f)
                {
                    if (flatEdge(*screen, false)) flags = kMenuBleedRight;
                    else placement = Placement::Right;
                }
            }
            drawMenu[slot] = static_cast<uint8_t>(static_cast<uint8_t>(placement) | flags);
        }
        if (menuFrame)
            return;
        // HUD (the world shows behind): a layer that reaches both edges is several elements, not one. What is
        // left after the backdrops and strips is grouped into clusters (draws whose boxes overlap or touch: a
        // gauge and its icon, the command ring and its labels), and each cluster goes to the side it is on:
        // left, right, or centred when it is in the middle or as wide as the screen.
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex)
        {
            if (!layers[layerIndex].mixed)
                continue;
            panelSlots.clear();
            for (size_t slot = 0; slot < frame.draws.size(); ++slot)
                if (drawLayer[slot] == static_cast<int32_t>(layerIndex) && drawMenu[slot] == kMenuNone)
                    panelSlots.push_back(static_cast<int32_t>(slot));
            if (panelSlots.empty() || panelSlots.size() > 512u)
                continue;
            panelRoot.resize(panelSlots.size());
            for (size_t i = 0; i < panelRoot.size(); ++i)
                panelRoot[i] = static_cast<int32_t>(i);
            const auto find = [&](int32_t i) {
                while (panelRoot[static_cast<size_t>(i)] != i)
                    i = panelRoot[static_cast<size_t>(i)] = panelRoot[static_cast<size_t>(panelRoot[static_cast<size_t>(i)])];
                return i;
            };
            constexpr float kJoin = 6.0f;
            for (size_t i = 0; i < panelSlots.size(); ++i)
                for (size_t j = i + 1; j < panelSlots.size(); ++j)
                {
                    const auto &a = drawBox[static_cast<size_t>(panelSlots[i])], &b = drawBox[static_cast<size_t>(panelSlots[j])];
                    if (a[0] <= b[1] + kJoin && b[0] <= a[1] + kJoin && a[2] <= b[3] + kJoin && b[2] <= a[3] + kJoin)
                        panelRoot[static_cast<size_t>(find(static_cast<int32_t>(i)))] = find(static_cast<int32_t>(j));
                }
            static std::vector<std::array<float, 2>> clusterX; // x0, x1 per root
            clusterX.assign(panelSlots.size(), {1e9f, -1e9f});
            for (size_t i = 0; i < panelSlots.size(); ++i)
            {
                auto &extent = clusterX[static_cast<size_t>(find(static_cast<int32_t>(i)))];
                extent[0] = std::min(extent[0], drawBox[static_cast<size_t>(panelSlots[i])][0]);
                extent[1] = std::max(extent[1], drawBox[static_cast<size_t>(panelSlots[i])][1]);
            }
            for (size_t i = 0; i < panelSlots.size(); ++i)
            {
                const auto &extent = clusterX[static_cast<size_t>(find(static_cast<int32_t>(i)))];
                const float sw = layers[layerIndex].width;
                const float visible = std::min(extent[1], sw) - std::max(extent[0], 0.0f);
                const float centre = 0.5f * (extent[0] + extent[1]);
                Placement placement = Placement::Centre;
                if (visible < 0.6f * sw)
                {
                    if (centre < 0.4f * sw) placement = Placement::Left;
                    else if (centre > 0.6f * sw) placement = Placement::Right;
                }
                drawMenu[static_cast<size_t>(panelSlots[i])] = static_cast<uint8_t>(placement);
            }
        }
    };
    // The placement of one screen draw and, for menu draws, which side to extend.
    struct ScreenPlace
    {
        Renderer::ScreenPlacement placement = Renderer::ScreenPlacement::Centre;
        bool bleedLeft = false, bleedRight = false;
    };
    const auto choosePlacement = [&](const ScreenDraw &screen, size_t slot) {
        using Placement = Renderer::ScreenPlacement;
        ScreenPlace result;
        if (slot < drawMenu.size() && drawMenu[slot] != kMenuNone)
        {
            result.placement = static_cast<Placement>(drawMenu[slot] & kMenuPlacementMask);
            result.bleedLeft = (drawMenu[slot] & kMenuBleedLeft) != 0;
            result.bleedRight = (drawMenu[slot] & kMenuBleedRight) != 0;
            return result;
        }
        Placement placement = Placement::Centre;
        if (!screen.vertices.empty() && slot < drawLayer.size() && drawLayer[slot] >= 0)
        {
            const ScreenLayer &layer = layers[static_cast<size_t>(drawLayer[slot])];
            const bool left = layer.x0 <= 0.5f, right = layer.x1 >= layer.width - 0.5f;
            // Only a compact layer is a corner element (location banner, date panel).
            const float visible = std::min(layer.x1, layer.width) - std::max(layer.x0, 0.0f);
            const bool compact = visible <= 0.6f * layer.width;
            if (left && right)
                placement = Placement::Stretch;
            else if (left && compact)
                placement = Placement::Left;
            else if (right && compact)
                placement = Placement::Right;
            else if (layer.hasStrip)
            {
                // A wide panel with content on it (the dialogue box): the panel's own
                // texture is stretched across the screen, what hangs over the right edge
                // (the speaker's portrait) is pinned to the right undeformed, and the rest
                // (name and text glyphs) keeps its shape and follows the panel's left end.
                // A panel of the same texture that does not run across the screen (the choice box) and what
                // is inside it stay together, centred.
                const int32_t piece = slot < drawStripPanel.size() ? drawStripPanel[slot] : -1;
                float x1 = screen.vertices[0].x, y0 = screen.vertices[0].y, y1 = y0, x0 = x1;
                for (const ScreenVertex &v : screen.vertices)
                {
                    x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
                    y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
                }
                bool inLoosePanel = false;
                if (piece < 0)
                    for (const StripPanel &panel : stripPanels)
                        if (!panel.spanning && panel.z == layer.z && 0.5f * (x0 + x1) >= panel.x0 && 0.5f * (x0 + x1) <= panel.x1 &&
                            0.5f * (y0 + y1) >= panel.y0 && 0.5f * (y0 + y1) <= panel.y1)
                            inLoosePanel = true;
                if (piece >= 0 && stripPanels[static_cast<size_t>(piece)].spanning)
                    placement = Placement::Stretch;
                else if (piece >= 0 || inLoosePanel)
                    placement = Placement::Centre;
                else if (x1 > layer.width + 0.5f)
                    placement = Placement::Right;
                else
                    placement = Placement::Left;
            }
        }
        result.placement = placement;
        return result;
    };
    // Extends the quad `screen` sideways to the edge of the framebuffer by repeating its column of texels at the
    // edge of the source area (the sliver is one column wide and reaches far out; Wide placement shows as much of
    // it as there is room for). The quad itself is drawn centred and clipped to the source area.
    const auto drawBleed = [&](const ScreenDraw &screen, bool left) {
        static std::vector<ScreenVertex> sliver;
        sliver = screen.vertices;
        float xl = sliver[0].x, xr = sliver[0].x;
        for (const ScreenVertex &v : sliver)
        {
            xl = std::min(xl, v.x);
            xr = std::max(xr, v.x);
        }
        if (xr - xl < 1.0f) return;
        float uL = 0.0f, uR = 0.0f;
        for (const ScreenVertex &v : screen.vertices)
            (std::fabs(v.x - xl) <= 0.01f ? uL : uR) = v.u;
        const float edge = left ? std::max(xl, 0.0f) : std::min(xr, screen.sourceWidth);
        float edgeU = uL + (edge - xl) / (xr - xl) * (uR - uL);
        // half a texel inside, so that point sampling (and the atlas neighbours of a small texture) cannot reach past the column
        if (const TextureDefinition *definition = screen.textureName.empty() ? nullptr : findHostTexture(screen.textureName))
            if (definition->texture.width > 0)
            {
                const float half = 0.5f / static_cast<float>(definition->texture.width);
                const bool increasing = uR >= uL;
                edgeU += ((left == increasing) ? half : -half);
            }
        const float far = left ? edge - 1000.0f : edge + 1000.0f;
        for (ScreenVertex &v : sliver)
        {
            const bool onEdgeSide = std::fabs(v.x - (left ? xl : xr)) <= 0.01f;
            // the colour is that of the vertex on the edge side of the same row (a vertical gradient stays one)
            const ScreenVertex *row = &v;
            for (const ScreenVertex &e : screen.vertices)
                if (std::fabs(e.x - (left ? xl : xr)) <= 0.01f && std::fabs(e.y - v.y) <= 0.01f) { row = &e; break; }
            v.color = row->color;
            v.u = edgeU;
            // outer side (reaching out) and inner side (on the edge of the source area)
            v.x = onEdgeSide ? far : edge;
        }
        g_renderer->setScreenPlacement(Renderer::ScreenPlacement::Wide);
        g_renderer->drawScreenTriangles(sliver, screen.sourceWidth, screen.sourceHeight, screen.textureName, screen.state);
    };
    lap(g_timing.prepass);
    const auto placeScreenDraw = [&](const ScreenDraw &screen, size_t slot) {
        const ScreenPlace place = choosePlacement(screen, slot);
        if (place.bleedLeft && !screen.lines) drawBleed(screen, true);
        if (place.bleedRight && !screen.lines) drawBleed(screen, false);
        g_renderer->setScreenPlacement(place.placement);
    };
    const FrameDiag diag = frameDiagFor(g_presentedFrames);
    if (diag.capture) requestCapture();
#if defined(PS2X_P3FES_FRAME_TRACE)
    const uint64_t texturePreloadStartNs = traceFrame ? p3fes_frame_trace::nowNs() : 0u;
    const uint64_t textureUploadsBefore = g_textureUploads;
#endif
    buildHostTextureIndex();
    if (widescreen == 2 && anyMixed)
        classifyMixedDraws();
    if (diag.dump)
    {
        static const char *const kNames[] = {"centre", "left", "right", "stretch", "wide"};
        dumpFrame(frame, [&](const ScreenDraw &screen, size_t slot) {
            if (widescreen != 2) return std::string();
            const ScreenPlace place = choosePlacement(screen, slot);
            std::string text = kNames[static_cast<size_t>(place.placement)];
            if (place.bleedLeft) text += "+bleedL";
            if (place.bleedRight) text += "+bleedR";
            if (slot < drawMenu.size() && drawMenu[slot] != kMenuNone) text += "(mixed)";
            return text;
        });
        if (widescreen == 2)
            std::fprintf(stderr, "[p3fes-frame] menuFrame=%d polygonBackdrop=%d mixedLayers=%d\n", menuFrame ? 1 : 0, polygonBackdrop ? 1 : 0,
                         anyMixed ? 1 : 0);
    }
    for (const TextureDefinition *definition : g_hostTextureList)
        ensureTexture(definition->texture.name);
#if defined(PS2X_P3FES_FRAME_TRACE)
    if (traceFrame)
        p3fes_frame_trace::texturePreload(traceHandle,
            p3fes_frame_trace::nowNs() - texturePreloadStartNs,
            g_textureUploads - textureUploadsBefore);
#endif
    lap(g_timing.preload);
    uint64_t transientIndex = 0u;
    g_lastFrameDraws = static_cast<uint32_t>(frame.draws.size());
    g_lastFrameMeshes = g_lastFrameColorClearsAfterMesh = 0u;
    g_lastFrameCamera = frame.camera;
    // Two replays of the same list: first only the render-to-texture brackets (before the display
    // frame begins, so the display scene is never split), then everything outside them.
    const auto runDraws = [&](const bool targetPass) {
#if defined(PS2X_P3FES_FRAME_TRACE)
    uint32_t drawIndex = 0;
#endif
    bool inTarget = false;
    size_t drawSlot = static_cast<size_t>(-1);
    for (const Draw &draw : frame.draws)
    {
        ++drawSlot;
        if (const auto *target = std::get_if<TargetDraw>(&draw))
        {
            if (targetPass)
            {
                if (inTarget) g_renderer->endTextureTarget();
                if (target->begin)
                    g_renderer->beginTextureTarget(target->name, static_cast<int>(target->width),
                                                   static_cast<int>(target->height));
            }
            inTarget = target->begin;
#if defined(PS2X_P3FES_FRAME_TRACE)
            ++drawIndex;
#endif
            continue;
        }
        if (inTarget != targetPass)
        {
#if defined(PS2X_P3FES_FRAME_TRACE)
            ++drawIndex;
#endif
            continue;
        }
#if defined(PS2X_P3FES_FRAME_TRACE)
        const bool traceDraw = traceFrame;
        if (traceDraw) p3fes_frame_trace::beginDraw(traceHandle, drawInfo(frame, draw, drawIndex));
        const uint64_t ensureStartNs = traceDraw ? p3fes_frame_trace::nowNs() : 0u;
        uint64_t submitStartNs = 0u;
#endif
        const uint64_t drawStartUs = timing ? nowUs() : 0u;
        if (const auto *mesh = std::get_if<MeshDraw>(&draw); mesh || std::holds_alternative<SharedMeshDraw>(draw))
        {
            // MeshDraw and SharedMeshDraw replay the same way; the shared one draws its retained state, with the colour
            // override patched in when the draw has one.
            const SharedMeshDraw *shared = mesh ? nullptr : &std::get<SharedMeshDraw>(draw);
            const MeshId meshId = mesh ? mesh->id : shared->id;
            ++g_lastFrameMeshes;
            // P3FES_SKIP_MESH=<hex id>[,<hex id>...]: leave those meshes out (to find which draw paints what).
            static const std::vector<MeshId> skipped = [] {
                std::vector<MeshId> ids;
                if (const char *list = std::getenv("P3FES_SKIP_MESH"))
                    for (const char *cursor = list; *cursor;)
                    {
                        char *end = nullptr;
                        const unsigned long long id = std::strtoull(cursor, &end, 16);
                        if (end == cursor) break;
                        ids.push_back(id);
                        cursor = *end == ',' ? end + 1 : end;
                    }
                return ids;
            }();
            if (!skipped.empty() && std::find(skipped.begin(), skipped.end(), meshId) != skipped.end())
                continue;
            if (timing) ++g_timing.meshDraws;
            const bool meshReady = ensureMesh(meshId);
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            if (meshReady)
            {
                if (mesh)
                    g_renderer->drawMesh(mesh->id, mesh->model, mesh->state);
                else if (shared->colorCount)
                {
                    // retained state with this draw's own material colours: patch a copy (host thread only)
                    static MeshDrawState patched;
                    patched = *shared->state;
                    for (uint32_t i = 0; i < shared->colorCount && i < patched.materialColors.size(); ++i)
                        std::memcpy(patched.materialColors[i].data(), &shared->colorOverride[i], 4);
                    g_renderer->drawMesh(shared->id, shared->model, patched);
                }
                else
                    g_renderer->drawMesh(shared->id, shared->model, *shared->state);
            }
        }
        else if (const auto *transient = std::get_if<TransientMeshDraw>(&draw))
        {
            ++g_lastFrameMeshes;
            bool texturesReady = true;
            for (const Material &material : transient->mesh.materials)
                if (!ensureTexture(material.textureName))
                    texturesReady = false;
            bool uploaded = false;
            MeshId id = 0;
            if (texturesReady)
            {
                // Bit 62 is outside the persistent atomic/sector ID spaces used by
                // this port. The mesh is removed immediately after the draw.
                id = (1ull << 62u) | ((frame.sequence & 0x3FFFFFFFull) << 20u) |
                     (transientIndex++ & 0xFFFFFull);
                std::string error;
                uploaded = g_renderer->uploadMesh(id, transient->mesh, error);
                if (!uploaded && g_errors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] transient mesh upload failed: %s\n", error.c_str());
            }
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            if (uploaded)
            {
                g_renderer->drawMesh(id, transient->model, transient->state);
                g_renderer->removeMesh(id);
            }
            if (timing) ++g_timing.meshDraws;
        }
        else if (const auto *lines = std::get_if<WorldLineDraw>(&draw))
        {
            const bool textureReady = ensureTexture(lines->textureName);
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            if (textureReady)
                g_renderer->drawWorldLines(lines->vertices, lines->model, lines->textureName, lines->state);
            if (timing) ++g_timing.meshDraws;
        }
        else if (const auto *screen = std::get_if<ScreenDraw>(&draw))
        {
            const bool textureReady = ensureTexture(screen->textureName);
            const uint64_t ensuredUs = timing ? nowUs() : 0u;
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            uint64_t placedUs = ensuredUs;
            if (textureReady)
            {
                if (widescreen == 2 && !targetPass) placeScreenDraw(*screen, drawSlot);
                if (timing) placedUs = nowUs();
                if (screen->lines)
                    g_renderer->drawScreenLines(screen->vertices, screen->sourceWidth, screen->sourceHeight,
                                                screen->textureName, screen->state);
                else
                    g_renderer->drawScreenTriangles(screen->vertices, screen->sourceWidth, screen->sourceHeight,
                                                    screen->textureName, screen->state);
            }
            if (timing && !targetPass)
            {
                const uint64_t doneUs = nowUs();
                g_timing.screenEnsure += ensuredUs - drawStartUs;
                g_timing.screenPlace += placedUs - ensuredUs;
                g_timing.screenDraw += doneUs - placedUs;
                ++g_timing.screenDraws;
                g_timing.screenVertices += static_cast<uint32_t>(screen->vertices.size());
            }
        }
        else if (const auto *clear = std::get_if<ClearDraw>(&draw))
        {
            if (clear->colorBuffer && g_lastFrameMeshes) ++g_lastFrameColorClearsAfterMesh;
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            g_renderer->clear(clear->color, clear->colorBuffer, clear->depthBuffer);
        }
        else if (const auto *camera = std::get_if<CameraDraw>(&draw))
        {
#if defined(PS2X_P3FES_FRAME_TRACE)
            if (traceDraw) submitStartNs = p3fes_frame_trace::nowNs();
#endif
            g_renderer->setCamera(camera->camera);
            g_lastFrameCamera = camera->camera;
        }
        if (timing && !targetPass)
        {
            // mesh-like draws and the rest; the screen draws account for themselves
            if (std::holds_alternative<MeshDraw>(draw) || std::holds_alternative<SharedMeshDraw>(draw) ||
                std::holds_alternative<TransientMeshDraw>(draw) || std::holds_alternative<WorldLineDraw>(draw))
                g_timing.meshes += nowUs() - drawStartUs;
            else if (!std::holds_alternative<ScreenDraw>(draw))
                g_timing.other += nowUs() - drawStartUs;
        }
#if defined(PS2X_P3FES_FRAME_TRACE)
        if (traceDraw)
        {
            const uint64_t endNs = p3fes_frame_trace::nowNs();
            const uint64_t submitNs = submitStartNs ? endNs - submitStartNs : 0u;
            const uint64_t ensureNs = submitStartNs ? submitStartNs - ensureStartNs : endNs - ensureStartNs;
            p3fes_frame_trace::endDraw(traceHandle, ensureNs, submitNs);
        }
#endif
#if defined(PS2X_P3FES_FRAME_TRACE)
        ++drawIndex;
#endif
    }
    if (targetPass && inTarget) g_renderer->endTextureTarget();
    };
    timingMark = timing ? nowUs() : 0u;
    if (frame.hasTargets) runDraws(true);
    lap(g_timing.targets);
#if defined(PS2X_P3FES_FRAME_TRACE)
    const uint64_t beginFrameStartNs = traceFrame ? p3fes_frame_trace::nowNs() : 0u;
#endif
    g_renderer->beginFrame(frame.camera, frame.clear);
    lap(g_timing.beginFrame);
#if defined(PS2X_P3FES_FRAME_TRACE)
    const uint64_t beginFrameNs = traceFrame
        ? p3fes_frame_trace::nowNs() - beginFrameStartNs : 0u;
#endif
    runDraws(false);
    timingMark = timing ? nowUs() : 0u;
#if defined(PS2X_P3FES_FRAME_TRACE)
    const uint64_t endFrameStartNs = traceFrame ? p3fes_frame_trace::nowNs() : 0u;
#endif
    g_renderer->endFrame();
    lap(g_timing.endFrame);
#if defined(PS2X_P3FES_FRAME_TRACE)
    const uint64_t endFrameNs = traceFrame
        ? p3fes_frame_trace::nowNs() - endFrameStartNs : 0u;
#endif
    ++g_presentedFrames;
    if (diag.capture) finishCapture();
    // Hand the drawn states (and their vectors' storage) back to the producer: it picks them up in
    // one lock at the next beginFrame instead of allocating per draw.
    std::vector<MeshDrawState> recycled;
    for (Draw &draw : g_hostFrame->draws)
        if (auto *mesh = std::get_if<MeshDraw>(&draw))
        {
            resetForReuse(mesh->state);
            recycled.push_back(std::move(mesh->state));
        }
        else if (auto *transient = std::get_if<TransientMeshDraw>(&draw); transient && recycleTransientStates())
        {
            resetForReuse(transient->state);
            recycled.push_back(std::move(transient->state));
        }
    g_hostFrame.reset();
    lap(g_timing.postFree);
    if (!recycled.empty())
    {
        // Pushed in one go so the lock is held for a few instructions (the vector's storage is the host's).
        std::vector<MeshDrawState> pool;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pool.swap(g_statePool);
        }
        for (MeshDrawState &state : recycled)
            if (pool.size() < kStatePoolLimit)
                pool.push_back(std::move(state));
        std::lock_guard<std::mutex> lock(g_mutex);
        // The producer may have taken (and emptied) the pool meanwhile: put ours back behind what is there now.
        if (g_statePool.empty())
            g_statePool.swap(pool);
        else
            for (MeshDrawState &state : pool)
                if (g_statePool.size() < kStatePoolLimit)
                    g_statePool.push_back(std::move(state));
    }
    recycled.clear();
    collectRetiredTextures();
    lap(g_timing.post);
    if (timing)
    {
        ++g_timing.frames;
        g_timing.total += nowUs() - timingStart;
    }
    reportResources();
    // P3FES_PRESENT_STATS=1: every 10 s, from this (host) thread, how many game frames were
    // submitted, how many of them were replaced before being shown, and how many were presented.
    {
        static const bool stats = [] {
            const char *value = std::getenv("P3FES_PRESENT_STATS");
            return value && *value == '1';
        }();
        if (stats)
        {
            static auto windowStart = std::chrono::steady_clock::now();
            static unsigned presented = 0u, lastSubmitted = 0u, lastSuperseded = 0u;
            ++presented;
            const auto now = std::chrono::steady_clock::now();
            // Interval between consecutive presents in display refreshes (16.68 ms): an even
            // 30 fps is all "2"; a mix of 1 and 3 is the same average shown unevenly.
            static auto lastPresent = now;
            static unsigned intervals[5] = {};
            {
                const double ms = std::chrono::duration<double, std::milli>(now - lastPresent).count();
                const int fields = static_cast<int>(ms / 16.683 + 0.5);
                ++intervals[fields <= 0 ? 0 : fields >= 4 ? 4 : fields];
                lastPresent = now;
            }
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds >= 10.0)
            {
                const unsigned submitted = g_submittedFrames.load(std::memory_order_relaxed);
                const unsigned superseded = g_supersededFrames.load(std::memory_order_relaxed);
                std::fprintf(stderr, "[native-present] stats %.1fs submitted=%u superseded=%u presented=%u intervals 0:%u 1:%u 2:%u 3:%u 4+:%u\n",
                             seconds, submitted - lastSubmitted, superseded - lastSuperseded, presented,
                             intervals[0], intervals[1], intervals[2], intervals[3], intervals[4]);
                for (unsigned &count : intervals) count = 0u;
                lastSubmitted = submitted; lastSuperseded = superseded; presented = 0u; windowStart = now;
            }
        }
    }
#if defined(PS2X_P3FES_FRAME_TRACE)
    if (traceFrame)
    {
        p3fes_frame_trace::resourceActivity(traceHandle,
            g_textureUploads - traceUploadsBefore, g_textureDeletes - traceDeletesBefore);
        p3fes_frame_trace::endRender(traceHandle, beginFrameNs, endFrameNs);
    }
#endif
#if defined(PLATFORM_VITA)
#if defined(PS2X_P3FES_FRAME_TRACE)
    if (traceFrame && p3fes_frame_trace::captureScreenshot(traceHandle))
    {
        sceDisplayWaitVblankStartMulti(2);
        sceClibPrintf("VITA3K_SCREENSHOT p3fes-frame-trace\n");
    }
#endif
    // The local Vita3K accepts named screenshots from sceClibPrintf. Capture
    // early UI frames deterministically: wall-time screenshots can miss the
    // short memory-card check entirely. Enable only for visual checks, since
    // waiting for the display/screenshot deliberately pauses this thread.
    static const bool capture = [] {
        const char *value = std::getenv("P3FES_VITA3K_CAPTURE");
        return value && *value == '1';
    }();
    if (capture)
    {
        static unsigned capturedFrame = 0;
        ++capturedFrame;
        if (capturedFrame == 1 || capturedFrame == 4 || capturedFrame == 8 ||
            capturedFrame == 16 || capturedFrame == 32 || capturedFrame == 64 ||
            capturedFrame == 96 || capturedFrame == 97 || capturedFrame == 128 || capturedFrame == 129 ||
            capturedFrame == 160 || capturedFrame == 161 || capturedFrame == 192 || capturedFrame == 193)
        {
            sceDisplayWaitVblankStartMulti(2);
            sceClibPrintf("VITA3K_SCREENSHOT p3fes-ui-%03u\n", capturedFrame);
        }
    }
#endif
    return true;
}

void shutdownHost()
{
    p3fes_native_movie::shutdownHost();
    if (g_renderer)
        g_renderer->close();
    g_renderer.reset();
    g_uploaded.clear();
    g_uploadedTextures.clear();
    ++g_uploadEpoch;
    g_presentedFrames = g_textureUploads = g_textureDeletes = 0;
    g_textureUploadLogged = false;
    g_textureUpdateLogged = false;
    g_hostFrame.reset();
    g_hostTextureIndex.clear();
    g_hostTextureList.clear();
    g_active.store(false, std::memory_order_release);
    // The producer-private frame and texture table are dropped by the producer itself at its next
    // call (syncProducer), which also clears the shared mesh/retired/state tables.
    g_resetEpoch.fetch_add(1u, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.reset();
    }
    g_hostPresentationPaused.store(false, std::memory_order_release);
    ps2x::gs::setPrimitiveRasterSuppressed(false);
}
} // namespace p3fes_native_present

#endif
