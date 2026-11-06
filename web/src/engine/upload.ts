// upload.ts — one upload, from file to stored (SPEC §12 "Uploads",
// DESIGN §8.4). Framework-free so it can be tested on its own; the React
// side (app/uploads.tsx) only renders snapshots.
//
//   Reading    fingerprint in the Web Worker (blocks + SHA-256)
//   Checking   create the session, send the manifest pages, learn `need`
//   Sending    send only the needed blocks, 3 requests in flight
//   Verifying  commit; the server re-reads and re-hashes the whole file
//   Done
//
// Failures keep the fingerprint, so Retry starts a new session at
// Checking: blocks that already arrived are on the server and are not
// sent again (that is the resume, SPEC §12).
import { ApiError, api } from '../api/client';
import type { CommitReply, FileItem } from '../api/types';
import type { Fingerprint } from './fingerprint';

export const PARALLEL_REQUESTS = 3; /* SPEC §12: up to 3 chunk requests in parallel */
export const BATCH_BODY = 4 * 1024 * 1024; /* well under the server's 8 MB cap */
export const BATCH_SPAN = 8 * 1024 * 1024; /* most file bytes read for one batch */
const MAX_COMMIT_ROUNDS = 3;

export type UploadPhase = 'queued' | 'reading' | 'checking' | 'sending' | 'verifying' | 'done' | 'failed' | 'cancelled';
export type FailureKind = 'connection' | 'hash' | 'disk' | 'session' | 'busy' | 'other';

export interface UploadSnapshot {
  key: string;
  name: string;
  size: number;
  phase: UploadPhase;
  /** Reading: bytes fingerprinted so far. */
  readBytes: number;
  chunkCount: number;
  /** Blocks the server did not have, and their bytes. */
  needCount: number;
  needBytes: number;
  /** Bytes of needed blocks the server accepted so far. */
  sentBytes: number;
  /** Bytes per second while sending, once sending has lasted over 1 s. */
  rate: number | null;
  failure: { kind: FailureKind; message: string; rejected?: number } | null;
  file: FileItem | null;
  /** Bytes that crossed the network for the stored file (Done). */
  bytesSent: number | null;
  startedAt: number;
}

/** What an upload needs from outside; tests replace these. */
export interface UploadDeps {
  fingerprint: (file: Blob, onProgress: (done: number) => void, signal: AbortSignal) => Promise<Fingerprint>;
  api: Pick<typeof api, 'uploadCreate' | 'uploadManifest' | 'uploadChunks' | 'uploadCommit' | 'uploadCancel'>;
  now: () => number;
}

// ---- pure helpers (unit-tested) ------------------------------------------------------

/** Byte offset of every block in the file. */
export function blockOffsets(lens: Uint32Array): Float64Array {
  const off = new Float64Array(lens.length);
  let pos = 0;
  for (let i = 0; i < lens.length; i++) {
    off[i] = pos;
    pos += lens[i]!;
  }
  return off;
}

/** Manifest page `page`: entries of (hash[32], u32 len little-endian). */
export function manifestPage(fp: Fingerprint, page: number, pageSize: number): Uint8Array {
  const from = page * pageSize;
  const n = Math.max(0, Math.min(pageSize, fp.lens.length - from));
  const out = new Uint8Array(n * 36);
  const view = new DataView(out.buffer);
  for (let i = 0; i < n; i++) {
    out.set(fp.hashes.subarray((from + i) * 32, (from + i) * 32 + 32), i * 36);
    view.setUint32(i * 36 + 32, fp.lens[from + i]!, true);
  }
  return out;
}

export interface Batch {
  indices: number[];
  /** File bytes to read for this batch: [start, end). */
  start: number;
  end: number;
  bodyBytes: number;
}

/**
 * Groups needed blocks (any order) into chunk requests: each body stays
 * under BATCH_BODY and each read under BATCH_SPAN of the file, so one
 * Blob read serves a whole batch. A block appearing twice in the file is
 * sent once (its first index).
 */
export function planBatches(need: number[], lens: Uint32Array, offsets: Float64Array): Batch[] {
  const sorted = [...new Set(need)].sort((a, b) => a - b);
  const out: Batch[] = [];
  let cur: Batch | null = null;
  for (const i of sorted) {
    const start = offsets[i]!;
    const len = lens[i]!;
    const body = 8 + len;
    if (cur && cur.bodyBytes + body <= BATCH_BODY && start + len - cur.start <= BATCH_SPAN) {
      cur.indices.push(i);
      cur.end = start + len;
      cur.bodyBytes += body;
    } else {
      cur = { indices: [i], start, end: start + len, bodyBytes: body };
      out.push(cur);
    }
  }
  return out;
}

