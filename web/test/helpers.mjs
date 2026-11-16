import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

export const here = path.dirname(fileURLToPath(import.meta.url));
export const webRoot = path.resolve(here, '..');
export const repoRoot = path.resolve(webRoot, '..');
export const gameMac = path.join(repoRoot, 'game-mac');
export const nativeAssets = path.join(repoRoot, 'p3fes/scripts/native_assets');

export const haveGame = ['BGM.CVM', 'BTL.CVM', 'DATA.CVM', 'SLUS_216.21'].every((f) => fs.existsSync(path.join(gameMac, f)));

export function has(cmd) {
  return spawnSync('which', [cmd]).status === 0;
}

/** Fresh temp dir (removed by the caller). P3FES_TEST_TMP can point it at a roomier volume. */
export function tmpDir(prefix) {
  return fs.mkdtempSync(path.join(process.env.P3FES_TEST_TMP || os.tmpdir(), prefix));
}

export function py(code, args = []) {
  const r = spawnSync('python3', ['-I', '-c', code, ...args], { encoding: 'utf8', maxBuffer: 1 << 28 });
  if (r.status !== 0) throw new Error('python failed: ' + r.stderr);
  return r.stdout;
}

/**
 * Minimal file-backed Blob (size, slice, arrayBuffer, name). Node's fs.openAsBlob returns empty
 * data for slices beyond 4 GiB, which the real-disc tests need, so we read with positional reads.
 */
export class FsBlob {
  constructor(file, start = 0, end = undefined, fd = undefined) {
    this.file = file;
    this.fd = fd ?? fs.openSync(file, 'r');
    this.start = start;
    this.end = end ?? fs.fstatSync(this.fd).size;
    this.name = path.basename(file);
  }
  get size() { return this.end - this.start; }
  slice(a = 0, b = this.size) {
    const norm = (v) => (v < 0 ? Math.max(0, this.size + v) : Math.min(v, this.size));
    const s = norm(a), e = norm(b);
    return new FsBlob(this.file, this.start + s, this.start + Math.max(s, e), this.fd);
  }
  async arrayBuffer() {
    const buf = Buffer.alloc(this.size);
    let done = 0;
    while (done < buf.length) {
      const n = fs.readSync(this.fd, buf, done, buf.length - done, this.start + done);
      if (!n) break;
      done += n;
    }
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + done);
  }
}
export async function fileBlob(p) { return new FsBlob(p); }
