// CVM archives: a CRI ROFS header followed by an ISO9660 image. Port of open_cvm() from
// p3fes/scripts/native_assets/p3fes_unpack.py.
import { SECTOR, hasPvd, listIso, fileBlob, IsoError } from './iso9660.js';

const HEADER_SCAN_LIMIT = 1024 * 1024;

/**
 * Open a CVM (Blob/File). Returns { inner, files } where `inner` is the Blob starting at the
 * ISO image inside the CVM, and files = [{ path, lba, size, offset }] relative to `inner`.
 */
export async function openCvm(blob) {
  const limit = Math.min(blob.size, HEADER_SCAN_LIMIT);
  let base = -1;
  for (let off = 16 * SECTOR; off + 6 <= limit; off += SECTOR) {
    if (await hasPvd(blob, off / SECTOR)) { base = off - 16 * SECTOR; break; }
  }
  if (base < 0) throw new IsoError('Could not find an ISO9660 file system inside this CVM.');
  const inner = blob.slice(base);
  return { inner, files: await listIso(inner) };
}

export { fileBlob };