/** Chunk request body: repeated (u32 index, u32 len, data), little-endian. */
export function chunksBody(batch: Batch, span: Uint8Array, lens: Uint32Array, offsets: Float64Array): Uint8Array {
  const out = new Uint8Array(batch.bodyBytes);
  const view = new DataView(out.buffer);
  let p = 0;
  for (const i of batch.indices) {
    const len = lens[i]!;
    const at = offsets[i]! - batch.start;
    view.setUint32(p, i, true);
    view.setUint32(p + 4, len, true);
    out.set(span.subarray(at, at + len), p + 8);
    p += 8 + len;
  }
  return out;
}

/** Turns an error into the tray's failure (DESIGN §8.4 copy). */
export function failureOf(e: unknown): UploadSnapshot['failure'] & object {
  if (e instanceof BlocksRejected)
    return { kind: 'hash', message: 'Blocks did not match their fingerprints.', rejected: e.count };
  if (e instanceof ApiError) {
    if (e.isNetwork || e.code === 'upload_not_found')
      return { kind: 'connection', message: 'Connection lost.' };
    if (e.status === 401) return { kind: 'session', message: 'Your session ended.' };
    if (e.status === 507) return { kind: 'disk', message: 'The server is out of space. Ask the lab admin.' };
    if (e.code === 'file_hash_mismatch')
      return { kind: 'hash', message: 'The server rebuilt the file and it did not match its fingerprint.' };
    if (e.status === 429) return { kind: 'busy', message: 'Too many uploads at once. Wait for one to finish.' };
    if (e.status === 503) return { kind: 'busy', message: 'The server is busy. Try again in a moment.' };
    return { kind: 'other', message: e.message };
  }
  return { kind: 'other', message: e instanceof Error ? e.message : 'The upload failed.' };
}

// ---- the job ------------------------------------------------------------------------

class Cancelled extends Error {}

/** The server refused blocks that did not match their fingerprints. */
export class BlocksRejected extends Error {
  constructor(readonly count: number) {
    super(`${count} blocks rejected`);
  }
}

let nextKey = 1;

export class UploadJob {
  private snap: UploadSnapshot;
  private readonly listeners = new Set<(s: UploadSnapshot) => void>();
  private fp: Fingerprint | null = null;
  private offsets: Float64Array | null = null;
  private abort = new AbortController();
  private uploadId: string | null = null;
  private sendingSince = 0;

  constructor(
    readonly file: File,
    private readonly deps: UploadDeps,
    /** versionOf for the create call: undefined = next version by name. */
    private readonly versionOf?: number | null,
  ) {
    this.snap = {
      key: `u${nextKey++}`,
      name: file.name,
      size: file.size,
      phase: 'queued',
      readBytes: 0,
      chunkCount: 0,
      needCount: 0,
      needBytes: 0,
      sentBytes: 0,
      rate: null,
      failure: null,
      file: null,
      bytesSent: null,
      startedAt: deps.now(),
    };
  }

  get snapshot(): UploadSnapshot {
    return this.snap;
  }

