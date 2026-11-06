// fingerprint.ts — cut a file into blocks and hash them (SPEC §6.3, DESIGN
// §8.4 "Reading"). Runs inside the upload Web Worker so a 1 GB file never
// freezes the page, and in vitest for the parity checks.
//
// The file is read in 8 MB windows (Blob.slice), so memory stays flat
// whatever the file size. Every window goes through the streaming chunker,
// which gives exactly the blocks the C code would cut (chunker.test.ts).
import { createSHA256 } from 'hash-wasm';
import { Chunker, type ChunkerKind } from './chunker';

export const READ_WINDOW = 8 * 1024 * 1024;

export interface Fingerprint {
  /** Block hashes, 32 bytes each, in file order. */
  hashes: Uint8Array;
  /** Block lengths, in file order. */
  lens: Uint32Array;
  /** Whole-file SHA-256, lower-case hex. */
  sha256: string;
  size: number;
}

/** Growable typed arrays: block counts are not known in advance. */
class Grow {
  hashes = new Uint8Array(32 * 1024);
  lens = new Uint32Array(1024);
  n = 0;

  push(hash: Uint8Array, len: number): void {
    if (this.n === this.lens.length) {
      const h = new Uint8Array(this.hashes.length * 2);
      h.set(this.hashes);
      this.hashes = h;
      const l = new Uint32Array(this.lens.length * 2);
      l.set(this.lens);
      this.lens = l;
    }
    this.hashes.set(hash, this.n * 32);
    this.lens[this.n] = len;
    this.n++;
  }
}

export async function fingerprint(
  file: Blob,
  kind: ChunkerKind = 'cdc',
  onProgress?: (done: number, total: number) => void,
  window = READ_WINDOW,
): Promise<Fingerprint> {
  const whole = await createSHA256();
  const block = await createSHA256();
  whole.init();
  const chunker = new Chunker(kind);
  const out = new Grow();
  const onBlock = (b: Uint8Array) => {
    block.init();
    block.update(b);
    out.push(block.digest('binary'), b.length);
  };
  for (let off = 0; off < file.size; off += window) {
    const data = new Uint8Array(await file.slice(off, Math.min(file.size, off + window)).arrayBuffer());
    whole.update(data);
    chunker.push(data, onBlock);
    onProgress?.(Math.min(file.size, off + window), file.size);
  }
  chunker.finish(onBlock);
  onProgress?.(file.size, file.size);
  return {
    hashes: out.hashes.slice(0, out.n * 32),
    lens: out.lens.slice(0, out.n),
    sha256: whole.digest('hex'),
    size: file.size,
  };
}
