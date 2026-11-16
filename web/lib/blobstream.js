/** ReadableStream over a Blob, read in large slices (never loads the whole blob). */
export function blobStream(blob, { chunk = 8 * 1024 * 1024, onBytes, signal } = {}) {
  let pos = 0;
  return new ReadableStream({
    async pull(controller) {
      signal?.throwIfAborted();
      if (pos >= blob.size) { controller.close(); return; }
      const end = Math.min(blob.size, pos + chunk);
      const buf = new Uint8Array(await blob.slice(pos, end).arrayBuffer());
      pos = end;
      onBytes?.(buf.length);
      controller.enqueue(buf);
    },
  });
}
