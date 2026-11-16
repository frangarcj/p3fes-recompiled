// In-browser conversion of Persona 3 FES .SFD movies (CRI Sofdec: MPEG-1/2 video + ADX
// audio) to Vita-friendly MP4, with exactly the recipe of
// p3fes/scripts/native_assets/prepare_movies.py:
//   H.264 Baseline L3.1 yuv420p, <= 640x448 (never upscaled), source frame rate
//   (-fps_mode passthrough), AAC 48 kHz stereo 128k, +faststart.
//
// Engine: ffmpeg.wasm (@ffmpeg/ffmpeg 0.12.15 JS wrapper, vendored under
// web/vendor/ffmpeg/, MIT) driving the stock @ffmpeg/core(-mt) 0.12.10 wasm builds
// (GPL-2.0-or-later: x264, loaded from a CDN at runtime, never redistributed here).
//
// Usage:
//   const conv = await createMovieConverter({ onLog });
//   const mp4 = await conv.convert('P3CTOP2', sfdUint8ArrayOrBlob, f => ...);
//   conv.terminate();

import { FFmpeg } from '../vendor/ffmpeg/ffmpeg/index.js';
import { toBlobURL } from '../vendor/ffmpeg/util/index.js';

// Bump whenever the ffmpeg arguments (or the validation rules) below change. The page
// stores it next to cached MP4s so stale conversions are redone.
export const MOVIE_RECIPE_VERSION =
  'h264-baseline-3.1-yuv420p-aac-48k-stereo-cap640x448-fps-passthrough-v1';

// Stems the page always converts up front (all verified present as /SFD/<stem>.SFD in
// DATA.CVM; see web/test/movies/README.md).
export const EARLY_MOVIES = ['P3CTOP2', 'P3CTOP', 'GAME_START', 'P3CT001_01', 'P3CT001_02'];

export const FFMPEG_CORE_VERSION = '0.12.10';
const CORE_CDNS = [
  'https://cdn.jsdelivr.net/npm',
  'https://unpkg.com',
];

// Same filter / codec arguments as prepare_movies.py (keep in sync, bump the version).
const SCALE =
  "scale=w='min(640,iw)':h='min(448,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2";

function recipeArgs(input, output, { preset, threads } = {}) {
  const args = ['-hide_banner', '-nostdin', '-y'];
  // Decoder threads (input option). Explicit counts keep the total below the 32-worker pool
  // of the mt core: with `auto` an x264 preset like veryfast on 8 cores dead-locked ffmpeg.
  if (threads) args.push('-threads', String(threads));
  args.push('-i', input,
    '-map', '0:v:0', '-map', '0:a:0?', '-vf', SCALE,
    '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
    '-pix_fmt', 'yuv420p', '-fps_mode', 'passthrough');
  if (preset) args.push('-preset', preset);          // not part of the reference recipe
  args.push('-c:a', 'aac', '-ar', '48000', '-ac', '2', '-b:a', '128k');
  if (threads) args.push('-threads', String(threads)); // encoder threads
  args.push('-movflags', '+faststart', output);
  return args;
}

function canUseThreads() {
  return typeof SharedArrayBuffer !== 'undefined' && globalThis.crossOriginIsolated === true;
}

