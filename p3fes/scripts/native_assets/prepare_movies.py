#!/usr/bin/env python3
"""Prepare selected Persona 3 FES SFD movies as Vita-friendly MP4 files.

This tool only reads the user's DATA.CVM and writes derived files to a separate
output directory. It never converts the whole archive unless --all is explicit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction

import p3fes_unpack


DEFAULT_CVM = Path("game-mac/DATA.CVM")
DEFAULT_OUT = Path("game-mac/native_movies")
RECIPE_VERSION = "h264-baseline-3.1-yuv420p-aac-48k-stereo-cap640x448-fps-passthrough-v1"


def ffprobe_path(ffmpeg: str) -> str:
    """Find ffprobe beside a custom ffmpeg, or on PATH."""
    candidate = Path(ffmpeg)
    if candidate.parent != Path("."):
        sibling = candidate.with_name("ffprobe" + candidate.suffix)
        if sibling.exists():
            return str(sibling)
    found = shutil.which("ffprobe")
    if found:
        return found
    raise RuntimeError("ffprobe was not found (install it with ffmpeg or put it on PATH)")


def probe(probe_exe: str, media: Path) -> dict:
    result = subprocess.run(
        [probe_exe, "-v", "error", "-show_entries",
         "format=format_name,format_long_name,duration:stream=codec_type,codec_name,profile,pix_fmt,level,width,height,r_frame_rate,avg_frame_rate,sample_rate,channels,channel_layout",
         "-of", "json", str(media)],
        check=True, capture_output=True, text=True,
    )
    return json.loads(result.stdout)


def rate_value(value: str | None) -> float | None:
    if not value or value in ("0/0", "N/A"):
        return None
    try:
        rate = float(Fraction(value))
    except (ValueError, ZeroDivisionError):
        return None
    return rate if rate > 0 else None


def media_metadata(info: dict) -> dict:
    streams = info.get("streams", [])
    video = next((stream for stream in streams if stream.get("codec_type") == "video"), None)
    if video is None:
        raise RuntimeError("ffprobe found no video stream")
    fmt = info.get("format", {})
    fps = rate_value(video.get("avg_frame_rate")) or rate_value(video.get("r_frame_rate"))
    duration = fmt.get("duration")
    if duration is None:
        raise RuntimeError("ffprobe did not report a movie duration")
    return {
        "duration": round(float(duration), 6),
        "fps": round(fps, 6) if fps is not None else None,
        "width": int(video["width"]),
        "height": int(video["height"]),
    }


def stream_metadata(info: dict) -> dict:
    streams = info.get("streams", [])
    video = next((stream for stream in streams if stream.get("codec_type") == "video"), None)
    audio = next((stream for stream in streams if stream.get("codec_type") == "audio"), None)
    if video is None:
        raise RuntimeError("ffprobe found no video stream")

    def video_fields(stream: dict) -> dict:
        return {
            "codec": stream.get("codec_name"),
            "profile": stream.get("profile"),
            "pix_fmt": stream.get("pix_fmt"),
            "level": stream.get("level"),
            "width": stream.get("width"),
            "height": stream.get("height"),
            "r_frame_rate": stream.get("r_frame_rate"),
            "avg_frame_rate": stream.get("avg_frame_rate"),
        }

    audio_fields = None
    if audio is not None:
        audio_fields = {
            "codec": audio.get("codec_name"),
            "profile": audio.get("profile"),
            "sample_rate": int(audio["sample_rate"]) if audio.get("sample_rate") else None,
            "channels": audio.get("channels"),
            "channel_layout": audio.get("channel_layout"),
        }
    fmt = info.get("format", {})
    return {
        "container": fmt.get("format_name"),
        "container_long_name": fmt.get("format_long_name"),
        "video": video_fields(video),
        "audio": audio_fields,
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_cached_output(path: Path, record: dict, source_hash: str,
                        source_info: dict, ffprobe: str) -> bool:
    if not path.is_file() or record.get("source_sha256") != source_hash:
        return False
    if record.get("recipe_version") != RECIPE_VERSION:
        return False
    expected_hash = record.get("output_sha256") or record.get("sha")
    if not expected_hash or sha256_file(path) != expected_hash:
        return False
    try:
        info = probe(ffprobe, path)
        actual = stream_metadata(info)
        metadata = media_metadata(info)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, KeyError):
        return False
    return (
        actual == record.get("output")
        and stream_metadata(source_info) == record.get("source")
        and metadata.get("duration") == record.get("duration")
        and metadata.get("fps") == record.get("fps")
        and metadata.get("width") == record.get("width")
        and metadata.get("height") == record.get("height")
    )


def movie_entries(cvm_path: Path) -> tuple[object, object, list[dict]]:
    f = open(cvm_path, "rb")
    size = os.fstat(f.fileno()).st_size
    opened = p3fes_unpack.open_cvm(f, {"lba": 0, "size": size})
    if opened is None:
        f.close()
        raise RuntimeError(f"Could not read an ISO9660 filesystem from CVM: {cvm_path}")
    inner, entries = opened
    return f, inner, [entry for entry in entries if entry["path"].upper().endswith(".SFD")]


def find_movie(entries: list[dict], requested: str) -> dict:
    normalized = requested.replace("\\", "/").strip("/").casefold()
    matches = [entry for entry in entries
               if entry["path"].replace("\\", "/").strip("/").casefold() == normalized]
    if not matches and "/" not in normalized:
        matches = [entry for entry in entries
                   if Path(entry["path"]).name.casefold() == normalized]
    if len(matches) != 1:
        choices = "\n".join(f"  {entry['path']}" for entry in entries)
        if not matches:
            raise RuntimeError(f"Movie not found in CVM: {requested}\nAvailable SFD files:\n{choices}")
        raise RuntimeError(f"Movie name is ambiguous: {requested}\nUse its full CVM path.")
    return matches[0]


def convert_movie(cvm_file, inner, entry: dict, out_dir: Path, ffmpeg: str,
                  ffprobe: str, cached: dict | None = None) -> tuple[dict, bool]:
    original_path = entry["path"].replace("\\", "/")
    source_stem = Path(original_path).stem
    out_path = out_dir / f"{source_stem}.mp4"
    out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="p3fes-movie-") as temp_dir:
        sfd_path = Path(temp_dir) / f"{source_stem}.SFD"
        view = p3fes_unpack.FileView(inner, entry)
        p3fes_unpack.write_range(view, 0, entry["size"], str(sfd_path))

        source_info = probe(ffprobe, sfd_path)
        source = media_metadata(source_info)
        source_hash = sha256_file(sfd_path)
        if cached and valid_cached_output(out_path, cached, source_hash, source_info, ffprobe):
            return cached, True

        has_audio = any(stream.get("codec_type") == "audio"
                        for stream in source_info.get("streams", []))
        scale = "scale=w='min(640,iw)':h='min(448,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2"
        out_dir.mkdir(parents=True, exist_ok=True)
        temp_fd, temp_name = tempfile.mkstemp(
            prefix=f".{source_stem}.", suffix=".tmp.mp4", dir=out_dir
        )
        os.close(temp_fd)
        temp_output = Path(temp_name)
        try:
            command = [
                ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(sfd_path),
                "-map", "0:v:0", "-map", "0:a:0?", "-vf", scale,
                "-c:v", "libx264", "-profile:v", "baseline", "-level:v", "3.1",
                "-pix_fmt", "yuv420p", "-fps_mode", "passthrough",
                "-c:a", "aac", "-ar", "48000", "-ac", "2", "-b:a", "128k",
                "-movflags", "+faststart", str(temp_output),
            ]
            try:
                subprocess.run(command, check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as exc:
                error = (exc.stderr or "") + (exc.stdout or "")
                if has_audio and any(token in error.lower() for token in
                                     ("decoder (codec adpcm_adx) not found", "unknown decoder 'adpcm_adx'",
                                      "decoder not found")):
                    raise RuntimeError(
                        f"ffmpeg cannot decode the ADX audio in {original_path}; "
                        "this movie requires an ffmpeg build with the ADX decoder and AAC encoder.\n"
                        f"{error.strip()}"
                    ) from exc
                raise RuntimeError(f"ffmpeg failed for {original_path}:\n{error.strip()}") from exc

            output_info = probe(ffprobe, temp_output)
            output_metadata = media_metadata(output_info)
            output_streams = stream_metadata(output_info)
            output_video = output_streams["video"]
            output_audio = output_streams["audio"]
            if (output_video.get("codec") != "h264" or
                    output_video.get("pix_fmt") != "yuv420p" or
                    output_video.get("level") != 31 or
                    output_video.get("width", 9999) > 640 or
                    output_video.get("height", 9999) > 448):
                raise RuntimeError(f"ffprobe rejected unexpected Vita video output for {original_path}: {output_video}")
            if has_audio and (not output_audio or output_audio.get("codec") != "aac" or
                              output_audio.get("sample_rate") != 48000 or
                              output_audio.get("channels") != 2):
                raise RuntimeError(f"ffprobe rejected missing or incompatible audio output for {original_path}: {output_audio}")

            output_hash = sha256_file(temp_output)
            os.replace(temp_output, out_path)
            record = {
                "path": original_path,
                "output_stem": source_stem,
                **output_metadata,
                "sha": output_hash,
                "output_sha256": output_hash,
                "source_sha256": source_hash,
                "recipe_version": RECIPE_VERSION,
                "source": stream_metadata(source_info),
                "output": output_streams,
                "audio": output_audio.get("codec") if output_audio else None,
                "source_width": source["width"],
                "source_height": source["height"],
            }
            return record, False
        finally:
            temp_output.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List or convert selected SFD movies from a Persona 3 FES DATA.CVM."
    )
    parser.add_argument("cvm", nargs="?", default=str(DEFAULT_CVM),
                        help=f"path to DATA.CVM (default: {DEFAULT_CVM})")
    parser.add_argument("--list", action="store_true", help="list SFD paths and sizes; do not convert")
    parser.add_argument("--movie", action="append", default=[], metavar="PATH",
                        help="convert one CVM path (repeatable; accepts a unique basename)")
    parser.add_argument("--all", action="store_true", help="convert every SFD in the CVM")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help=f"output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    args = parser.parse_args()

    if args.list and (args.movie or args.all):
        parser.error("--list cannot be combined with --movie or --all")
    if args.movie and args.all:
        parser.error("choose --movie or --all")

    cvm_path = Path(args.cvm)
    if not cvm_path.is_file():
        parser.error(f"CVM not found: {cvm_path}")
    try:
        cvm_file, inner, entries = movie_entries(cvm_path)
        try:
            if args.list or (not args.movie and not args.all):
                for entry in entries:
                    print(f"{entry['path']}\t{entry['size']} bytes")
                print(f"{len(entries)} SFD movies")
                return 0

            ffprobe = ffprobe_path(args.ffmpeg)
            selected = entries if args.all else [find_movie(entries, path) for path in args.movie]
            names = [Path(entry["path"]).stem.casefold() for entry in selected]
            if len(names) != len(set(names)):
                raise RuntimeError("Selected movies have duplicate output stems; select them by another tool.")

            out_dir = Path(args.out)
            manifest_path = out_dir / "manifest.json"
            existing = {item.get("path", "").casefold(): item
                        for item in json.loads(manifest_path.read_text(encoding="utf-8")).get("movies", [])} \
                if manifest_path.exists() else {}
            for entry in selected:
                key = entry["path"].replace("\\", "/").casefold()
                cached = existing.get(key)
                print(f"Checking {entry['path']} ...", flush=True)
                record, reused = convert_movie(cvm_file, inner, entry, out_dir, args.ffmpeg, ffprobe, cached)
                existing[record["path"].casefold()] = record
                action = "Reused" if reused else "Wrote"
                print(f"  {action} {out_dir / (record['output_stem'] + '.mp4')} "
                      f"({record['width']}x{record['height']}, {record['fps']} fps, "
                      f"{record['duration']} s, sha256 {record['sha']})")
            ordered = sorted(existing.values(), key=lambda item: item["path"].casefold())
            manifest = {"format": "p3fes-native-movies-v1", "movies": ordered}
            out_dir.mkdir(parents=True, exist_ok=True)
            manifest_fd, manifest_name = tempfile.mkstemp(prefix=".manifest.", suffix=".tmp.json", dir=out_dir)
            try:
                with os.fdopen(manifest_fd, "w", encoding="utf-8") as manifest_file:
                    manifest_file.write(json.dumps(manifest, indent=2) + "\n")
                os.replace(manifest_name, manifest_path)
            finally:
                Path(manifest_name).unlink(missing_ok=True)
            print(f"Wrote {manifest_path}")
        finally:
            cvm_file.close()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError,
            p3fes_unpack.ToolError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
