// ADX -> PCM16 WAV, a JavaScript port of p3fes/scripts/native_assets/prepare_bgm.py.
//
// The Python tool shells out to ffmpeg (ADX decode + aresample 48 kHz + atrim). This module
// reproduces that pipeline sample for sample:
//   * ffmpeg's adx decoder (libavcodec/adxdec.c, 12-bit coefficients, unclipped history);
//   * 24 kHz -> 48 kHz through libswresample's default polyphase resampler (32-tap Kaiser
//     beta 9, 2 phases, float32 samples, 4-lane fused-multiply-add accumulation as on arm64,
//     start/end edges mirrored), converted back to s16 via s32 (truncate, then >> 16);
//   * stereo/48 kHz tracks pass through untouched;
//   * the RIFF layout ffmpeg writes (fmt, LIST/INFO, data) plus the smpl loop chunk.
// All 223 tracks of the NTSC-U BGM.CVM are verified byte-identical to the Python/ffmpeg
// output (see web/test/adx.test.mjs).

export const BGM_RECIPE_VERSION = 'adx-to-pcm-s16le-wav-48k-stereo-trim-smpl-inclusive-v2';
export const OUTPUT_RATE = 48000;
export const OUTPUT_CHANNELS = 2;
// libavformat version string ffmpeg embeds in the WAV LIST/INFO/ISFT chunk (the reference
// tool run used ffmpeg 9.0.2 = libavformat 63.1.102). Kept so output is byte-identical.
export const WAV_ISFT = 'Lavf63.1.102';

const BLOCK_SIZE = 18;
const BLOCK_SAMPLES = 32;
const COEFF_BITS = 12;

/** Port of parse_adx_header(): standard CRI ADX v3/v4 loop metadata with checked bounds. */
export function parseAdxHeader(data, fileSize = data.length) {
  if (data.length < 0x20) throw new Error('ADX header is truncated');
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (dv.getUint16(0) !== 0x8000) throw new Error('ADX signature 0x8000 is missing');
  const headerSize = dv.getUint16(2) + 4;
  const encoding = data[4], frameSize = data[5], bits = data[6], channels = data[7];
  const sampleRate = dv.getUint32(8), sampleCount = dv.getUint32(12);
  const cutoff = dv.getUint16(0x10);
  const version = dv.getUint16(0x12);
  if (encoding !== 3 || frameSize !== 0x12 || bits !== 4) {
    throw new Error(`Unsupported ADX encoding parameters: encoding=${encoding}, frame_size=${frameSize}, bits_per_sample=${bits}`);
  }
  if (channels !== 1 && channels !== 2) throw new Error(`Expected mono/stereo BGM ADX, got ${channels} channels`);
  if (sampleRate <= 0 || sampleCount <= 0 || headerSize > fileSize) throw new Error('ADX sample rate/count/header size is invalid');
  let loopOffset;
  if (version === 0x0300) loopOffset = 0x14;
  else if (version === 0x0400) loopOffset = 0x18 + (channels > 1 ? 4 * channels : 8);
  else throw new Error(`Unsupported ADX version 0x${version.toString(16).padStart(4, '0')}; only clear-text v3/v4 is handled`);
  if (data.length < loopOffset + 0x18) throw new Error('ADX loop metadata is truncated');
  const base = { version, headerSize, channels, sampleRate, sampleCount, cutoff, loop: null };
  if (headerSize - 6 < loopOffset + 0x18) return base; // valid track without loop metadata
  const loopCount = dv.getUint32(loopOffset + 4);
  const loopStart = dv.getUint32(loopOffset + 8);
  const loopEnd = dv.getUint32(loopOffset + 16);
  if (loopCount) {
    if (!(loopStart >= 0 && loopStart < loopEnd && loopEnd <= sampleCount)) {
      throw new Error(`Invalid ADX loop range [${loopStart}, ${loopEnd}) for ${sampleCount} samples`);
    }
    base.loop = { start_sample: loopStart, end_sample_exclusive: loopEnd };
  }
  return base;
}

export function scaledSample(sample, sourceRate) {
  // (sample * 48000 + rate // 2) // rate, kept exact with BigInt-free safe integers
  return Math.floor((sample * OUTPUT_RATE + Math.floor(sourceRate / 2)) / sourceRate);
}

/** Exact number of bytes convertAdxToWav() will produce, from the header alone. */
export function wavSizeFor(header) {
  const frames = scaledSample(header.sampleCount, header.sampleRate);
  return 12 + (8 + 16) + (header.loop ? 8 + 60 : 0) + (8 + 26) + 8 + frames * 4;
}

// ffmpeg adx.c ff_adx_calculate_coeffs
function adxCoeffs(cutoff, sampleRate) {
  const a = Math.SQRT2 - Math.cos(2.0 * Math.PI * cutoff / sampleRate);
  const b = Math.SQRT2 - 1.0;
  const c = (a - Math.sqrt((a + b) * (a - b))) / b;
  return [Math.floor(c * 8192 + 0.5), Math.floor(c * c * -4096 + 0.5)];
}

