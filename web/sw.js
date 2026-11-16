/*
 * Service worker + page bootstrap, one file (used both as `<script src="sw.js">` and as the worker).
 *
 * 1. Cross-origin isolation (COOP/COEP) for GitHub Pages, which cannot send those headers: this is
 *    the coi-serviceworker v0.1.7 logic (Guido Zuidhof and contributors, MIT, see
 *    vendor/ffmpeg/LICENSE.coi-serviceworker.txt). It lets the movie converter use the
 *    multi-thread ffmpeg.wasm core. The first visit reloads the page once.
 * 2. Download streaming for browsers without the File System Access API (Firefox, Safari): the
 *    page posts ZIP chunks over a MessagePort and a navigation to ./__p3fes_dl__/<id>/<name> is
 *    answered with a streaming "attachment" response. No network access of its own.
 */
let coepCredentialless = false;
const sessions = new Map();

if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

  self.addEventListener('message', (ev) => {
    const d = ev.data;
    if (!d) return;
    if (d.type === 'open' && ev.ports[0]) {
      sessions.set(d.id, { port: ev.ports[0], filename: d.filename });
    } else if (d.type === 'deregister') {
      self.registration.unregister()
        .then(() => self.clients.matchAll())
        .then((clients) => clients.forEach((client) => client.navigate(client.url)));
    } else if (d.type === 'coepCredentialless') {
      coepCredentialless = d.value;
    }
  });

  const isolate = (headers) => {
    headers.set('Cross-Origin-Embedder-Policy', coepCredentialless ? 'credentialless' : 'require-corp');
    if (!coepCredentialless) headers.set('Cross-Origin-Resource-Policy', 'cross-origin');
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    return headers;
  };

  self.addEventListener('fetch', (event) => {
    const r = event.request;
    const url = new URL(r.url);

    // ---- streaming download
    const m = url.pathname.match(/\/__p3fes_dl__\/([^/]+)\/([^/]*)$/);
    if (m) {
      const s = sessions.get(m[1]);
      if (!s) { event.respondWith(new Response('Unknown download', { status: 404 })); return; }
      sessions.delete(m[1]);
      const port = s.port;
      const stream = new ReadableStream({
        start(controller) {
          port.onmessage = (e) => {
            const d = e.data;
            if (d.type === 'chunk') controller.enqueue(new Uint8Array(d.buf));
            else if (d.type === 'end') controller.close();
            else if (d.type === 'abort') controller.error(new Error('aborted'));
          };
        },
        pull() { port.postMessage({ type: 'pull' }); },
        cancel() { port.postMessage({ type: 'cancel' }); },
      }, new CountQueuingStrategy({ highWaterMark: 1 }));
      const headers = isolate(new Headers({
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': `attachment; filename*=UTF-8''${encodeURIComponent(s.filename)}`,
        'Cache-Control': 'no-store',
      }));
      event.respondWith(new Response(stream, { headers }));
      return;
    }

    // ---- everything else: pass through, adding the isolation headers
    if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
    const request = (coepCredentialless && r.mode === 'no-cors') ? new Request(r, { credentials: 'omit' }) : r;
    event.respondWith(
      fetch(request).then((response) => {
        if (response.status === 0) return response;
        return new Response(response.body, { status: response.status, statusText: response.statusText, headers: isolate(new Headers(response.headers)) });
      }).catch((e) => console.error(e)),
    );
  });
} else {
  (() => {
    const coi = {
      shouldRegister: () => true,
      coepCredentialless: () => !(window.chrome || window.netscape),
      doReload: () => window.location.reload(),
      quiet: true,
      ...window.coi,
    };
    const n = navigator;
    if (n.serviceWorker && n.serviceWorker.controller) {
      n.serviceWorker.controller.postMessage({ type: 'coepCredentialless', value: coi.coepCredentialless() });
    }
    // already isolated (or the browser has no such notion): nothing to do
    if (window.crossOriginIsolated !== false || !coi.shouldRegister()) return;
    if (!window.isSecureContext || !n.serviceWorker) return;
    n.serviceWorker.register(window.document.currentScript.src).then(
      (registration) => {
        registration.addEventListener('updatefound', () => coi.doReload());
        if (registration.active && !n.serviceWorker.controller) coi.doReload();
      },
      (err) => { !coi.quiet && console.error('Service worker failed to register:', err); },
    );
  })();
}
