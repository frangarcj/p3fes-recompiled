# P3FES Vita data builder (static web tool)

A single-page tool, hosted on GitHub Pages (`.github/workflows/pages.yml`), that turns the user's
own Persona 3 FES NTSC-U disc (`SLUS-21621`) into one ZIP to extract at the root of `ux0:` on a
PS Vita. Everything runs in the browser; the page makes no network requests with user data and has
no analytics. No game data is stored in this repository.

## Output (`data/p3fes/...` inside the ZIP)

| entry | source |
| --- | --- |
| `SLUS_216.21` | disc, SHA-1 checked against `3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb` |
| `SYSTEM.CNF`, `LIB31/**`, `BGM.CVM`, `BTL.CVM`, `DATA.CVM` | copied as they are |
| `native_bgm/<STEM>.wav` | every ADX of `BGM.CVM` (223), JS port of `prepare_bgm.py` |
| `native_movies/<STEM>.mp4` | SFD movies of `DATA.CVM`, via `lib/movies.js` (other module) |
| `runtime.cfg` | fixed text (see `lib/pipeline.js`) |
| `p3fes-assets.json` | tool/recipe versions, converted movies, ISO SHA-1 |

No memory card is shipped (the runtime creates `memcards/Mcd001.ps2` itself).
Total size is about 6 GB (4.57 GB game data + 1.60 GB WAV + ~0.3 GB movies).

## Layout

```
index.html style.css app.js      UI (EN/ES), progress, cancel, error and install screens
sw.js                            download-streaming service worker (Firefox/Safari fallback)
lib/iso9660.js lib/cvm.js        ISO9660 + CRI ROFS(CVM) readers over Blob.slice
lib/disc.js                      finds the needed files in an ISO / folder / loose files
lib/adx.js                       ADX -> WAV (byte-identical to prepare_bgm.py + ffmpeg)
lib/bgm-pool.js bgm-worker.js    Web Worker pool for the conversion
lib/pipeline.js                  verify -> copy -> BGM -> movies -> manifest, as a streamed ZIP
lib/sink.js                      File System Access sink / service-worker sink
lib/movies.js                    (not in this module set) movie converter, contract below
vendor/client-zip/               client-zip 2.5.0 (MIT), zip64 + streaming, STORE only
test/                            node --test suites (see below)
```

### Movie module contract

```js
export const EARLY_MOVIES;                 // stems always converted
export async function createMovieConverter({ onLog }); // -> { convert(stem, sfdBytes, onProgress), terminate() }
// optional: export const MOVIE_RECIPE_VERSION  (written to p3fes-assets.json)
```

`lib/movies.js` is loaded with a dynamic `import()`: if it is missing the page still works and the
movies are left out (the option is disabled and a note is shown).

## Output streaming

The ZIP is never built in memory. Chromium writes it straight to the file chosen with
`showSaveFilePicker`. Browsers without that API stream the same bytes through `sw.js` as a normal
download (`./__p3fes_dl__/<id>/p3fes-vita-data.zip`); `?sink=sw` forces this path.

## Tests

```
cd web && node --test test/*.test.mjs   # needs ../game-mac, python3, ffmpeg, macOS hdiutil (test/movies/ belongs to the movie module)
```

Environment: `P3FES_ISO_SPARSE` (cached sparse image of the real disc, built from the `.7z` with
`7z x -so` pipes; `P3FES_DISC_7Z` points at the archive), `P3FES_TEST_TMP` (scratch folder).
Tests skip themselves when their inputs are missing.
