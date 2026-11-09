// Adapted from Ryubara/coney's RenderWare-to-MeshData conversion (GPL-3.0-or-later).
#pragma once

#include "../native_renderer/mesh.h"

#include <optional>
#include <string>

namespace rw
{
struct Geometry;
}

namespace p3fes::native_rw
{
std::optional<native_renderer::MeshData> convertGeometry(rw::Geometry &geometry, std::string &error);
} // namespace p3fes::native_rw
