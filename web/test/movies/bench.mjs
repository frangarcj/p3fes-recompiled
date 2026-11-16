// Drive the harness in headless Chrome and print one JSON line per run.
//   BENCH_NODE_MODULES=/dir/with/node_modules node bench.mjs \
//     --url http://localhost:8099 --stem P3CTOP2 --sfd /sfd/P3CTOP2.SFD \
//     [--threads st|mt|auto] [--preset veryfast] [--core /core/core/dist/esm] [--coi] [--blob] [--slice] [--out out.mp4]
//     [--caps] [--seq A,B,C] [--chrome "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
// Memory: samples the renderer + all worker processes' RSS through `ps` every 500 ms and
// reports the peak of the browser process tree (what the OS sees), next to the JS heap.
import { createRequire } from 'node:module';
import path from 'node:path';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';

const req = createRequire(path.join(process.env.BENCH_NODE_MODULES || process.cwd(), 'x.js'));
const puppeteer = req('puppeteer-core');
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i < 0 ? d : args[i + 1]; };
const flag = (n) => args.includes('--' + n);

const base = opt('url', 'http://localhost:8099');
const chrome = opt('chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
const threads = opt('threads', 'auto');
const q = new URLSearchParams();
q.set('threads', threads);
if (opt('seq')) q.set('seq', opt('seq'));
else if (flag('caps')) q.set('caps', '1'); else { q.set('sfd', opt('sfd')); q.set('stem', opt('stem')); }
if (opt('preset')) q.set('preset', opt('preset'));
if (opt('stall')) q.set('stall', opt('stall'));
if (opt('x264threads')) q.set('x264threads', opt('x264threads'));
if (opt('core')) q.set('core', opt('core'));
if (flag('blob')) q.set('blob', '1');
if (flag('slice')) q.set('slice', '1');
if (flag('coi')) q.set('coi', '1');

function rssMB(rootPid) {
  // Browser process tree total, and the renderer processes alone (a tab and its dedicated
  // workers live in the renderer process, so that is the number that matters for the tab).
  const out = execFileSync('ps', ['-axo', 'pid=,ppid=,rss=,command=']).toString().trim().split('\n');
  const kids = new Map(); const info = new Map();
  for (const l of out) {
    const m = /^\s*(\d+)\s+(\d+)\s+(\d+)\s+(.*)$/.exec(l); if (!m) continue;
    const [p, pp, r] = [+m[1], +m[2], +m[3]];
    info.set(p, { r, cmd: m[4] }); (kids.get(pp) || kids.set(pp, []).get(pp)).push(p);
  }
  let tree = 0, renderer = 0; const st = [rootPid];
  while (st.length) {
    const p = st.pop(); const i = info.get(p); if (i) { tree += i.r; if (i.cmd.includes('--type=renderer')) renderer += i.r; }
    st.push(...(kids.get(p) || []));
  }
  return [tree / 1024, renderer / 1024];
}

const browser = await puppeteer.launch({
  executablePath: chrome, headless: 'new', protocolTimeout: 3600000,
  args: ['--no-first-run', '--no-default-browser-check', '--enable-precise-memory-info',
    `--user-data-dir=${fs.mkdtempSync('/private/tmp/claude-501/bench-prof-')}`],
});
const pid = browser.process().pid;
let peak = 0, peakR = 0, baseR = 0;
const timer = setInterval(() => { try { const [t, r] = rssMB(pid); peak = Math.max(peak, t); peakR = Math.max(peakR, r); } catch {} }, 250);
const page = await browser.newPage();
page.on('console', (m) => { if (flag('verbose')) console.error('[page]', m.text()); });
page.on('error', (e) => console.error('[page crashed]', e.message));
page.on('pageerror', (e) => console.error('[pageerror]', e.message));
const t0 = Date.now();
await page.goto(`${base}/test/movies/harness.html?${q}`);
if (flag('coi')) { // first load registers the SW and reloads itself; wait for isolation
  await page.waitForFunction('self.crossOriginIsolated === true', { timeout: 30000 }).catch(() => {});
  if (!(await page.evaluate('self.crossOriginIsolated'))) {
    await page.goto(`${base}/test/movies/harness.html?${q}`);
  }
}
try {
  await page.waitForFunction('window.__result !== null', { timeout: +opt('timeout', 3600) * 1000, polling: 1000 });
} catch (e) {
  console.error('TIMEOUT; page state:', JSON.stringify(await page.evaluate(() => ({ status: document.getElementById('status').textContent, bar: document.getElementById('bar').value, tail: window.__lines.slice(-12) }))));
  clearInterval(timer); await browser.close(); process.exit(2);
}
const result = await page.evaluate('window.__result');
result.treeRssPeakMB = Math.round(peak);
result.rendererRssPeakMB = Math.round(peakR);
result.wallSeconds = (Date.now() - t0) / 1000;
if (opt('out') && result.ok) {
  const b64 = await page.evaluate(`(() => { let s=''; const a=window.__mp4; const CH=0x8000; for (let i=0;i<a.length;i+=CH) s+=String.fromCharCode.apply(null,a.subarray(i,i+CH)); return btoa(s); })()`);
  fs.writeFileSync(opt('out'), Buffer.from(b64, 'base64'));
}
console.log(JSON.stringify(result, null, flag('caps') ? 1 : 0));
clearInterval(timer);
await browser.close();
