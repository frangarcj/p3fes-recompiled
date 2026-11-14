#!/usr/bin/env python3
"""Extract selected BGM.CVM ADX tracks and prepare native 48 kHz PCM WAVs.

The tool reads the user's BGM.CVM and only writes derived audio into the
requested output folder. It does not convert every track unless each is
explicitly selected with --track.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

import p3fes_unpack


DEFAULT_CVM = Path("game-mac/BGM.CVM")
DEFAULT_OUT = Path("game-mac/native_bgm")
OUTPUT_RATE = 48000
OUTPUT_CHANNELS = 2
RECIPE_VERSION = "adx-to-pcm-s16le-wav-48k-stereo-trim-smpl-inclusive-v2"
MANIFEST_FORMAT = "p3fes-native-bgm-v1"


def ffprobe_path(ffmpeg: str) -> str:
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
         "format=format_name,format_long_name,duration:"
         "stream=codec_type,codec_name,profile,sample_fmt,sample_rate,channels,"
         "channel_layout,bits_per_sample,bits_per_raw_sample,duration,duration_ts,time_base",
         "-of", "json", str(media)],
        check=True, capture_output=True, text=True,
    )
    return json.loads(result.stdout)


def stream_metadata(info: dict) -> dict:
    audio = next((stream for stream in info.get("streams", [])
                  if stream.get("codec_type") == "audio"), None)
    if audio is None:
        raise RuntimeError("ffprobe found no audio stream")
    return {
        "container": info.get("format", {}).get("format_name"),
        "codec": audio.get("codec_name"),
        "profile": audio.get("profile"),
        "sample_fmt": audio.get("sample_fmt"),
        "sample_rate": int(audio["sample_rate"]) if audio.get("sample_rate") else None,
        "channels": audio.get("channels"),
        "channel_layout": audio.get("channel_layout"),
        "bits_per_sample": audio.get("bits_per_sample"),
        "bits_per_raw_sample": audio.get("bits_per_raw_sample"),
        "duration": round(float(audio["duration"]), 6) if audio.get("duration") else None,
        "duration_ts": int(audio["duration_ts"]) if audio.get("duration_ts") else None,
        "time_base": audio.get("time_base"),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_adx_header(data: bytes, file_size: int) -> dict:
    """Parse standard CRI ADX v3/v4 loop metadata with checked bounds."""
    if len(data) < 0x20:
        raise RuntimeError("ADX header is truncated")
    if struct.unpack_from(">H", data, 0)[0] != 0x8000:
        raise RuntimeError("ADX signature 0x8000 is missing")
    header_size = struct.unpack_from(">H", data, 2)[0] + 4
    encoding, frame_size, bits_per_sample, channels = struct.unpack_from(">BBBB", data, 4)
    sample_rate, sample_count = struct.unpack_from(">II", data, 8)
    version = struct.unpack_from(">H", data, 0x12)[0]
    if encoding != 3 or frame_size != 0x12 or bits_per_sample != 4:
        raise RuntimeError(
            f"Unsupported ADX encoding parameters: encoding={encoding}, "
            f"frame_size={frame_size}, bits_per_sample={bits_per_sample}"
        )
    if channels not in (1, 2):
        raise RuntimeError(f"Expected mono/stereo BGM ADX, got {channels} channels")
    if sample_rate <= 0 or sample_count <= 0 or header_size > file_size:
        raise RuntimeError("ADX sample rate/count/header size is invalid")

    if version == 0x0300:
        loop_offset = 0x14
    elif version == 0x0400:
        history_size = 4 * channels if channels > 1 else 8
        loop_offset = 0x18 + history_size
    else:
        raise RuntimeError(
            f"Unsupported ADX version 0x{version:04x}; only clear-text v3/v4 is handled"
        )

    if len(data) < loop_offset + 0x18:
        raise RuntimeError("ADX loop metadata is truncated")
    if header_size - 6 < loop_offset + 0x18:
        # Some otherwise valid v3/v4 tracks omit loop metadata. Treat these as
        # non-looping; never infer a loop from audio data or file duration.
        return {
            "version": version,
            "header_size": header_size,
            "channels": channels,
            "sample_rate": sample_rate,
            "sample_count": sample_count,
            "loop": None,
        }

    loop_count = struct.unpack_from(">I", data, loop_offset + 4)[0]
    loop_start = struct.unpack_from(">I", data, loop_offset + 8)[0]
    loop_end_exclusive = struct.unpack_from(">I", data, loop_offset + 16)[0]
    loop = None
    if loop_count:
        if not (0 <= loop_start < loop_end_exclusive <= sample_count):
            raise RuntimeError(
                f"Invalid ADX loop range [{loop_start}, {loop_end_exclusive}) "
                f"for {sample_count} samples"
            )
        loop = {"start_sample": loop_start, "end_sample_exclusive": loop_end_exclusive}

    return {
        "version": version,
        "header_size": header_size,
        "channels": channels,
        "sample_rate": sample_rate,
        "sample_count": sample_count,
        "loop": loop,
    }


def pcm_wav_frames(path: Path) -> tuple[int, list[tuple[bytes, bytes]], bytes]:
    """Return frame count and RIFF chunks while requiring stereo 48 kHz PCM16."""
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError("ffmpeg output is not a RIFF/WAVE file")
    riff_size = struct.unpack_from("<I", data, 4)[0]
    if riff_size + 8 != len(data):
        raise RuntimeError("WAV RIFF length does not match the file size")

    chunks: list[tuple[bytes, bytes]] = []
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        start = pos + 8
        end = start + chunk_size
        if end > len(data):
            raise RuntimeError(f"WAV chunk {chunk_id!r} extends past file end")
        payload = data[start:end]
        chunks.append((chunk_id, payload))
        if chunk_id == b"fmt ":
            fmt = payload
        elif chunk_id == b"data":
            pcm = payload
        pos = end + (chunk_size & 1)

    if pos != len(data) or fmt is None or pcm is None or len(fmt) < 16:
        raise RuntimeError("WAV is missing a valid fmt or data chunk")
    encoding, channels, sample_rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if (encoding, channels, sample_rate, block_align, bits) != (1, 2, OUTPUT_RATE, 4, 16):
        raise RuntimeError(
            "Expected PCM16 stereo 48 kHz WAV, got "
            f"encoding={encoding}, channels={channels}, rate={sample_rate}, "
            f"block_align={block_align}, bits={bits}"
        )
    if len(pcm) % block_align:
        raise RuntimeError("WAV PCM payload is not a whole number of sample frames")
    return len(pcm) // block_align, chunks, fmt


def scaled_sample(sample: int, source_rate: int) -> int:
    return (sample * OUTPUT_RATE + source_rate // 2) // source_rate


def smpl_chunk(loop: dict, source_rate: int) -> tuple[bytes, dict]:
    start = scaled_sample(loop["start_sample"], source_rate)
    end_exclusive = scaled_sample(loop["end_sample_exclusive"], source_rate)
    if end_exclusive <= start:
        raise RuntimeError("Loop collapsed to zero samples when resampled to 48 kHz")
    end_inclusive = end_exclusive - 1
    # RIFF smpl: nine DWORDs, followed by one six-DWORD forward-loop record.
    payload = struct.pack(
        "<9I6I",
        0, 0, round(1_000_000_000 / OUTPUT_RATE), 60, 0, 0, 0, 1, 0,
        0, 0, start, end_inclusive, 0, 0,
    )
    return b"smpl" + struct.pack("<I", len(payload)) + payload, {
        "start_sample": start,
        "end_sample_inclusive": end_inclusive,
        "end_sample_exclusive": end_exclusive,
        "sample_rate": OUTPUT_RATE,
    }


def write_wav_with_loop(source: Path, destination: Path, loop: dict | None,
                        source_rate: int) -> dict:
    frames, chunks, fmt = pcm_wav_frames(source)
    converted_loop = None
    if loop:
        chunk, converted_loop = smpl_chunk(loop, source_rate)
        if converted_loop["end_sample_exclusive"] > frames:
            raise RuntimeError(
                f"Resampled ADX loop ends at {converted_loop['end_sample_exclusive']} "
                f"but decoded WAV contains only {frames} frames"
            )
    else:
        chunk = None

    output_chunks = []
    inserted = False
    for chunk_id, payload in chunks:
        if chunk_id == b"smpl":
            continue
        output_chunks.append(chunk_id + struct.pack("<I", len(payload)) + payload +
                             (b"\0" if len(payload) & 1 else b""))
        if chunk_id == b"fmt " and chunk is not None:
            output_chunks.append(chunk)
            inserted = True
    if chunk is not None and not inserted:
        raise RuntimeError("Could not insert WAV smpl chunk after fmt")
    riff_body = b"WAVE" + b"".join(output_chunks)
    if len(riff_body) > 0xFFFFFFFF:
        raise RuntimeError("WAV exceeds classic RIFF's 32-bit size limit")
    destination.write_bytes(b"RIFF" + struct.pack("<I", len(riff_body)) + riff_body)
    return {
        "frames": frames,
        "sample_rate": OUTPUT_RATE,
        "channels": OUTPUT_CHANNELS,
        "bits_per_sample": 16,
        "loop": converted_loop,
    }


def record_matches(path: Path, record: dict, source_hash: str,
                   source_info: dict, ffprobe: str) -> bool:
    if not path.is_file() or record.get("source_sha256") != source_hash:
        return False
    if record.get("recipe_version") != RECIPE_VERSION:
        return False
    expected_hash = record.get("output_sha256")
    if not expected_hash or sha256_file(path) != expected_hash:
        return False
    try:
        actual_streams = stream_metadata(probe(ffprobe, path))
        frames, chunks, _ = pcm_wav_frames(path)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, KeyError):
        return False
    smpl = next((payload for chunk_id, payload in chunks if chunk_id == b"smpl"), None)
    loop = record.get("loop")
    if smpl is not None and len(smpl) >= 60:
        sampler_loop_count = struct.unpack_from("<I", smpl, 28)[0]
        actual_loop = None
        if sampler_loop_count:
            start, end = struct.unpack_from("<II", smpl, 44)
            actual_loop = {"start_sample": start, "end_sample_inclusive": end,
                           "end_sample_exclusive": end + 1, "sample_rate": OUTPUT_RATE}
    else:
        sampler_loop_count = 0
        actual_loop = None
    return (
        actual_streams == record.get("output")
        and frames == record.get("frames")
        and sampler_loop_count == (1 if loop else 0)
        and actual_loop == loop
        and record.get("source") == source_info
    )


def find_track(entries: list[dict], requested: str) -> dict:
    normalized = requested.replace("\\", "/").strip("/").casefold()
    if not normalized.endswith(".adx"):
        normalized += ".adx"
    matches = [entry for entry in entries
               if entry["path"].replace("\\", "/").strip("/").casefold() == normalized]
    if not matches and "/" not in normalized:
        matches = [entry for entry in entries
                   if Path(entry["path"]).name.casefold() == normalized]
    if len(matches) != 1:
        choices = "\n".join(f"  {entry['path']}" for entry in entries)
        if not matches:
            raise RuntimeError(f"ADX track not found in BGM.CVM: {requested}\nAvailable tracks:\n{choices}")
        raise RuntimeError(f"ADX track name is ambiguous: {requested}; use its full CVM path")
    return matches[0]


def convert_track(inner, entry: dict, out_dir: Path, ffmpeg: str,
                  ffprobe: str, cached: dict | None) -> tuple[dict, bool]:
    original_path = entry["path"].replace("\\", "/")
    stem = Path(original_path).stem
    output_path = out_dir / f"{stem}.wav"
    out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="p3fes-bgm-") as temp_dir:
        source_path = Path(temp_dir) / f"{stem}.ADX"
        view = p3fes_unpack.FileView(inner, entry)
        p3fes_unpack.write_range(view, 0, entry["size"], str(source_path))
        source_hash = sha256_file(source_path)
        with source_path.open("rb") as source_file:
            adx_header = parse_adx_header(source_file.read(0x100), entry["size"])
        source_info = {
            **adx_header,
            "sha256": source_hash,
            "source_codec": stream_metadata(probe(ffprobe, source_path))["codec"],
        }
        if cached and record_matches(output_path, cached, source_hash, source_info, ffprobe):
            return cached, True

        expected_frames = scaled_sample(adx_header["sample_count"], adx_header["sample_rate"])

        fd, decoded_name = tempfile.mkstemp(prefix=f".{stem}.", suffix=".decoded.tmp.wav", dir=out_dir)
        os.close(fd)
        decoded = Path(decoded_name)
        fd, final_name = tempfile.mkstemp(prefix=f".{stem}.", suffix=".validated.tmp.wav", dir=out_dir)
        os.close(fd)
        validated = Path(final_name)
        try:
            command = [
                ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(source_path),
                "-map", "0:a:0", "-vn", "-c:a", "pcm_s16le", "-ar", str(OUTPUT_RATE),
                "-ac", str(OUTPUT_CHANNELS), "-af",
                f"aresample={OUTPUT_RATE},atrim=end_sample={expected_frames},asetpts=N/SR/TB",
                "-f", "wav", str(decoded),
            ]
            try:
                subprocess.run(command, check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as exc:
                detail = (exc.stderr or "") + (exc.stdout or "")
                raise RuntimeError(f"ffmpeg failed to decode {original_path}:\n{detail.strip()}") from exc

            decoded_info = probe(ffprobe, decoded)
            output_streams = stream_metadata(decoded_info)
            if (output_streams.get("codec") != "pcm_s16le"
                    or output_streams.get("sample_rate") != OUTPUT_RATE
                    or output_streams.get("channels") != OUTPUT_CHANNELS):
                raise RuntimeError(f"ffprobe rejected PCM output for {original_path}: {output_streams}")

            output_audio = write_wav_with_loop(
                decoded, validated, adx_header["loop"], adx_header["sample_rate"]
            )
            if output_audio["frames"] != expected_frames:
                raise RuntimeError(
                    f"ADX header declares {expected_frames} output frames at 48 kHz, "
                    f"but ffmpeg produced {output_audio['frames']} for {original_path}"
                )
            final_streams = stream_metadata(probe(ffprobe, validated))
            if final_streams.get("codec") != "pcm_s16le":
                raise RuntimeError(f"ffprobe rejected final WAV for {original_path}: {final_streams}")
            output_hash = sha256_file(validated)
            os.replace(validated, output_path)
            record = {
                "path": original_path,
                "output_stem": stem,
                "output_file": output_path.name,
                "source_sha256": source_hash,
                "output_sha256": output_hash,
                "recipe_version": RECIPE_VERSION,
                "source": source_info,
                "output": final_streams,
                **output_audio,
            }
            return record, False
        finally:
            decoded.unlink(missing_ok=True)
            validated.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="List or convert selected ADX tracks from a Persona 3 FES BGM.CVM."
    )
    parser.add_argument("cvm", nargs="?", default=str(DEFAULT_CVM),
                        help=f"path to BGM.CVM (default: {DEFAULT_CVM})")
    parser.add_argument("--list", action="store_true", help="list ADX paths and sizes; do not convert")
    parser.add_argument("--track", action="append", default=[], metavar="NAME.ADX",
                        help="convert one track by basename or CVM path (repeatable)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help=f"output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    args = parser.parse_args()
    if args.list and args.track:
        parser.error("--list cannot be combined with --track")

    cvm_path = Path(args.cvm)
    if not cvm_path.is_file():
        parser.error(f"CVM not found: {cvm_path}")

    try:
        cvm_file = cvm_path.open("rb")
        size = os.fstat(cvm_file.fileno()).st_size
        opened = p3fes_unpack.open_cvm(cvm_file, {"lba": 0, "size": size})
        if opened is None:
            cvm_file.close()
            raise RuntimeError(f"Could not read an ISO9660 filesystem from CVM: {cvm_path}")
        inner, all_entries = opened
        entries = [entry for entry in all_entries if entry["path"].upper().endswith(".ADX")]
        try:
            if args.list or not args.track:
                for entry in entries:
                    print(f"{entry['path']}\t{entry['size']} bytes")
                print(f"{len(entries)} ADX tracks")
                return 0

            ffprobe = ffprobe_path(args.ffmpeg)
            selected = [find_track(entries, name) for name in args.track]
            stems = [Path(entry["path"]).stem.casefold() for entry in selected]
            if len(stems) != len(set(stems)):
                raise RuntimeError("Selected tracks have duplicate output stems")

            out_dir = Path(args.out)
            manifest_path = out_dir / "manifest.json"
            existing = {
                item.get("path", "").casefold(): item
                for item in json.loads(manifest_path.read_text(encoding="utf-8")).get("tracks", [])
            } if manifest_path.exists() else {}

            for entry in selected:
                key = entry["path"].replace("\\", "/").casefold()
                print(f"Checking {entry['path']} ...", flush=True)
                record, reused = convert_track(
                    inner, entry, out_dir, args.ffmpeg, ffprobe, existing.get(key)
                )
                existing[record["path"].casefold()] = record
                action = "Reused" if reused else "Wrote"
                loop = record["loop"]
                loop_note = (f", loop [{loop['start_sample']}, {loop['end_sample_inclusive']}] @ 48 kHz"
                             if loop else ", no loop")
                print(f"  {action} {out_dir / record['output_file']} "
                      f"({record['frames']} frames, 48 kHz stereo PCM16{loop_note}, "
                      f"sha256 {record['output_sha256']})")

            manifest = {
                "format": MANIFEST_FORMAT,
                "tracks": sorted(existing.values(), key=lambda item: item["path"].casefold()),
            }
            out_dir.mkdir(parents=True, exist_ok=True)
            fd, temp_manifest_name = tempfile.mkstemp(
                prefix=".manifest.", suffix=".tmp.json", dir=out_dir
            )
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as manifest_file:
                    manifest_file.write(json.dumps(manifest, indent=2) + "\n")
                os.replace(temp_manifest_name, manifest_path)
            finally:
                Path(temp_manifest_name).unlink(missing_ok=True)
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
