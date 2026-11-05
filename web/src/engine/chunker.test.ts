// Parity: the TypeScript chunker + hash-wasm must reproduce the C
// chunker's cut points and block hashes for every fixture (SPEC §6.4).
// The reference is server/tests/fixtures/chunks_expected.txt (made by the
// C tool chunkdump). Any mismatch fails the build.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createSHA256, type IHasher } from 'hash-wasm';
import { beforeAll, describe, expect, it } from 'vitest';
import { cdcCut, CDC_MAX, CDC_MIN, Chunker, type ChunkerKind } from './chunker';
import { FIXTURE_DIR } from '../../test/fixturePaths';

interface Expected {
  size: number;
  sha256: string;
  cdc: { count: number; digest: string; lens: number[] };
  fixed: { count: number; digest: string };
}

function parseExpected(): Map<string, Expected> {
  const text = readFileSync(join(FIXTURE_DIR, 'chunks_expected.txt'), 'utf8');
  const map = new Map<string, Expected>();
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (line === '' || line.startsWith('#')) continue;
    const [kind, name, a, digest, lens] = line.split(' ');
    if (!kind || !name || a === undefined || !digest) throw new Error(`bad line: ${line}`);
    const e = map.get(name) ?? {
      size: 0,
      sha256: '',
      cdc: { count: 0, digest: '', lens: [] },
      fixed: { count: 0, digest: '' },
    };
    if (kind === 'file') {
      e.size = Number(a);
      e.sha256 = digest;
    } else if (kind === 'cdc') {
      e.cdc = {
        count: Number(a),
        digest,
        lens: !lens || lens === '-' ? [] : lens.split(',').map(Number),
      };
    } else if (kind === 'fixed') {
      e.fixed = { count: Number(a), digest };
    }
    map.set(name, e);
  }
  return map;
}


let blockHasher: IHasher;
let digestHasher: IHasher;
let fileHasher: IHasher;

/** Chunks `data` in windows of `window` bytes, like Blob.slice reads. */
function chunkAll(kind: ChunkerKind, data: Uint8Array, window: number) {
  const lens: number[] = [];
  digestHasher.init();
  const chunker = new Chunker(kind);
  const onBlock = (block: Uint8Array): void => {
    lens.push(block.length);
    blockHasher.init();
    blockHasher.update(block);
    digestHasher.update(blockHasher.digest('binary'));
  };
  for (let off = 0; off < data.length; off += window) {
    chunker.push(data.subarray(off, Math.min(off + window, data.length)), onBlock);
  }
  chunker.finish(onBlock);
  return { lens, digest: digestHasher.digest('hex') };
}

const expected = parseExpected();

beforeAll(async () => {
  blockHasher = await createSHA256();
  digestHasher = await createSHA256();
  fileHasher = await createSHA256();
});

describe('chunker parity with the C implementation', () => {
  it('has all 10 fixtures in the reference file', () => {
    expect(expected.size).toBe(10);
  });

  for (const [name, exp] of expected) {
    it(`${name}: file hash, CDC and fixed blocks match`, () => {
      const data = new Uint8Array(readFileSync(join(FIXTURE_DIR, 'data', name)));
      expect(data.length).toBe(exp.size);
      fileHasher.init();
      fileHasher.update(data);
      expect(fileHasher.digest('hex')).toBe(exp.sha256);

      // 8 MB windows (SPEC §6.3), and an odd window size to stress joins.
      for (const window of [8 * 1024 * 1024, 1_000_003]) {
        const cdc = chunkAll('cdc', data, window);
        expect(cdc.lens.length).toBe(exp.cdc.count);
        // Compare lengths first: the failure message then points at a block.
        expect(cdc.lens).toEqual(exp.cdc.lens);
        expect(cdc.digest).toBe(exp.cdc.digest);

        const fixed = chunkAll('fixed', data, window);
        expect(fixed.lens.length).toBe(exp.fixed.count);
        expect(fixed.digest).toBe(exp.fixed.digest);
      }
    });
  }
});

describe('cdcCut bounds', () => {
  it('stays within MIN..MAX for every length', () => {
    const buf = new Uint8Array(3 * CDC_MAX);
    let x = 12345;
    for (let i = 0; i < buf.length; i++) {
      x = (Math.imul(x, 1103515245) + 12345) >>> 0;
      buf[i] = x >>> 24;
    }
    for (let n = 0; n <= buf.length; n += n < 5000 ? 1 : 97) {
      const c = cdcCut(buf, 0, n);
      if (n <= CDC_MIN) expect(c).toBe(n);
      else {
        expect(c).toBeGreaterThan(CDC_MIN);
        expect(c).toBeLessThanOrEqual(Math.min(CDC_MAX, n));
      }
    }
  });
});

