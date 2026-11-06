// The browser upload engine (SPEC §12, DESIGN §8.4): fingerprinting in
// windows, the manifest and chunk wire formats, batching, and the job's
// phases and failures against a fake server that follows the protocol.
import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { ApiError } from '../api/client';
import type { CommitReply } from '../api/types';
import { Chunker } from './chunker';
import { fingerprint, type Fingerprint } from './fingerprint';
import {
  BATCH_BODY,
  BATCH_SPAN,
  blockOffsets,
  chunksBody,
  manifestPage,
  planBatches,
  UploadJob,
  type UploadDeps,
  type UploadSnapshot,
} from './upload';

/** Deterministic bytes that look like a CSV (so CDC finds cut points). */
function csv(len: number, seed = 1): Uint8Array<ArrayBuffer> {
  let x = seed >>> 0;
  const rnd = () => ((x = (Math.imul(x, 1664525) + 1013904223) >>> 0), x);
  const out = new Uint8Array(len);
  let p = 0;
  while (p < len) {
    const row = new TextEncoder().encode(`${rnd() % 100000},${rnd() % 977},lab-${rnd() % 50}\n`);
    out.set(row.subarray(0, Math.min(row.length, len - p)), p);
    p += row.length;
  }
  return out;
}

const sha = (b: Uint8Array) => createHash('sha256').update(b).digest();

describe('fingerprint', () => {
  it('matches the chunker and SHA-256, whatever the read window', async () => {
    const data = csv(3 * 1024 * 1024 + 777);
    // Reference: whole-buffer chunking, Node's hash.
    const ref: { hash: Buffer; len: number }[] = [];
    const c = new Chunker('cdc');
    c.push(data, (b) => ref.push({ hash: sha(b), len: b.length }));
    c.finish((b) => ref.push({ hash: sha(b), len: b.length }));
    for (const win of [1 << 20, 8 << 20, 65_537]) {
      const seen: number[] = [];
      const fp = await fingerprint(new Blob([data]), 'cdc', (d) => seen.push(d), win);
      expect(fp.sha256).toBe(sha(data).toString('hex'));
      expect(fp.size).toBe(data.length);
      expect(fp.lens.length).toBe(ref.length);
      ref.forEach((r, i) => {
        expect(fp.lens[i]).toBe(r.len);
        expect(Buffer.from(fp.hashes.subarray(i * 32, i * 32 + 32)).equals(r.hash)).toBe(true);
      });
      expect(seen.at(-1)).toBe(data.length);
    }
  });

  it('handles an empty file', async () => {
    const fp = await fingerprint(new Blob([]));
    expect(fp.lens.length).toBe(0);
    expect(fp.sha256).toBe(sha(new Uint8Array(0)).toString('hex'));
  });
});

describe('wire formats', () => {
  const fp: Fingerprint = {
    hashes: Uint8Array.from({ length: 3 * 32 }, (_, i) => i),
    lens: Uint32Array.from([4096, 70000, 5]),
    sha256: '',
    size: 74101,
  };

  it('writes manifest pages as (hash[32], u32 len LE)', () => {
    const p0 = manifestPage(fp, 0, 2);
    expect(p0.length).toBe(72);
    expect(Array.from(p0.subarray(0, 32))).toEqual(Array.from(fp.hashes.subarray(0, 32)));
    expect(Array.from(p0.subarray(32, 36))).toEqual([0x00, 0x10, 0, 0]);
    expect(new DataView(p0.buffer).getUint32(68, true)).toBe(70000);
    const p1 = manifestPage(fp, 1, 2);
    expect(p1.length).toBe(36);
    expect(new DataView(p1.buffer).getUint32(32, true)).toBe(5);
    expect(Array.from(p1.subarray(0, 32))).toEqual(Array.from(fp.hashes.subarray(64, 96)));
  });

  it('writes chunk bodies as (u32 index, u32 len, data) LE', () => {
    const lens = Uint32Array.from([3, 2, 4]);
    const off = blockOffsets(lens);
    const file = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9]);
    const [b] = planBatches([2, 0], lens, off);
    expect(b).toEqual({ indices: [0, 2], start: 0, end: 9, bodyBytes: 8 + 3 + 8 + 4 });
    const body = chunksBody(b!, file.subarray(b!.start, b!.end), lens, off);
    const v = new DataView(body.buffer);
    expect([v.getUint32(0, true), v.getUint32(4, true)]).toEqual([0, 3]);
    expect(Array.from(body.subarray(8, 11))).toEqual([1, 2, 3]);
    expect([v.getUint32(11, true), v.getUint32(15, true)]).toEqual([2, 4]);
    expect(Array.from(body.subarray(19))).toEqual([6, 7, 8, 9]);
  });

  it('batches within the body and read-span limits, each block once', () => {
    const lens = new Uint32Array(5000).fill(4096);
    const off = blockOffsets(lens);
    const need = [...Array.from({ length: 5000 }, (_, i) => 4999 - i), 7, 7];
    const batches = planBatches(need, lens, off);
    const all = batches.flatMap((b) => b.indices);
    expect(all).toEqual(Array.from({ length: 5000 }, (_, i) => i));
    for (const b of batches) {
      expect(b.bodyBytes).toBeLessThanOrEqual(BATCH_BODY);
      expect(b.end - b.start).toBeLessThanOrEqual(BATCH_SPAN);
    }
    // Sparse needs: far-apart blocks do not share one huge read.
    const sparse = planBatches([0, 4000], lens, off);
    expect(sparse.length).toBe(2);
  });
});

