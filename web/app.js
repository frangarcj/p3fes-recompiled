import { STRINGS, detectLang, saveLang, format } from './lib/i18n.js';
import { openIsoDisc, openFolderDisc, verifyElf, listBgmTracks, listMovies, DiscError } from './lib/disc.js';
import { buildPackage } from './lib/pipeline.js';
import { hasFilePicker, hasServiceWorkerSink, openFileSink, openServiceWorkerSink } from './lib/sink.js';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
let lang = detectLang();
let disc = null;           // selected disc
let discInfo = null;       // { tracks, movies }
let moviesMod = null;      // optional converter module
let running = false;
let abort = null;
let lastError = null;      // { key, vars } | { raw }
let statusState = null;    // for re-rendering on language change

const ZIP_NAME = 'p3fes-vita-data.zip';

// ---------------------------------------------------------------- i18n
const t = (key, vars) => format(STRINGS[lang][key] ?? STRINGS.en[key] ?? key, vars);

function applyLang() {
  document.documentElement.lang = lang;
  for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  $('langBtn').textContent = t('langToggle');
  $('moviesHelp').textContent = t('allMoviesHelp', { n: moviesMod?.EARLY_MOVIES?.length ?? 5 });
  $('startHelp').textContent = useServiceWorker() ? t('startHelpSw') : t('startHelpFile');
  renderStatus();
  renderSteps();
  if (lastError) renderError();
}
$('langBtn').addEventListener('click', () => { lang = lang === 'en' ? 'es' : 'en'; saveLang(lang); applyLang(); });

// ---------------------------------------------------------------- helpers
const fmtBytes = (n) => (n >= 1e9 ? (n / 1073741824).toFixed(2) + ' GB' : (n / 1048576).toFixed(0) + ' MB');
function fmtTime(sec) {
  if (!isFinite(sec) || sec < 0) return '...';
  sec = Math.round(sec);
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  return h ? `${h} h ${String(m).padStart(2, '0')} min` : m ? `${m} min ${String(s).padStart(2, '0')} s` : `${s} s`;
}
const useServiceWorker = () => params.get('sink') === 'sw' || !hasFilePicker();

function show(id, on) { $(id).hidden = !on; }
function logLine(msg, level) {
  const pre = $('log');
  const ts = new Date().toLocaleTimeString();
  pre.textContent += `[${ts}] ${level === 'error' ? 'ERROR ' : ''}${msg}\n`;
  const lines = pre.textContent.split('\n');
  if (lines.length > 600) pre.textContent = lines.slice(-500).join('\n');
  pre.scrollTop = pre.scrollHeight;
}

// ---------------------------------------------------------------- status of the selected game
function setStatus(kind, key, vars) { statusState = { kind, key, vars }; renderStatus(); }
function renderStatus() {
  const el = $('gameStatus');
  if (!statusState) { el.textContent = ''; el.className = 'status'; return; }
  el.className = 'status ' + (statusState.kind || '');
  el.textContent = statusState.raw ?? t(statusState.key, statusState.vars);
  if (statusState.extra) el.textContent += ' ' + t(statusState.extra.key, statusState.extra.vars);
}

function errorKeyFor(e) {
  if (e instanceof DiscError) {
    if (e.code === 'not-iso') return { key: 'errNotIso' };
    if (e.code === 'bad-elf') return { key: 'errBadElf' };
    if (e.code === 'missing') return { key: 'errMissing', vars: { name: e.detail } };
    if (e.code === 'no-elf') return { key: 'errNoElf' };
  }
  return { key: 'errGeneric', vars: { msg: (e && e.message) || String(e) } };
}

async function selectDisc(opener) {
  disc = null; discInfo = null;
  show('options', false); show('build', false); show('done', false); show('error', false);
  setStatus('', 'checking');
  try {
    const d = await opener();
    await verifyElf(d);
    const [tracks, movies] = await Promise.all([listBgmTracks(d), listMovies(d)]);
    disc = d;
    discInfo = { tracks: tracks.length, movies: movies.length };
    statusState = { kind: 'ok', key: 'gameOk', extra: { key: 'gameInfo', vars: { tracks: tracks.length, movies: movies.length } } };
    renderStatus();
    show('options', true); show('build', true);
    $('startBtn').focus();
  } catch (e) {
    const k = errorKeyFor(e);
    statusState = { kind: 'err', key: k.key, vars: k.vars };
    renderStatus();
    console.error(e);
  }
}

