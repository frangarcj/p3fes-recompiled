// ADX -> WAV must be byte-identical to p3fes/scripts/native_assets/prepare_bgm.py (ffmpeg based).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { gameMac, nativeAssets, haveGame, has, tmpDir, fileBlob } from './helpers.mjs';
import { openCvm } from '../lib/cvm.js';
import { convertAdxToWav, parseAdxHeader, wavSizeFor, wavNameForStem, decodeAdx } from '../lib/adx.js';

// stereo, 24 kHz looped | 48 kHz looped | 48 kHz unlooped | 24 kHz unlooped | clipping-heavy | mixed
const TRACKS = ['01', '44', '53', '54', '79', '115', 'THEME', 'MAP02', '0501', 'COMBINE', '61', 'ENV_050'];

const skip = !haveGame || !has('python3') || !has('ffmpeg') ? 'needs game-mac/, python3 and ffmpeg' : false;

test('ADX header parsing and naming', () => {
  assert.equal(wavNameForStem('map02'), 'MAP02.WAV'.replace('WAV', 'wav'));
  assert.equal(wavNameForStem('a b.c'), 'A_B_C.wav');
  assert.throws(() => parseAdxHeader(new Uint8Array(64)), /signature/);
});

test('byte-identical to prepare_bgm.py', { skip, timeout: 600000 }, async (t) => {
  const refDir = tmpDir('p3fes-bgm-ref-');
  try {
    const args = [path.join(nativeAssets, 'prepare_bgm.py'), path.join(gameMac, 'BGM.CVM'), '--out', refDir];
    for (const s of TRACKS) args.push('--track', s + '.ADX');
    const r = spawnSync('python3', args, { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    const { inner, files } = await openCvm(await fileBlob(path.join(gameMac, 'BGM.CVM')));
    const seen = new Set();
    for (const s of TRACKS) {
      const e = files.find((f) => f.path.toUpperCase() === `/${s}.ADX`);
      assert.ok(e, s);
      const adx = new Uint8Array(await inner.slice(e.offset, e.offset + e.size).arrayBuffer());
      const h = parseAdxHeader(adx.subarray(0, 0x100), adx.length);
      seen.add(`${h.channels}ch/${h.sampleRate}/${h.loop ? 'loop' : 'noloop'}`);
      const { wav } = convertAdxToWav(adx);
      const ref = fs.readFileSync(path.join(refDir, s + '.wav'));
      assert.equal(wav.length, wavSizeFor(h), `${s} predicted size`);
      assert.equal(Buffer.compare(Buffer.from(wav.buffer, wav.byteOffset, wav.length), ref), 0, `${s}.wav differs from the Python/ffmpeg output`);
    }
    t.diagnostic('variants covered: ' + [...seen].join(', '));
    assert.ok(seen.size >= 4);
  } finally {
    fs.rmSync(refDir, { recursive: true, force: true });
  }
});

test('mono ADX is rejected (none exist on the disc; ffmpeg upmix is -3 dB and not reproduced)', () => {
  const adx = new Uint8Array(0x100 + 18 * 4);
  const dv = new DataView(adx.buffer);
  dv.setUint16(0, 0x8000); dv.setUint16(2, 0x0100 - 4); adx[4] = 3; adx[5] = 0x12; adx[6] = 4; adx[7] = 1;
  dv.setUint32(8, 24000); dv.setUint32(12, 64); dv.setUint16(0x12, 0x0400);
  assert.throws(() => convertAdxToWav(adx), /mono/i);
});
