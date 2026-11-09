#pragma once

#include "mesh.h"

#include <string>
#include <vector>

namespace p3fes::native_renderer
{
struct NativeModel
{
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
};

bool loadNativeModel(const std::string &path, NativeModel &model, std::string &error);
} // namespace p3fes::native_renderer
