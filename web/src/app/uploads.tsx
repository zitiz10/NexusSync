// Uploads in this tab (DESIGN §8.4): the jobs, the tray's open state, the
// hidden file picker, and drag-and-drop anywhere in the app (DESIGN §7).
//
// - Two uploads run at a time; more wait their turn ("Waiting to start").
// - Fingerprinting runs in a Web Worker, one per upload, ended when the
//   upload is cancelled.
// - Jobs belong to the user who started them. If someone else signs in on
//   this PC, those jobs are cancelled and dropped, so a retry can never
//   send one person's file into another person's account. The same user
//   signing in again (session ended) keeps them, with Retry (DESIGN §8.4).
// - `finished` bumps when an upload is stored, so screens refetch.
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';
import { api } from '../api/client';
import type { Fingerprint } from '../engine/fingerprint';
import type { WorkerReply } from '../engine/fingerprint.worker';
import { UploadJob, type UploadDeps, type UploadSnapshot } from '../engine/upload';
import { useAuth } from './auth';

const RUN_AT_ONCE = 2;

function workerFingerprint(file: Blob, onProgress: (done: number) => void, signal: AbortSignal): Promise<Fingerprint> {
  return new Promise((resolve, reject) => {
    const w = new Worker(new URL('../engine/fingerprint.worker.ts', import.meta.url), { type: 'module' });
    const stop = () => {
      w.terminate();
      reject(new DOMException('Upload cancelled', 'AbortError'));
    };
    if (signal.aborted) return stop();
    signal.addEventListener('abort', stop, { once: true });
    w.onmessage = (ev: MessageEvent<WorkerReply>) => {
      const m = ev.data;
      if (m.type === 'progress') return onProgress(m.done);
      signal.removeEventListener('abort', stop);
      w.terminate();
      if (m.type === 'done') resolve({ hashes: m.hashes, lens: m.lens, sha256: m.sha256, size: m.size });
      else reject(new Error(`Could not read the file: ${m.message}`));
    };
    w.onerror = (e) => {
      signal.removeEventListener('abort', stop);
      w.terminate();
      reject(new Error(`Could not read the file: ${e.message}`));
    };
    w.postMessage({ file });
  });
}

const DEPS: UploadDeps = { fingerprint: workerFingerprint, api, now: () => Date.now() };

interface Entry {
  job: UploadJob;
  userId: number;
}

interface UploadsValue {
  /** This user's uploads, newest first. */
  uploads: UploadSnapshot[];
  /** Server upload ids of running uploads (to skip their own echo events). */
  serverIds: () => Set<string>;
  start: (files: Iterable<File>) => void;
  cancel: (key: string) => void;
  retry: (key: string) => void;
  dismiss: (key: string) => void;
  /** Opens the system file picker. */
  pick: () => void;
  trayOpen: boolean;
  setTrayOpen: (open: boolean) => void;
  /** Bumps whenever an upload is stored. */
  finished: number;
}

const UploadsContext = createContext<UploadsValue | null>(null);

