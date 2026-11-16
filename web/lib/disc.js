// Locates the files the Vita port needs, either inside a PS2 DVD image (.iso) or inside a
// folder / loose files of an already-extracted disc. Everything is exposed as Blobs so the
// large files are only ever read in slices.
import { listIso, fileBlob } from './iso9660.js';
import { openCvm } from './cvm.js';
import { parseAdxHeader, wavNameForStem } from './adx.js';

export const ELF_NAME = 'SLUS_216.21';
export const ELF_SHA1 = '3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb';
const CVM_NAMES = ['DATA.CVM', 'BTL.CVM', 'BGM.CVM'];

export class DiscError extends Error {
  constructor(code, message, detail) { super(message); this.code = code; this.detail = detail; }
}

/** Open a PS2 DVD image. `file` is a File/Blob. */
export async function openIsoDisc(file) {
  let entries;
  try { entries = await listIso(file); }
  catch (e) { throw new DiscError('not-iso', 'This file is not a readable PS2 DVD image (ISO9660, 2048-byte sectors).', e); }
  const blobs = new Map();
  for (const e of entries) blobs.set(e.path.toUpperCase(), { blob: fileBlob(file, e), name: e.path.slice(1).split('/').map((p) => (p.length > 1 && p.endsWith('.') ? p.slice(0, -1) : p)).join('/') }); // "ENTRIES." = file without extension
  return buildDisc('iso', file.name || 'disc.iso', blobs);
}

/**
 * Open an extracted disc. `items` = [{ path: 'Folder/LIB31/X.IRX', blob }] with browser-provided
 * relative paths (webkitRelativePath / directory handles). Paths without any directory part are
 * accepted too: every unknown file is then assumed to belong to LIB31.
 */
export async function openFolderDisc(items, label = 'folder') {
  const norm = items.map((it) => ({ parts: String(it.path || it.blob.name).replace(/\\/g, '/').split('/').filter(Boolean), blob: it.blob }));
  const elfs = norm.filter((n) => n.parts[n.parts.length - 1].toUpperCase() === ELF_NAME);
  if (!elfs.length) throw new DiscError('no-elf', `${ELF_NAME} was not found in the selected folder.`);
  elfs.sort((a, b) => a.parts.length - b.parts.length);
  const rootParts = elfs[0].parts.slice(0, -1);
  const flat = rootParts.length === 0 && norm.every((n) => n.parts.length === 1);
  const blobs = new Map();
  for (const n of norm) {
    const rel = n.parts;
    let sub;
    if (flat) {
      const up = rel[0].toUpperCase();
      sub = (up === ELF_NAME || up === 'SYSTEM.CNF' || CVM_NAMES.includes(up)) ? [rel[0]] : ['LIB31', rel[0]];
    } else {
      if (rel.length <= rootParts.length) continue;
      let ok = true;
      for (let i = 0; i < rootParts.length; i++) if (rel[i] !== rootParts[i]) { ok = false; break; }
      if (!ok) continue;
      sub = rel.slice(rootParts.length);
    }
    if (sub.length === 1) {
      const up = sub[0].toUpperCase();
      if (up === ELF_NAME || up === 'SYSTEM.CNF' || CVM_NAMES.includes(up)) blobs.set('/' + up, { blob: n.blob, name: sub[0] });
    } else if (sub[0].toUpperCase() === 'LIB31') {
      blobs.set('/' + sub.join('/').toUpperCase(), { blob: n.blob, name: ['LIB31', ...sub.slice(1)].join('/') });
    }
  }
  return buildDisc('folder', label, blobs);
}

function buildDisc(kind, label, blobs) {
  const need = (name) => {
    const b = blobs.get('/' + name);
    if (!b) throw new DiscError('missing', `${name} was not found on the disc.`, name);
    return b.blob;
  };
  const elf = need(ELF_NAME);
  const cvms = {};
  for (const n of CVM_NAMES) cvms[n.replace('.CVM', '')] = need(n);
  const systemCnf = blobs.get('/SYSTEM.CNF')?.blob || null;
  const lib31 = [];
  for (const [p, v] of blobs) {
    if (p.startsWith('/LIB31/')) lib31.push({ name: v.name, blob: v.blob });
  }
  lib31.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return { kind, label, elf, systemCnf, cvms, lib31 };
}

/** Read SLUS_216.21 and check its SHA-1. Returns { bytes, sha1 }. */
export async function verifyElf(disc) {
  const bytes = new Uint8Array(await disc.elf.arrayBuffer());
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-1', bytes));
  const sha1 = [...digest].map((b) => b.toString(16).padStart(2, '0')).join('');
  if (sha1 !== ELF_SHA1) {
    throw new DiscError('bad-elf', 'This is not the Persona 3 FES NTSC-U disc (SLUS-21621): the game executable does not match.', { sha1, expected: ELF_SHA1 });
  }
  return { bytes, sha1 };
}

/** ADX tracks of BGM.CVM, sorted by path, with exact output WAV sizes. */
export async function listBgmTracks(disc) {
  const { inner, files } = await openCvm(disc.cvms.BGM);
  const tracks = [];
  const seen = new Map();
  for (const f of files) {
    if (!f.path.toUpperCase().endsWith('.ADX')) continue;
    const stem = f.path.split('/').pop().replace(/\.ADX$/i, '');
    const name = wavNameForStem(stem);
    if (seen.has(name)) throw new DiscError('dup-bgm', `Two BGM tracks map to the same output name ${name}.`);
    seen.set(name, true);
    tracks.push({ path: f.path, stem, name, blob: fileBlob(inner, f), size: f.size });
  }
  tracks.sort((a, b) => (a.path.toLowerCase() < b.path.toLowerCase() ? -1 : 1));
  return tracks;
}

/** SFD movies of DATA.CVM (one per upper-cased stem, preferring /SFD/), sorted by stem. */
export async function listMovies(disc) {
  const { inner, files } = await openCvm(disc.cvms.DATA);
  const byStem = new Map();
  for (const f of files) {
    if (!f.path.toUpperCase().endsWith('.SFD')) continue;
    const stem = f.path.split('/').pop().replace(/\.SFD$/i, '').toUpperCase();
    const prev = byStem.get(stem);
    const preferred = f.path.toUpperCase().startsWith('/SFD/');
    if (!prev || (preferred && !prev.preferred)) byStem.set(stem, { stem, path: f.path, size: f.size, blob: fileBlob(inner, f), preferred });
  }
  return [...byStem.values()].sort((a, b) => (a.stem < b.stem ? -1 : 1));
}

export { parseAdxHeader };
