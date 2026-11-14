#!/usr/bin/env python3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vita3k_sample_report as report


class SampleParserTests(unittest.TestCase):
    def test_callgraph_single_and_multi_pc_leaf_counts(self):
        parsed = report.parse_callgraph("""Call graph:
    10 Thread_1: EE
    + 10 dispatch  [0x10000000]
    +   4 single leaf  [0x20000000]
    +   4 inline leaf  [0x30000000,0x30000004,...]
    +   2 truncated inline leaf  [0x40000000,...]
Total number in stack: 10
""")

        self.assertEqual(parsed.total_root_samples, 10)
        self.assertEqual(parsed.total_leaf_samples, 10)
        self.assertEqual(parsed.leaves, {0x20000000: 4})
        self.assertEqual(parsed.multi_pc_leaf_samples, 6)
        self.assertEqual(parsed.no_pc_leaf_samples, 0)
        self.assertEqual(report.row_pc("inline [0x30000000,0x30000004,...]"), None)
        self.assertEqual(report.row_pc("inline [0x30000000,...]"), None)
        self.assertEqual(report.row_pc("function ... at offset 123 [0x30000000]"), 0x30000000)
        self.assertEqual(report.row_pc("leaf [0x30000000]"), 0x30000000)

    def test_raw_stacks_do_not_assign_multi_pc_leaf_to_last_address(self):
        parsed = report.parse_callgraph("""Sample 1:
  caller [0x10000000]
  leaf [0x20000000]
Sample 2:
  caller [0x10000000]
  inline leaf [0x30000000,0x30000004]
Sample 3:
  caller [0x10000000]
  truncated inline leaf [0x40000000,...]
""")

        self.assertEqual(parsed.total_root_samples, 3)
        self.assertEqual(parsed.total_leaf_samples, 3)
        self.assertEqual(parsed.leaves, {0x20000000: 1})
        self.assertEqual(parsed.multi_pc_leaf_samples, 2)
        self.assertEqual(parsed.no_pc_leaf_samples, 0)

    def test_mapping_index_keeps_overlaps_and_exclusive_ends(self):
        entries = [report.Mapping(start, size, guest, "thumb", "A", 1)
                   for start, size, guest in [(30, 2, 3), (10, 20, 1), (15, 2, 2)]]
        index = report.MappingIndex(entries)
        for pc in [0, 9, 10, 14, 15, 16, 17, 29, 30, 31, 32, 100]:
            expected = sorted(e.guest for e in entries if e.start <= pc < e.end)
            self.assertEqual(sorted(e.guest for e in index.find(pc)), expected, hex(pc))

    def test_clear_cache_index_keeps_nearest_future_event_per_cache(self):
        with tempfile.TemporaryDirectory(prefix="vita3k-map-test-") as temp:
            path = Path(temp) / "map.txt"
            path.write_text("""1000 10 0x100 a32 A 10
2000 10 0x200 a32 A 30
3000 10 0x300 a32 B 10
# clear_cache A 20
# clear_cache A 40
# clear_cache B 15
""")
            entries = report.parse_map(path)

        self.assertEqual([entry.clear for entry in entries], [20, 40, 15])


if __name__ == "__main__":
    unittest.main()
