// End-to-end: fake disc (folder and ISO) -> buildPackage -> zip, checked with unzip and Python.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { Writable } from 'node:stream';
import { spawnSync } from 'node:child_process';
import { gameMac, haveGame, has, tmpDir, fileBlob } from './helpers.mjs';
import { buildFakeDisc } from './fakedisc.mjs';
import * as stubMovies from './stub-movies.mjs';
import { buildPackage, RUNTIME_CFG } from '../lib/pipeline.js';
import { openFolderDisc, openIsoDisc, DiscError } from '../lib/disc.js';
import { openCvm } from '../lib/cvm.js';
import { convertAdxToWav } from '../lib/adx.js';

const skip = !haveGame || process.platform !== 'darwin' || !has('python3') ? 'needs game-mac/, macOS hdiutil and python3' : false;

function walk(dir, base = dir) {
  const out = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) out.push(...walk(p, base)); else out.push(path.relative(base, p).split(path.sep).join('/'));
  }
  return out;
}
async function folderItems(folder) {
  return Promise.all(walk(folder).map(async (rel) => ({ path: 'disc/' + rel, blob: await fileBlob(path.join(folder, rel)) })));
}
const sha = (p) => crypto.createHash('sha1').update(fs.readFileSync(p)).digest('hex');

async function run(disc, outZip, opts = {}) {
  const events = [];
  const summary = await buildPackage({
    disc, allMovies: !!opts.allMovies, movies: opts.movies === undefined ? stubMovies : opts.movies,
    sink: Writable.toWeb(fs.createWriteStream(outZip)), signal: opts.signal,
    onStep: (s) => events.push(s.id), onProgress: opts.onProgress, onLog: () => {},
  });
  return { summary, events };
}

function extract(zip, dest) {
  const code = `
import zipfile, sys
z = zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None
names = [i.filename for i in z.infolist()]
assert len(set(names)) == len(names)
z.extractall(sys.argv[2]); print('\\n'.join(names))
`;
  const r = spawnSync('python3', ['-I', '-c', code, zip, dest], { encoding: 'utf8', maxBuffer: 1 << 26 });
  assert.equal(r.status, 0, r.stderr);
  return r.stdout.trim().split('\n');
}