/** Decode the whole ADX file into per-channel Int16Arrays (stops at the 0x8000 EOF block). */
export function decodeAdx(data, header) {
  const ch = header.channels;
  const blocks = Math.floor((data.length - header.headerSize) / (BLOCK_SIZE * ch));
  const out = [];
  for (let c = 0; c < ch; c++) out.push(new Int16Array(blocks * BLOCK_SAMPLES));
  const [c0, c1] = adxCoeffs(header.cutoff, header.sampleRate);
  const s1 = new Int32Array(ch), s2 = new Int32Array(ch);
  let n = 0;
  for (; n < blocks; n++) {
    const base = header.headerSize + n * BLOCK_SIZE * ch;
    if (data[base] & 0x80) break; // EOF marker: the demuxer stops here
    for (let c = 0; c < ch; c++) {
      const p = base + c * BLOCK_SIZE;
      const scale = (data[p] << 8) | data[p + 1];
      let h1 = s1[c], h2 = s2[c];
      const dst = out[c];
      let o = n * BLOCK_SAMPLES;
      for (let i = 0; i < BLOCK_SAMPLES; i++) {
        const byte = data[p + 2 + (i >> 1)];
        let d = (i & 1) ? (byte & 15) : (byte >> 4);
        if (d > 7) d -= 16;
        // s0 = ((d << 12) * scale + c0*h1 + c1*h2) >> 12, all in C int32 arithmetic
        const s0 = (Math.imul(d << COEFF_BITS, scale) + Math.imul(c0, h1) + Math.imul(c1, h2)) >> COEFF_BITS;
        const clipped = s0 > 32767 ? 32767 : s0 < -32768 ? -32768 : s0;
        h2 = h1; h1 = clipped; // ffmpeg keeps the clipped sample as predictor history
        dst[o++] = clipped;
      }
      s1[c] = h1; s2[c] = h2;
    }
  }
  return { channels: out.map((a) => (n === blocks ? a : a.subarray(0, n * BLOCK_SAMPLES))), frames: n * BLOCK_SAMPLES };
}

// ---- libswresample 24 kHz -> 48 kHz (exact ratio 1:2, 2 phases, 32 taps, Kaiser beta 9) ----

function besselI0(x) {
  let s = 1, t = 1, k = 1;
  for (;;) {
    t *= (x / 2 / k) * (x / 2 / k);
    s += t;
    if (t < 1e-18 * s) break;
    k++;
  }
  return s;
}

let halfPhaseTaps = null; // phase 1 (the point halfway between two input samples), float32
function getHalfPhaseTaps() {
  if (halfPhaseTaps) return halfPhaseTaps;
  const n = 32, center = (n - 1) >> 1, beta = 9;
  const raw = [];
  let norm = 0;
  for (let i = 0; i < n; i++) {
    const x = Math.PI * ((i - center) - 0.5);
    let y = Math.sin(x) / x;
    const w = 2.0 * x / (n * Math.PI);
    y *= besselI0(beta * Math.sqrt(Math.max(1 - w * w, 0)));
    raw.push(y);
  }
  // swr normalises every phase by the sum of phase 0 (a lone centre tap of I0(beta))
  norm = besselI0(beta);
  halfPhaseTaps = Float32Array.from(raw, (v) => Math.fround(v / norm));
  return halfPhaseTaps;
}

/** Upsample one channel 2x, returning only the first `outFrames` output samples. */
export function upsample2x(x, outFrames) {
  const N = x.length;
  const h = getHalfPhaseTaps();
  const out = new Int16Array(outFrames);
  // padded float32 copy: 16 mirrored samples on the left, 17 mirrored on the right
  const L = 16, R = 17;
  const xf = new Float32Array(L + N + R);
  for (let i = 0; i < N; i++) xf[L + i] = x[i] / 32768;
  for (let j = 1; j <= L; j++) xf[L - j] = x[Math.min(j, N - 1)] / 32768;   // x[-j] = x[j]
  for (let m = 0; m < R; m++) xf[L + N + m] = x[Math.max(N - 1 - m, 0)] / 32768; // x[N+m] = x[N-1-m]
  const fr = Math.fround;
  for (let n = 0; n < outFrames; n++) {
    const k = n >> 1;
    if ((n & 1) === 0) { out[n] = x[k]; continue; }
    let l0 = 0, l1 = 0, l2 = 0, l3 = 0;
    const b = L + k - 15;
    for (let i = 0; i < 32; i += 4) {
      l0 = fr(xf[b + i] * h[i] + l0);
      l1 = fr(xf[b + i + 1] * h[i + 1] + l1);
      l2 = fr(xf[b + i + 2] * h[i + 2] + l2);
      l3 = fr(xf[b + i + 3] * h[i + 3] + l3);
    }
    // libswresample converts float -> s32 (truncating) -> s16 (arithmetic >> 16)
    let r = Math.trunc(fr(fr(l0 + l1) + fr(l2 + l3)) * 2147483648);
    if (r > 2147483647) r = 2147483647; else if (r < -2147483648) r = -2147483648;
    out[n] = r >> 16;
  }
  return out;
}

