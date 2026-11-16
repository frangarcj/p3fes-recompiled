// Output sinks for the streamed ZIP. Nothing is ever buffered in memory:
//   * 'file'  : File System Access API (Chromium) - bytes go straight to the chosen file;
//   * 'sw'    : a service worker turns the chunks into a normal browser download (Firefox, Safari).

export function hasFilePicker() {
  return typeof window !== 'undefined' && typeof window.showSaveFilePicker === 'function';
}

export function hasServiceWorkerSink() {
  return typeof navigator !== 'undefined' && 'serviceWorker' in navigator && typeof ReadableStream === 'function' && window.isSecureContext;
}

/** Must be called from a click handler (user activation). Returns { writable, kind, close? }. */
export async function openFileSink(suggestedName) {
  const handle = await window.showSaveFilePicker({
    suggestedName,
    types: [{ description: 'ZIP archive', accept: { 'application/zip': ['.zip'] } }],
  });
  const writable = await handle.createWritable();
  return { writable, kind: 'file', name: handle.name };
}

let swReg = null;
async function getServiceWorker() {
  if (!swReg) swReg = await navigator.serviceWorker.register(new URL('../sw.js', import.meta.url), { scope: new URL('../', import.meta.url).pathname });
  const reg = swReg;
  if (!reg.active) {
    const sw = reg.installing || reg.waiting;
    await new Promise((resolve, reject) => {
      if (!sw) return reject(new Error('Service worker is not available'));
      if (sw.state === 'activated') return resolve();
      sw.addEventListener('statechange', () => { if (sw.state === 'activated') resolve(); else if (sw.state === 'redundant') reject(new Error('Service worker failed to install')); });
    });
  }
  return reg.active;
}

/** Streams through a service worker so the browser writes a normal download to disk. */
export async function openServiceWorkerSink(filename) {
  const sw = await getServiceWorker();
  const id = (crypto.randomUUID ? crypto.randomUUID() : String(Date.now()) + Math.random().toString(16).slice(2));
  const { port1, port2 } = new MessageChannel();
  let credits = 0;
  let waiter = null;
  let cancelled = false;
  let started;
  const startedPromise = new Promise((r) => { started = r; });
  port1.onmessage = (ev) => {
    const t = ev.data && ev.data.type;
    if (t === 'pull') { credits++; started(); if (waiter) { const w = waiter; waiter = null; w(); } }
    else if (t === 'cancel') { cancelled = true; if (waiter) { const w = waiter; waiter = null; w(); } }
  };
  sw.postMessage({ type: 'open', id, filename }, [port2]);
  const url = new URL(`./__p3fes_dl__/${id}/${encodeURIComponent(filename)}`, new URL('../', import.meta.url)).href;
  const frame = document.createElement('iframe');
  frame.hidden = true;
  frame.src = url;
  document.body.appendChild(frame);
  const keepAlive = setInterval(() => port1.postMessage({ type: 'ping' }), 10000);
  const timeout = new Promise((_, rej) => setTimeout(() => rej(new Error('The browser did not start the download. Use a Chromium-based browser (Chrome, Edge) for the safest result.')), 20000));
  await Promise.race([startedPromise, timeout]);

  const finish = () => { clearInterval(keepAlive); setTimeout(() => frame.remove(), 5000); };
  const writable = new WritableStream({
    async write(chunk) {
      while (credits === 0 && !cancelled) await new Promise((r) => { waiter = r; });
      if (cancelled) throw new Error('The download was cancelled in the browser.');
      credits--;
      let buf = chunk;
      if (buf.byteOffset !== 0 || buf.byteLength !== buf.buffer.byteLength) buf = buf.slice();
      port1.postMessage({ type: 'chunk', buf: buf.buffer }, [buf.buffer]);
    },
    close() { port1.postMessage({ type: 'end' }); finish(); },
    abort() { port1.postMessage({ type: 'abort' }); finish(); },
  }, { highWaterMark: 1 });
  return { writable, kind: 'sw', name: filename };
}
