#!/usr/bin/env python3
"""Compare browser-produced MP4s against the reference MP4 made by prepare_movies.py.

  python3 compare.py REF_DIR out1.mp4 [out2.mp4 ...]      # stem = file name up to the first dot

Checks metadata parity (not byte identity): codec, profile, pix_fmt, level, size,
r_frame_rate, frame count, audio codec/sample rate/channels, duration within 0.1 s.
Needs ffprobe on PATH. Exit status 1 if any check fails.
"""
import json, subprocess, sys
from pathlib import Path


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "format=duration:stream=codec_type,codec_name,profile,pix_fmt,level,width,height,"
         "r_frame_rate,avg_frame_rate,sample_rate,channels,nb_frames",
         "-of", "json", str(path)], check=True, capture_output=True, text=True).stdout
    d = json.loads(out)
    v = next(s for s in d["streams"] if s["codec_type"] == "video")
    a = next((s for s in d["streams"] if s["codec_type"] == "audio"), None)
    return v, a, float(d["format"]["duration"])


def main():
    ref_dir = Path(sys.argv[1])
    bad = 0
    for arg in sys.argv[2:]:
        p = Path(arg)
        stem = p.name.split(".")[0]
        rv, ra, rd = probe(ref_dir / f"{stem}.mp4")
        v, a, d = probe(p)
        checks = [
            ("video codec", rv["codec_name"], v["codec_name"]),
            ("profile", rv["profile"], v["profile"]),
            ("pix_fmt", rv["pix_fmt"], v["pix_fmt"]),
            ("level", rv["level"], v["level"]),
            ("size", f'{rv["width"]}x{rv["height"]}', f'{v["width"]}x{v["height"]}'),
            ("r_frame_rate", rv["r_frame_rate"], v["r_frame_rate"]),
            ("video frames", rv.get("nb_frames"), v.get("nb_frames")),
            ("audio codec", ra and ra["codec_name"], a and a["codec_name"]),
            ("audio rate", ra and ra["sample_rate"], a and a["sample_rate"]),
            ("audio channels", ra and ra["channels"], a and a["channels"]),
        ]
        print(f"== {p.name} vs {stem}.mp4")
        for name, want, got in checks:
            ok = want == got
            bad += not ok
            print(f"  {'ok ' if ok else 'BAD'} {name:15} ref={want} got={got}")
        ok = abs(rd - d) <= 0.1
        bad += not ok
        print(f"  {'ok ' if ok else 'BAD'} {'duration':15} ref={rd:.3f} got={d:.3f} (delta {d - rd:+.3f}s)")
        print(f"      size ref={(ref_dir / (stem + '.mp4')).stat().st_size} got={p.stat().st_size}")
    return 1 if bad else 0


raise SystemExit(main())