// ---- the job against a fake server --------------------------------------------------

interface FakeOpts {
  /** Hashes the server already has (hex). */
  have?: Set<string>;
  /** Make the n-th chunks request fail with this error. */
  failChunksAt?: { n: number; err: Error };
  rejectAll?: boolean;
  commitError?: ApiError;
  missingOnce?: boolean;
}

function fakeServer(fp: Fingerprint, opts: FakeOpts = {}) {
  const have = opts.have ?? new Set<string>();
  const log: string[] = [];
  let sessions = 0;
  let chunkCalls = 0;
  let missingDone = false;
  const hex = (i: number) => Buffer.from(fp.hashes.subarray(i * 32, i * 32 + 32)).toString('hex');
  const api: UploadDeps['api'] = {
    uploadCreate: async () => {
      sessions++;
      log.push('create');
      return { uploadId: `s${sessions}`, manifestPageSize: 4, maxChunkBody: 8 << 20 };
    },
    uploadManifest: async (_id, page, body) => {
      log.push(`manifest ${page}`);
      const need: number[] = [];
      for (let k = 0; k < body.length / 36; k++) if (!have.has(hex(page * 4 + k))) need.push(k);
      return { need };
    },
    uploadChunks: async (_id, body) => {
      chunkCalls++;
      if (opts.failChunksAt && chunkCalls === opts.failChunksAt.n) throw opts.failChunksAt.err;
      const v = new DataView(body.buffer, body.byteOffset, body.byteLength);
      const accepted: number[] = [];
      const rejected: { index: number; reason: 'hash_mismatch' }[] = [];
      for (let p = 0; p < body.length; ) {
        const i = v.getUint32(p, true);
        const len = v.getUint32(p + 4, true);
        const ok = !opts.rejectAll && sha(body.subarray(p + 8, p + 8 + len)).toString('hex') === hex(i);
        if (ok) {
          have.add(hex(i));
          accepted.push(i);
        } else rejected.push({ index: i, reason: 'hash_mismatch' });
        p += 8 + len;
      }
      log.push(`chunks ${accepted.length}`);
      return { accepted, rejected };
    },
    uploadCommit: async () => {
      log.push('commit');
      if (opts.commitError) throw opts.commitError;
      if (opts.missingOnce && !missingDone) {
        missingDone = true;
        have.delete(hex(0));
        throw new ApiError(409, 'missing_chunks', 'Some chunks are not on the server yet.', {
          error: { code: 'missing_chunks', message: '', indices: [0] },
        });
      }
      return { id: 9, stats: { bytesSent: 123, chunks: fp.lens.length, newChunks: 1, reusedPercent: 0 } } as unknown as CommitReply;
    },
    uploadCancel: async (id) => {
      log.push(`cancel ${id}`);
    },
  };
  return { api, log, have, hex };
}

async function setup(len = 200_000) {
  const data = csv(len, 7);
  const fp = await fingerprint(new Blob([data]));
  const file = new File([data], 'notes.csv');
  return { data, fp, file };
}

