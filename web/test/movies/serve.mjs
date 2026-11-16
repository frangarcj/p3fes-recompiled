// Tiny static server for the movie harness.
//   node serve.mjs [--port 8099] [--isolate] [--sfd DIR] [--core NODE_MODULES_FFMPEG_DIR] [--ref DIR]
// Serves web/ at "/", plus /sfd/ (extracted SFDs), /core/ (a node_modules/@ffmpeg folder, to
// test without the CDN) and /ref/. --isolate adds COOP/COEP (what a real host would do);
// without it the page is not cross-origin isolated unless ?coi=1 is used.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const webRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const port = +opt('port', 8099);
const isolate = args.includes('--isolate');
const mounts = {
  '/sfd/': opt('sfd', null), '/core/': opt('core', null), '/ref/': opt('ref', null),
};
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json', '.mp4': 'video/mp4', '.txt': 'text/plain' };

http.createServer((req, res) => {
  let url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  let root = webRoot;
  for (const [p, dir] of Object.entries(mounts)) {
    if (dir && url.startsWith(p)) { root = path.resolve(dir); url = '/' + url.slice(p.length); }
  }
  if (url === '/coi-serviceworker.js') { root = webRoot; url = '/vendor/ffmpeg/coi-serviceworker.js'; }
  const file = path.join(root, url);
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404); return res.end('not found');
  }
  const h = { 'content-type': types[path.extname(file)] || 'application/octet-stream',
    'cache-control': 'no-store', 'access-control-allow-origin': '*',
    'content-length': fs.statSync(file).size };
  if (isolate) {
    h['cross-origin-opener-policy'] = 'same-origin';
    h['cross-origin-embedder-policy'] = 'require-corp';
    h['cross-origin-resource-policy'] = 'cross-origin';
  }
  res.writeHead(200, h);
  fs.createReadStream(file).pipe(res);
}).listen(port, () => console.log(`http://localhost:${port}/ (web root ${webRoot}${isolate ? ', COOP/COEP on' : ''})`));
