#include "../static_vertex_cache.h"

#include <cassert>

using namespace p3fes::native_renderer;
using p3fes::native_renderer::detail::StaticVertexCacheKey;

int main()
{
    Matrix4 model = kIdentityMatrix;
    Material material{};
    std::array<std::uint8_t, 4> materialColor{255u, 240u, 220u, 200u};
    bool meshLighting = true;
    bool hasPrelight = true;
    MeshDrawState state{};
    state.lightingEnabled = true;
    state.ambientLight = {0.1f, 0.2f, 0.3f};
    state.directionalLights.push_back({{0.0f, -1.0f, 0.0f}, {0.8f, 0.7f, 0.6f}});
    state.localLights.push_back({MeshDrawState::LocalLight::Type::Point,
                                 {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -1.0f},
                                 {0.5f, 0.6f, 0.7f}, 4.0f, -0.25f});
    MeshDrawState::UvTransform uv{};

    StaticVertexCacheKey key;
    key.capture(model, material, materialColor, meshLighting, hasPrelight, state, uv);
    auto matches = [&]
    {
        return key.matches(model, material, materialColor, meshLighting, hasPrelight, state, uv);
    };
    assert(matches());
    assert(p3fes::native_renderer::detail::canReuseStaticVertexData(false, matches()));

    const auto expectMiss = [&]
    {
        assert(!matches());
        assert(!p3fes::native_renderer::detail::canReuseStaticVertexData(false, matches()));
    };
    const auto restore = [&]
    {
        key.capture(model, material, materialColor, meshLighting, hasPrelight, state, uv);
        assert(matches());
    };

    model[12] = 1.0f; expectMiss(); model = kIdentityMatrix; restore();
    material.ambient += 0.1f; expectMiss(); material.ambient = 1.0f; restore();
    material.diffuse += 0.1f; expectMiss(); material.diffuse = 1.0f; restore();
    materialColor[0]--; expectMiss(); materialColor[0]++; restore();
    meshLighting = false; expectMiss(); meshLighting = true; restore();
    hasPrelight = false; expectMiss(); hasPrelight = true; restore();

    state.lightingEnabled = false; expectMiss(); state.lightingEnabled = true; restore();
    state.ambientLight[1] += 0.1f; expectMiss(); state.ambientLight[1] -= 0.1f; restore();
    state.directionalLights[0].direction[2] = 0.5f; expectMiss();
    state.directionalLights[0].direction[2] = 0.0f; restore();
    state.directionalLights[0].color[1] = 0.25f; expectMiss();
    state.directionalLights[0].color[1] = 0.7f; restore();
    state.directionalLights.push_back({}); expectMiss(); state.directionalLights.pop_back(); restore();

    state.localLights[0].type = MeshDrawState::LocalLight::Type::Spot; expectMiss();
    state.localLights[0].type = MeshDrawState::LocalLight::Type::Point; restore();
    state.localLights[0].position[0] += 1.0f; expectMiss(); state.localLights[0].position[0] -= 1.0f; restore();
    state.localLights[0].direction[0] += 1.0f; expectMiss(); state.localLights[0].direction[0] -= 1.0f; restore();
    state.localLights[0].color[2] += 0.1f; expectMiss(); state.localLights[0].color[2] -= 0.1f; restore();
    state.localLights[0].radius += 1.0f; expectMiss(); state.localLights[0].radius -= 1.0f; restore();
    state.localLights[0].minusCosAngle += 0.1f; expectMiss(); state.localLights[0].minusCosAngle -= 0.1f; restore();
    state.localLights.push_back({}); expectMiss(); state.localLights.pop_back(); restore();

    uv.enabled = true; expectMiss(); uv.enabled = false; restore();
    uv.rightX = 0.5f; expectMiss(); uv.rightX = 1.0f; restore();
    uv.rightY = 0.5f; expectMiss(); uv.rightY = 0.0f; restore();
    uv.upX = 0.5f; expectMiss(); uv.upX = 0.0f; restore();
    uv.upY = 0.5f; expectMiss(); uv.upY = 1.0f; restore();
    uv.posX = 0.5f; expectMiss(); uv.posX = 0.0f; restore();
    uv.posY = 0.5f; expectMiss(); uv.posY = 0.0f; restore();

    // Skin is never served from the static cache. A dual/environment pass
    // invalidates the captured base after modifying the VBO, so next draw misses.
    assert(!p3fes::native_renderer::detail::canReuseStaticVertexData(true, true));
    key.invalidate(); // same invalidation used after an applied multipass write
    assert(!matches());
    assert(!p3fes::native_renderer::detail::canReuseStaticVertexData(false, matches()));
        // Skinned parts: the key carries the palette entries the part uses.
    {
        std::vector<Matrix4> palette(4, kIdentityMatrix);
        palette[2][12] = 3.0f;
        const std::vector<std::uint8_t> used{1u, 2u};
        key.capture(model, material, materialColor, meshLighting, hasPrelight, state, uv, &used, &palette);
        assert(key.mismatch(model, material, materialColor, meshLighting, hasPrelight, state, uv, &used, &palette) == 0u);
        palette[0][12] = 9.0f; // a bone the part does not use
        assert(key.mismatch(model, material, materialColor, meshLighting, hasPrelight, state, uv, &used, &palette) == 0u);
        palette[2][12] = 4.0f; // a bone it uses
        assert(key.mismatch(model, material, materialColor, meshLighting, hasPrelight, state, uv, &used, &palette) ==
               StaticVertexCacheKey::ReasonSkin);
        palette[2][12] = 3.0f;
        palette.push_back(kIdentityMatrix); // palette size is part of the key
        assert(key.mismatch(model, material, materialColor, meshLighting, hasPrelight, state, uv, &used, &palette) ==
               StaticVertexCacheKey::ReasonSkin);
    }
    return 0;
}
