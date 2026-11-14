#include "p3fes_frame_trace.h"
#include "ps2_runtime.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>

namespace p3fes_frame_trace
{
namespace
{
    constexpr uint32_t kNative = 0xF0000000u;
    constexpr size_t kMaxRoutes = 512u, kMaxDraws = 128u, kMaxVertices = 2048u;
    struct Route
    {
        uint32_t target = 0, source = 0;
        uint64_t calls = 0, executorEntries = 0, inclusive = 0, exclusive = 0, checkpoints = 0;
    };
    struct Pass { uint32_t primitive, vertices, bytes; };
    struct Draw { DrawInfo info; std::vector<Pass> passes; };
    PS2Runtime *g_runtime = nullptr; // Set before the executor is started.
    Handle g_producer;             // EE executor only.
    GuestScope *g_scope = nullptr; // EE executor only.
    Handle g_render;               // Presentation thread only.
    bool g_taken = false;          // EE executor only.
    struct Config
    {
        bool enabled = false;
        double after = 145.0;
        uint32_t draws = 72u;
        std::string path = "ux0:data/p3fes/frame-trace.json";
    };
    const Config &config()
    {
        static const Config c = [] {
            Config out;
            const char *on = std::getenv("P3FES_FRAME_TRACE");
            out.enabled = on && on[0] == '1';
            if (const char *s = std::getenv("P3FES_FRAME_TRACE_AFTER_SEC")) out.after = std::max(0.0, std::strtod(s, nullptr));
            if (const char *s = std::getenv("P3FES_FRAME_TRACE_DRAWS")) out.draws = std::strtoul(s, nullptr, 0);
            if (const char *s = std::getenv("P3FES_FRAME_TRACE_PATH")) out.path = s;
            return out;
        }();
        return c;
    }
    bool selected(uint32_t pc)
    {
        return pc >= kNative || (pc >= 0x00460000u && pc < 0x00500000u) ||
               (pc >= 0x00110000u && pc < 0x00116000u) ||
               (pc >= 0x00271000u && pc < 0x00272000u);
    }
    const char *stageName(uint32_t target)
    {
        switch (static_cast<Stage>(target & ~kNative))
        {
        case Stage::TextureResolve: return "native.texture-resolve";
        case Stage::VertexDecode: return "native.vertex-decode";
        case Stage::StateDecode: return "native.state-decode";
        case Stage::PrimitiveExpand: return "native.primitive-expand";
        case Stage::CommandQueue: return "native.command-queue";
        }
        return "native.unknown";
    }
    void quoted(FILE *f, const std::string &s)
    {
        std::fputc('"', f);
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { std::fputc('\\', f); std::fputc(c, f); }
            else if (c < 32u) std::fprintf(f, "\\u%04x", c);
            else std::fputc(c, f);
        }
        std::fputc('"', f);
    }
    void number(FILE *f, float v) { if (std::isfinite(v)) std::fprintf(f, "%.9g", static_cast<double>(v)); else std::fputs("null", f); }
}

struct Frame
{
    uint64_t sequence = 0, started = 0, producerEnd = 0, renderStart = 0, renderEnd = 0;
    uint64_t beginNs = 0, endNs = 0, preloadNs = 0, preloadUploads = 0, uploads = 0, deletes = 0;
    uint64_t guestDispatches = 0, checkpoints = 0, eeCycles = 0;
    uint64_t iopStart = 0, iopCyclesStart = 0, iopInstructions = 0, iopCycles = 0;
    uint32_t draws = 0, meshes = 0, textures = 0, activeScopes = 0, droppedRoutes = 0, droppedDraws = 0;
    size_t recordedVertices = 0;
    uint32_t droppedVertices = 0;
    int activeCommand = -1; // Presentation thread only; dropped commands never reuse an earlier slot.
    std::vector<Route> routes;
    std::unordered_map<uint64_t, uint32_t> routeIndex;
    std::vector<Draw> commands;
    p3fes::native_renderer::Camera camera{};
    p3fes::native_renderer::ClearColor clear{};
    bool sealing = false;
    std::atomic<bool> accepted{false}, producerDone{false}, hostDone{false}, written{false}, screenshot{false};
};