// ---------------------------------------------------------------- input handling
$('isoBtn').addEventListener('click', async () => {
  if (window.showOpenFilePicker) {
    try {
      const [h] = await window.showOpenFilePicker({ types: [{ description: 'PS2 disc image', accept: { 'application/octet-stream': ['.iso', '.bin', '.img'] } }] });
      return selectDisc(async () => openIsoDisc(await h.getFile()));
    } catch (e) { if (e && e.name === 'AbortError') return; }
  }
  $('isoInput').click();
});
$('isoInput').addEventListener('change', (e) => {
  const f = e.target.files[0]; e.target.value = '';
  if (f) selectDisc(() => openIsoDisc(f));
});

async function collectDirectory(handle) {
  const items = [];
  async function walk(dir, prefix, depth) {
    for await (const [name, h] of dir.entries()) {
      if (h.kind === 'file') items.push({ path: prefix + name, blob: await h.getFile() });
      else if (depth === 0 ? name.toUpperCase() === 'LIB31' : depth > 0 && depth < 6) await walk(h, prefix + name + '/', depth + 1);
    }
  }
  await walk(handle, handle.name + '/', 0);
  return items;
}
$('folderBtn').addEventListener('click', async () => {
  if (window.showDirectoryPicker) {
    try {
      const h = await window.showDirectoryPicker();
      return selectDisc(async () => openFolderDisc(await collectDirectory(h), h.name));
    } catch (e) { if (e && e.name === 'AbortError') return; }
  }
  $('folderInput').click();
});
function fromFileInput(e) {
  const files = [...e.target.files]; e.target.value = '';
  if (!files.length) return;
  selectDisc(async () => openFolderDisc(files.map((f) => ({ path: f.webkitRelativePath || f.name, blob: f })), 'folder'));
}
$('folderInput').addEventListener('change', fromFileInput);
$('filesBtn').addEventListener('click', () => $('filesInput').click());
$('filesInput').addEventListener('change', fromFileInput);

const drop = $('drop');
['dragenter', 'dragover'].forEach((n) => drop.addEventListener(n, (e) => { e.preventDefault(); drop.classList.add('over'); }));
['dragleave', 'drop'].forEach((n) => drop.addEventListener(n, (e) => { e.preventDefault(); drop.classList.remove('over'); }));
drop.addEventListener('drop', (e) => {
  const f = e.dataTransfer?.files?.[0];
  if (f) selectDisc(() => openIsoDisc(f));
});

// ---------------------------------------------------------------- progress rendering
const STEP_IDS = ['verify', 'copy', 'bgm', 'movies', 'finish'];
let curStep = -1;
let stepInfo = null;      // latest progress for current step
let stepStart = 0;
let samples = [];         // [time, done] for ETA
let byteSamples = [];     // [time, bytesWritten]
let bytesWritten = 0;
let raf = 0;

function renderSteps() {
  const ol = $('stepList');
  ol.textContent = '';
  STEP_IDS.forEach((id, i) => {
    const li = document.createElement('li');
    li.textContent = STRINGS[lang].stepNames[id];
    if (i < curStep) li.className = 'done'; else if (i === curStep) li.className = 'active';
    ol.appendChild(li);
  });
}

function scheduleRender() { if (!raf) raf = requestAnimationFrame(() => { raf = 0; renderProgress(); }); }

function renderProgress() {
  if (curStep < 0) return;
  const now = performance.now();
  $('stepText').textContent = `${t('stepOf', { i: curStep + 1, n: STEP_IDS.length })}: ${STRINGS[lang].stepNames[STEP_IDS[curStep]]}`;
  let frac = 0, detail = '', eta = NaN;
  if (stepInfo && stepInfo.total > 0) {
    frac = Math.min(1, stepInfo.done / stepInfo.total);
    if (stepInfo.unit === 'bytes' && stepInfo.step === 'copy') detail = `${fmtBytes(stepInfo.done)} / ${fmtBytes(stepInfo.total)}`;
    else if (stepInfo.step === 'movies') detail = `${t('itemsDone', { a: stepInfo.item ?? 0, b: stepInfo.items ?? 0 })}${stepInfo.file ? ' (' + stepInfo.file + ')' : ''}`;
    else detail = t('itemsDone', { a: stepInfo.done, b: stepInfo.total });
    const recent = samples.filter(([ts]) => now - ts < 15000);
    if (recent.length >= 2) {
      const [t0, d0] = recent[0]; const [t1, d1] = recent[recent.length - 1];
      const rate = (d1 - d0) / ((t1 - t0) / 1000);
      if (rate > 0) eta = (stepInfo.total - stepInfo.done) / rate;
    }
  }
  $('barFill').style.width = (frac * 100).toFixed(1) + '%';
  $('bar').setAttribute('aria-valuenow', (frac * 100).toFixed(0));
  $('detailText').textContent = detail;
  const rb = byteSamples.filter(([ts]) => now - ts < 5000);
  let speed = 0;
  if (rb.length >= 2) speed = (rb[rb.length - 1][1] - rb[0][1]) / ((rb[rb.length - 1][0] - rb[0][0]) / 1000) / 1048576;
  $('speedText').textContent = speed > 0 ? t('speed', { v: speed.toFixed(speed >= 100 ? 0 : 1) }) : '';
  $('etaText').textContent = isFinite(eta) ? t('eta', { t: fmtTime(eta) }) : '';
}