  subscribe(fn: (s: UploadSnapshot) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  /** The server's id for the current session (matches upload.progress). */
  get serverId(): string | null {
    return this.uploadId;
  }

  get cancellable(): boolean {
    const p = this.snap.phase;
    return p === 'queued' || p === 'reading' || p === 'checking' || p === 'sending';
  }

  private set(patch: Partial<UploadSnapshot>): void {
    this.snap = { ...this.snap, ...patch };
    this.listeners.forEach((fn) => fn(this.snap));
  }

  private check(): void {
    if (this.abort.signal.aborted) throw new Cancelled();
  }

  /** Runs to Done, Failed, or Cancelled. Never throws. */
  async run(): Promise<void> {
    if (this.snap.phase === 'cancelled') return; /* cancelled while queued */
    try {
      if (!this.fp) {
        this.set({ phase: 'reading', readBytes: 0 });
        this.fp = await this.deps.fingerprint(this.file, (done) => this.set({ readBytes: done }), this.abort.signal);
        this.offsets = blockOffsets(this.fp.lens);
        this.set({ readBytes: this.file.size, chunkCount: this.fp.lens.length });
      }
      this.check();
      await this.store(this.fp, this.offsets!);
    } catch (e) {
      if (e instanceof Cancelled || this.abort.signal.aborted) {
        this.set({ phase: 'cancelled' });
        return;
      }
      // A session that failed half-way is useless now; free it on the
      // server (blocks it stored stay, so Retry still skips them).
      this.dropSession();
      this.set({ phase: 'failed', failure: failureOf(e), rate: null });
    }
  }

  private async store(fp: Fingerprint, offsets: Float64Array): Promise<void> {
    const signal = this.abort.signal;
    this.set({ phase: 'checking', failure: null, sentBytes: 0, needCount: 0, needBytes: 0, rate: null });
    const created = await this.deps.api.uploadCreate({
      name: this.file.name,
      size: fp.size,
      sha256: fp.sha256,
      chunker: 'cdc',
      chunkCount: fp.lens.length,
      ...(this.versionOf !== undefined ? { versionOf: this.versionOf } : {}),
    });
    this.uploadId = created.uploadId;
    this.check();
    const pageSize = created.manifestPageSize;
    const need: number[] = [];
    for (let page = 0; page * pageSize < fp.lens.length; page++) {
      const r = await this.deps.api.uploadManifest(created.uploadId, page, manifestPage(fp, page, pageSize), signal);
      for (const i of r.need) need.push(page * pageSize + i);
      this.check();
    }
    let needBytes = 0;
    for (const i of new Set(need)) needBytes += fp.lens[i]!;
    this.set({ phase: 'sending', needCount: new Set(need).size, needBytes });
    this.sendingSince = this.deps.now();
    await this.send(need, fp, offsets);

    for (let round = 0; ; round++) {
      this.set({ phase: 'verifying', rate: null });
      try {
        const done: CommitReply = await this.deps.api.uploadCommit(created.uploadId, signal);
        this.uploadId = null;
        this.set({ phase: 'done', file: done, bytesSent: done.stats.bytesSent });
        return;
      } catch (e) {
        // 409 missing_chunks: blocks vanished between need and commit (or
        // were never accepted). Send them and commit again.
        const missing = e instanceof ApiError && e.code === 'missing_chunks' ? missingIndices(e.body) : null;
        if (!missing || round + 1 >= MAX_COMMIT_ROUNDS) throw e;
        this.set({ phase: 'sending' });
        await this.send(missing, fp, offsets);
      }
    }
  }

  /** Sends the given blocks, PARALLEL_REQUESTS batches at a time. */
  private async send(need: number[], fp: Fingerprint, offsets: Float64Array): Promise<void> {
    const batches = planBatches(need, fp.lens, offsets);
    let next = 0;
    let rejected = 0;
    // The first failure stops the other workers and aborts their requests,
    // so nothing changes the snapshot after the upload is reported failed.
    let failure: unknown = null;
    const local = new AbortController();
    const stopAll = () => local.abort();
    this.abort.signal.addEventListener('abort', stopAll);
    const worker = async () => {
      try {
        while (next < batches.length && rejected === 0 && failure === null) {
          const b = batches[next++]!;
          this.check();
          const span = new Uint8Array(await this.file.slice(b.start, b.end).arrayBuffer());
          if (failure !== null) return;
          this.check();
          const body = chunksBody(b, span, fp.lens, offsets);
          const r = await this.deps.api.uploadChunks(this.uploadId!, body, local.signal);
          if (failure !== null) return;
          let acc = 0;
          for (const i of r.accepted) acc += fp.lens[i] ?? 0;
          rejected += r.rejected.length;
          const sent = this.snap.sentBytes + acc;
          const elapsed = this.deps.now() - this.sendingSince;
          this.set({ sentBytes: sent, rate: elapsed > 1000 ? (sent * 1000) / elapsed : null });
        }
      } catch (e) {
        if (failure === null) failure = e;
        local.abort();
      }
    };
    await Promise.all(Array.from({ length: Math.min(PARALLEL_REQUESTS, batches.length) }, worker));
    this.abort.signal.removeEventListener('abort', stopAll);
    if (failure !== null) throw failure;
    // The file no longer matches what was fingerprinted (changed on disk),
    // or bytes were damaged on the way.
    if (rejected > 0) throw new BlocksRejected(rejected);
  }

  private dropSession(): void {
    const id = this.uploadId;
    this.uploadId = null;
    if (id) void this.deps.api.uploadCancel(id).catch(() => undefined);
  }

  /** Stops the upload (until Verifying) and frees the server session. */
  cancel(): void {
    if (!this.cancellable) return;
    this.abort.abort();
    this.dropSession();
    this.set({ phase: 'cancelled', rate: null });
  }

  /** After a failure: a new session; the fingerprint is reused. */
  retry(): Promise<void> {
    if (this.snap.phase !== 'failed') return Promise.resolve();
    this.abort = new AbortController();
    return this.run();
  }
}

function missingIndices(body: unknown): number[] | null {
  const ix = (body as { error?: { indices?: unknown } } | null)?.error?.indices;
  return Array.isArray(ix) && ix.every((i) => Number.isInteger(i)) ? (ix as number[]) : null;
}
