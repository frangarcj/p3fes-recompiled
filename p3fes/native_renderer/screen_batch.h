// Screen-space (Im2D) draw batching shared by the native backends.
//
// A dialogue frame is ~190 small 2-D draws: a text glyph is a 6-vertex quad with a 32x32 texture of its own,
// a fade is a row of untextured tiles. Each draw costs a fixed ~50 us of host thread on the Vita (vitaGL's
// client-array draw plus the state around it), so the backends append consecutive draws that would be
// rasterised identically into one draw call, and give small textures a slot in an atlas page so that runs of
// different glyphs share a texture too.
//
// Everything here is GL-free: the backends own the textures and the draw calls.
#pragma once

#include "renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace p3fes::native_renderer::detail
{
// P3FES_SCREEN_BATCH=0 restores one draw call per screen draw (and no atlas); P3FES_SCREEN_ATLAS=0 keeps the
// merging of same-texture runs but not the atlas.
inline bool screenBatchEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_SCREEN_BATCH");
        return !value || *value != '0';
    }();
    return enabled;
}

inline bool screenAtlasEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_SCREEN_ATLAS");
        return screenBatchEnabled() && (!value || *value != '0');
    }();
    return enabled;
}

inline bool sameScreenState(const ScreenDrawState &a, const ScreenDrawState &b)
{
    return a.blend == b.blend && a.depthTest == b.depthTest && a.depthWrite == b.depthWrite &&
           a.depthFunction == b.depthFunction && a.alphaFailAction == b.alphaFailAction &&
           a.gouraudShading == b.gouraudShading && a.clampU == b.clampU && a.clampV == b.clampV &&
           a.linearFilter == b.linearFilter && a.alphaTest.enabled == b.alphaTest.enabled &&
           a.alphaTest.function == b.alphaTest.function && a.alphaTest.reference == b.alphaTest.reference &&
           a.fog.enabled == b.fog.enabled && a.fog.start == b.fog.start && a.fog.end == b.fog.end &&
           a.fog.color == b.fog.color && a.destinationAlphaTest == b.destinationAlphaTest;
}

// A draw whose alpha test has a "failed" outcome that is drawn too (the GS keeps writing the frame buffer, the
// depth buffer or the colour when the alpha test fails) is two GL passes: the passing pixels, then the failing ones.
// Mirrors the decision in the backends' drawScreenPrimitive.
inline bool screenHasSecondPass(const ScreenDrawState &state)
{
    if (!state.alphaTest.enabled || state.alphaFailAction == ScreenAlphaFailAction::Keep)
        return false;
    if (state.alphaFailAction == ScreenAlphaFailAction::FramebufferOnly && (!state.depthWrite || !state.depthTest))
        return false; // one draw without alpha test
    return complementGsAlphaFunction(state.alphaTest.function) != 0u &&
           (state.alphaFailAction != ScreenAlphaFailAction::DepthOnly || state.depthWrite);
}

// The failing pass of "alpha > 0" draws only alpha == 0 texels with a source-alpha blend, which leaves the colour
// as it is: running the passes of several quads as pass 1 of all, then pass 2 of all, shows the same picture.
inline bool screenSecondPassInert(const ScreenDrawState &state)
{
    return state.alphaFailAction == ScreenAlphaFailAction::FramebufferOnly && state.alphaTest.function == 6u &&
           state.alphaTest.reference == 0u &&
           (state.blend == ScreenBlend::Alpha || state.blend == ScreenBlend::Additive ||
            state.blend == ScreenBlend::Subtractive);
}

// Placement of a small texture inside an atlas page. x, y is the first texel of the texture itself; a one
// texel gutter of replicated edge texels (clamp-to-edge) surrounds it so that bilinear filtering at the border of
// a draw never reads the neighbouring slot.
struct AtlasSlot
{
    int page = -1;
    int x = 0, y = 0, w = 0, h = 0;
    bool valid() const { return page >= 0; }
};

class AtlasAllocator
{
public:
    static constexpr int kPageSize = 512;
    static constexpr int kMaxPages = 3;
    static constexpr int kMaxTexture = 64;

    static bool eligible(std::uint32_t width, std::uint32_t height)
    {
        return width >= 1u && height >= 1u && width <= static_cast<std::uint32_t>(kMaxTexture) &&
               height <= static_cast<std::uint32_t>(kMaxTexture);
    }

    // A free slot of exactly this size when there is one (glyph rasters are all the same size), else shelf space.
    AtlasSlot allocate(int w, int h)
    {
        for (std::size_t i = 0; i < m_free.size(); ++i)
            if (m_free[i].w == w && m_free[i].h == h)
            {
                const AtlasSlot slot = m_free[i];
                m_free[i] = m_free.back();
                m_free.pop_back();
                return slot;
            }
        const int cellW = w + 2, cellH = h + 2;
        for (int page = 0; page < kMaxPages; ++page)
        {
            if (page >= static_cast<int>(m_pages.size()))
                m_pages.emplace_back();
            Page &p = m_pages[page];
            for (Shelf &shelf : p.shelves)
                if (shelf.height >= cellH && shelf.height <= cellH + 8 && shelf.x + cellW <= kPageSize)
                {
                    const AtlasSlot slot{page, shelf.x + 1, shelf.y + 1, w, h};
                    shelf.x += cellW;
                    return slot;
                }
            if (p.nextY + cellH <= kPageSize && cellW <= kPageSize)
            {
                p.shelves.push_back({p.nextY, cellH, cellW});
                const AtlasSlot slot{page, 1, p.nextY + 1, w, h};
                p.nextY += cellH;
                return slot;
            }
        }
        return {};
    }

