// Web Worker: ADX -> WAV conversion (see adx.js). Protocol: {id, adx: ArrayBuffer} -> {id, wav: ArrayBuffer} | {id, error}
import { convertAdxToWav } from './adx.js';

self.onmessage = (ev) => {
  const { id, adx } = ev.data;
  try {
    const { wav } = convertAdxToWav(new Uint8Array(adx));
    self.postMessage({ id, wav: wav.buffer }, [wav.buffer]);
  } catch (e) {
    self.postMessage({ id, error: String(e && e.message || e) });
  }
};
