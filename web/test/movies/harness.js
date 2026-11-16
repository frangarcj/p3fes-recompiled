import { createMovieConverter, MOVIE_RECIPE_VERSION, EARLY_MOVIES } from '../../lib/movies.js';

const q = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);
const logEl = $('log');
const lines = [];
window.__lines = lines;
function log(s) {
  lines.push(s);
  if (lines.length > 400) lines.shift();
  logEl.textContent = lines.join('\n');
  logEl.scrollTop = logEl.scrollHeight;
}
const heap = () => (performance.memory ? performance.memory.usedJSHeapSize / 1048576 : null);

window.__result = null;
$('threads').value = q.get('threads') || 'auto';

async function runCaps() {
  const { FFmpeg } = await import('../../vendor/ffmpeg/ffmpeg/index.js');
  const { toBlobURL } = await import('../../vendor/ffmpeg/util/index.js');
  const threads = q.get('threads') || 'st';
  const mt = threads === 'mt';
  const base = q.get('core') || `https://cdn.jsdelivr.net/npm/@ffmpeg/core${mt ? '-mt' : ''}@0.12.10/dist/esm`;
  const f = new FFmpeg();
  const out = [];
  f.on('log', ({ message }) => out.push(message));
  await f.load({
    coreURL: await toBlobURL(`${base}/ffmpeg-core.js`, 'text/javascript'),
    wasmURL: await toBlobURL(`${base}/ffmpeg-core.wasm`, 'application/wasm'),
    ...(mt ? { workerURL: await toBlobURL(`${base}/ffmpeg-core.worker.js`, 'text/javascript') } : {}),
  });
  const res = { crossOriginIsolated: self.crossOriginIsolated, mode: threads };
  for (const a of [['-version'], ['-codecs'], ['-formats'], ['-encoders'], ['-decoders'], ['-demuxers']]) {
    out.length = 0;
    await f.exec(['-hide_banner', ...a]);
    res[a[0]] = out.join('\n');
  }
  const want = {
    codecs: ['mpeg1video', 'mpeg2video', 'adpcm_adx', 'aac', 'h264'],
    formats: ['mpeg', 'mpegps', 'mp4'],
    encoders: ['libx264', 'aac'],
  };
  res.check = {};
  for (const [k, names] of Object.entries(want)) {
    const text = res['-' + k];
    for (const n of names) {
      res.check[`${k}:${n}`] = text.split('\n').filter((l) => new RegExp(`\\s${n}(\\s|$)`).test(l)).map((l) => l.trim());
    }
  }
  window.__result = res;
  $('status').textContent = 'caps done';
  log(JSON.stringify(res.check, null, 1));
}

async function run(stem, bytes) {
  const threads = $('threads').value;
  const t0 = performance.now();
  $('status').textContent = `converting ${stem} (${threads}) ...`;
  let peakHeap = heap() || 0;
  const poll = setInterval(() => { peakHeap = Math.max(peakHeap, heap() || 0); }, 250);
  const conv = await createMovieConverter({
    onLog: (s) => { if (!/^frame=|^\s*$/.test(s)) log(s); },
    threads,
    preset: q.get('preset') || undefined,
    x264Threads: q.get('x264threads') ? +q.get('x264threads') : undefined,
    stallMs: q.get('stall') ? +q.get('stall') * 1000 : undefined,
    coreBaseURL: q.get('core') || undefined,
  });
  const startMode = conv.mode;
  const tLoaded = performance.now();
  let last = 0;
  try {
    const mp4 = await conv.convert(stem, bytes, (f) => { $('bar').value = f; last = f; });
    const t1 = performance.now();
    const secs = (t1 - tLoaded) / 1000;
    const blobURL = URL.createObjectURL(new Blob([mp4], { type: 'video/mp4' }));
    $('dl').innerHTML = `<a download="${stem}.mp4" href="${blobURL}">${stem}.mp4</a>`;
    const res = {
      ok: true, stem, mode: conv.mode, startedAs: startMode, crossOriginIsolated: self.crossOriginIsolated,
      recipe: MOVIE_RECIPE_VERSION, sfdBytes: bytes.size ?? bytes.length, mp4Bytes: mp4.length,
      loadSeconds: (tLoaded - t0) / 1000, convertSeconds: secs,
      jsHeapPeakMB: peakHeap, jsHeapNowMB: heap(), cores: navigator.hardwareConcurrency,
    };
    window.__result = res;
    $('status').textContent = JSON.stringify(res);
    // Hand the MP4 to the driver (puppeteer) through a global.
    window.__mp4 = mp4;
  } catch (e) {
    window.__result = { ok: false, stem, error: String(e && e.message || e), stack: String(e && e.stack || '').slice(0, 600), tail: lines.slice(-8) };
    $('status').textContent = 'FAILED: ' + window.__result.error;
  } finally {
    clearInterval(poll);
    conv.terminate();
  }
}

$('go').onclick = async () => {
  const file = $('file').files[0];
  if (!file) return alert('pick an SFD');
  await run(file.name.replace(/\.[^.]+$/, ''), file);
};

window.__runUrl = async (url, stem) => {
  // blob=1: hand convert() a Blob (what the page should do: cvmFile.slice(off, off+size));
  // default: a Uint8Array copy in the JS heap.
  const r = await fetch(url);
  let input = q.get('blob') === '1' ? await r.blob() : new Uint8Array(await r.arrayBuffer());
  if (q.get('slice') === '1') { // emulate cvmFile.slice(offset, offset + size)
    const pad = new Uint8Array(1234567).fill(0x5a);
    input = new Blob([pad, input, pad]).slice(pad.length, pad.length + (input.size ?? input.length));
  }
  await run(stem, input);
  return window.__result;
};
window.__EARLY = EARLY_MOVIES;

// Reuse ONE converter for several movies in a row (stems resolved as /sfd/<stem>.SFD).
// A stem that fails must reject with a clear Error and must not break the next movie.
async function runSeq(stems) {
  const results = [];
  const conv = await createMovieConverter({
    onLog: (s) => { if (!/^frame=|^\s*$/.test(s)) log(s); },
    threads: $('threads').value, preset: q.get('preset') || undefined, x264Threads: q.get('x264threads') ? +q.get('x264threads') : undefined, stallMs: q.get('stall') ? +q.get('stall') * 1000 : undefined,
    coreBaseURL: q.get('core') || undefined,
  });
  for (const stem of stems) {
    const t = performance.now();
    try {
      const blob = await (await fetch(`/sfd/${stem}.SFD`)).blob();
      const mp4 = await conv.convert(stem, blob, (f) => { $('bar').value = f; });
      results.push({ stem, ok: true, mp4Bytes: mp4.length, seconds: (performance.now() - t) / 1000, jsHeapMB: heap() });
    } catch (e) {
      results.push({ stem, ok: false, error: String(e.message).split('\n')[0], seconds: (performance.now() - t) / 1000 });
    }
    $('status').textContent = JSON.stringify(results.at(-1));
  }
  conv.terminate();
  window.__result = { ok: true, seq: results };
}

if (q.get('seq')) runSeq(q.get('seq').split(',')).catch((e) => { window.__result = { ok: false, error: String(e) }; });
else if (q.get('caps') === '1') runCaps().catch((e) => { window.__result = { ok: false, error: String(e) }; log(String(e)); });
else if (q.get('sfd')) {
  const url = q.get('sfd');
  window.__runUrl(url, q.get('stem') || url.split('/').pop().replace(/\.[^.]+$/, ''));
}