function u32(v) { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v >>> 0, true); return b; }

/**
 * Convert a whole ADX file (Uint8Array) to the exact WAV the Python tool writes.
 * Returns { wav: Uint8Array, header, frames, loop } where `loop` is the converted 48 kHz loop.
 */
export function convertAdxToWav(data) {
  const header = parseAdxHeader(data.subarray(0, 0x100), data.length);
  if (header.channels !== 2) {
    // ffmpeg's mono -> stereo upmix applies a -3 dB matrix whose exact rounding is not
    // reproduced here; every BGM track on the disc is stereo, so refuse rather than guess.
    throw new Error(`Unsupported mono ADX (${header.channels} channel); all Persona 3 FES BGM tracks are stereo`);
  }
  const expected = scaledSample(header.sampleCount, header.sampleRate);
  const dec = decodeAdx(data, header);
  let chans = dec.channels;
  if (header.sampleRate === 48000) {
    // no resampling; atrim to the expected length
  } else if (header.sampleRate === 24000) {
    chans = chans.map((c) => upsample2x(c, Math.min(expected, c.length * 2)));
  } else {
    throw new Error(`Unsupported ADX sample rate ${header.sampleRate} (only 24000 and 48000 occur on the disc)`);
  }
  const have = chans[0].length;
  if (have < expected) {
    throw new Error(`ADX header declares ${expected} output frames at 48 kHz, but decoding produced ${have}`);
  }
  const frames = expected;
  let loopOut = null;
  if (header.loop) {
    const start = scaledSample(header.loop.start_sample, header.sampleRate);
    const endEx = scaledSample(header.loop.end_sample_exclusive, header.sampleRate);
    if (endEx <= start) throw new Error('Loop collapsed to zero samples when resampled to 48 kHz');
    if (endEx > frames) throw new Error(`Resampled ADX loop ends at ${endEx} but decoded WAV contains only ${frames} frames`);
    loopOut = { start_sample: start, end_sample_inclusive: endEx - 1, end_sample_exclusive: endEx, sample_rate: OUTPUT_RATE };
  }
  const total = wavSizeFor(header);
  const wav = new Uint8Array(total);
  const dv = new DataView(wav.buffer);
  let p = 0;
  const tag = (s) => { for (let i = 0; i < 4; i++) wav[p++] = s.charCodeAt(i); };
  tag('RIFF'); dv.setUint32(p, total - 8, true); p += 4; tag('WAVE');
  tag('fmt '); dv.setUint32(p, 16, true); p += 4;
  dv.setUint16(p, 1, true); dv.setUint16(p + 2, 2, true); dv.setUint32(p + 4, OUTPUT_RATE, true);
  dv.setUint32(p + 8, OUTPUT_RATE * 4, true); dv.setUint16(p + 12, 4, true); dv.setUint16(p + 14, 16, true); p += 16;
  if (loopOut) {
    tag('smpl'); dv.setUint32(p, 60, true); p += 4;
    const vals = [0, 0, Math.round(1e9 / OUTPUT_RATE), 60, 0, 0, 0, 1, 0, 0, 0, loopOut.start_sample, loopOut.end_sample_inclusive, 0, 0];
    for (const v of vals) { dv.setUint32(p, v >>> 0, true); p += 4; }
  }
  const isft = new TextEncoder().encode(WAV_ISFT + '\0');
  const infoLen = 4 + 8 + isft.length + (isft.length & 1);
  tag('LIST'); dv.setUint32(p, infoLen, true); p += 4;
  tag('INFO'); tag('ISFT'); dv.setUint32(p, isft.length, true); p += 4;
  wav.set(isft, p); p += isft.length + (isft.length & 1);
  tag('data'); dv.setUint32(p, frames * 4, true); p += 4;
  const left = chans[0], right = chans[1];
  for (let i = 0; i < frames; i++) {
    dv.setInt16(p, left[i], true); dv.setInt16(p + 2, right[i], true); p += 4;
  }
  if (p !== total) throw new Error('internal error: WAV size mismatch');
  return { wav, header, frames, loop: loopOut };
}

/** Output file stem the runtime opens: upper-case, [A-Z0-9_-] kept, anything else '_'. */
export function wavNameForStem(stem) {
  let s = stem.toUpperCase().replace(/[^A-Z0-9_-]/g, '_');
  return s + '.wav';
}

export function __taps() { return getHalfPhaseTaps(); }