uint64_t nowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace
{
    void tryWrite(const Handle &frame) noexcept
    {
        if (!frame || !frame->accepted.load(std::memory_order_acquire) ||
            !frame->producerDone.load(std::memory_order_acquire) || !frame->hostDone.load(std::memory_order_acquire)) return;
        if (frame->written.exchange(true, std::memory_order_relaxed)) return;
        FILE *f = std::fopen(config().path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "[frame-trace] could not open %s\n", config().path.c_str()); return; }
        const Frame &r = *frame;
        std::fprintf(f, "{\"format\":\"p3fes-native-frame-v1\",\"scope\":\"Camera/native-frame construction and presentation; selected graphics routes; intrusive wall timings, not GPU time\",\"frame\":{\"sequence\":%llu,\"draws\":%u,\"meshes\":%u,\"resident_textures\":%u},", static_cast<unsigned long long>(r.sequence), r.draws, r.meshes, r.textures);
        std::fprintf(f, "\"timings\":{\"producer_wall_ns\":%llu,\"render_wall_ns\":%llu,\"begin_frame_ns\":%llu,\"end_frame_ns\":%llu,\"texture_preload_ns\":%llu},", static_cast<unsigned long long>(r.producerEnd-r.started), static_cast<unsigned long long>(r.renderEnd-r.renderStart), static_cast<unsigned long long>(r.beginNs), static_cast<unsigned long long>(r.endNs), static_cast<unsigned long long>(r.preloadNs));
        std::fprintf(f, "\"counters\":{\"guest_dispatches\":%llu,\"checkpoints\":%llu,\"charged_ee_cycles\":%llu,\"iop_instructions\":%llu,\"iop_cycles\":%llu,\"texture_preload_uploads\":%llu,\"texture_uploads\":%llu,\"texture_deletes\":%llu},", static_cast<unsigned long long>(r.guestDispatches), static_cast<unsigned long long>(r.checkpoints), static_cast<unsigned long long>(r.eeCycles), static_cast<unsigned long long>(r.iopInstructions), static_cast<unsigned long long>(r.iopCycles), static_cast<unsigned long long>(r.preloadUploads), static_cast<unsigned long long>(r.uploads), static_cast<unsigned long long>(r.deletes));
        std::fprintf(f, "\"coverage\":{\"dropped_routes\":%u,\"dropped_draws\":%u,\"dropped_vertices\":%u,\"exclusive_means\":\"exclusive among recorded routes, includes unrecorded runtime/services\"},\"camera\":{\"use_matrices\":%s},\"routes\":[", r.droppedRoutes,r.droppedDraws,r.droppedVertices,r.camera.useMatrices?"true":"false");
        bool comma = false;
        for (const Route &route : r.routes)
        {
            if (comma) std::fputc(',',f); comma = true;
            std::fprintf(f, "{\"target\":%u,\"source\":%u,\"calls\":%llu,\"executor_entries\":%llu,\"inclusive_ns\":%llu,\"exclusive_ns\":%llu,\"checkpoints\":%llu", route.target, route.source, static_cast<unsigned long long>(route.calls), static_cast<unsigned long long>(route.executorEntries), static_cast<unsigned long long>(route.inclusive), static_cast<unsigned long long>(route.exclusive), static_cast<unsigned long long>(route.checkpoints));
            if (route.target >= kNative) { std::fputs(",\"name\":",f); quoted(f,stageName(route.target)); }
            std::fputc('}',f);
        }
        std::fputs("],\"draws\":[",f); comma=false;
        for (const Draw &draw : r.commands)
        {
            if (comma) std::fputc(',',f); comma=true;
            const auto &d=draw.info; const auto &s=d.state;
            std::fprintf(f,"{\"index\":%u,\"kind\":",d.index);quoted(f,d.kind);
            std::fprintf(f,",\"origin\":{\"target\":%u,\"source\":%u},\"texture\":",d.origin.target,d.origin.source);quoted(f,d.texture);
            std::fprintf(f,",\"texture_width\":%u,\"texture_height\":%u,\"texture_revision\":%llu,\"native_format\":\"RGBA8\",\"source_width\":%.9g,\"source_height\":%.9g,\"vertex_count\":%u,\"ensure_ns\":%llu,\"submit_ns\":%llu,\"state\":{\"blend\":%u,\"depth_test\":%s,\"depth_write\":%s,\"depth_function\":%u,\"alpha_test_enabled\":%s,\"alpha_test_function\":%u,\"alpha_reference\":%u,\"alpha_fail\":%u,\"linear_filter\":%s,\"clamp_u\":%s,\"clamp_v\":%s},\"vertices\":[",d.textureWidth,d.textureHeight,static_cast<unsigned long long>(d.textureRevision),static_cast<double>(d.sourceWidth),static_cast<double>(d.sourceHeight),d.vertexCount,static_cast<unsigned long long>(d.ensureNs),static_cast<unsigned long long>(d.submitNs),unsigned(s.blend),s.depthTest?"true":"false",s.depthWrite?"true":"false",unsigned(s.depthFunction),s.alphaTest.enabled?"true":"false",unsigned(s.alphaTest.function),unsigned(s.alphaTest.reference),unsigned(s.alphaFailAction),s.linearFilter?"true":"false",s.clampU?"true":"false",s.clampV?"true":"false");
            bool vc=false;
            for (const auto &v:d.vertices)
            {
                if(vc)std::fputc(',',f);vc=true;std::fputs("{\"xyz\":[",f);number(f,v.x);std::fputc(',',f);number(f,v.y);std::fputc(',',f);number(f,v.z);std::fputs("],\"uv\":[",f);number(f,v.u);std::fputc(',',f);number(f,v.v);std::fprintf(f,"],\"rgba\":[%u,%u,%u,%u]}",v.color[0],v.color[1],v.color[2],v.color[3]);
            }
            std::fputs("],\"gpu_passes\":[",f);bool pc=false;
            for(const auto &pass:draw.passes){if(pc)std::fputc(',',f);pc=true;std::fprintf(f,"{\"primitive\":%u,\"vertices\":%u,\"bytes\":%u}",pass.primitive,pass.vertices,pass.bytes);}
            std::fputs("]}",f);
        }
        std::fputs("]}\n",f);const long bytes=std::ftell(f);std::fclose(f);
        std::fprintf(stderr,"[frame-trace] captured sequence=%llu draws=%u routes=%u bytes=%ld path=%s\n",static_cast<unsigned long long>(r.sequence),r.draws,static_cast<unsigned>(r.routes.size()),bytes,config().path.c_str());
    }
    void sealIfReady(const Handle &frame) noexcept
    {
        if(frame && frame->sealing && frame->activeScopes==0u)
        {
            frame->producerEnd = nowNs();
            frame->producerDone.store(true,std::memory_order_release);
            tryWrite(frame);
        }
    }
}

