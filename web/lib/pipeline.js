// Builds the Vita data package as a streamed ZIP: verify ELF -> copy big files -> convert BGM
// -> convert movies -> manifest. Nothing is ever held in memory beyond a few tracks / one movie.
import { makeZip } from '../vendor/client-zip/index.js';
import { verifyElf, listBgmTracks, listMovies, DiscError } from './disc.js';
import { parseAdxHeader, wavSizeFor, BGM_RECIPE_VERSION } from './adx.js';
import { blobStream } from './blobstream.js';
import { createBgmPool } from './bgm-pool.js';

export const TOOL_VERSION = '1.0.0';
export const ZIP_PREFIX = 'data/p3fes/';
export const RUNTIME_CFG = [
  'PS2X_LOG_FILE=ux0:data/p3fes/runtime.log',
  'P3FES_NATIVE_RW=1',
  'P3FES_NATIVE_MOVIES=1',
  'P3FES_NATIVE_CAMERA=1',
  'P3FES_NATIVE_BGM=1',
  'P3FES_NATIVE_MATH=1',
  'P3FES_RW_HOOK_DIAG=0',
  'P3FES_NATIVE_RW_MATH=1',
  'PS2X_AUDIO=1',
  '',
].join('\n');
// Fixed modification time so the archive does not depend on when it was made.
const ENTRY_DATE = new Date(2026, 0, 1, 0, 0, 0);

const enc = new TextEncoder();

/**
 * @param {object} o
 * @param {object} o.disc            from disc.js
 * @param {boolean} o.allMovies      convert every movie (else only EARLY_MOVIES)
 * @param {object|null} o.movies     the movies module ({EARLY_MOVIES, createMovieConverter}) or null
 * @param {WritableStream} o.sink    destination for the zip bytes
 * @param {(s:{id:string,index:number,count:number})=>void} o.onStep
 * @param {(p:{step:string,done:number,total:number,unit:'bytes'|'items'})=>void} o.onProgress
 * @param {(msg:string, level?:string)=>void} o.onLog
 * @param {AbortSignal} o.signal
 * @param {(n:number)=>void} [o.onBytes] called with the size of every chunk written to the sink
 * @param {object} [o.bgmPool]       injectable for tests
 */
