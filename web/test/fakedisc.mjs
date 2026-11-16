// Builds a small fake disc (real SLUS_216.21 + tiny CVMs with a few real ADX tracks and fake SFD
// movies) both as an extracted folder and as an ISO. Used by the pipeline test and the browser smoke test.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { gameMac, fileBlob } from './helpers.mjs';
import { openCvm } from '../lib/cvm.js';

const REAL_ADX = ['0501', 'ENV_050', 'COMBINE'];
export const FAKE_MOVIES = [
  ['/SFD/P3CTOP.SFD', 70000], ['/SFD/GAME_START.SFD', 33000], ['/SFD/P3CT001_01.SFD', 51000],
  ['/SFD/P3CT099.SFD', 20000], ['/SFD/STAFF_ROLL_Z.SFD', 41000], ['/SOUND/P3CT003.SFD', 5000], ['/SFD/P3CT003.SFD', 9000],
];

function pseudo(seed, len) {
  const b = Buffer.alloc(len);
  let x = seed >>> 0 || 1;
  for (let i = 0; i < len; i++) { x = (Math.imul(x, 1664525) + 1013904223) >>> 0; b[i] = x >>> 24; }
  return b;
}

function makeIso(srcDir, out) {
  const r = spawnSync('hdiutil', ['makehybrid', '-iso', '-joliet', '-o', out, srcDir], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error('hdiutil failed: ' + r.stderr + r.stdout);
  return fs.existsSync(out) ? out : out + '.iso';
}

function makeCvm(srcDir, out) {
  const tmp = out + '.iso';
  const iso = makeIso(srcDir, tmp);
  const header = Buffer.alloc(0x1800);
  header.write('CVMH');
  fs.writeFileSync(out, Buffer.concat([header, fs.readFileSync(iso)]));
  fs.rmSync(iso);
}

/** Returns { folder, iso, tracks: [stems], movies: [stems], big } */
export async function buildFakeDisc(dir, { withIso = true } = {}) {
  fs.mkdirSync(dir, { recursive: true });
  const work = path.join(dir, '_work');
  const folder = path.join(dir, 'disc');
  fs.mkdirSync(path.join(folder, 'LIB31'), { recursive: true });
  // BGM.CVM from a few real tracks
  const bgmSrc = path.join(work, 'bgm');
  fs.mkdirSync(bgmSrc, { recursive: true });
  const { inner, files } = await openCvm(await fileBlob(path.join(gameMac, 'BGM.CVM')));
  for (const s of REAL_ADX) {
    const e = files.find((f) => f.path === `/${s}.ADX`);
    fs.writeFileSync(path.join(bgmSrc, `${s}.ADX`), Buffer.from(await inner.slice(e.offset, e.offset + e.size).arrayBuffer()));
  }
  makeCvm(bgmSrc, path.join(folder, 'BGM.CVM'));
  // DATA.CVM with fake SFD movies
  const dataSrc = path.join(work, 'data');
  for (const [p, size] of FAKE_MOVIES) {
    fs.mkdirSync(path.dirname(path.join(dataSrc, p)), { recursive: true });
    fs.writeFileSync(path.join(dataSrc, p), pseudo(size, size));
  }
  fs.writeFileSync(path.join(dataSrc, 'OTHER.BIN'), pseudo(3, 3000));
  makeCvm(dataSrc, path.join(folder, 'DATA.CVM'));
  // BTL.CVM with an arbitrary file
  const btlSrc = path.join(work, 'btl');
  fs.mkdirSync(btlSrc, { recursive: true });
  fs.writeFileSync(path.join(btlSrc, 'ENEMY.BIN'), pseudo(9, 50000));
  makeCvm(btlSrc, path.join(folder, 'BTL.CVM'));
  // real executable, real SYSTEM.CNF, a few real LIB31 modules
  fs.copyFileSync(path.join(gameMac, 'SLUS_216.21'), path.join(folder, 'SLUS_216.21'));
  fs.copyFileSync(path.join(gameMac, 'SYSTEM.CNF'), path.join(folder, 'SYSTEM.CNF'));
  for (const f of ['LIBSD.IRX', 'SIO2MAN.IRX', 'PADMAN.IRX']) fs.copyFileSync(path.join(gameMac, 'LIB31', f), path.join(folder, 'LIB31', f));
  fs.mkdirSync(path.join(folder, 'LIB31', 'CVS'), { recursive: true });
  fs.copyFileSync(path.join(gameMac, 'LIB31', 'CVS', 'ROOT'), path.join(folder, 'LIB31', 'CVS', 'ROOT'));
  let iso = null;
  if (withIso) iso = makeIso(folder, path.join(dir, 'fake.iso'));
  fs.rmSync(work, { recursive: true, force: true });
  return { folder, iso };
}
