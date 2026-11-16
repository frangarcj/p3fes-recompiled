// CVM listings must match the Python tools (p3fes_unpack.open_cvm, cvm_ls.py, prepare_*.py --list).
import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { gameMac, nativeAssets, haveGame, has, py, fileBlob, repoRoot } from './helpers.mjs';
import { openCvm } from '../lib/cvm.js';

const skip = !haveGame || !has('python3') ? 'needs game-mac/ and python3' : false;

const LIST_PY = `
import sys, os
sys.path.insert(0, ${JSON.stringify(nativeAssets)})
import p3fes_unpack as u
f = open(sys.argv[1], 'rb')
inner, entries = u.open_cvm(f, {"lba": 0, "size": os.fstat(f.fileno()).st_size})
for e in entries:
    print(f"{e['path']}\\t{e['lba']}\\t{e['size']}")
`;

for (const name of ['BGM', 'BTL', 'DATA']) {
  test(`${name}.CVM listing matches p3fes_unpack.py`, { skip }, async (t) => {
    const file = path.join(gameMac, name + '.CVM');
    const ref = py(LIST_PY, [file]).trim().split('\n').map((l) => l.split('\t')).map(([p, lba, size]) => [p, +lba, +size]);
    const { files } = await openCvm(await fileBlob(file));
    const got = files.map((e) => [e.path, e.lba, e.size]);
    assert.deepEqual(got, ref);
    t.diagnostic(`${name}.CVM: ${got.length} files`);

    // cvm_ls.py prints "files N" for the same image
    const r = spawnSync('python3', [path.join(repoRoot, 'p3fes/scripts/cvm_ls.py'), file, '0'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    const m = r.stdout.match(/files (\d+)/);
    assert.ok(m, r.stdout);
    assert.equal(+m[1], got.length);
  });
}

test('prepare_bgm.py / prepare_movies.py --list agree', { skip: skip || (!has('python3') ? 'no python' : false) }, async () => {
  const bgm = spawnSync('python3', [path.join(nativeAssets, 'prepare_bgm.py'), path.join(gameMac, 'BGM.CVM'), '--list'], { encoding: 'utf8' });
  const mov = spawnSync('python3', [path.join(nativeAssets, 'prepare_movies.py'), path.join(gameMac, 'DATA.CVM'), '--list'], { encoding: 'utf8' });
  const parse = (out) => out.trim().split('\n').filter((l) => l.includes('\t')).map((l) => { const [p, s] = l.split('\t'); return [p, parseInt(s, 10)]; });
  const bgmRef = parse(bgm.stdout);
  const movRef = parse(mov.stdout);
  const b = (await openCvm(await fileBlob(path.join(gameMac, 'BGM.CVM')))).files.filter((e) => e.path.toUpperCase().endsWith('.ADX')).map((e) => [e.path, e.size]);
  const d = (await openCvm(await fileBlob(path.join(gameMac, 'DATA.CVM')))).files.filter((e) => e.path.toUpperCase().endsWith('.SFD')).map((e) => [e.path, e.size]);
  assert.deepEqual(b, bgmRef);
  assert.deepEqual(d, movRef);
  assert.equal(b.length, 223);
  assert.equal(d.length, 42);
});