void setRuntime(PS2Runtime *runtime) noexcept {g_runtime=runtime;}
Handle beginFrame(uint64_t sequence) noexcept
{
    try
    {
        static const uint64_t epoch=nowNs();
        if(!config().enabled || g_taken || (nowNs()-epoch)*1e-9<config().after)return {};
        auto frame=std::make_shared<Frame>();frame->sequence=sequence;
        frame->routes.reserve(256);frame->routeIndex.reserve(256);frame->commands.reserve(96);
        if(g_runtime){const auto s=g_runtime->iopDebugSnapshot();frame->iopStart=s.emulatorInstructions;frame->iopCyclesStart=s.emulatorCycles;}
        frame->started=nowNs();g_producer=frame;return frame;
    }
    catch(...){return {};}
}
void endProducer(const Handle &frame,uint32_t draws,uint32_t meshes,uint32_t textures) noexcept
{
    if(!frame)return;
    frame->producerEnd=nowNs();frame->draws=draws;frame->meshes=meshes;frame->textures=textures;
    if(g_runtime){try{const auto s=g_runtime->iopDebugSnapshot();frame->iopInstructions=s.emulatorInstructions-frame->iopStart;frame->iopCycles=s.emulatorCycles-frame->iopCyclesStart;}catch(...){}}
    const bool accepted=(config().draws==0u || draws==config().draws) && meshes==0u;
    frame->accepted.store(accepted,std::memory_order_release);if(accepted)g_taken=true;
    frame->sealing=true;if(g_producer==frame)g_producer.reset();sealIfReady(frame);
}

