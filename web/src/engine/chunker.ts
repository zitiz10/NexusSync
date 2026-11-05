// chunker.ts — the browser copy of the server's chunkers (SPEC §6).
//
// This must cut every file at exactly the same byte offsets as
// server/src/engine/chunk_cdc.c and chunker.c. Otherwise the browser
// would fingerprint different blocks than the server stores and nothing
// would deduplicate. src/engine/chunker.test.ts checks parity against
// server/tests/fixtures/chunks_expected.txt, produced by the C code.
//
// All Gear-hash arithmetic is unsigned 32-bit: `>>> 0` after every step.

import { GEAR } from './gearTable';

export const FIXED_SIZE = 4096;
export const CDC_MIN = 2048;
export const CDC_AVG = 4096;
export const CDC_MAX = 16384;
export const CDC_MASK_S = 0xfffc0000; // 14 bits: stricter, before AVG
export const CDC_MASK_L = 0xffc00000; // 10 bits: looser, after AVG
export const CHUNK_MAX_SIZE = CDC_MAX;

export type ChunkerKind = 'cdc' | 'fixed';

/** Length of the first content-defined block of buf[start, end). */
export function cdcCut(buf: Uint8Array, start: number, end: number): number {
  const n = end - start;
  if (n <= CDC_MIN) return n;
  const normal = n < CDC_AVG ? n : CDC_AVG;
  const limit = n < CDC_MAX ? n : CDC_MAX;
  let h = 0;
  let i = CDC_MIN;
  for (; i < normal; i++) {
    h = ((h << 1) + (GEAR[buf[start + i]!] as number)) >>> 0;
    if ((h & CDC_MASK_S) === 0) return i + 1;
  }
  for (; i < limit; i++) {
    h = ((h << 1) + (GEAR[buf[start + i]!] as number)) >>> 0;
    if ((h & CDC_MASK_L) === 0) return i + 1;
  }
  return limit;
}

/** Length of the first fixed-size block of buf[start, end). */
export function fixedCut(_buf: Uint8Array, start: number, end: number): number {
  const n = end - start;
  return n < FIXED_SIZE ? n : FIXED_SIZE;
}

/**
 * Streaming chunker. Feed the file in windows of any size (the upload
 * worker reads 8 MB Blob slices) and get the same blocks as cutting the
 * whole file at once: a cut never looks more than CHUNK_MAX_SIZE bytes
 * ahead, and we only cut with a full buffer until the input ends.
 *
 * Blocks passed to `onBlock` are views into an internal buffer that is
 * reused. Hash or copy them before returning.
 */
export class Chunker {
  private readonly cut: (buf: Uint8Array, start: number, end: number) => number;
  private readonly buf = new Uint8Array(CHUNK_MAX_SIZE);
  private len = 0;

  constructor(kind: ChunkerKind) {
    this.cut = kind === 'fixed' ? fixedCut : cdcCut;
  }

  push(data: Uint8Array, onBlock: (block: Uint8Array) => void): void {
    let off = 0;
    while (off < data.length) {
      const take = Math.min(this.buf.length - this.len, data.length - off);
      this.buf.set(data.subarray(off, off + take), this.len);
      this.len += take;
      off += take;
      while (this.len === this.buf.length) this.emitOne(onBlock);
    }
  }

  finish(onBlock: (block: Uint8Array) => void): void {
    while (this.len > 0) this.emitOne(onBlock);
  }

  private emitOne(onBlock: (block: Uint8Array) => void): void {
    const n = this.cut(this.buf, 0, this.len);
    onBlock(this.buf.subarray(0, n));
    this.buf.copyWithin(0, n, this.len);
    this.len -= n;
  }
}