async function loadCore(ffmpeg, { mt, coreBaseURL, onLog }) {
  const pkg = mt ? 'core-mt' : 'core';
  const bases = coreBaseURL ? [coreBaseURL] : CORE_CDNS.map(
    (c) => `${c}/@ffmpeg/${pkg}@${FFMPEG_CORE_VERSION}/dist/esm`);
  let lastErr;
  for (const base of bases) {
    try {
      // toBlobURL fetches with CORS and re-serves from a same-origin blob: URL, which is
      // what makes the Worker + dynamic import() work across origins (the well-known
      // "cannot construct a Worker from cross-origin script" problem). The wrapper's own
      // worker.js is vendored same-origin, so no classWorkerURL hack is needed.
      onLog?.(`[movies] fetching ffmpeg core (${mt ? 'mt' : 'st'}) from ${base}`);
      const [coreURL, wasmURL, workerURL] = await Promise.all([
        toBlobURL(`${base}/ffmpeg-core.js`, 'text/javascript'),
        toBlobURL(`${base}/ffmpeg-core.wasm`, 'application/wasm'),
        mt ? toBlobURL(`${base}/ffmpeg-core.worker.js`, 'text/javascript') : undefined,
      ]);
      onLog?.('[movies] core downloaded, instantiating wasm');
      await ffmpeg.load({ coreURL, wasmURL, ...(workerURL ? { workerURL } : {}) });
      onLog?.('[movies] core ready');
      return;
    } catch (e) {
      lastErr = e;
      onLog?.(`ffmpeg core load failed from ${base}: ${e?.message || e}`);
    }
  }
  throw new Error(`Could not load ffmpeg.wasm core: ${lastErr?.message || lastErr}`);
}

/**
 * @param {object} [opts]
 * @param {(line:string)=>void} [opts.onLog]  ffmpeg + converter log lines.
 * @param {'auto'|'st'|'mt'} [opts.threads='auto']  'auto' = multi-thread core only when
 *        the page is cross-origin isolated (SharedArrayBuffer available). If the mt core
 *        stalls or fails on a movie, that movie is retried once with the st core and the
 *        converter stays on st afterwards.
 * @param {string} [opts.preset]  optional libx264 preset (NOT in the reference recipe;
 *        speed knob, output stays Baseline 3.1 but gets bigger; unreliable on the mt core).
 * @param {number} [opts.x264Threads]  ffmpeg `-threads` for decoder and encoder. Default
 *        (mt only): min(8, hardwareConcurrency). The mt core has a fixed pool of 32
 *        workers; asking ffmpeg for more threads than the pool holds dead-locks it (seen
 *        with `-threads 12` or `auto` on a 16-core machine), so never raise this above 8.
 * @param {number} [opts.stallMs=90000]  abort a conversion that produced no ffmpeg output
 *        or progress for this long.
 * @param {number} [opts.recycleAfter=0]  terminate/reload the wasm worker every N movies
 *        (wasm heap never shrinks). 0 = never.
 * @param {string} [opts.coreBaseURL]  override the core location (testing / vendoring).
 */
