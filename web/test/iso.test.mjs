// ISO9660 reader: synthetic image (hdiutil) and the real disc (64 MB head + ELF/SYSTEM.CNF regions).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { gameMac, haveGame, has, tmpDir, fileBlob } from './helpers.mjs';
import { listIso, fileBlob as isoFileBlob } from '../lib/iso9660.js';
import { openIsoDisc, ELF_SHA1, verifyElf } from '../lib/disc.js';

test('synthetic ISO built with hdiutil makehybrid', { skip: process.platform !== 'darwin' ? 'macOS only' : false }, async () => {
  const dir = tmpDir('p3fes-iso-');
  try {
    const src = path.join(dir, 'src');
    fs.mkdirSync(path.join(src, 'SUB', 'DEEP'), { recursive: true });
    const expected = new Map();
    const put = (rel, size, seed) => {
      const buf = crypto.createHash('sha256').update(String(seed)).digest();
      const data = Buffer.alloc(size);
      for (let i = 0; i < size; i++) data[i] = buf[i % 32] ^ (i & 255);
      fs.writeFileSync(path.join(src, rel), data);
      expected.set('/' + rel.toUpperCase(), data);
    };
    put('SMALL.TXT', 13, 1);
    put('EMPTY.BIN', 0, 2);
    put('BIG.BIN', 5 * 2048 + 77, 3);
    put('SUB/ONE.DAT', 2048, 4);
    put('SUB/DEEP/TWO.DAT', 3000, 5);
    for (let i = 0; i < 120; i++) put(`SUB/F${String(i).padStart(4, '0')}.IRX`, 10 + i, 100 + i); // directory spans > 1 sector
    const iso = path.join(dir, 'test.iso');
    const r = spawnSync('hdiutil', ['makehybrid', '-iso', '-joliet', '-o', iso, src], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr + r.stdout);
    const isoFile = fs.existsSync(iso) ? iso : iso + '.iso';
    const blob = await fileBlob(isoFile);
    const list = await listIso(blob);
    const got = new Map(list.map((e) => [e.path.toUpperCase(), e]));
    assert.equal(got.size, expected.size, `entries: ${[...got.keys()].join(' ')}`);
    for (const [p, data] of expected) {
      const e = got.get(p);
      assert.ok(e, 'missing ' + p);
      assert.equal(e.size, data.length, p);
      const read = Buffer.from(await isoFileBlob(blob, e).arrayBuffer());
      assert.ok(read.equals(data), 'content ' + p);
    }
    assert.deepEqual(list.map((e) => e.lba), [...list.map((e) => e.lba)].sort((a, b) => a - b));
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('rejects non-ISO data', async () => {
  await assert.rejects(() => listIso(new Blob([new Uint8Array(100000)])), /ISO9660/);
});

// ---- real disc -------------------------------------------------------------------------
const ARCHIVE = process.env.P3FES_DISC_7Z || path.join(os.homedir(), 'Downloads', 'Shin Megami Tensei - Persona 3 FES (USA).7z');
const SPARSE = process.env.P3FES_ISO_SPARSE || path.join(process.env.P3FES_TEST_TMP || os.tmpdir(), 'p3fes-real-iso-sparse.iso');
const ISO_SIZE = 4594302976;
const HEAD = 64000000;
const ELF_LBA = 2228615, CNF_LBA = 2233057;

/** 64 MB head + the ELF/SYSTEM.CNF region, written at their real offsets into a sparse file. */
function buildSparse() {
  if (fs.existsSync(SPARSE) && fs.statSync(SPARSE).size === ISO_SIZE) return;
  const start = ELF_LBA * 2048, end = CNF_LBA * 2048 + 51;
  const sh = (c) => { const r = spawnSync('sh', ['-c', c], { encoding: 'utf8', maxBuffer: 1 << 26 }); return r; };
  const q = (s) => `'${s.replace(/'/g, "'\\''")}'`;
  const tmpHead = SPARSE + '.head', tmpReg = SPARSE + '.region';
  sh(`7z x -so ${q(ARCHIVE)} 2>/dev/null | head -c ${HEAD} > ${q(tmpHead)}`);
  sh(`7z x -so ${q(ARCHIVE)} 2>/dev/null | tail -c +${start + 1} | head -c ${end - start} > ${q(tmpReg)}`);
  const fd = fs.openSync(SPARSE, 'w');
  fs.ftruncateSync(fd, ISO_SIZE);
  fs.writeSync(fd, fs.readFileSync(tmpHead), 0, HEAD, 0);
  const reg = fs.readFileSync(tmpReg);
  fs.writeSync(fd, reg, 0, reg.length, start);
  fs.closeSync(fd);
  fs.rmSync(tmpHead); fs.rmSync(tmpReg);
}

const realSkip = !haveGame ? 'needs game-mac/' : !(has('7z') && (fs.existsSync(ARCHIVE) || fs.existsSync(SPARSE))) ? 'needs 7z and the disc archive (set P3FES_DISC_7Z)' : false;

test('real disc: root listing, sizes, LIB31 bytes and ELF SHA-1 via extent offset', { skip: realSkip, timeout: 900000 }, async (t) => {
  buildSparse();
  const blob = await fileBlob(SPARSE);
  assert.equal(blob.size, ISO_SIZE);
  const list = await listIso(blob);
  const byPath = new Map(list.map((e) => [e.path, e]));
  for (const f of ['SLUS_216.21', 'SYSTEM.CNF', 'BGM.CVM', 'BTL.CVM', 'DATA.CVM']) {
    const e = byPath.get('/' + f);
    assert.ok(e, f);
    assert.equal(e.size, fs.statSync(path.join(gameMac, f)).size, 'size of ' + f);
  }
  // every LIB31 file: same size and same bytes as the extracted copy (they live inside the 64 MB head)
  const lib = list.filter((e) => e.path.startsWith('/LIB31/'));
  assert.ok(lib.length >= 23, 'LIB31 entries: ' + lib.length);
  for (const e of lib) {
    const local = path.join(gameMac, e.path.slice(1).replace(/\.$/, '')); // ISO names like "ENTRIES." have no extension
    assert.equal(e.size, fs.statSync(local).size, e.path);
    assert.ok(Buffer.from(await isoFileBlob(blob, e).arrayBuffer()).equals(fs.readFileSync(local)), e.path);
  }
  // the executable is 4.5 GB into the image: read through the extent offset and hash it
  const elf = byPath.get('/SLUS_216.21');
  const sha1 = crypto.createHash('sha1').update(Buffer.from(await isoFileBlob(blob, elf).arrayBuffer())).digest('hex');
  assert.equal(sha1, ELF_SHA1);
  assert.equal(elf.lba, ELF_LBA);
  const cnf = Buffer.from(await isoFileBlob(blob, byPath.get('/SYSTEM.CNF')).arrayBuffer());
  assert.ok(cnf.equals(fs.readFileSync(path.join(gameMac, 'SYSTEM.CNF'))));
  t.diagnostic(`root entries: ${list.filter((e) => e.path.split('/').length === 2).map((e) => e.path.slice(1)).join(', ')}; LIB31 files: ${lib.length}`);
});

test('real disc through disc.js: verifyElf accepts it', { skip: realSkip, timeout: 900000 }, async () => {
  buildSparse();
  const disc = await openIsoDisc(await fileBlob(SPARSE));
  const { sha1 } = await verifyElf(disc);
  assert.equal(sha1, ELF_SHA1);
  assert.deepEqual(Object.keys(disc.cvms).sort(), ['BGM', 'BTL', 'DATA']);
  assert.ok(disc.lib31.length >= 23);
});