GuestScope::GuestScope(uint32_t target,uint32_t source,bool resumed) noexcept
{
    if(!g_producer)return;
    if(target<kNative)++g_producer->guestDispatches;
    if(!selected(target))return;
    try
    {
        const uint64_t key=(uint64_t(target)<<32u)|source;
        auto &frame=*g_producer;auto it=frame.routeIndex.find(key);
        if(it==frame.routeIndex.end())
        {
            if(frame.routes.size()>=kMaxRoutes){++frame.droppedRoutes;return;}
            const auto index=static_cast<uint32_t>(frame.routes.size());frame.routes.push_back(Route{target,source});frame.routeIndex.emplace(key,index);m_route=index;
        }
        else m_route=it->second;
        m_frame=g_producer;m_previous=g_scope;g_scope=this;++frame.activeScopes;
        auto &route=frame.routes[m_route];++route.calls;if(resumed)++route.executorEntries;
        m_started=nowNs();
    }
    catch(...){m_frame.reset();}
}
GuestScope::~GuestScope()
{
    if(!m_frame)return;
    const uint64_t elapsed=nowNs()-m_started;auto &r=m_frame->routes[m_route];r.inclusive+=elapsed;r.exclusive+=elapsed>=m_children?elapsed-m_children:0u;
    if(m_previous && m_previous->m_frame==m_frame)m_previous->m_children+=elapsed;
    g_scope=m_previous;--m_frame->activeScopes;sealIfReady(m_frame);
}
NativeScope::NativeScope(Stage stage) noexcept:GuestScope(kNative|static_cast<uint32_t>(stage),origin().target){}
Origin origin() noexcept
{
    for(auto *s=g_scope;s;s=s->m_previous)
        if(s->m_frame && s->m_frame->routes[s->m_route].target<kNative)
        {const auto &r=s->m_frame->routes[s->m_route];return {r.target,r.source};}
    return {};
}
void noteCheckpoint(uint32_t cycles) noexcept
{
    if(!g_producer)return;
    ++g_producer->checkpoints;g_producer->eeCycles+=cycles;
    if(g_scope && g_scope->m_frame==g_producer)++g_producer->routes[g_scope->m_route].checkpoints;
}
bool rendering(const Handle &frame) noexcept{return frame && frame->accepted.load(std::memory_order_acquire);}
void beginRender(const Handle &frame) noexcept{if(rendering(frame)){g_render=frame;frame->renderStart=nowNs();}}
void setFrameState(const Handle &frame,const p3fes::native_renderer::Camera &camera,p3fes::native_renderer::ClearColor clear) noexcept{if(rendering(frame)){frame->camera=camera;frame->clear=clear;}}
void texturePreload(const Handle &frame,uint64_t ns,uint64_t uploads) noexcept{if(rendering(frame)){frame->preloadNs=ns;frame->preloadUploads=uploads;}}
void resourceActivity(const Handle &frame,uint64_t uploads,uint64_t deletes) noexcept{if(rendering(frame)){frame->uploads=uploads;frame->deletes=deletes;}}
void beginDraw(const Handle &frame,DrawInfo info) noexcept
{
    if(!rendering(frame))return;
    frame->activeCommand=-1;
    try
    {
        if(frame->commands.size()>=kMaxDraws){++frame->droppedDraws;return;}
        if(info.texture.size()>128u)info.texture.resize(128u);
        if(info.kind.size()>32u)info.kind.resize(32u);
        if(info.vertexCount==0u)info.vertexCount=static_cast<uint32_t>(info.vertices.size());
        const size_t available=kMaxVertices-frame->recordedVertices;
        if(info.vertices.size()>available){frame->droppedVertices+=static_cast<uint32_t>(info.vertices.size()-available);info.vertices.resize(available);}
        frame->recordedVertices+=info.vertices.size();
        frame->commands.push_back(Draw{std::move(info),{}});
        frame->activeCommand=static_cast<int>(frame->commands.size()-1u);
    }
    catch(...){++frame->droppedDraws;}
}
void endDraw(const Handle &frame,uint64_t ensureNs,uint64_t submitNs) noexcept{if(rendering(frame) && frame->activeCommand>=0){auto &d=frame->commands[frame->activeCommand];d.info.ensureNs=ensureNs;d.info.submitNs=submitNs;frame->activeCommand=-1;}}
void gpuPass(uint32_t primitive,uint32_t vertices,uint32_t bytes) noexcept{if(g_render && g_render->activeCommand>=0){try{auto &passes=g_render->commands[g_render->activeCommand].passes;if(passes.size()<8u)passes.push_back({primitive,vertices,bytes});}catch(...){}}}
void endRender(const Handle &frame,uint64_t beginNs,uint64_t endNs) noexcept
{
    if(!rendering(frame))return;
    frame->beginNs=beginNs;frame->endNs=endNs;frame->renderEnd=nowNs();g_render.reset();frame->hostDone.store(true,std::memory_order_release);tryWrite(frame);
}
bool captureScreenshot(const Handle &frame) noexcept{return rendering(frame) && !frame->screenshot.exchange(true,std::memory_order_relaxed);}
}
