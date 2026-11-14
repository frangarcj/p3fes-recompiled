#!/usr/bin/env python3
"""Summarize one p3fes native-frame trace without conflating host and GPU time.

The report keeps route inclusive and exclusive costs as separate aggregates:
inclusive routes can contain one another, so their durations are not a valid
single total. GPU pass byte/vertex counts are reported separately from CPU-side
ensure/submit wall times; the latter are not measurements of GPU execution.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import sys
from pathlib import Path
from typing import Any


DEFAULT_MAP = Path(__file__).resolve().parents[1] / "p3fes_map.csv"
NATIVE_BASE = 0xF0000000


def number(value: Any, default: int = 0) -> int:
    """Parse JSON integers and common decimal/hex string encodings."""
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        value = value.strip()
        if not value:
            return default
        try:
            return int(value, 0)
        except ValueError:
            return int(value, 10)
    raise ValueError(f"expected an integer-like value, got {type(value).__name__}")


class FunctionIndex:
    def __init__(self, entries: list[tuple[int, int, str]]):
        self.entries = sorted(entries, key=lambda item: (item[0], item[1] - item[0]))
        self.starts = [entry[0] for entry in self.entries]
        self.prefix_max_end: list[int] = []
        maximum = 0
        for _, end, _ in self.entries:
            maximum = max(maximum, end)
            self.prefix_max_end.append(maximum)

    @classmethod
    def from_csv(cls, path: Path) -> "FunctionIndex":
        entries: list[tuple[int, int, str]] = []
        with path.open(newline="", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            required = {"Name", "Start", "End"}
            if not reader.fieldnames or not required.issubset(reader.fieldnames):
                raise ValueError(f"{path}: expected CSV columns Name, Start, End")
            for line, row in enumerate(reader, 2):
                try:
                    start = number(row["Start"])
                    end = number(row["End"])
                except (TypeError, ValueError) as exc:
                    raise ValueError(f"{path}:{line}: invalid function range: {exc}") from exc
                if end > start:
                    entries.append((start, end, row["Name"]))
        return cls(entries)

    def resolve(self, pc: Any) -> str | None:
        if pc is None:
            return None
        value = number(pc)
        index = bisect.bisect_right(self.starts, value) - 1
        candidates: list[tuple[int, int, str]] = []
        while index >= 0 and self.prefix_max_end[index] > value:
            start, end, name = self.entries[index]
            if start <= value < end:
                candidates.append((start, end, name))
            index -= 1
        if not candidates:
            return None
        # Map files can contain aliases/overlaps. Attribute the PC to the
        # narrowest containing function interval, with stable lexical tie-break.
        return min(candidates, key=lambda item: (item[1] - item[0], item[0], item[2]))[2]


def _pc(value: Any) -> int | None:
    return None if value is None else number(value)


def _resolve_pc(pc: Any, route_name: str | None, index: FunctionIndex) -> str | None:
    if pc is None:
        return None
    value = number(pc)
    if value >= NATIVE_BASE:
        return route_name or f"native@0x{value:08X}"
    return index.resolve(value) or f"unmapped@0x{value:08X}"


def _frame_count(value: Any) -> int | None:
    if isinstance(value, list):
        return len(value)
    if isinstance(value, dict):
        for key in ("count", "total", "value"):
            if key in value:
                return number(value[key])
    if isinstance(value, (int, float, str)):
        return number(value)
    return None


def _route_summary(routes: Any, index: FunctionIndex) -> tuple[list[dict[str, Any]], dict[str, int]]:
    if routes is None:
        routes = []
    if not isinstance(routes, list):
        raise ValueError("routes must be an array")
    grouped: dict[tuple[Any, ...], dict[str, Any]] = {}
    raw_totals = {"calls": 0, "executor_entries": 0, "inclusive_ns": 0, "exclusive_ns": 0}
    for route in routes:
        if not isinstance(route, dict):
            continue
        target = route.get("target")
        source = route.get("source")
        name = route.get("name")
        target_pc = _pc(target)
        source_pc = _pc(source)
        target_name = _resolve_pc(target, name, index)
        source_name = _resolve_pc(source, None, index)
        key = (target_pc, source_pc, name, target_name, source_name)
        row = grouped.setdefault(key, {
            "target": target_pc,
            "target_function": target_name,
            "source": source_pc,
            "source_function": source_name,
            "name": name,
            "calls": 0,
            "executor_entries": 0,
            "inclusive_ns": 0,
            "exclusive_ns": 0,
        })
        for field in raw_totals:
            value = number(route.get(field), 0)
            row[field] += value
            raw_totals[field] += value
    result = sorted(grouped.values(), key=lambda row: (-row["exclusive_ns"], -row["inclusive_ns"], row["target"] or 0))
    # Deliberately no combined route-time total: inclusive intervals overlap.
    return result, raw_totals


def _draw_summary(draws: Any) -> dict[str, Any]:
    if draws is None:
        draws = []
    if not isinstance(draws, list):
        raise ValueError("draws detail must be an array")
    kinds: collections.Counter[str] = collections.Counter()
    textures: dict[tuple[Any, ...], dict[str, Any]] = {}
    gpu: dict[str, dict[str, int]] = {}
    ensure_total = submit_total = vertex_total = 0
    records = 0
    origins: collections.Counter[tuple[int | None, int | None]] = collections.Counter()
    for draw in draws:
        if not isinstance(draw, dict):
            continue
        records += 1
        kind = str(draw.get("kind", "unknown"))
        kinds[kind] += 1
        origin = draw.get("origin") or {}
        target = _pc(origin.get("target")) if isinstance(origin, dict) else None
        source = _pc(origin.get("source")) if isinstance(origin, dict) else None
        origins[(target, source)] += 1
        texture = draw.get("texture")
        dims = (draw.get("texture_width"), draw.get("texture_height"))
        revision = draw.get("texture_revision")
        native_format = draw.get("native_format")
        texture_key = (texture, dims[0], dims[1], revision, native_format)
        tex_row = textures.setdefault(texture_key, {
            "texture": texture,
            "width": dims[0],
            "height": dims[1],
            "revision": revision,
            "native_format": native_format,
            "draws": 0,
            "vertices": 0,
        })
        tex_row["draws"] += 1
        vertices = number(draw.get("vertex_count"), 0)
        tex_row["vertices"] += vertices
        vertex_total += vertices
        ensure_total += number(draw.get("ensure_ns"), 0)
        submit_total += number(draw.get("submit_ns"), 0)
        for gpu_pass in draw.get("gpu_passes") or []:
            if not isinstance(gpu_pass, dict):
                continue
            primitive = str(gpu_pass.get("primitive", "unknown"))
            row = gpu.setdefault(primitive, {"passes": 0, "vertices": 0, "bytes": 0})
            row["passes"] += 1
            row["vertices"] += number(gpu_pass.get("vertices"), 0)
            row["bytes"] += number(gpu_pass.get("bytes"), 0)

    return {
        "records": records,
        "kinds": dict(sorted(kinds.items())),
        "origin_draw_counts": [
            {"target": target, "source": source, "draws": count}
            for (target, source), count in sorted(origins.items(), key=lambda item: (-item[1], item[0]))
        ],
        "texture_usage": sorted(textures.values(), key=lambda row: (-row["draws"], str(row["texture"]))),
        "vertex_count_sum": vertex_total,
        "gpu_passes_by_primitive": dict(sorted(gpu.items())),
        "cpu_wall_ns": {
            "ensure_sum": ensure_total,
            "submit_sum": submit_total,
            "submit_is_gpu_time": False,
        },
    }


def analyze_frame(document: Any, index: FunctionIndex) -> dict[str, Any]:
    if isinstance(document, list):
        if len(document) != 1:
            raise ValueError("input must contain exactly one frame; select a single frame first")
        document = document[0]
    if not isinstance(document, dict):
        raise ValueError("trace root must be a JSON object")
    if document.get("format") != "p3fes-native-frame-v1":
        raise ValueError(f"unsupported trace format: {document.get('format')!r}")
    frame = document.get("frame") or {}
    if not isinstance(frame, dict):
        raise ValueError("frame must be an object")
    draw_records = document.get("draws", [])
    if not draw_records and isinstance(frame.get("draws"), list):
        draw_records = frame["draws"]
    routes, route_fields = _route_summary(document.get("routes", []), index)
    draw_summary = _draw_summary(draw_records)
    frame_draw_count = _frame_count(frame.get("draws"))
    frame_mesh_count = _frame_count(frame.get("meshes"))
    frame_texture_count = _frame_count(frame.get("resident_textures"))
    warnings = []
    if frame_draw_count is not None and frame_draw_count != draw_summary["records"]:
        warnings.append(f"frame.draws={frame_draw_count} but detail records={draw_summary['records']}")
    return {
        "format": document["format"],
        "frame": {
            "sequence": frame.get("sequence"),
            "draws": frame_draw_count,
            "meshes": frame_mesh_count,
            "resident_textures": frame_texture_count,
            "is_72_draw_0_mesh_frame": frame_draw_count == 72 and frame_mesh_count == 0,
        },
        "timings": document.get("timings", {}),
        "counters": document.get("counters", {}),
        "routes": routes,
        "route_field_sums_non_additive": route_fields,
        "draws": draw_summary,
        "warnings": warnings,
    }


def _format_time(value: Any) -> str:
    return f"{number(value):,} ns"


def render_summary(report: dict[str, Any]) -> str:
    frame = report["frame"]
    timings = report["timings"]
    counters = report["counters"]
    draws = report["draws"]
    lines = [
        f"Frame {frame['sequence']}: draws={frame['draws']} meshes={frame['meshes']} resident_textures={frame['resident_textures']}",
        f"Detailed draws={draws['records']} vertices={draws['vertex_count_sum']} textures={len(draws['texture_usage'])}",
        "Timings: " + ", ".join(f"{key}={_format_time(value)}" for key, value in timings.items()),
        "Counters: " + ", ".join(f"{key}={value}" for key, value in counters.items()),
        "Routes (exclusive first; inclusive is overlapping, totals are not additive):",
    ]
    for route in report["routes"][:20]:
        label = route["name"] or route["target_function"] or "unknown"
        target_text = f"0x{route['target']:08X}" if route["target"] is not None else "—"
        lines.append(
            f"  {label} target={target_text} source={('0x%08X' % route['source']) if route['source'] is not None else '—'}"
            f" calls={route['calls']} entries={route['executor_entries']}"
            f" excl={_format_time(route['exclusive_ns'])} incl={_format_time(route['inclusive_ns'])}"
        )
    lines.append("Draw submission: " + ", ".join(
        f"{key}={_format_time(value)}" for key, value in draws["cpu_wall_ns"].items() if key.endswith("sum")
    ) + " (CPU wall time; submit is not GPU time)")
    if draws["gpu_passes_by_primitive"]:
        lines.append("GPU pass payload: " + json.dumps(draws["gpu_passes_by_primitive"], sort_keys=True))
    if draws["texture_usage"]:
        lines.append("Textures: " + json.dumps(draws["texture_usage"], ensure_ascii=False, separators=(",", ":")))
    if report["frame"]["is_72_draw_0_mesh_frame"]:
        lines.append("Shape note: this frame matches the observed 72-draw / 0-mesh case.")
    for warning in report["warnings"]:
        lines.append(f"Warning: {warning}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="one p3fes-native-frame-v1 JSON trace")
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP, help=f"function map CSV (default: {DEFAULT_MAP})")
    parser.add_argument("--json", action="store_true", help="write compact machine-readable JSON")
    args = parser.parse_args(argv)
    try:
        document = json.loads(args.trace.read_text(encoding="utf-8"))
        report = analyze_frame(document, FunctionIndex.from_csv(args.map))
        if args.json:
            print(json.dumps(report, separators=(",", ":")))
        else:
            print(render_summary(report))
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"frame_trace_report: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
