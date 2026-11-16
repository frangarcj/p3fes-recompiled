// Streaming zip64: a 4.5 GiB virtual entry (plus entries after the 4 GiB mark) written to a
// sparse file, then validated with unzip, zipinfo and Python's zipfile.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { has, tmpDir } from './helpers.mjs';
import { makeZip } from '../vendor/client-zip/index.js';

const BIG = 4.5 * 1024 ** 3;
const CHUNK = 8 * 1024 * 1024;

function bigStream(total) {
  let sent = 0, n = 0;
  return new ReadableStream({
    pull(c) {
      if (sent >= total) { c.close(); return; }
      const len = Math.min(CHUNK, total - sent);
      const buf = new Uint8Array(len);
      new DataView(buf.buffer).setUint32(0, ++n, true); // non-zero marker per chunk, rest zeros
      sent += len;
      c.enqueue(buf);
    },
  });
}

/** Writable that stores only non-zero 4 KiB pages, leaving holes: a 4.5 GiB file costs a few MB of disk. */
function sparseSink(file) {
  const fd = fs.openSync(file, 'w');
  let pos = 0;
  return new WritableStream({
    write(chunk) {
      for (let off = 0; off < chunk.length; off += 4096) {
        const end = Math.min(chunk.length, off + 4096);
        let nz = false;
        for (let i = off; i < end; i++) if (chunk[i]) { nz = true; break; }
        if (nz) fs.writeSync(fd, chunk, off, end - off, pos + off);
      }
      pos += chunk.length;
    },
    close() { fs.ftruncateSync(fd, pos); fs.closeSync(fd); },
  });
}

const skip = !has('python3') ? 'needs python3' : false;

test('zip64: 4.5 GiB stored entry + entries beyond 4 GiB', { skip, timeout: 900000 }, async (t) => {
  const dir = tmpDir('p3fes-zip64-');
  const out = path.join(dir, 'big.zip');
  try {
    const small = new TextEncoder().encode('hello zip64\n');
    const entries = (async function* () {
      yield { name: 'data/p3fes/small.txt', lastModified: new Date(2026, 0, 1), input: small };
      yield { name: 'data/p3fes/BIG.CVM', lastModified: new Date(2026, 0, 1), size: BIG, input: bigStream(BIG) };
      yield { name: 'data/p3fes/after.bin', lastModified: new Date(2026, 0, 1), input: new Uint8Array(1000).fill(7) };
      yield { name: 'data/p3fes/after2.bin', lastModified: new Date(2026, 0, 1), input: new Uint8Array(5).fill(9) };
    })();
    const t0 = Date.now();
    await makeZip(entries).pipeTo(sparseSink(out));
    const secs = (Date.now() - t0) / 1000;
    const logical = fs.statSync(out).size;
    t.diagnostic(`zip logical size ${logical} bytes, written in ${secs.toFixed(1)} s (${(logical / 1048576 / secs).toFixed(0)} MB/s incl. crc32); on disk ${fs.statSync(out).blocks * 512} bytes`);
    assert.ok(logical > BIG);

    if (has('unzip')) {
      const r = spawnSync('unzip', ['-t', out], { encoding: 'utf8', maxBuffer: 1 << 24 });
      t.diagnostic('unzip -t: ' + r.stdout.trim().split('\n').slice(-2).join(' | '));
      assert.equal(r.status, 0, r.stdout + r.stderr);
      assert.match(r.stdout, /No errors detected/);
    }
    if (has('zipinfo')) {
      const r = spawnSync('zipinfo', [out], { encoding: 'utf8' });
      t.diagnostic(r.stdout.trim().split('\n').join(' | '));
      assert.equal(r.status, 0, r.stderr);
      assert.match(r.stdout, /4831838208/);
    }
    const code = `
import zipfile, sys, zlib
z = zipfile.ZipFile(sys.argv[1])
bad = z.testzip()
assert bad is None, bad
info = {i.filename: i for i in z.infolist()}
assert info['data/p3fes/BIG.CVM'].file_size == ${BIG}, info['data/p3fes/BIG.CVM'].file_size
assert info['data/p3fes/BIG.CVM'].compress_type == 0
assert info['data/p3fes/after.bin'].header_offset > 2**32, info['data/p3fes/after.bin'].header_offset
assert z.read('data/p3fes/after.bin') == bytes([7])*1000
assert z.read('data/p3fes/after2.bin') == bytes([9])*5
assert z.read('data/p3fes/small.txt') == b'hello zip64\\n'
print('python zipfile OK', [ (i.filename, i.file_size, i.header_offset) for i in z.infolist() ])
`;
    const r = spawnSync('python3', ['-I', '-c', code, out], { encoding: 'utf8', maxBuffer: 1 << 24 });
    t.diagnostic(r.stdout.trim());
    assert.equal(r.status, 0, r.stderr);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
