#!/usr/bin/env python3
"""Prepare one Persona 3 FES RMD as a compact native runtime package.

This follows the RECV Vita pattern: expensive PS2 asset decoding happens on
the user's PC/Mac, while the target only reads simple vertices, indices,
materials and RGBA textures. No game data belongs in the repository.

Usage:
    python prepare_native_model.py DATA.CVM /FIELD/GRMD/FOBJ000.RMD -o /tmp/FOBJ000.p3n
    python prepare_native_model.py BTL.CVM /MODEL/PERSONA/PS000.RMD -o /tmp/PS000.p3n
"""

import argparse
import os
import struct
import tempfile

import p3fes_rmd as r
import p3fes_unpack
from prepare_model import find_entry


MAGIC = b"P3NMDL1\0"
VERSION = 1
HEADER = struct.Struct("<8sIIII")
MESH_HEADER = struct.Struct("<7I")
VERTEX = struct.Struct("<8f4B4B4f")
SUBMESH = struct.Struct("<III")
MATERIAL = struct.Struct("<4B32s")
TEXTURE = struct.Struct("<32sIII")


def fixed_name(name):
    raw = (name or "").encode("latin-1", "replace")
    if len(raw) >= 32:
        raw = raw[:31]
    return raw + b"\0" * (32 - len(raw))


def decoded_textures(data, texdict):
    textures = []
    seen = set()
    if not texdict:
        return textures
    for tex in texdict.find(0x15):
        strings = tex.find(2)
        name = r.read_string(data, strings[0]) if strings else ""
        key = name.lower()
        if not name or key in seen:
            continue
        width, height, rgba = r.decode_native_texture(data, tex)
        if len(rgba) != width * height * 4:
            raise r.ToolError("decoded texture %s has an unexpected byte count" % name)
        textures.append((name, width, height, rgba))
        seen.add(key)
    return textures


def native_meshes(data, scene):
    meshes = []
    for atomic_index, (_frame_index, geometry_index, _atomic_flags) in enumerate(scene["atomics"]):
        if geometry_index >= len(scene["geos"]):
            raise r.ToolError("atomic %d has an out-of-range geometry index" % atomic_index)
        geometry = scene["geos"][geometry_index]
        info = r.read_geometry(data, geometry)
        if info.get("native"):
            raise r.ToolError("geometry %d is still PS2-native and cannot be packed yet" % geometry_index)

        skin = r.geometry_skin(data, geometry, info["nvert"])
        if skin and len(skin["matrices"]) != skin["bones"]:
            skin = None
        materials = r.read_materials(data, geometry)
        groups = {}
        for a, b, c, material_index in info["tris"]:
            groups.setdefault(material_index, []).extend((a, b, c))

        indices = []
        submeshes = []
        for material_index, group in sorted(groups.items()):
            first = len(indices)
            indices.extend(group)
            submeshes.append((first, len(group), material_index))

        vertices = []
        uv0 = info["uvs"][0] if info["uvs"] else []
        normals = info["normals"]
        colors = info.get("colors", [])
        for i, position in enumerate(info["verts"]):
            normal = normals[i] if i < len(normals) else (0.0, 0.0, 0.0)
            uv = uv0[i] if i < len(uv0) else (0.0, 0.0)
            color = colors[i] if i < len(colors) else (255, 255, 255, 255)
            bones = skin["indices"][i] if skin else (0, 0, 0, 0)
            weights = skin["weights"][i] if skin else (0.0, 0.0, 0.0, 0.0)
            vertices.append((position, normal, uv, color, bones, weights))

        mesh_flags = 0
        if normals:
            mesh_flags |= 1
        if info["flags"] & r.GEO_PRELIT:
            mesh_flags |= 2
        if skin:
            mesh_flags |= 4
        meshes.append({
            "flags": mesh_flags,
            "vertices": vertices,
            "indices": indices,
            "submeshes": submeshes,
            "materials": materials,
            "bones": skin["bones"] if skin else 0,
        })
    return meshes


def write_pack(model_path, output_path):
    with open(model_path, "rb") as fh:
        data = fh.read()
    scene = r.read_scene(data)
    meshes = native_meshes(data, scene)
    textures = decoded_textures(data, scene["texdict"])

    with open(output_path, "wb") as out:
        out.write(HEADER.pack(MAGIC, VERSION, len(meshes), len(textures), 0))
        for mesh in meshes:
            out.write(MESH_HEADER.pack(
                mesh["flags"], len(mesh["vertices"]), len(mesh["indices"]), len(mesh["submeshes"]),
                len(mesh["materials"]), mesh["bones"], 0))
            for position, normal, uv, color, bones, weights in mesh["vertices"]:
                out.write(VERTEX.pack(
                    *(tuple(position) + tuple(normal) + tuple(uv)),
                    *color,
                    *(tuple(bones[:4]) + (0,) * (4 - len(bones[:4]))),
                    *(tuple(weights[:4]) + (0.0,) * (4 - len(weights[:4])))))
            if mesh["indices"]:
                out.write(struct.pack("<%dI" % len(mesh["indices"]), *mesh["indices"]))
            for first, count, material_index in mesh["submeshes"]:
                out.write(SUBMESH.pack(first, count, material_index))
            for material in mesh["materials"]:
                out.write(MATERIAL.pack(*material["color"], fixed_name(material.get("texture"))))

        for name, width, height, rgba in textures:
            out.write(TEXTURE.pack(fixed_name(name), width, height, len(rgba)))
            out.write(rgba)
    return {"meshes": len(meshes), "textures": len(textures), "bytes": os.path.getsize(output_path)}


def main():
    ap = argparse.ArgumentParser(description="Prepare one Persona 3 FES RMD as a native runtime package.")
    ap.add_argument("cvm", help="path to DATA.CVM, BTL.CVM, or another readable CVM")
    ap.add_argument("model", help="path inside the CVM, for example /FIELD/GRMD/FOBJ000.RMD")
    ap.add_argument("-o", "--out", required=True, help="output .p3n file")
    args = ap.parse_args()
    if not args.model.lower().endswith(".rmd"):
        raise SystemExit("Only .RMD models are accepted by this helper.")

    cvm_path = os.path.abspath(args.cvm)
    output_path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(cvm_path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        opened = p3fes_unpack.open_cvm(f, {"lba": 0, "size": size})
        if opened is None:
            raise SystemExit(f"No readable ISO9660 filesystem found inside {cvm_path}")
        inner, entries = opened
        entry = find_entry(entries, args.model)
        with tempfile.TemporaryDirectory(prefix="p3fes-native-pack-") as temp:
            local_rmd = os.path.join(temp, os.path.basename(entry["path"]))
            p3fes_unpack.write_range(inner, entry["lba"] * p3fes_unpack.SECTOR, entry["size"], local_rmd)
            result = write_pack(local_rmd, output_path)

    print("Prepared %s -> %s (%d meshes, %d textures, %d bytes)" %
          (entry["path"], output_path, result["meshes"], result["textures"], result["bytes"]))


if __name__ == "__main__":
    main()
