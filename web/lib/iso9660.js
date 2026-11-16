// Minimal ISO9660 reader over a Blob/File (random access via slice, never loads the whole image).
//
// Handles the plain 2048-byte-sector images used by PS2 DVDs: uppercase names with ";1"
// suffixes, directory records spanning sectors with zero padding at sector ends. Joliet and
// Rock Ridge are ignored on purpose (the PS2 uses the primary volume descriptor).

export const SECTOR = 2048;

export class IsoError extends Error {}

async function readAt(blob, pos, len) {
  if (pos < 0 || pos >= blob.size || len <= 0) return new Uint8Array(0);
  const end = Math.min(blob.size, pos + len);
  return new Uint8Array(await blob.slice(pos, end).arrayBuffer());
}

export async function hasPvd(blob, sectorIndex) {
  const b = await readAt(blob, sectorIndex * SECTOR, 6);
  return b.length === 6 && b[0] === 1 && b[1] === 0x43 && b[2] === 0x44 && b[3] === 0x30 && b[4] === 0x30 && b[5] === 0x31;
}

/**
 * List every file of the ISO9660 volume in `blob`.
 * Returns [{ path: '/DIR/NAME.EXT', lba, size, offset }] sorted by lba; `offset` = lba*2048
 * relative to the start of `blob`. Version suffixes (";1") are stripped.
 */
export async function listIso(blob) {
  const pvd = await readAt(blob, 16 * SECTOR, SECTOR);
  if (pvd.length < 190 || pvd[0] !== 1 || String.fromCharCode(...pvd.subarray(1, 6)) !== 'CD001') {
    throw new IsoError('No ISO9660 volume descriptor found (is this a plain 2048-byte-sector .iso?).');
  }
  const dv = new DataView(pvd.buffer, pvd.byteOffset, pvd.byteLength);
  const rootLba = dv.getUint32(156 + 2, true);
  const rootSize = dv.getUint32(156 + 10, true);
  const out = [];
  await walk(blob, rootLba, rootSize, '', new Set(), out, 0);
  out.sort((a, b) => a.lba - b.lba);
  return out;
}

async function walk(blob, lba, size, path, visited, out, depth) {
  const key = lba + ':' + size;
  if (visited.has(key) || size <= 0 || size > 64 * 1024 * 1024 || depth > 32) return;
  visited.add(key);
  const data = await readAt(blob, lba * SECTOR, size);
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos = 0;
  while (pos < data.length) {
    const n = data[pos];
    if (n === 0) { pos = (Math.floor(pos / SECTOR) + 1) * SECTOR; continue; }
    if (n < 34 || pos + n > data.length) break;
    const extent = dv.getUint32(pos + 2, true);
    const length = dv.getUint32(pos + 10, true);
    const flags = data[pos + 25];
    const nameLen = data[pos + 32];
    const nameBytes = data.subarray(pos + 33, pos + 33 + nameLen);
    pos += n;
    if (nameLen === 1 && (nameBytes[0] === 0 || nameBytes[0] === 1)) continue; // "." and ".."
    let name = '';
    for (const c of nameBytes) name += String.fromCharCode(c);
    const semi = name.indexOf(';');
    if (semi >= 0) name = name.slice(0, semi);
    const full = path + '/' + name;
    if (flags & 2) await walk(blob, extent, length, full, visited, out, depth + 1);
    else out.push({ path: full, lba: extent, size: length, offset: extent * SECTOR });
  }
}

/** Blob view of one listed file. */
export function fileBlob(blob, entry) {
  return blob.slice(entry.offset, entry.offset + entry.size);
}
