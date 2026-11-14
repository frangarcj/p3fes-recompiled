#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frame_trace_report as report


class FrameTraceReportTests(unittest.TestCase):
    def test_ranges_native_routes_draws_and_overlapping_costs(self):
        with tempfile.TemporaryDirectory(prefix="p3fes-frame-report-") as temp:
            map_path = Path(temp) / "p3fes_map.csv"
            map_path.write_text(
                "Name,Start,End,Size\n"
                "Outer,0x1000,0x1100,256\n"
                "Inner,0x1040,0x1080,64\n"
                "Caller,0x2000,0x2100,256\n",
                encoding="utf-8",
            )
            functions = report.FunctionIndex.from_csv(map_path)

        document = {
            "format": "p3fes-native-frame-v1",
            "frame": {"sequence": 11, "draws": 72, "meshes": 0, "resident_textures": 3},
            "timings": {"producer_wall_ns": 800, "render_wall_ns": 700},
            "counters": {"guest_dispatches": 12},
            "routes": [
                {"target": "0x1044", "source": "0x2004", "calls": 2,
                 "executor_entries": 2, "inclusive_ns": 100, "exclusive_ns": 60},
                {"target": "0xF0000001", "source": "0x1044", "name": "native_skin",
                 "calls": 1, "executor_entries": 1, "inclusive_ns": 50, "exclusive_ns": 35},
            ],
            "draws": [
                {"index": 0, "kind": "mesh", "origin": {"target": "0x1044", "source": "0x2004"},
                 "texture": "tex/a", "texture_width": 64, "texture_height": 32,
                 "texture_revision": 4, "native_format": "RGBA8", "vertex_count": 3,
                 "ensure_ns": 10, "submit_ns": 20,
                 "gpu_passes": [{"primitive": "triangles", "vertices": 3, "bytes": 36}]},
                {"index": 1, "kind": "ui", "origin": {"target": "0xF0000001", "source": "0x1044"},
                 "texture": "tex/a", "texture_width": 64, "texture_height": 32,
                 "texture_revision": 4, "native_format": "RGBA8", "vertex_count": 6,
                 "ensure_ns": 5, "submit_ns": 7,
                 "gpu_passes": [{"primitive": "triangles", "vertices": 6, "bytes": 72}]},
            ],
        }

        summary = report.analyze_frame(document, functions)
        self.assertEqual(summary["routes"][0]["target_function"], "Inner")
        self.assertEqual(summary["routes"][1]["target_function"], "native_skin")
        self.assertEqual(summary["routes"][1]["source_function"], "Inner")
        # Inclusive (100+50) and exclusive (60+35) are shown independently;
        # neither is combined with the other, since route intervals overlap.
        self.assertEqual(summary["route_field_sums_non_additive"]["inclusive_ns"], 150)
        self.assertEqual(summary["route_field_sums_non_additive"]["exclusive_ns"], 95)
        self.assertEqual(summary["frame"]["is_72_draw_0_mesh_frame"], True)
        self.assertEqual(summary["draws"]["vertex_count_sum"], 9)
        self.assertEqual(summary["draws"]["gpu_passes_by_primitive"]["triangles"],
                         {"passes": 2, "vertices": 9, "bytes": 108})
        self.assertFalse(summary["draws"]["cpu_wall_ns"]["submit_is_gpu_time"])
        self.assertEqual(len(summary["draws"]["texture_usage"]), 1)

    def test_route_timings_are_not_folded_into_one_total(self):
        routes, sums = report._route_summary([
            {"target": 0x1000, "source": 0x2000, "inclusive_ns": 100, "exclusive_ns": 40},
            {"target": 0x1001, "source": 0x2000, "inclusive_ns": 70, "exclusive_ns": 30},
        ], report.FunctionIndex([]))
        self.assertEqual(len(routes), 2)
        self.assertEqual(sums["inclusive_ns"], 170)
        self.assertEqual(sums["exclusive_ns"], 70)
        self.assertNotIn("total_ns", sums)


if __name__ == "__main__":
    unittest.main()