    void release(const AtlasSlot &slot)
    {
        if (slot.valid())
            m_free.push_back(slot);
    }

    int pageCount() const { return static_cast<int>(m_pages.size()); }

    void clear()
    {
        m_pages.clear();
        m_free.clear();
    }

private:
    struct Shelf
    {
        int y, height, x;
    };
    struct Page
    {
        std::vector<Shelf> shelves;
        int nextY = 0;
    };
    std::vector<Page> m_pages;
    std::vector<AtlasSlot> m_free;
};

// (w + 2) x (h + 2) RGBA block: the texture with its edge texels replicated one texel outwards.
inline void padAtlasPixels(const std::uint8_t *rgba, int w, int h, std::vector<std::uint8_t> &out)
{
    const int pw = w + 2, ph = h + 2;
    out.resize(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 4u);
    for (int y = 0; y < ph; ++y)
    {
        const int sy = std::clamp(y - 1, 0, h - 1);
        std::uint8_t *row = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) * 4u;
        const std::uint8_t *src = rgba + static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) * 4u;
        std::copy(src, src + 4, row);
        std::copy(src, src + static_cast<std::size_t>(w) * 4u, row + 4);
        std::copy(src + static_cast<std::size_t>(w - 1) * 4u, src + static_cast<std::size_t>(w) * 4u,
                  row + static_cast<std::size_t>(w + 1) * 4u);
    }
}

// The draw being collected. `texture` is whatever id the backend uses for the texture the vertices sample
// (an atlas page or the texture itself, 0 for none); two draws merge only when everything that selects GL state
// is equal, so the merged draw rasterises exactly what the separate ones did, in the same order.
struct ScreenBatch
{
    std::vector<ScreenVertex> vertices;
    float sourceWidth = 0.0f, sourceHeight = 0.0f;
    std::uintptr_t texture = 0;
    bool linear = false;
    Renderer::ScreenPlacement placement = Renderer::ScreenPlacement::Centre;
    ScreenDrawState state{};
    bool active = false;
    // Two-pass states whose second pass shows something (a translucent box texel failing "alpha >= 127" is
    // still blended) only merge quads that do not overlap: otherwise pass 2 of an earlier quad would land on top
    // of pass 1 of a later one. Bounding boxes x0, y0, x1, y1 of the draws collected so far.
    bool ordered = false;
    std::vector<std::array<float, 4>> rects;
    static constexpr std::size_t kMaxVertices = 3072;
    static constexpr std::size_t kMaxOrderedDraws = 96;

    static std::array<float, 4> boundsOf(const ScreenVertex *v, std::size_t count)
    {
        std::array<float, 4> r{v[0].x, v[0].y, v[0].x, v[0].y};
        for (std::size_t i = 1; i < count; ++i)
        {
            r[0] = std::min(r[0], v[i].x); r[1] = std::min(r[1], v[i].y);
            r[2] = std::max(r[2], v[i].x); r[3] = std::max(r[3], v[i].y);
        }
        return r;
    }

    bool accepts(float sw, float sh, std::uintptr_t tex, bool lin, Renderer::ScreenPlacement place,
                 const ScreenDrawState &st, const ScreenVertex *incoming, std::size_t count) const
    {
        if (!active || sw != sourceWidth || sh != sourceHeight || tex != texture || lin != linear ||
            place != placement || vertices.size() + count > kMaxVertices || !sameScreenState(st, state))
            return false;
        if (ordered)
        {
            if (rects.size() >= kMaxOrderedDraws)
                return false;
            const std::array<float, 4> r = boundsOf(incoming, count);
            for (const auto &known : rects)
                if (r[0] < known[2] && known[0] < r[2] && r[1] < known[3] && known[1] < r[3])
                    return false;
        }
        return true;
    }
    void added(const ScreenVertex *incoming, std::size_t count)
    {
        if (ordered)
            rects.push_back(boundsOf(incoming, count));
    }
    void begin(float sw, float sh, std::uintptr_t tex, bool lin, Renderer::ScreenPlacement place,
               const ScreenDrawState &st)
    {
        ordered = screenHasSecondPass(st) && !screenSecondPassInert(st);
        rects.clear();
        vertices.clear();
        sourceWidth = sw;
        sourceHeight = sh;
        texture = tex;
        linear = lin;
        placement = place;
        state = st;
        active = true;
    }
};
} // namespace p3fes::native_renderer::detail
