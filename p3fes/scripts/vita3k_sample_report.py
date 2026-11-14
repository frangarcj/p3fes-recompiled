#!/usr/bin/env python3
"""Map Vita3K/Dynarmic host PCs in an Apple `sample` call graph to guest functions.

The parser consumes the indented `Call graph:` section, treating each row's
sample count as inclusive and subtracting its direct children's counts. This
attributes counts to leaves only, so callers are not double-counted. A leaf
row with several inline PCs is reported as ambiguous instead of assigning its
count to an arbitrary address. Rows with raw stacks can instead be delimited
by `Sample N:` headings; a multi-PC leaf frame is treated the same way.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import dataclasses
import os
import re
import subprocess
import sys
from pathlib import Path


HEX_RE = re.compile(r"(?<![\w])0x([0-9a-fA-F]{8,16})(?![\w])", re.I)
SAMPLE_RE = re.compile(r"^\s*(?:sample|stack)\s+#?\d+\s*:?\s*$", re.I)
COUNT_RE = re.compile(r"^([ \t]*(?:(?:[+!|:])[ \t]*)*)(\d+)\s+(.*)$")


@dataclasses.dataclass
class Mapping:
    start: int
    size: int
    guest: int
    isa: str
    cache: str
    timestamp: int
    clear: int | None = None

    @property
    def end(self) -> int:
        return self.start + self.size


@dataclasses.dataclass
class ParsedSamples:
    leaves: collections.Counter[int]
    leaf_host_labels: dict[int, str]
    no_pc_leaf_samples: int
    multi_pc_leaf_samples: int
    total_root_samples: int
    max_thread_samples: int
    total_leaf_samples: int


def parse_map(path: Path) -> list[Mapping]:
    entries: list[Mapping] = []
    clears: dict[str, list[int]] = collections.defaultdict(list)
    for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        fields = line.split()
        if len(fields) >= 4 and fields[0] == "#" and fields[1] == "clear_cache":
            if len(fields) < 4:
                continue
            try:
                clears[fields[2]].append(int(fields[3], 10))
            except ValueError:
                raise ValueError(f"{path}:{lineno}: invalid clear-cache event")
            continue
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) < 6:
            raise ValueError(f"{path}:{lineno}: expected 6 map columns")
        try:
            entries.append(Mapping(int(fields[0], 16), int(fields[1], 16),
                                   int(fields[2], 0), fields[3], fields[4],
                                   int(fields[5], 10)))
        except ValueError as exc:
            raise ValueError(f"{path}:{lineno}: invalid map record: {exc}") from exc
    for timestamps in clears.values():
        timestamps.sort()
    for entry in entries:
        timestamps = clears.get(entry.cache)
        if timestamps:
            index = bisect.bisect_left(timestamps, entry.timestamp)
            if index < len(timestamps):
                entry.clear = timestamps[index]
    return entries



class MappingIndex:
    """Find every containing interval without scanning the whole JIT map."""
    def __init__(self, entries: list[Mapping]):
        self.entries = sorted(entries, key=lambda entry: entry.start)
        self.starts = [entry.start for entry in self.entries]
        self.max_ends: list[int] = []
        end = 0
        for entry in self.entries:
            end = max(end, entry.end)
            self.max_ends.append(end)

    def find(self, pc: int) -> list[Mapping]:
        matches = []
        index = bisect.bisect_right(self.starts, pc) - 1
        while index >= 0 and self.max_ends[index] > pc:
            entry = self.entries[index]
            if pc < entry.end:
                matches.append(entry)
            index -= 1
        return matches


def row_pcs(text: str) -> tuple[int, ...]:
    # Prefer tokens that actually resolve into one of the JIT ranges. The caller
    # passes arbitrary sample text, so numeric counts and other offsets are not
    # sufficient evidence by themselves.
    # Apple `sample` emits absolute PCs in [0x...]; prefer those so thread IDs,
    # offsets and hexadecimal-looking numbers in symbols cannot become PCs. A
    # bracket can contain several PCs for collapsed inline frames; preserve all
    # of them so a sample is never silently assigned to just the last address.
    bracket_groups = re.findall(r"\[([^\]]*)\]", text)
    matches: list[str] = []
    for group in bracket_groups:
        matches.extend(HEX_RE.findall(group))
    if not matches:
        matches = HEX_RE.findall(text)
    return tuple(dict.fromkeys(int(match, 16) for match in matches))


def row_pc_list_truncated(text: str) -> bool:
    """Whether a bracketed PC list shows an ellipsis after one or more PCs."""
    for group in re.findall(r"\[([^\]]*)\]", text):
        if HEX_RE.search(group) and ("..." in group or "…" in group):
            return True
    return False


def row_pc(text: str) -> int | None:
    """Return a PC only when the sample row identifies one unambiguous address."""
    pcs = row_pcs(text)
    return pcs[0] if len(pcs) == 1 and not row_pc_list_truncated(text) else None


def parse_callgraph(text: str) -> ParsedSamples:
    lines = text.splitlines()
    try:
        begin = next(i for i, line in enumerate(lines) if line.strip().lower() == "call graph:") + 1
    except StopIteration:
        begin = 0
    end = next((i for i in range(begin, len(lines))
                if lines[i].strip().lower().startswith(("total number in stack", "sort by", "binary images:", "sample analysis"))), len(lines))

    # Parse rows as an indentation tree. The count on each row is inclusive;
    # direct-child totals are subtracted to obtain counts attributed at that node.
    nodes: list[dict[str, int | str | tuple[int, ...] | bool | None]] = []
    stack: list[int] = []
    for line in lines[begin:end]:
        match = COUNT_RE.match(line)
        if not match:
            continue
        count = int(match.group(2))
        body = match.group(3)
        pcs = row_pcs(body)
        truncated_pc_list = row_pc_list_truncated(body)
        # Apple encodes tree depth in the count column, after branch markers.
        depth = match.start(2)
        while stack and int(nodes[stack[-1]]["depth"]) >= depth:
            stack.pop()
        label = re.sub(r"\s+\[0x[0-9a-fA-F]+\].*$", "", body).strip()
        node: dict[str, int | str | tuple[int, ...] | bool | None] = {"count": count, "pcs": pcs, "truncated_pc_list": truncated_pc_list, "depth": depth, "parent": stack[-1] if stack else None, "children": 0, "label": label}
        idx = len(nodes)
        nodes.append(node)
        if stack:
            nodes[stack[-1]]["children"] = int(nodes[stack[-1]]["children"]) + count
        stack.append(idx)

    leaves: collections.Counter[int] = collections.Counter()
    leaf_host_labels: dict[int, str] = {}
    no_pc_leaf_samples = multi_pc_leaf_samples = total_leaf_samples = 0
    root_counts = [int(node["count"]) for node in nodes if node["parent"] is None]
    total_root_samples = sum(root_counts)
    max_thread_samples = max(root_counts, default=0)
    for node in nodes:
        attributed = max(0, int(node["count"]) - int(node["children"]))
        total_leaf_samples += attributed
        if attributed:
            pcs = node["pcs"]
            truncated_pc_list = bool(node["truncated_pc_list"])
            if isinstance(pcs, tuple) and len(pcs) == 1 and not truncated_pc_list:
                pc = pcs[0]
                leaves[pc] += attributed
                leaf_host_labels[pc] = str(node["label"])
            elif isinstance(pcs, tuple) and (len(pcs) > 1 or truncated_pc_list):
                # Apple sample can collapse inline frames into one row with
                # several PCs. The row's count belongs to the sample once, but
                # the text does not identify which address is the sampled PC.
                multi_pc_leaf_samples += attributed
            else:
                no_pc_leaf_samples += attributed

    # For raw per-sample stacks, inspect the last printed frame. If that frame
    # collapses multiple inline PCs, keep its sample count separate as ambiguous.
    if not nodes:
        stack_leaf_pcs: tuple[int, ...] = ()
        raw_samples = 0
        raw_no_pc_samples = 0
        raw_multi_pc_samples = 0
        raw_active = False
        raw_leaf_pc_list_truncated = False

        def finish_raw_stack() -> None:
            nonlocal raw_samples, raw_no_pc_samples, raw_multi_pc_samples
            nonlocal stack_leaf_pcs, raw_active, raw_leaf_pc_list_truncated
            if not raw_active:
                return
            raw_samples += 1
            if len(stack_leaf_pcs) == 1 and not raw_leaf_pc_list_truncated:
                leaves[stack_leaf_pcs[0]] += 1
            elif len(stack_leaf_pcs) > 1 or raw_leaf_pc_list_truncated:
                raw_multi_pc_samples += 1
            else:
                raw_no_pc_samples += 1
            stack_leaf_pcs = ()
            raw_active = False
            raw_leaf_pc_list_truncated = False

        for line in lines:
            if SAMPLE_RE.match(line):
                finish_raw_stack()
                raw_active = True
                continue
            if not line.strip():
                finish_raw_stack()
                continue
            if not raw_active:
                continue
            line_pcs = row_pcs(line)
            if line_pcs:
                stack_leaf_pcs = line_pcs
                raw_leaf_pc_list_truncated = row_pc_list_truncated(line)
        finish_raw_stack()
        no_pc_leaf_samples = raw_no_pc_samples
        multi_pc_leaf_samples = raw_multi_pc_samples
        total_root_samples = max_thread_samples = total_leaf_samples = raw_samples
    return ParsedSamples(leaves, leaf_host_labels, no_pc_leaf_samples, multi_pc_leaf_samples,
                         total_root_samples, max_thread_samples, total_leaf_samples)


def addr2line_batch(elf: Path, tool: str, pcs: list[int]) -> dict[int, str]:
    if not pcs:
        return {}
    cmd = [tool, "-a", "-f", "-C", "-i", "-e", str(elf), *(f"0x{pc:x}" for pc in pcs)]
    proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        raise RuntimeError(f"addr2line failed ({proc.returncode}): {proc.stderr.strip()}")
    results: dict[int, str] = {}
    current: int | None = None
    funcs: list[str] = []
    for line in proc.stdout.splitlines():
        if line.startswith("0x"):
            if current is not None:
                results[current] = next((f for f in funcs if f and f != "??"), "??")
            try:
                current = int(line, 16)
            except ValueError:
                current = None
            funcs = []
        elif current is not None and len(funcs) % 2 == 0:
            funcs.append(line.strip())
        elif current is not None:
            funcs.append("")  # consume corresponding file:line row
    if current is not None:
        results[current] = next((f for f in funcs if f and f != "??"), "??")
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sample", type=Path, help="Apple sample output text")
    ap.add_argument("map", type=Path, help="VITA3K_JIT_PERF_MAP output")
    ap.add_argument("elf", type=Path, help="guest eboot ELF for addr2line")
    ap.add_argument("--addr2line", default=os.environ.get("ARM_VITA_EABI_ADDR2LINE", "arm-vita-eabi-addr2line"))
    ap.add_argument("--since-ns", type=int, help="capture window start; retain ranges valid after this wall-clock ns")
    ap.add_argument("--until-ns", type=int, help="capture window end; retain ranges valid before this wall-clock ns")
    ap.add_argument("--at-ns", type=int, help="map sample PCs at this exact wall-clock ns (disambiguates cache reuse)")
    args = ap.parse_args()

    try:
        entries = parse_map(args.map)
        if args.at_ns is not None:
            entries = [e for e in entries if e.timestamp <= args.at_ns and (e.clear is None or args.at_ns < e.clear)]
        else:
            lo = args.since_ns if args.since_ns is not None else -(1 << 63)
            hi = args.until_ns if args.until_ns is not None else (1 << 63) - 1
            entries = [e for e in entries if e.timestamp <= hi and (e.clear is None or e.clear > lo)]
        parsed = parse_callgraph(args.sample.read_text(errors="replace"))
        grouped: collections.Counter[tuple[str, str]] = collections.Counter()
        ambiguous = 0
        unmapped = parsed.no_pc_leaf_samples
        unmapped_host: collections.Counter[str] = collections.Counter()
        guest_pcs: set[int] = set()
        pending: list[tuple[int, int, Mapping]] = []
        mapping_index = MappingIndex(entries)
        for host_pc, count in parsed.leaves.items():
            matches = mapping_index.find(host_pc)
            # If sample time was not supplied, multiple cache generations that
            # cover the capture interval are intentionally reported ambiguous.
            if len(matches) > 1:
                ambiguous += count
                continue
            if not matches:
                unmapped += count
                label = re.sub(r"\s*\[0x[0-9a-fA-F]+\]", "", parsed.leaf_host_labels.get(host_pc, "unknown host frame"))
                label = re.sub(r"\s+[A-Za-z0-9_./+-]+:\d+.*$", "", label).strip()
                unmapped_host[label or "unknown host frame"] += count
                continue
            entry = matches[0]
            pending.append((host_pc, count, entry))
            guest_pcs.add(entry.guest)
        names = addr2line_batch(args.elf, args.addr2line, sorted(guest_pcs))
        for host_pc, count, entry in pending:
            func = names.get(entry.guest, "??")
            grouped[(entry.isa, func)] += count
        for (isa, function), count in sorted(grouped.items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"{count:8d}  {isa:5s}  {function}")
        print(f"\nsampling ticks (max per-thread count): {parsed.max_thread_samples}; thread-stack observations (sum of roots): {parsed.total_root_samples}; leaf-attributed: {parsed.total_leaf_samples}; mapped: {sum(grouped.values())}; ambiguous (cache reuse/overlap): {ambiguous}; ambiguous multi-PC leaf rows: {parsed.multi_pc_leaf_samples}; unmapped/no-PC: {unmapped}")
        if parsed.total_root_samples != parsed.total_leaf_samples:
            print("warning: leaf attribution does not reconcile to top-level thread counts; inspect sample branch structure")
        if unmapped_host:
            print("top unmapped host leaves:")
            for label, count in unmapped_host.most_common(12):
                print(f"{count:8d}  {label}")
        if args.at_ns is None and (args.since_ns is None or args.until_ns is None):
            print("note: use --at-ns for a precise sample instant, or --since-ns/--until-ns for the capture window")
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