test('folder and ISO inputs produce the same package', { skip, timeout: 600000 }, async (t) => {
  const dir = tmpDir('p3fes-pipe-');
  try {
    const { folder, iso } = await buildFakeDisc(dir);
    const results = {};
    for (const kind of ['folder', 'iso']) {
      const disc = kind === 'folder' ? await openFolderDisc(await folderItems(folder), 'disc') : await openIsoDisc(await fileBlob(iso));
      const zip = path.join(dir, kind + '.zip');
      const { summary, events } = await run(disc, zip, { allMovies: false });
      assert.deepEqual(events, ['verify', 'copy', 'bgm', 'movies', 'finish']);
      const dest = path.join(dir, 'out-' + kind);
      const names = extract(zip, dest);
      results[kind] = { names, dest, summary };
      if (has('unzip')) {
        const r = spawnSync('unzip', ['-t', zip], { encoding: 'utf8' });
        assert.equal(r.status, 0, r.stdout);
      }
    }
    const root = (k) => path.join(results[k].dest, 'data/p3fes');
    // identical file lists and contents (except the timestamped manifest)
    assert.deepEqual([...results.folder.names].sort(), [...results.iso.names].sort());
    const names = results.folder.names;
    for (const n of names) {
      if (n.endsWith('p3fes-assets.json')) continue;
      assert.ok(fs.readFileSync(path.join(results.folder.dest, n)).equals(fs.readFileSync(path.join(results.iso.dest, n))), 'differs: ' + n);
    }
    // expected layout
    const need = ['SLUS_216.21', 'SYSTEM.CNF', 'DATA.CVM', 'BTL.CVM', 'BGM.CVM', 'runtime.cfg', 'p3fes-assets.json',
      'LIB31/LIBSD.IRX', 'LIB31/SIO2MAN.IRX', 'LIB31/PADMAN.IRX', 'LIB31/CVS/ROOT',
      'native_bgm/0501.wav', 'native_bgm/ENV_050.wav', 'native_bgm/COMBINE.wav',
      'native_movies/P3CTOP.mp4', 'native_movies/GAME_START.mp4', 'native_movies/P3CT001_01.mp4'];
    for (const n of need) assert.ok(names.includes('data/p3fes/' + n), 'missing ' + n + ' in ' + names.join(' '));
    assert.ok(!names.some((n) => /memcards|\.ps2$/i.test(n)), 'no memory card may be shipped');
    assert.ok(!names.includes('data/p3fes/native_movies/P3CT099.mp4'), 'late movie must be skipped when not converting all');
    assert.equal(sha(path.join(root('folder'), 'SLUS_216.21')), '3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb');
    assert.equal(fs.readFileSync(path.join(root('folder'), 'runtime.cfg'), 'utf8'), RUNTIME_CFG);
    assert.equal(RUNTIME_CFG, [
      'PS2X_LOG_FILE=ux0:data/p3fes/runtime.log', 'P3FES_NATIVE_RW=1', 'P3FES_NATIVE_MOVIES=1', 'P3FES_NATIVE_CAMERA=1', 'P3FES_NATIVE_BGM=1',
      'P3FES_NATIVE_MATH=1', 'P3FES_RW_HOOK_DIAG=0', 'P3FES_NATIVE_RW_MATH=1', 'PS2X_AUDIO=1', ''].join('\n'));
    // WAVs equal the converter output of the very ADX files inside BGM.CVM
    const { inner, files } = await openCvm(await fileBlob(path.join(folder, 'BGM.CVM')));
    for (const f of files) {
      const stem = f.path.slice(1).replace(/\.ADX$/, '');
      const adx = new Uint8Array(await inner.slice(f.offset, f.offset + f.size).arrayBuffer());
      assert.ok(Buffer.from(convertAdxToWav(adx).wav).equals(fs.readFileSync(path.join(root('folder'), 'native_bgm', stem + '.wav'))), stem);
    }
    for (const c of ['BGM', 'BTL', 'DATA']) assert.equal(sha(path.join(root('folder'), c + '.CVM')), sha(path.join(folder, c + '.CVM')));
    const manifest = JSON.parse(fs.readFileSync(path.join(root('folder'), 'p3fes-assets.json'), 'utf8'));
    assert.equal(manifest.source.slus_21621_sha1, '3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb');
    assert.equal(manifest.bgm.tracks.length, 3);
    assert.deepEqual([...manifest.movies.converted].sort(), ['GAME_START', 'P3CT001_01', 'P3CTOP']);
    assert.equal(manifest.movies.all, false);
    assert.ok(manifest.movies.available.includes('P3CT003') && manifest.movies.available.filter((s) => s === 'P3CT003').length === 1, 'duplicate stems collapse');
    t.diagnostic(`zip entries: ${names.length}; ${results.folder.summary.bytesWritten} bytes`);

    // all movies
    const disc = await openFolderDisc(await folderItems(folder), 'disc');
    const zipAll = path.join(dir, 'all.zip');
    const { summary } = await run(disc, zipAll, { allMovies: true });
    assert.equal(summary.movies.converted.length, 6); // 7 files, 2 share the stem P3CT003
    const namesAll = extract(zipAll, path.join(dir, 'out-all'));
    assert.ok(namesAll.includes('data/p3fes/native_movies/P3CT099.mp4'));
    assert.equal(fs.readFileSync(path.join(dir, 'out-all/data/p3fes/native_movies/P3CT003.mp4'), 'utf8'), 'FAKE-MP4 P3CT003 from 9000 bytes of SFD'); // /SFD/ copy preferred
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('errors: wrong executable, missing CVM, cancel, no movie module', { skip, timeout: 600000 }, async () => {
  const dir = tmpDir('p3fes-pipe-err-');
  try {
    const { folder } = await buildFakeDisc(dir, { withIso: false });
    // wrong ELF
    fs.appendFileSync(path.join(folder, 'SLUS_216.21'), 'x');
    await assert.rejects(async () => { const d = await openFolderDisc(await folderItems(folder), 'disc'); await run(d, path.join(dir, 'bad.zip')); }, (e) => e instanceof DiscError && e.code === 'bad-elf');
    fs.copyFileSync(path.join(gameMac, 'SLUS_216.21'), path.join(folder, 'SLUS_216.21'));
    // missing CVM
    fs.rmSync(path.join(folder, 'BTL.CVM'));
    await assert.rejects(async () => openFolderDisc(await folderItems(folder), 'disc'), (e) => e instanceof DiscError && e.code === 'missing' && /BTL\.CVM/.test(e.message));
    // no ELF at all
    await assert.rejects(() => openFolderDisc([{ path: 'x/readme.txt', blob: new Blob(['hi']) }]), (e) => e.code === 'no-elf');
    fs.writeFileSync(path.join(folder, 'BTL.CVM'), fs.readFileSync(path.join(folder, 'BGM.CVM')));
    // cancel while converting movies
    const ac = new AbortController();
    const disc = await openFolderDisc(await folderItems(folder), 'disc');
    await assert.rejects(() => run(disc, path.join(dir, 'cancel.zip'), { allMovies: true, signal: ac.signal, onProgress: (p) => { if (p.step === 'movies' && p.done > 0) ac.abort(); } }), (e) => e.name === 'AbortError' || /abort/i.test(String(e)));
    // movie module missing: package still builds, movies skipped
    const { summary } = await run(disc, path.join(dir, 'nomovies.zip'), { movies: null });
    assert.equal(summary.movies.converted.length, 0);
    // converter that fails on one movie: skipped and reported
    const flaky = { ...stubMovies, async createMovieConverter() { return { async convert(stem) { if (stem === 'P3CTOP') throw new Error('boom'); return new Uint8Array([1, 2, 3]); }, terminate() {} }; } };
    const r2 = await run(disc, path.join(dir, 'flaky.zip'), { movies: flaky });
    assert.deepEqual(r2.summary.movies.failed, ['P3CTOP']);
    assert.ok(r2.summary.movies.converted.includes('GAME_START'));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