export async function createMovieConverter({
  onLog, threads = 'auto', preset, x264Threads, stallMs = 90000, recycleAfter = 0, coreBaseURL,
} = {}) {
  let mt = threads === 'mt' || (threads === 'auto' && canUseThreads());
  if (mt && !canUseThreads()) {
    throw new Error('Multi-thread ffmpeg needs cross-origin isolation (SharedArrayBuffer); ' +
      'include coi-serviceworker.js or use threads: "st".');
  }
  const encThreads = () => (x264Threads ?? (mt ? Math.min(8, Math.max(2, navigator.hardwareConcurrency || 4)) : 0));

  let ffmpeg = null;
  let loading = null;
  let done = 0;
  let lastLog = [];
  let sink = null; // collects ffprobe stdout while a probe runs
  let curProgress = null; // (fraction) => void for the movie being converted
  let curDuration = 0;
  let terminated = false;
  let lastActivity = 0;

  const log = (s) => onLog?.(s);

  async function ensure() {
    if (terminated) throw new Error('Movie converter was terminated');
    if (ffmpeg) return ffmpeg;
    if (!loading) {
      loading = (async () => {
        const f = new FFmpeg();
        f.on('log', ({ message }) => {
          lastActivity = Date.now();
          if (sink) { sink.push(message); return; }
          lastLog.push(message);
          if (lastLog.length > 60) lastLog.shift();
          const m = /Duration: (\d+):(\d+):(\d+(?:\.\d+)?)/.exec(message);
          if (m && !curDuration) curDuration = (+m[1]) * 3600 + (+m[2]) * 60 + parseFloat(m[3]);
          log(message);
        });
        f.on('progress', ({ progress, time }) => {
          lastActivity = Date.now();
          if (!curProgress) return;
          let frac = progress;
          // `progress` is out_time/duration as computed by the core; fall back to our own
          // parse of the input duration when the core reports garbage.
          if (!(frac >= 0 && frac <= 1.5) && curDuration > 0 && time > 0) frac = time / 1e6 / curDuration;
          if (Number.isFinite(frac)) curProgress(Math.max(0, Math.min(1, frac)));
        });
        await loadCore(f, { mt, coreBaseURL, onLog: log });
        ffmpeg = f;
        return f;
      })().finally(() => { loading = null; });
    }
    return loading;
  }

  function drop() {
    if (ffmpeg) { try { ffmpeg.terminate(); } catch { /* ignore */ } }
    ffmpeg = null;
    done = 0;
  }

  async function probe(f, path) {
    // ffprobe in the same wasm; its stdout arrives through the log callback.
    sink = [];
    let rc;
    try {
      rc = await f.ffprobe([
        '-v', 'error', '-show_entries',
        'format=format_name,duration:stream=codec_type,codec_name,profile,pix_fmt,level,width,height,r_frame_rate,avg_frame_rate,sample_rate,channels',
        '-of', 'json', path]);
    } finally {
      var text = sink.join('\n');
      sink = null;
    }
    // The wasm ffprobe reports a non-zero exit code even on success, so judge by its JSON.
    try { return JSON.parse(text); } catch {
      throw new Error(`unreadable or unsupported movie (ffprobe code ${rc}: ${text.replace(/\s+/g, ' ').slice(0, 200)})`);
    }
  }

  async function convertOne(stem, input, onProgress) {
    if (!/^[A-Za-z0-9_.-]+$/.test(stem)) throw new Error(`Bad movie stem: ${stem}`);
    const f = await ensure();
    const dir = `/in_${stem}`;
    const inName = `${stem}.SFD`;
    const inPath = `${dir}/${inName}`;
    const outPath = `/${stem}.mp4`;
    lastLog = [];
    curDuration = 0;
    curProgress = onProgress || null;
    let mounted = false;
    let failed = false;
    try {
      // The SFD is mounted read-only from a Blob (WORKERFS): nothing is copied into the
      // 32-bit wasm heap and the caller's Uint8Array is not detached (the wrapper's
      // writeFile would transfer its whole ArrayBuffer).
      const blob = input instanceof Blob ? input : new Blob([input]);
      log(`[movies] ${stem}: mounting ${blob.size} bytes`);
      await f.createDir(dir);
      const ok = await f.mount('WORKERFS', { blobs: [{ name: inName, data: blob }] }, dir);
      if (ok === false) throw new Error('WORKERFS is not available in this ffmpeg core');
      mounted = true;

      log(`[movies] ${stem}: probing source`);
      const src = await probe(f, inPath);
      const sv = (src.streams || []).find((s) => s.codec_type === 'video');
      const hasAudio = (src.streams || []).some((s) => s.codec_type === 'audio');
      if (!sv) throw new Error('no video stream found in the SFD');
      onProgress?.(0);

      log(`[movies] ${stem}: transcoding`);
      const rc = await f.exec(recipeArgs(inPath, outPath, { preset, threads: encThreads() }));
      if (rc !== 0) {
        const tail = lastLog.slice(-12).join('\n');
        if (hasAudio && /decoder.*adpcm_adx|adpcm_adx.*not found|Decoder .* not found/i.test(tail)) {
          throw new Error(`${stem}: this ffmpeg build cannot decode ADX audio\n${tail}`);
        }
        throw new Error(`${stem}: ffmpeg exited with code ${rc}\n${tail}`);
      }

      const info = await probe(f, outPath);
      const v = (info.streams || []).find((s) => s.codec_type === 'video');
      const a = (info.streams || []).find((s) => s.codec_type === 'audio');
      if (!v || v.codec_name !== 'h264' || v.pix_fmt !== 'yuv420p' || v.level !== 31 ||
          v.width > 640 || v.height > 448) {
        throw new Error(`${stem}: unexpected video output ${JSON.stringify(v)}`);
      }
      if (hasAudio && (!a || a.codec_name !== 'aac' || +a.sample_rate !== 48000 || a.channels !== 2)) {
        throw new Error(`${stem}: missing or incompatible audio output ${JSON.stringify(a)}`);
      }
      const data = await f.readFile(outPath);
      if (!(data instanceof Uint8Array) || data.length < 1024) {
        throw new Error(`${stem}: ffmpeg produced an empty MP4`);
      }
      onProgress?.(1);
      return data;
    } catch (e) {
      failed = true;
      const msg = e instanceof Error ? e.message : String(e);
      throw new Error(msg.startsWith(`${stem}:`) ? msg : `${stem}: ${msg}`);
    } finally {
      curProgress = null;
      if (!failed) {
        // Free MEMFS / mounts (the wasm heap itself keeps its high-water mark).
        try { await f.deleteFile(outPath); } catch { /* ignore */ }
        try { if (mounted) await f.unmount(dir); } catch { /* ignore */ }
        try { await f.deleteDir(dir); } catch { /* ignore */ }
      }
    }
  }

  // Run one conversion with a no-activity watchdog (the mt core can dead-lock inside
  // ffmpeg; a hung wasm worker never settles its promise, so we terminate it ourselves).
  async function guarded(stem, input, onProgress) {
    lastActivity = Date.now();
    let stalled = false;
    const timer = setInterval(() => {
      if (Date.now() - lastActivity > stallMs) {
        stalled = true;
        clearInterval(timer);
        log(`[movies] ${stem}: no ffmpeg activity for ${stallMs / 1000} s, restarting the worker`);
        drop();
      }
    }, 2000);
    try {
      return await convertOne(stem, input, onProgress);
    } catch (e) {
      if (stalled) throw new Error(`${stem}: ffmpeg.wasm stalled (${mt ? 'multi' : 'single'}-thread core, no activity for ${stallMs / 1000} s)`);
      throw e;
    } finally {
      clearInterval(timer);
    }
  }

  async function convertWithFallback(stem, input, onProgress) {
    try {
      return await guarded(stem, input, onProgress);
    } catch (e) {
      if (!mt || terminated) throw e;
      // The multi-thread core is the fragile one: retry this movie once on the stock
      // single-thread core. If that works, stay on it; if it fails too the movie itself is
      // bad (not the core), so report that error and keep the multi-thread core.
      log(`[movies] ${stem}: multi-thread core failed (${String(e.message).split('\n')[0]}); retrying single-thread`);
      drop();
      mt = false;
      try {
        return await guarded(stem, input, onProgress);
      } catch (e2) {
        mt = true;
        drop();
        throw e2;
      }
    }
  }

  // Download/compile the core now (first call is ~30 MB from the CDN, cached afterwards),
  // so a failure shows up here and convert() timings are pure transcode time.
  await ensure();

  let chain = Promise.resolve();
  return {
    get mode() { return mt ? 'mt' : 'st'; },
    convert(stem, sfdBytes, onProgress) {
      const run = async () => {
        try {
          const out = await convertWithFallback(stem, sfdBytes, onProgress);
          done++;
          if (recycleAfter > 0 && done >= recycleAfter) drop();
          return out;
        } catch (e) {
          // After a failed/aborted exec the wasm instance may be in a bad state: start
          // from a fresh worker for the next movie.
          drop();
          throw e;
        }
      };
      const p = chain.then(run, run);
      chain = p.catch(() => {});
      return p;
    },
    terminate() {
      terminated = true;
      drop();
    },
  };
}