export function UploadsProvider({ children }: { children: ReactNode }) {
  const { auth } = useAuth();
  const userId = auth.status === 'signed-in' ? auth.user.id : null;
  const entries = useRef<Entry[]>([]);
  const [snaps, setSnaps] = useState<Map<string, UploadSnapshot>>(new Map());
  const [trayOpen, setTrayOpen] = useState(false);
  const [finished, setFinished] = useState(0);
  const input = useRef<HTMLInputElement>(null);

  const publish = useCallback((s: UploadSnapshot) => {
    setSnaps((m) => new Map(m).set(s.key, s));
  }, []);

  // Start queued jobs while fewer than RUN_AT_ONCE are busy.
  const pump = useCallback(() => {
    const busy = entries.current.filter((e) => {
      const p = e.job.snapshot.phase;
      return p !== 'queued' && p !== 'done' && p !== 'failed' && p !== 'cancelled';
    }).length;
    const waiting = entries.current
      .filter((e) => e.job.snapshot.phase === 'queued' && e.userId === userId)
      .reverse(); // oldest first
    for (const e of waiting.slice(0, Math.max(0, RUN_AT_ONCE - busy))) {
      void e.job.run().then(() => {
        if (e.job.snapshot.phase === 'done') setFinished((n) => n + 1);
        pump();
      });
    }
  }, [userId]);

  // Someone else signed in: drop the previous person's uploads.
  useEffect(() => {
    if (userId === null) return;
    const others = entries.current.filter((e) => e.userId !== userId);
    if (others.length === 0) return;
    others.forEach((e) => e.job.cancel());
    entries.current = entries.current.filter((e) => e.userId === userId);
    setSnaps((m) => {
      const n = new Map(m);
      others.forEach((e) => n.delete(e.job.snapshot.key));
      return n;
    });
  }, [userId]);

  const start = useCallback(
    (files: Iterable<File>) => {
      if (userId === null) return;
      let added = 0;
      for (const f of files) {
        const job = new UploadJob(f, DEPS);
        job.subscribe(publish);
        entries.current = [{ job, userId }, ...entries.current];
        publish(job.snapshot);
        added++;
      }
      if (added) {
        setTrayOpen(true);
        pump();
      }
    },
    [userId, publish, pump],
  );

  const find = (key: string) => entries.current.find((e) => e.job.snapshot.key === key);
  const cancel = useCallback(
    (key: string) => {
      find(key)?.job.cancel();
      pump();
    },
    [pump],
  );
  const retry = useCallback(
    (key: string) => {
      const e = find(key);
      if (!e) return;
      void e.job.retry().then(() => {
        if (e.job.snapshot.phase === 'done') setFinished((n) => n + 1);
        pump();
      });
    },
    [pump],
  );
  const dismiss = useCallback((key: string) => {
    const e = find(key);
    if (e?.job.cancellable) return; /* still running: cancel first */
    entries.current = entries.current.filter((x) => x.job.snapshot.key !== key);
    setSnaps((m) => {
      const n = new Map(m);
      n.delete(key);
      return n;
    });
  }, []);
  const pick = useCallback(() => input.current?.click(), []);
  const serverIds = useCallback(
    () => new Set(entries.current.map((e) => e.job.serverId).filter((x): x is string => x !== null)),
    [],
  );

  const uploads = useMemo(
    () =>
      entries.current
        .filter((e) => e.userId === userId)
        .map((e) => snaps.get(e.job.snapshot.key))
        .filter((s): s is UploadSnapshot => s !== undefined),
    [snaps, userId],
  );

  const value = useMemo<UploadsValue>(
    () => ({ uploads, serverIds, start, cancel, retry, dismiss, pick, trayOpen, setTrayOpen, finished }),
    [uploads, serverIds, start, cancel, retry, dismiss, pick, trayOpen, finished],
  );
  return (
    <UploadsContext.Provider value={value}>
      {children}
      <input
        ref={input}
        type="file"
        multiple
        hidden
        tabIndex={-1}
        aria-hidden="true"
        data-testid="upload-input"
        onChange={(e) => {
          if (e.target.files) start(Array.from(e.target.files));
          e.target.value = ''; /* picking the same file again still fires */
        }}
      />
    </UploadsContext.Provider>
  );
}

export function useUploads(): UploadsValue {
  const v = useContext(UploadsContext);
  if (!v) throw new Error('useUploads outside UploadsProvider');
  return v;
}

/** True while files are dragged over the window; drops start uploads. */
export function useDropAnywhere(enabled: boolean): boolean {
  const { start } = useUploads();
  const [over, setOver] = useState(false);
  useEffect(() => {
    if (!enabled) return;
    let depth = 0;
    const hasFiles = (e: DragEvent) => Array.from(e.dataTransfer?.types ?? []).includes('Files');
    const enter = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      depth++;
      setOver(true);
    };
    const leave = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      depth = Math.max(0, depth - 1);
      if (depth === 0) setOver(false);
    };
    const overFn = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
    };
    const drop = (e: DragEvent) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      depth = 0;
      setOver(false);
      const files = Array.from(e.dataTransfer?.files ?? []);
      if (files.length) start(files);
    };
    window.addEventListener('dragenter', enter);
    window.addEventListener('dragleave', leave);
    window.addEventListener('dragover', overFn);
    window.addEventListener('drop', drop);
    return () => {
      window.removeEventListener('dragenter', enter);
      window.removeEventListener('dragleave', leave);
      window.removeEventListener('dragover', overFn);
      window.removeEventListener('drop', drop);
    };
  }, [enabled, start]);
  return over;
}
