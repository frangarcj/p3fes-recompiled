# Movie converter tests / benchmark

`web/lib/movies.js` converts a Persona 3 FES `.SFD` (MPEG-1/2 + ADX) to the Vita MP4 with
ffmpeg.wasm and exactly the arguments of `p3fes/scripts/native_assets/prepare_movies.py`
(H.264 Baseline L3.1 yuv420p, <= 640x448, source fps, AAC 48 kHz stereo 128k, faststart).

## API

```js
import { createMovieConverter, MOVIE_RECIPE_VERSION, EARLY_MOVIES } from './lib/movies.js';
const conv = await createMovieConverter({ onLog, threads: 'auto' /* | 'st' | 'mt' */ });
const mp4 = await conv.convert('P3CTOP2', sfdBlobOrUint8Array, (fraction) => {});
conv.terminate();
```

* `threads: 'auto'` uses the multi-thread core only when `crossOriginIsolated` is true.
* Pass a `Blob` (e.g. `cvmFile.slice(off, off + size)`) when you can: it is mounted
  read-only in the wasm FS (WORKERFS), nothing is copied into the 32-bit wasm heap and the
  JS copy can be freed. A `Uint8Array` also works (it is wrapped in a Blob; it is not detached).
* `threads: 'mt'` uses explicit `-threads min(8, cores)`. Do not raise it: the mt core has a
  fixed pool of 32 pthread workers and ffmpeg dead-locks when it asks for more (seen with
  `-threads 12`, and with `-preset veryfast`). A watchdog (`stallMs`, 90 s without ffmpeg
  output) and an automatic retry of the movie on the single-thread core cover that.
* Calls are serialized per converter. Failures throw an `Error` whose first line names the
  stem; the worker is then restarted for the next call. Temporary files are removed after
  each movie; `recycleAfter: N` additionally restarts the worker every N movies.
* `await createMovieConverter()` downloads/compiles the core (about 9.3 MB brotli from
  jsDelivr, immutable-cached) so core load errors surface there.

## Cross-origin isolation on GitHub Pages (for the multi-thread core)

GitHub Pages cannot send COOP/COEP headers. The site handles this in `web/sw.js` (the
coi-serviceworker logic merged with the download worker; `index.html` loads it as its first
classic script). The notes below describe the standalone `coi-serviceworker` setup that the
benchmark server (`serve.mjs`) emulates:

1. `coi-serviceworker.js` served from the page's own directory (a service worker only
   controls its own directory).
2. First script in `<head>` of the page that converts movies, as a classic script:
   `<script src="coi-serviceworker.js"></script>`.
3. First visit registers the worker and reloads the page once (HTTPS or localhost only);
   afterwards `crossOriginIsolated === true` and `threads: 'auto'` picks the mt core.
   If it is not isolated (service workers blocked, private window, ...) the converter
   silently uses the single-thread core, about 3.5x slower.
4. Every cross-origin subresource of that page needs CORS/CORP headers (jsDelivr and unpkg
   send them; images/fonts from other hosts must too, or the page breaks).

## Measured (headless Chrome, 8-core Apple Silicon, core from jsDelivr)

Source video seconds / transcode seconds (excludes core download). Variance between runs of
the same movie was up to +-40 % on this shared machine.

| Movie | video | st | mt (8 threads) |
|---|---|---|---|
| GAME_START (MPEG-1 640x448) | 26 s | 9.4 s (83 fps) | 2.4 s (326 fps) |
| P3CTOP2 (MPEG-1 640x448) | 89 s | 109.5 s (24 fps) | 20.6 s (129 fps), 30 s in other runs |
| P3CTOP (MPEG-2 640x368) | 93 s | 68.0 s (41 fps) | 14.3 s (195 fps) |
| P3CT001_02 (MPEG-1 640x368) | 60 s | 46.4 s (39 fps) | 9.4 s (193 fps) |
| P3CT001_01 (MPEG-1 640x368, 143 MB) | 232 s | 196.7 s (35 fps) | 40.8 s (170 fps) |
| STAFF_ROLL_F (MPEG-2 640x448, 192 MB) | 352 s | 226.1 s (47 fps) | 44.3 s (238 fps), 52 s in other runs |

Linear extrapolation over all 41 movies in `/SFD/` (3575 s of video, 2.05 GB): st about 46 min,
mt about 9 to 15 min. The five `EARLY_MOVIES` (501 s): st about 7.2 min, mt about 1.5 to 2.3 min.
`-preset veryfast` (not in the recipe, st only) made P3CTOP2 2.7x faster (40 s) and 5 % smaller.
Peak renderer RSS (tab plus workers): mt 0.74 to 0.98 GB, st 0.53 to 0.63 GB with a Blob input
(+0.2 GB when the 192 MB SFD is passed as a `Uint8Array`); JS heap stays below 220 MB.

## Rerun the benchmark

Needs headless Chrome (`/Applications/Google Chrome.app`), node >= 18, and ffprobe on PATH.

```sh
# 1. puppeteer-core in a scratch dir
mkdir -p /tmp/p3movies/npm && cd /tmp/p3movies/npm && npm init -y && npm i puppeteer-core
# 2. extract SFDs from the CVM (from the repo root)
python3 - <<'PY'
import sys; sys.path.insert(0, 'p3fes/scripts/native_assets')
import prepare_movies as pm, p3fes_unpack
from pathlib import Path
f, inner, entries = pm.movie_entries(Path('game-mac/DATA.CVM'))
for name in ['P3CTOP2.SFD', 'P3CTOP.SFD', 'GAME_START.SFD']:
    e = pm.find_movie(entries, name)
    p3fes_unpack.write_range(p3fes_unpack.FileView(inner, e), 0, e['size'], f'/tmp/p3movies/sfd/{name}')
PY
# 3. two servers: plain (single thread, or ?coi=1 for the service worker) and isolated (headers)
node web/test/movies/serve.mjs --port 8099 --sfd /tmp/p3movies/sfd &
node web/test/movies/serve.mjs --port 8098 --isolate --sfd /tmp/p3movies/sfd &
# 4. run
export BENCH_NODE_MODULES=/tmp/p3movies/npm
B=web/test/movies/bench.mjs
node $B --caps --threads st                                        # codec/format evidence from inside wasm
node $B --threads st --stem P3CTOP2 --sfd /sfd/P3CTOP2.SFD --blob --out /tmp/p3movies/P3CTOP2.st.mp4
node $B --url http://localhost:8098 --threads mt --stem P3CTOP2 --sfd /sfd/P3CTOP2.SFD --blob --out /tmp/p3movies/P3CTOP2.mt.mp4
node $B --threads auto --coi --stem GAME_START --sfd /sfd/GAME_START.SFD --blob   # service-worker path
node $B --threads st --seq GAME_START,BADMOVIE,GAME_START         # failure + recovery on one converter (put junk in /sfd/BADMOVIE.SFD)
node $B --url http://localhost:8098 --threads mt --blob --slice --stem GAME_START --sfd /sfd/GAME_START.SFD   # Blob.slice input
# 5. metadata parity against the prepare_movies.py output
python3 -I web/test/movies/compare.py game-mac/native_movies /tmp/p3movies/P3CTOP2.*.mp4
```

Each bench run prints one JSON line (`convertSeconds` is pure transcode time, `loadSeconds` the
core load; `rendererRssPeakMB` is the OS-level RSS of the Chrome renderer process that holds
the tab and its workers, `treeRssPeakMB` the whole browser). Frames per second = source
seconds x 29.97 / `convertSeconds`.

You can also open `harness.html` by hand through `serve.mjs` and pick an `.SFD` file.
