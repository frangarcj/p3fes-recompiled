# Vendored: ffmpeg.wasm JS wrapper and coi-serviceworker

Only small, MIT-licensed JavaScript is vendored here. The 32 MB wasm cores are NOT
copied into the repository; `web/lib/movies.js` downloads them at run time from
jsDelivr (fallback unpkg) at the exact pinned version and serves them to the worker through
`blob:` URLs (so the cross-origin `Worker`/`import()` restrictions do not apply).

| File | Source | Version | License |
|---|---|---|---|
| `ffmpeg/*.js` | `@ffmpeg/ffmpeg` `dist/esm` | 0.12.15 | MIT (`LICENSE.ffmpeg.wasm.txt`) |
| `util/*.js` | `@ffmpeg/util` `dist/esm` | 0.12.2 | MIT (`LICENSE.ffmpeg.wasm.txt`) |
| `coi-serviceworker.js` | `coi-serviceworker` | 0.1.7 | MIT (`LICENSE.coi-serviceworker.txt`) |
| (CDN) `@ffmpeg/core`, `@ffmpeg/core-mt` | `dist/esm/ffmpeg-core.{js,wasm}` (+ `.worker.js` for mt) | 0.12.10 (FFmpeg 5.1.4, x264) | GPL-2.0-or-later |

`ffmpeg/worker.js` has to stay next to `ffmpeg/classes.js` (the wrapper starts it with
`new Worker(new URL('./worker.js', import.meta.url), {type:'module'})`); because it is
vendored on the page's own origin there is no cross-origin worker problem and no
`classWorkerURL` is needed.

`coi-serviceworker.js` is kept here only as the upstream reference and for the movie benchmark
(`web/test/movies/serve.mjs`). The site itself does not load it: its logic is merged into
`web/sw.js` together with the streaming-download worker, because one scope can have only one
service worker. Do not copy it to the site root.

To upgrade: `npm i @ffmpeg/ffmpeg@X @ffmpeg/util@Y`, copy `dist/esm/*.js`, bump
`FFMPEG_CORE_VERSION` in `web/lib/movies.js`, rerun `web/test/movies` (caps + a movie).
