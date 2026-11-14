#pragma once

#include "renderer.h"

namespace p3fes::native_renderer::detail
{
// Captures only the draw inputs used to generate unskinned vertex colors and
// base UVs. The vectors are copied only after regeneration; matches() compares
// the live state against these snapshots without allocating or copying.
struct StaticVertexCacheKey
{
    Matrix4 model{};
    std::array<std::uint8_t, 4> materialColor{};
    float materialAmbient = 0.0f;
    float materialDiffuse = 0.0f;
    bool meshLighting = false;
    bool hasPrelight = false;
    bool lightingEnabled = false;
    std::array<float, 3> ambientLight{};
    MeshDrawState::UvTransform uvTransform{};
    // Skinned parts: the palette entries the part's vertices use (see usedBones in the renderer).
    std::vector<Matrix4> skinMatrices;
    std::size_t skinPaletteSize = 0;
    std::vector<MeshDrawState::DirectionalLight> directionalLights;
    std::vector<MeshDrawState::LocalLight> localLights;
    bool valid = false;

    static bool equal(const MeshDrawState::UvTransform &a, const MeshDrawState::UvTransform &b)
    {
        return a.enabled == b.enabled && a.rightX == b.rightX && a.rightY == b.rightY &&
               a.upX == b.upX && a.upY == b.upY && a.posX == b.posX && a.posY == b.posY;
    }

    static bool equal(const MeshDrawState::DirectionalLight &a,
                      const MeshDrawState::DirectionalLight &b)
    {
        return a.direction == b.direction && a.color == b.color;
    }

    static bool equal(const MeshDrawState::LocalLight &a, const MeshDrawState::LocalLight &b)
    {
        return a.type == b.type && a.position == b.position && a.direction == b.direction &&
               a.color == b.color && a.radius == b.radius && a.minusCosAngle == b.minusCosAngle;
    }

    // Why a draw does not match the snapshot (bit set per differing input; 0 = reusable).
    enum Reason : unsigned
    {
        ReasonInvalid = 1u,
        ReasonModel = 2u,
        ReasonMaterial = 4u,
        ReasonLightingFlags = 8u,
        ReasonAmbient = 16u,
        ReasonUv = 32u,
        ReasonDirectional = 64u,
        ReasonLocal = 128u,
        ReasonSkin = 256u,
    };

    unsigned mismatch(const Matrix4 &currentModel,
                      const Material &material,
                      const std::array<std::uint8_t, 4> &currentMaterialColor,
                      bool currentMeshLighting,
                      bool currentHasPrelight,
                      const MeshDrawState &state,
                      const MeshDrawState::UvTransform &currentUvTransform,
                      const std::vector<std::uint8_t> *usedBones = nullptr,
                      const std::vector<Matrix4> *palette = nullptr) const
    {
        if (!valid)
            return ReasonInvalid;
        unsigned reasons = 0u;
        if (usedBones && palette && !skinMatches(*usedBones, *palette))
            reasons |= ReasonSkin;
        if (model != currentModel)
            reasons |= ReasonModel;
        if (materialColor != currentMaterialColor || materialAmbient != material.ambient ||
            materialDiffuse != material.diffuse)
            reasons |= ReasonMaterial;
        if (meshLighting != currentMeshLighting || hasPrelight != currentHasPrelight ||
            lightingEnabled != state.lightingEnabled)
            reasons |= ReasonLightingFlags;
        if (ambientLight != state.ambientLight)
            reasons |= ReasonAmbient;
        if (!equal(uvTransform, currentUvTransform))
            reasons |= ReasonUv;
        if (directionalLights.size() != state.directionalLights.size())
            reasons |= ReasonDirectional;
        else
            for (std::size_t i = 0; i < directionalLights.size(); ++i)
                if (!equal(directionalLights[i], state.directionalLights[i]))
                {
                    reasons |= ReasonDirectional;
                    break;
                }
        if (localLights.size() != state.localLights.size())
            reasons |= ReasonLocal;
        else
            for (std::size_t i = 0; i < localLights.size(); ++i)
                if (!equal(localLights[i], state.localLights[i]))
                {
                    reasons |= ReasonLocal;
                    break;
                }
        return reasons;
    }

    bool skinMatches(const std::vector<std::uint8_t> &usedBones, const std::vector<Matrix4> &palette) const
    {
        if (skinPaletteSize != palette.size() || skinMatrices.size() != usedBones.size())
            return false;
        for (std::size_t i = 0; i < usedBones.size(); ++i)
            if (usedBones[i] < palette.size() && skinMatrices[i] != palette[usedBones[i]])
                return false;
        return true;
    }

    bool matches(const Matrix4 &currentModel,
                 const Material &material,
                 const std::array<std::uint8_t, 4> &currentMaterialColor,
                 bool currentMeshLighting,
                 bool currentHasPrelight,
                 const MeshDrawState &state,
                 const MeshDrawState::UvTransform &currentUvTransform) const
    {
        return mismatch(currentModel, material, currentMaterialColor, currentMeshLighting, currentHasPrelight,
                        state, currentUvTransform) == 0u;
    }

    void capture(const Matrix4 &currentModel,
                 const Material &material,
                 const std::array<std::uint8_t, 4> &currentMaterialColor,
                 bool currentMeshLighting,
                 bool currentHasPrelight,
                 const MeshDrawState &state,
                 const MeshDrawState::UvTransform &currentUvTransform,
                 const std::vector<std::uint8_t> *usedBones = nullptr,
                 const std::vector<Matrix4> *palette = nullptr)
    {
        skinMatrices.clear();
        skinPaletteSize = 0;
        if (usedBones && palette)
        {
            skinPaletteSize = palette->size();
            skinMatrices.resize(usedBones->size());
            for (std::size_t i = 0; i < usedBones->size(); ++i)
                if ((*usedBones)[i] < palette->size())
                    skinMatrices[i] = (*palette)[(*usedBones)[i]];
        }
        model = currentModel;
        materialColor = currentMaterialColor;
        materialAmbient = material.ambient;
        materialDiffuse = material.diffuse;
        meshLighting = currentMeshLighting;
        hasPrelight = currentHasPrelight;
        lightingEnabled = state.lightingEnabled;
        ambientLight = state.ambientLight;
        uvTransform = currentUvTransform;
        directionalLights = state.directionalLights;
        localLights = state.localLights;
        valid = true;
    }

    void invalidate() { valid = false; }
};

inline bool canReuseStaticVertexData(bool skinned, bool keyMatches)
{
    return !skinned && keyMatches;
}

// Skinned parts carry their palette entries in the key as well, so a matching key means reusable for them too.
inline bool canReuseVertexData(bool keyMatches)
{
    return keyMatches;
}
} // namespace p3fes::native_renderer::detail
