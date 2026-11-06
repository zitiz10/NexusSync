// The upload Web Worker (SPEC §2, DESIGN §8.4): fingerprints one file off
// the main thread and posts progress, then the result. The result buffers
// are transferred, not copied.
/// <reference lib="webworker" />
import { fingerprint } from './fingerprint';

export type WorkerRequest = { file: Blob };
export type WorkerReply =
  | { type: 'progress'; done: number; total: number }
  | { type: 'done'; hashes: Uint8Array; lens: Uint32Array; sha256: string; size: number }
  | { type: 'error'; message: string };

declare const self: DedicatedWorkerGlobalScope;

self.onmessage = async (ev: MessageEvent<WorkerRequest>) => {
  const post = (m: WorkerReply, transfer: Transferable[] = []) => self.postMessage(m, transfer);
  try {
    let last = 0;
    const r = await fingerprint(ev.data.file, 'cdc', (done, total) => {
      const now = Date.now();
      if (now - last > 100 || done === total) {
        last = now;
        post({ type: 'progress', done, total });
      }
    });
    post({ type: 'done', ...r }, [r.hashes.buffer, r.lens.buffer]);
  } catch (e) {
    post({ type: 'error', message: e instanceof Error ? e.message : String(e) });
  }
};
