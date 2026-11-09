#!/usr/bin/env python3
"""
Extract one Persona 3 FES RMD directly from a CVM and prepare it as glTF.

The heavy game archives stay in place. Only the requested model is copied to a
temporary directory, converted with the reused Virtual-AK tools, then deleted.

Usage:
    python prepare_model.py DATA.CVM /FIELD/GRMD/FOBJ000.RMD -o out/model
    python prepare_model.py BTL.CVM /MODEL/PERSONA/PS000.RMD -o out/persona
"""

import argparse
import os
import tempfile

import p3fes_gltf
import p3fes_unpack


def find_entry(entries, wanted):
    normalized = wanted.replace("\\", "/")
    if not normalized.startswith("/"):
        normalized = "/" + normalized
    normalized = normalized.upper()
    for entry in entries:
        if entry["path"].upper() == normalized:
            return entry
    raise SystemExit(f"Model not found in CVM: {wanted}")


def main():
    ap = argparse.ArgumentParser(description="Prepare one RMD from a Persona 3 FES CVM as glTF.")
    ap.add_argument("cvm", help="path to DATA.CVM, BTL.CVM, or another readable CVM")
    ap.add_argument("model", help="path inside the CVM, for example /FIELD/GRMD/FOBJ000.RMD")
    ap.add_argument("-o", "--out", required=True, help="output directory for glTF/bin/textures")
    ap.add_argument("--mirror-x", action="store_true", help="mirror model geometry on X")
    ap.add_argument("--no-textures", action="store_true", help="do not export texture PNGs")
    args = ap.parse_args()

    if not args.model.lower().endswith(".rmd"):
        raise SystemExit("Only .RMD models are accepted by this helper.")

    cvm_path = os.path.abspath(args.cvm)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    with open(cvm_path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        opened = p3fes_unpack.open_cvm(f, {"lba": 0, "size": size})
        if opened is None:
            raise SystemExit(f"No readable ISO9660 filesystem found inside {cvm_path}")
        inner, entries = opened
        entry = find_entry(entries, args.model)

        with tempfile.TemporaryDirectory(prefix="p3fes-native-model-") as temp:
            local_rmd = os.path.join(temp, os.path.basename(entry["path"]))
            p3fes_unpack.write_range(inner, entry["lba"] * p3fes_unpack.SECTOR, entry["size"], local_rmd)
            result = p3fes_gltf.export_gltf(
                local_rmd,
                out_dir,
                args.mirror_x,
                not args.no_textures,
                verbose=False,
            )

    print(
        f"Prepared {entry['path']} -> {os.path.join(out_dir, result['base'] + '.gltf')} "
        f"({result['meshes']} meshes, {result['bones']} bones, {result['animations']} animations)"
    )


if __name__ == "__main__":
    main()