function deps(fp: Fingerprint, api: UploadDeps['api']): UploadDeps {
  return { fingerprint: async (_f, onProgress) => (onProgress(fp.size), fp), api, now: () => Date.now() };
}

describe('UploadJob', () => {
  it('goes Reading → Checking → Sending → Verifying → Done, sending only what is needed', async () => {
    const { fp, file } = await setup();
    const srv = fakeServer(fp);
    srv.have.add(srv.hex(1)); // the server already has block 1
    const job = new UploadJob(file, deps(fp, srv.api));
    const phases: string[] = [];
    job.subscribe((s) => phases.at(-1) !== s.phase && phases.push(s.phase));
    await job.run();
    expect(phases).toEqual(['reading', 'checking', 'sending', 'verifying', 'done']);
    const s = job.snapshot;
    expect(s.chunkCount).toBe(fp.lens.length);
    expect(s.needCount).toBe(fp.lens.length - 1);
    expect(s.needBytes).toBe(fp.size - fp.lens[1]!);
    expect(s.sentBytes).toBe(s.needBytes);
    expect(s.bytesSent).toBe(123);
    expect(srv.log.filter((l) => l.startsWith('manifest')).length).toBe(Math.ceil(fp.lens.length / 4));
  });

  it('loses the connection, then Retry resumes without resending stored blocks', async () => {
    const { fp, file } = await setup(30 << 20); // several batches
    const srv = fakeServer(fp, { failChunksAt: { n: 3, err: new ApiError(0, 'network', 'x') } });
    const job = new UploadJob(file, deps(fp, srv.api));
    await job.run();
    expect(job.snapshot.phase).toBe('failed');
    expect(job.snapshot.failure?.kind).toBe('connection');
    expect(srv.log).toContain('cancel s1'); // the broken session is freed
    const storedBefore = srv.have.size;
    expect(storedBefore).toBeGreaterThan(0);
    await job.retry();
    expect(job.snapshot.phase).toBe('done');
    expect(job.snapshot.needCount).toBe(fp.lens.length - storedBefore);
  });

  it('reports blocks the server rejected, with the count', async () => {
    const { fp, file } = await setup();
    const srv = fakeServer(fp, { rejectAll: true });
    const job = new UploadJob(file, deps(fp, srv.api));
    await job.run();
    expect(job.snapshot.failure).toMatchObject({ kind: 'hash', rejected: fp.lens.length });
    expect(srv.log).not.toContain('commit');
  });

  it('sends the blocks a 409 names, then commits again', async () => {
    const { fp, file } = await setup();
    const srv = fakeServer(fp, { missingOnce: true });
    const job = new UploadJob(file, deps(fp, srv.api));
    await job.run();
    expect(job.snapshot.phase).toBe('done');
    expect(srv.log.filter((l) => l === 'commit').length).toBe(2);
    expect(srv.log.at(-2)).toBe('chunks 1');
  });

  it.each([
    [new ApiError(507, 'disk_full', ''), 'disk'],
    [new ApiError(401, 'unauthorized', ''), 'session'],
    [new ApiError(422, 'file_hash_mismatch', ''), 'hash'],
    [new ApiError(429, 'too_many_uploads', ''), 'busy'],
  ])('maps %s to a %s failure', async (err, kind) => {
    const { fp, file } = await setup(20_000);
    const job = new UploadJob(file, deps(fp, fakeServer(fp, { commitError: err }).api));
    await job.run();
    expect(job.snapshot.failure?.kind).toBe(kind);
  });

  it('cancels while sending and frees the session', async () => {
    const { fp, file } = await setup(30 << 20);
    const srv = fakeServer(fp);
    const job = new UploadJob(file, deps(fp, srv.api));
    let cancelled = false;
    job.subscribe((s: UploadSnapshot) => {
      if (s.phase === 'sending' && s.sentBytes > 0 && !cancelled) {
        cancelled = true;
        job.cancel();
      }
    });
    await job.run();
    expect(job.snapshot.phase).toBe('cancelled');
    expect(srv.log).toContain('cancel s1');
    expect(srv.log).not.toContain('commit');
    expect(job.cancellable).toBe(false);
  });
});
