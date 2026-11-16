// Pool of BGM conversion workers; falls back to converting on the calling thread when Web
// Workers are unavailable (Node tests, very old browsers).
import { convertAdxToWav } from './adx.js';

export function createBgmPool({ size, workerUrl } = {}) {
  const hc = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
  const n = Math.max(1, Math.min(size || hc - 1, 6));
  const workers = [];
  const pending = new Map();
  let nextId = 1;
  let rr = 0;
  let inline = typeof Worker === 'undefined';
  if (!inline) {
    try {
      const url = workerUrl || new URL('./bgm-worker.js', import.meta.url);
      for (let i = 0; i < n; i++) {
        const w = new Worker(url, { type: 'module' });
        w.onmessage = (ev) => {
          const { id, wav, error } = ev.data;
          const p = pending.get(id);
          if (!p) return;
          pending.delete(id);
          if (error) p.reject(new Error(error)); else p.resolve(new Uint8Array(wav));
        };
        w.onerror = (ev) => {
          const err = new Error('BGM worker failed: ' + (ev.message || 'unknown error'));
          for (const [id, p] of pending) { pending.delete(id); p.reject(err); }
        };
        workers.push(w);
      }
    } catch (e) {
      workers.forEach((w) => w.terminate());
      workers.length = 0;
      inline = true;
    }
  }
  return {
    size: inline ? 1 : workers.length,
    inline,
    convert(adx /* Uint8Array */) {
      if (inline) {
        return new Promise((resolve, reject) => setTimeout(() => {
          try { resolve(convertAdxToWav(adx).wav); } catch (e) { reject(e); }
        }, 0));
      }
      const id = nextId++;
      const w = workers[rr++ % workers.length];
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        const buf = adx.byteOffset === 0 && adx.byteLength === adx.buffer.byteLength ? adx.buffer : adx.slice().buffer;
        w.postMessage({ id, adx: buf }, [buf]);
      });
    },
    terminate() {
      workers.forEach((w) => w.terminate());
      for (const [id, p] of pending) { pending.delete(id); p.reject(new Error('cancelled')); }
    },
  };
}