function onStep(s) {
  curStep = s.index; stepInfo = null; samples = []; stepStart = performance.now();
  renderSteps(); scheduleRender();
  logLine(`Step ${s.index + 1}/${s.count}: ${s.id}`);
}
function onProgress(p) {
  stepInfo = p;
  const now = performance.now();
  samples.push([now, p.done]);
  if (samples.length > 400) samples.splice(0, samples.length - 300);
  scheduleRender();
}
function onBytes(n) {
  bytesWritten += n;
  const now = performance.now();
  const last = byteSamples[byteSamples.length - 1];
  if (!last || now - last[0] > 250) byteSamples.push([now, bytesWritten]);
  if (byteSamples.length > 200) byteSamples.splice(0, 100);
}

// ---------------------------------------------------------------- build
async function loadMovies() {
  if (moviesMod !== null) return;
  try { moviesMod = await import('./lib/movies.js'); } catch { moviesMod = false; }
}

$('startBtn').addEventListener('click', async () => {
  if (running || !disc) return;
  // Open the sink first: showSaveFilePicker needs the click's user activation.
  let sink;
  try {
    sink = useServiceWorker() ? await openServiceWorkerSink(ZIP_NAME) : await openFileSink(ZIP_NAME);
  } catch (e) {
    if (e && e.name === 'AbortError') return;
    lastError = { key: 'tooOldSw', vars: { msg: e.message } };
    show('error', true); renderError();
    return;
  }
  running = true;
  lastError = null;
  abort = new AbortController();
  show('error', false); show('done', false); show('pick', false); show('options', false); show('needs', false); show('build', false);
  show('progress', true);
  $('log').textContent = ''; $('errLog').textContent = '';
  $('cancelBtn').disabled = false; $('cancelBtn').textContent = t('cancel');
  bytesWritten = 0; byteSamples = []; curStep = -1;
  window.addEventListener('beforeunload', warnUnload);
  await loadMovies();
  if (moviesMod === false) logLine('Movie module not available; movies will be skipped.', 'error');
  try {
    const summary = await buildPackage({
      disc, allMovies: $('allMovies').checked, movies: moviesMod || null, sink: sink.writable,
      signal: abort.signal, onStep, onProgress, onBytes, onLog: logLine,
    });
    curStep = STEP_IDS.length; renderSteps();
    show('progress', false);
    $('doneSummary').textContent = t('doneSummary', { size: fmtBytes(summary.bytesWritten), tracks: summary.bgm.tracks, movies: summary.movies.converted.length });
    const skipped = summary.movies.failed;
    $('doneSkipped').hidden = !skipped.length;
    if (skipped.length) $('doneSkipped').textContent = t('doneSkipped', { list: skipped.join(', ') });
    show('done', true);
    logLine(`Finished: ${summary.bytesWritten} bytes.`);
  } catch (e) {
    show('progress', false);
    if (abort.signal.aborted) {
      statusState = { kind: '', key: 'cancelled' };
      show('pick', true); show('needs', true); show('options', true); show('build', true);
      renderStatus();
    } else {
      console.error(e);
      lastError = errorKeyFor(e);
      $('errLog').textContent = $('log').textContent + '\n' + (e && e.stack || e);
      show('pick', true); show('needs', true);
      show('error', true); renderError();
    }
  } finally {
    running = false;
    window.removeEventListener('beforeunload', warnUnload);
  }
});

function warnUnload(e) { e.preventDefault(); e.returnValue = ''; }
function renderError() { $('errText').textContent = t(lastError.key, lastError.vars); }

$('cancelBtn').addEventListener('click', () => {
  if (!abort) return;
  $('cancelBtn').disabled = true; $('cancelBtn').textContent = t('cancelling');
  abort.abort();
});
$('againBtn').addEventListener('click', () => {
  show('done', false); show('pick', true); show('needs', true); show('options', !!disc); show('build', !!disc);
});

// ---------------------------------------------------------------- init
if (typeof ReadableStream === 'undefined' || typeof Blob === 'undefined' || !Blob.prototype.slice) {
  statusState = { kind: 'err', key: 'unsupported' };
}
loadMovies().then(() => { $('moviesMissing').hidden = !!moviesMod; if (!moviesMod) $('allMovies').disabled = true; applyLang(); });
applyLang();