export async function buildPackage(o) {
  const { disc, allMovies, movies, sink, signal } = o;
  const onStep = o.onStep || (() => {});
  const onProgress = o.onProgress || (() => {});
  const onLog = o.onLog || (() => {});
  const steps = ['verify', 'copy', 'bgm', 'movies', 'finish'];
  const stepIndex = (id) => steps.indexOf(id);
  const step = (id) => { signal?.throwIfAborted(); onStep({ id, index: stepIndex(id), count: steps.length }); };

  // ---- 1. verify the executable
  step('verify');
  onLog('Reading SLUS_216.21 and checking its SHA-1...');
  const elf = await verifyElf(disc);
  onLog('Executable verified (Persona 3 FES NTSC-U, SLUS-21621).');

  onLog('Reading the BGM and movie directories...');
  const tracks = await listBgmTracks(disc);
  for (const t of tracks) {
    const head = new Uint8Array(await t.blob.slice(0, 0x100).arrayBuffer());
    t.header = parseAdxHeader(head, t.size);
    t.wavSize = wavSizeFor(t.header);
  }
  const movieList = await listMovies(disc);
  const earlySet = new Set((movies?.EARLY_MOVIES || []).map((s) => s.toUpperCase()));
  let movieJobs = [];
  if (movies) {
    movieJobs = movieList.filter((m) => allMovies || earlySet.has(m.stem));
    // early movies first (they matter for the first minutes of the game), then the rest
    movieJobs.sort((a, b) => (earlySet.has(b.stem) ? 1 : 0) - (earlySet.has(a.stem) ? 1 : 0) || (a.stem < b.stem ? -1 : 1));
  }
  onLog(`${tracks.length} music tracks, ${movieList.length} movies on the disc; converting ${movieJobs.length} movies.`);

  const copyFiles = [
    ...(disc.systemCnf ? [{ name: 'SYSTEM.CNF', blob: disc.systemCnf }] : []),
    ...disc.lib31,
    { name: 'BGM.CVM', blob: disc.cvms.BGM },
    { name: 'BTL.CVM', blob: disc.cvms.BTL },
    { name: 'DATA.CVM', blob: disc.cvms.DATA },
  ];
  const copyTotal = elf.bytes.length + copyFiles.reduce((a, f) => a + f.blob.size, 0);
  const bgmTotalBytes = tracks.reduce((a, t) => a + t.wavSize, 0);

  const summary = {
    iso_sha1: elf.sha1,
    bgm: { tracks: tracks.length, bytes: bgmTotalBytes },
    movies: { available: movieList.map((m) => m.stem), requested: movieJobs.map((m) => m.stem), converted: [], failed: [] },
    bytesWritten: 0,
  };

  const ownPool = !o.bgmPool;
  const pool = o.bgmPool || createBgmPool();
  let converter = null;
  let copied = 0;

  async function* entries() {
    // ---- 2. files copied as they are
    step('copy');
    yield { name: ZIP_PREFIX + ELF_NAME_OUT, lastModified: ENTRY_DATE, input: elf.bytes };
    copied += elf.bytes.length;
    onProgress({ step: 'copy', done: copied, total: copyTotal, unit: 'bytes' });
    yield { name: ZIP_PREFIX + 'runtime.cfg', lastModified: ENTRY_DATE, input: enc.encode(RUNTIME_CFG) };
    for (const f of copyFiles) {
      yield {
        name: ZIP_PREFIX + f.name, lastModified: ENTRY_DATE, size: f.blob.size,
        input: blobStream(f.blob, {
          signal,
          onBytes: (n) => { copied += n; onProgress({ step: 'copy', done: copied, total: copyTotal, unit: 'bytes', file: f.name }); },
        }),
      };
    }

    // ---- 3. BGM
    step('bgm');
    onProgress({ step: 'bgm', done: 0, total: tracks.length, unit: 'items' });
    const ahead = Math.max(2, pool.size + 1);
    const jobs = new Array(tracks.length);
    let launched = 0;
    const launch = (upTo) => {
      while (launched < tracks.length && launched < upTo) {
        const t = tracks[launched++];
        const idx = launched - 1;
        jobs[idx] = (async () => {
          const adx = new Uint8Array(await t.blob.arrayBuffer());
          return pool.convert(adx);
        })();
        jobs[idx].catch(() => {}); // surfaced when awaited in order
      }
    };
    for (let i = 0; i < tracks.length; i++) {
      signal?.throwIfAborted();
      launch(i + ahead);
      let wav;
      try { wav = await jobs[i]; }
      catch (e) { throw new DiscError('bgm', `Could not convert music track ${tracks[i].path}: ${e.message}`, e); }
      if (wav.length !== tracks[i].wavSize) throw new DiscError('bgm', `Unexpected WAV size for ${tracks[i].path}`);
      jobs[i] = null;
      yield { name: ZIP_PREFIX + 'native_bgm/' + tracks[i].name, lastModified: ENTRY_DATE, input: wav };
      onProgress({ step: 'bgm', done: i + 1, total: tracks.length, unit: 'items', file: tracks[i].name });
    }
    if (ownPool) pool.terminate();

    // ---- 4. movies
    step('movies');
    if (movieJobs.length) {
      try {
        converter = await movies.createMovieConverter({ onLog: (m) => onLog(String(m), 'ffmpeg') });
      } catch (e) {
        onLog(`The movie converter could not start (${e.message}); movies will be skipped.`, 'error');
        summary.movies.failed.push(...movieJobs.map((m) => m.stem));
        movieJobs = [];
      }
    }
    const movieBytesTotal = movieJobs.reduce((a, m) => a + m.size, 0);
    let movieBytesDone = 0;
    onProgress({ step: 'movies', done: 0, total: movieBytesTotal, unit: 'bytes', item: 0, items: movieJobs.length });
    for (let i = 0; i < movieJobs.length; i++) {
      signal?.throwIfAborted();
      const m = movieJobs[i];
      onLog(`Converting movie ${m.stem} (${i + 1}/${movieJobs.length})...`);
      let mp4 = null;
      try {
        const sfd = new Uint8Array(await m.blob.arrayBuffer());
        mp4 = await converter.convert(m.stem, sfd, (fraction) => {
          signal?.throwIfAborted();
          onProgress({ step: 'movies', done: movieBytesDone + m.size * Math.max(0, Math.min(1, fraction || 0)), total: movieBytesTotal, unit: 'bytes', item: i, items: movieJobs.length, file: m.stem });
        });
      } catch (e) {
        if (signal?.aborted) throw e;
        onLog(`Movie ${m.stem} could not be converted and was skipped: ${e.message}`, 'error');
        summary.movies.failed.push(m.stem);
      }
      movieBytesDone += m.size;
      if (mp4) {
        summary.movies.converted.push(m.stem);
        yield { name: ZIP_PREFIX + 'native_movies/' + m.stem + '.mp4', lastModified: ENTRY_DATE, input: mp4 };
      }
      onProgress({ step: 'movies', done: movieBytesDone, total: movieBytesTotal, unit: 'bytes', item: i + 1, items: movieJobs.length, file: m.stem });
    }
    try { converter?.terminate(); } catch { /* ignore */ }
    converter = null;

    // ---- 5. manifest
    step('finish');
    const manifest = {
      format: 'p3fes-assets-v1',
      tool: { name: 'p3fes-vita-data-builder', version: TOOL_VERSION },
      created: new Date().toISOString(),
      source: { kind: disc.kind, slus_21621_sha1: elf.sha1 },
      bgm: { recipe_version: BGM_RECIPE_VERSION, tracks: tracks.map((t) => t.name) },
      movies: {
        recipe_version: movies?.MOVIE_RECIPE_VERSION ?? null,
        all: !!allMovies,
        early: [...earlySet],
        converted: summary.movies.converted,
        failed: summary.movies.failed,
        available: summary.movies.available,
      },
    };
    yield { name: ZIP_PREFIX + 'p3fes-assets.json', lastModified: ENTRY_DATE, input: enc.encode(JSON.stringify(manifest, null, 2) + '\n') };
  }

  // client-zip forwards a stream cancel to gen.throw(); a generator that rethrows would surface as an
  // unhandled rejection, so the iterator swallows it (the real error reaches us through pipeTo).
  const gen = entries();
  const entryIterable = {
    [Symbol.asyncIterator]() {
      return {
        next: () => gen.next(),
        return: (v) => gen.return(v).catch(() => ({ done: true, value: undefined })),
        throw: () => gen.return().catch(() => ({ done: true, value: undefined })),
      };
    },
  };
  const zip = makeZip(entryIterable);
  let written = 0;
  const counter = new TransformStream({
    transform(chunk, c) { written += chunk.length; summary.bytesWritten = written; o.onBytes?.(chunk.length); c.enqueue(chunk); },
  });
  try {
    await zip.pipeThrough(counter).pipeTo(sink, { signal });
  } catch (e) {
    try { converter?.terminate(); } catch { /* ignore */ }
    if (ownPool) pool.terminate();
    throw e;
  }
  return summary;
}

const ELF_NAME_OUT = 'SLUS_216.21';
