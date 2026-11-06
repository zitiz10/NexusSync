// Engine (DESIGN §8.6, admin): the systems showcase. Two columns on
// desktop, one on compact. Each section: a heading, a key-value list, a
// small visual where it helps, at most one action. Live: stats are polled
// every 2 s while this page is open (for the hit-rate sparkline), engine
// events refresh their sections, and the event log follows the socket.
// Cleanup and the integrity scan (M12) run on the server as jobs; their
// progress arrives as engine.gc / engine.scan events, and the last result is
// read back from the event log, so a reload still shows it.
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ApiError, api } from '../api/client';
import type { Addresses, GcEvent, GcResult, Recovery, ScanEvent, ScanResult, ServerEvent, Stats } from '../api/types';
import { useConnection, useNow, useServerEvent } from '../app/connection';
import {
  count,
  gcProgressLine,
  gcResultLine,
  nextRunLine,
  scanFailureLine,
  scanFilesLine,
  scanProgressLine,
  scanResultLine,
} from '../app/copy';
import { formatAgo, formatBytes } from '../app/format';
import { Eraser, Save, ShieldCheck } from 'lucide-react';
import { Button } from '../components/Button';
import { Icon } from '../components/icons';
import { JoinCode } from '../components/JoinCode';
import { QrCode } from '../components/QrCode';
import { useToast } from '../components/Toast';
import { useTitle } from './Pages';

const POLL_MS = 2000;
const SPARK_POINTS = 30; /* 60 s at one sample per 2 s */
const LOG_MAX = 200;
const QUIET = new Set(['stats.updated', 'upload.progress']);
const RECOVERIES_SHOWN = 10;
const START_GRACE_MS = 3000; /* show "running" this long after a 202, until the first event */
/* Progress ticks are not log lines (the server leaves them out of its log too). */
const PROGRESS_PHASES = new Set(['freeing', 'compacting', 'scanning']);
const BUSY_NOTE = 'Cleanup or a scan is already running. Try again when it is done.';

function phaseOf(e: ServerEvent): string | null {
  const d = e.data as { phase?: unknown } | null;
  return d && typeof d.phase === 'string' ? d.phase : null;
}

function isProgress(e: ServerEvent): boolean {
  return (e.type === 'engine.gc' || e.type === 'engine.scan') && PROGRESS_PHASES.has(phaseOf(e) ?? '');
}

/** The newest finished run of a job in the log: its result, or 'failed'. */
function lastFinished<T>(events: LoggedEvent[], type: string): (T & { at: string }) | 'failed' | null {
  for (let i = events.length - 1; i >= 0; i--) {
    const { e } = events[i]!;
    if (e.type !== type) continue;
    const p = phaseOf(e);
    if (p === 'done') return { ...(e.data as T), at: e.at };
    if (p === 'failed') return 'failed';
  }
  return null;
}

function Meter({ percent, label }: { percent: number; label: string }) {
  const p = Math.max(0, Math.min(100, percent));
  return (
    <div className="meter" role="progressbar" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={p}>
      <span className="meter__fill" style={{ transform: `scaleX(${p / 100})` }} />
    </div>
  );
}

function uptime(s: number): string {
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} h ${m % 60} min`;
  return `${Math.floor(h / 24)} days`;
}

function KV({ rows }: { rows: [string, React.ReactNode][] }) {
  return (
    <dl className="kv">
      {rows.map(([k, v]) => (
        <div key={k} className="kv__row">
          <dt>{k}</dt>
          <dd>{v}</dd>
        </div>
      ))}
    </dl>
  );
}

/** Hit rate per polling interval, from the raw counters (D-046). */
function useHitRate(stats: Stats | null): (number | null)[] {
  const [points, setPoints] = useState<(number | null)[]>([]);
  const last = useRef<{ h: number; m: number } | null>(null);
  useEffect(() => {
    const ix = stats?.index;
    if (!ix) return;
    const prev = last.current;
    last.current = { h: ix.bufferHits, m: ix.bufferMisses };
    if (!prev) return;
    const dh = ix.bufferHits - prev.h;
    const dm = ix.bufferMisses - prev.m;
    const rate = dh + dm > 0 ? (100 * dh) / (dh + dm) : null; /* null: no page lookups this interval */
    setPoints((p) => [...p, rate].slice(-SPARK_POINTS));
  }, [stats]);
  return points;
}

/** 60 s of hit rate on a fixed 0–100% scale (a moving floor would make a
 * one-point dip look like a crash), with a baseline before samples arrive. */
function Sparkline({ points }: { points: (number | null)[] }) {
  const w = 120;
  const h = 28;
  const lo = 0;
  const step = w / (SPARK_POINTS - 1);
  let d = '';
  let pen = false;
  points.forEach((p, i) => {
    if (p === null) {
      pen = false;
      return;
    }
    const x = (SPARK_POINTS - points.length + i) * step;
    const y = h - 1 - ((p - lo) / Math.max(1, 100 - lo)) * (h - 2);
    d += `${pen ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`;
    pen = true;
  });
  return (
    <svg className="spark" viewBox={`0 0 ${w} ${h}`} aria-hidden="true" preserveAspectRatio="none">
      <line className="spark__base" x1="0" x2={w} y1={h - 0.5} y2={h - 0.5} />
      <path d={d} />
    </svg>
  );
}

function Histogram({ bins }: { bins: number[] }) {
  const max = Math.max(1, ...bins);
  const total = bins.reduce((a, b) => a + b, 0);
  const text = bins
    .map((n, i) => (n ? `${n} ${n === 1 ? 'container' : 'containers'} ${i * 10}–${i * 10 + 10}% live` : ''))
    .filter(Boolean)
    .join('; ');
  return (
    <figure className="hist">
      <div className="hist__bars" aria-hidden="true">
        {bins.map((n, i) => (
          <span key={i} className="hist__bar" style={{ transform: `scaleY(${n / max})` }} title={`${i * 10}–${i * 10 + 10}%: ${n}`} />
        ))}
      </div>
      <div className="hist__axis meta" aria-hidden="true">
        <span>0% live</span>
        <span>100%</span>
      </div>
      <figcaption className="meta">
        {total ? `${text.charAt(0).toUpperCase()}${text.slice(1)}.` : 'No containers with data yet.'}
      </figcaption>
    </figure>
  );
}

/** One event as a log line: time, type, compact details. */
function details(e: ServerEvent): string {
  const d = e.data;
  if (!d || typeof d !== 'object') return '';
  const parts: string[] = [];
  for (const [k, v] of Object.entries(d as Record<string, unknown>)) {
    if (v === null || typeof v === 'object') continue;
    if (k === 'sha256' || k === 'uploadId') continue;
    parts.push(`${k}=${String(v)}`);
  }
  const s = parts.join(' ');
  return s.length > 140 ? `${s.slice(0, 139)}…` : s;
}

interface LoggedEvent {
  id: number; /* stable key: a dropped oldest event must not re-render (and re-announce) the rest */
  e: ServerEvent;
}
let nextEventId = 1;
const tag = (e: ServerEvent): LoggedEvent => ({ id: nextEventId++, e });

function EventLog({ events }: { events: LoggedEvent[] }) {
  const box = useRef<HTMLDivElement>(null);
  const follow = useRef(true);
  useEffect(() => {
    const el = box.current;
    if (el && follow.current) el.scrollTop = el.scrollHeight;
  }, [events]);
  return (
    <div
      ref={box}
      className="log"
      tabIndex={0}
      role="log"
      aria-label="Engine events, newest last"
      onScroll={(e) => {
        const el = e.currentTarget;
        follow.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24; /* scrolled up: stop following */
      }}
      data-testid="event-log"
    >
      {events.length === 0 ? (
        <p className="meta">No events yet.</p>
      ) : (
        <ol className="log__list">
          {events.map(({ id, e }) => (
            <li key={id} className="log__line">
              <time className="log__time" dateTime={e.at}>
                {new Date(e.at).toLocaleTimeString('en-GB', { hour12: false })}
              </time>
              <span className="log__type">{e.type}</span>
              <span className="log__data">{details(e)}</span>
            </li>
          ))}
        </ol>
      )}
    </div>
  );
}

export function Engine() {
  useTitle('Engine');
  const toast = useToast();
  const now = useNow(5000);
  const { epoch, subscribe, link } = useConnection();
  const live = useRef(true);
  live.current = link.state !== 'offline';
  const [stats, setStats] = useState<Stats | null>(null);
  const [addresses, setAddresses] = useState<Addresses | null>(null);
  const [recoveries, setRecoveries] = useState<Recovery[] | null>(null);
  const [events, setEvents] = useState<LoggedEvent[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [restError, setRestError] = useState<string | null>(null);
  const [checkpointing, setCheckpointing] = useState(false);
  const [gcLive, setGcLive] = useState<GcEvent | null>(null);
  const [scanLive, setScanLive] = useState<ScanEvent | null>(null);
  const [starting, setStarting] = useState<'gc' | 'scan' | null>(null);
  const [jobNote, setJobNote] = useState<{ job: 'gc' | 'scan'; text: string } | null>(null);
  const points = useHitRate(stats);

  const loadStats = useCallback(async () => {
    // Offline or hidden: skip; the banner owns the message (DESIGN §8.8).
    if (!live.current || document.hidden) return;
    try {
      setStats(await api.stats());
      setError(null);
    } catch (e) {
      if (!(e instanceof ApiError && e.status === 401)) setError('Could not load the engine numbers.');
    }
  }, []);
  const loadRest = useCallback(async () => {
    const [a, r, ev] = await Promise.allSettled([api.addresses(), api.recoveries(), api.events(LOG_MAX)]);
    if (a.status === 'fulfilled') setAddresses(a.value);
    if (r.status === 'fulfilled') setRecoveries(r.value.items);
    if (ev.status === 'fulfilled') setEvents(ev.value.items.map(tag));
    setRestError(
      [a, r, ev].some((x) => x.status === 'rejected') ? 'Could not load the addresses, recovery history, or event log.' : null,
    );
  }, []);

  useEffect(() => {
    void loadStats();
    void loadRest();
    const t = setInterval(() => void loadStats(), POLL_MS);
    return () => clearInterval(t);
  }, [loadStats, loadRest, epoch]);

  // The event log follows the socket (everything but the chatty two).
  useEffect(
    () =>
      subscribe((e) => {
        if (QUIET.has(e.type) || isProgress(e)) return;
        setEvents((list) => [...list, tag(e)].slice(-LOG_MAX));
      }),
    [subscribe],
  );
  useServerEvent('engine.recovery', () => void loadRest());
  useServerEvent('engine.gc', (d) => {
    const ev = d as GcEvent;
    setStarting((j) => (j === 'gc' ? null : j));
    if (ev.phase === 'done' || ev.phase === 'failed') {
      setGcLive(null);
      void loadStats();
    } else {
      setGcLive(ev);
    }
  });
  useServerEvent('engine.scan', (d) => {
    const ev = d as ScanEvent;
    setStarting((j) => (j === 'scan' ? null : j));
    if (ev.phase === 'done' || ev.phase === 'failed') {
      setScanLive(null);
      void loadStats();
    } else {
      setScanLive(ev);
    }
  });

  const runJob = async (job: 'gc' | 'scan') => {
    setJobNote(null);
    setStarting(job);
    try {
      await (job === 'gc' ? api.gc() : api.scan());
      // Normally the first progress event arrives within milliseconds;
      // without the socket, the stats poll takes over after this.
      setTimeout(() => setStarting((j) => (j === job ? null : j)), START_GRACE_MS);
      void loadStats();
    } catch (e) {
      setStarting(null);
      const what = job === 'gc' ? 'Cleanup' : 'The scan';
      setJobNote({
        job,
        text:
          e instanceof ApiError && e.status === 409
            ? BUSY_NOTE
            : `${what} could not start. ${e instanceof ApiError ? e.message : 'Check the connection and try again.'}`,
      });
    }
  };

  const checkpoint = async () => {
    setCheckpointing(true);
    try {
      const r = await api.checkpoint();
      toast({
        text:
          r.pages === 0
            ? 'Checkpoint done. Nothing was waiting to be written.'
            : `Checkpoint written: ${count(r.pages, 'page')} in ${r.durationMs.toLocaleString('en-US')} ms.`,
      });
      void loadStats();
    } catch (e) {
      toast({ text: `The checkpoint failed. ${e instanceof ApiError ? e.message : ''}`.trim() });
    } finally {
      setCheckpointing(false);
    }
  };

  const s = stats;
  const wal = s?.wal;
  const ix = s?.index;
  const ct = s?.containers;
  const integ = s?.integrity;
  const cl = s?.cleanup;
  const srv = s?.server;
  const gcRunning = starting === 'gc' || gcLive !== null || !!cl?.running;
  const scanRunning = starting === 'scan' || scanLive !== null || !!integ?.running;
  const lastGc = useMemo(() => lastFinished<GcResult>(events, 'engine.gc'), [events]);
  const lastScan = useMemo(() => lastFinished<ScanResult>(events, 'engine.scan'), [events]);
  const scanPercent = scanLive?.phase === 'scanning' ? scanLive.percent : (integ?.percent ?? 0);
  const minRate = useMemo(() => {
    const k = points.filter((p): p is number => p !== null);
    return k.length ? Math.min(...k) : null;
  }, [points]);
  const lastRate = useMemo(() => [...points].reverse().find((p): p is number => p !== null) ?? null, [points]);

  return (
    <>
      <div className="page-head page-head--bare">
        <h1>Engine</h1>
        <p className="lead">The storage engine underneath the lab store, live. Every number here comes from the server.</p>
      </div>
      {error ? (
        <p className="inline-error" role="alert">
          {error}{' '}
          <button type="button" className="text-btn" onClick={() => void loadStats()}>
            Retry
          </button>
        </p>
      ) : null}
      {restError ? (
        <p className="inline-error" role="alert">
          {restError}{' '}
          <button type="button" className="text-btn" onClick={() => void loadRest()}>
            Retry
          </button>
        </p>
      ) : null}
      <div className="engine" aria-busy={!s || undefined}>
        <section className="engine__section" aria-labelledby="eng-server">
          <h2 id="eng-server">Server</h2>
          {!srv || !s ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <KV
              rows={[
                ['Version', srv.version],
                ['Running for', uptime(s.uptimeS)],
                ['Data directory', <span className="mono">{srv.dataDir}</span>],
                [
                  'Chunking',
                  srv.chunker === 'cdc'
                    ? `Content-defined, ${formatBytes(srv.cdcMin)} to ${formatBytes(srv.cdcMax)}, about ${formatBytes(srv.cdcAvg)} on average`
                    : `Fixed ${formatBytes(srv.fixedSize)} blocks`,
                ],
                ['Container size', formatBytes(srv.containerSize)],
              ]}
            />
          )}
          <h3 className="engine__h3">Open on another laptop</h3>
          {!addresses ? (
            <span className="skeleton skeleton--line" />
          ) : addresses.items.length === 0 ? (
            <p className="meta">
              {addresses.listen === '127.0.0.1' || addresses.listen === 'localhost'
                ? 'The server listens on this computer only (listen = 127.0.0.1). Set listen = 0.0.0.0:' + addresses.port + ' to let other laptops connect.'
                : 'No network address found. Connect this computer to the lab network.'}
            </p>
          ) : (
            <ul className="addresses">
              {addresses.items.map((a) => (
                <li key={a.url} className="addresses__item">
                  <QrCode text={a.url} label={`QR code for ${a.url}`} />
                  <span className="mono">{a.url}</span>
                </li>
              ))}
            </ul>
          )}
          <JoinCode />
        </section>

        <section className="engine__section" aria-labelledby="eng-wal">
          <h2 id="eng-wal">Write-ahead log</h2>
          {!wal ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <KV
              rows={[
                ['Current LSN', <span className="mono">{wal.lsn.toLocaleString('en-US')}</span>],
                ['Records since checkpoint', wal.recordsSinceCheckpoint.toLocaleString('en-US')],
                ['Log size', formatBytes(wal.sizeBytes)],
                [
                  'Last checkpoint',
                  wal.lastCheckpointAt
                    ? `${formatAgo(Date.parse(wal.lastCheckpointAt), now)}, took ${wal.lastCheckpointMs.toLocaleString('en-US')} ms`
                    : 'None since the server started',
                ],
              ]}
            />
          )}
          <p className="meta">Every change is written here and flushed to disk before it counts; after a crash the log is replayed.</p>
          <div className="actions">
            <Button variant="secondary" loading={checkpointing} icon={<Icon of={Save} />} onClick={() => void checkpoint()}>
              Checkpoint now
            </Button>
          </div>
        </section>

        <section className="engine__section" aria-labelledby="eng-index">
          <h2 id="eng-index">Block index</h2>
          {!ix ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <KV
              rows={[
                ['Entries', ix.entries.toLocaleString('en-US')],
                ['Global depth', String(ix.globalDepth)],
                ['Buckets', ix.buckets.toLocaleString('en-US')],
                ['Average bucket fill', `${ix.avgFillPercent.toLocaleString('en-US')}%`],
                [
                  'Buffer pool hit rate',
                  <span className="spark-row">
                    <Sparkline points={points} />
                    <span>
                      {lastRate !== null
                        ? `${lastRate.toFixed(1)}% now, lowest ${minRate!.toFixed(1)}% in the last minute`
                        : ix.bufferHitRate !== null
                          ? `${ix.bufferHitRate}% since start`
                          : 'No lookups yet'}
                    </span>
                  </span>,
                ],
                ['Dirty pages', ix.dirtyPages.toLocaleString('en-US')],
              ]}
            />
          )}
          <p className="meta">Extendible hashing: the directory doubles when a bucket overflows, so lookups stay one page read.</p>
        </section>

        <section className="engine__section" aria-labelledby="eng-ct">
          <h2 id="eng-ct">Containers</h2>
          {!ct || !cl ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <>
              <KV
                rows={[
                  ['Containers', ct.count.toLocaleString('en-US')],
                  ['Total size', formatBytes(ct.bytes)],
                  ['Unused space inside containers', formatBytes(ct.reclaimableBytes)],
                  ['Waiting for cleanup', formatBytes(cl.pendingBytes)],
                  [
                    'Last cleanup',
                    cl.lastRunAt ? formatAgo(Date.parse(cl.lastRunAt), now) : 'None since the server started',
                  ],
                  ['Next nightly cleanup', nextRunLine(cl.nextRunAt)],
                ]}
              />
              <h3 className="engine__h3">How full each container is</h3>
              <Histogram bins={ct.liveHistogram} />
            </>
          )}
          <p className="meta">
            Cleanup frees blocks no file uses, then rewrites containers that are less than half full. Unused space in fuller
            containers stays until they empty further.
          </p>
          <div className="actions">
            <Button
              variant="secondary"
              loading={gcRunning}
              icon={<Icon of={Eraser} />}
              aria-disabled={scanRunning || undefined}
              className={scanRunning ? 'btn--unavailable' : undefined}
              aria-describedby="gc-status"
              onClick={() => (scanRunning ? undefined : void runJob('gc'))}
              data-testid="run-cleanup"
            >
              Run cleanup
            </Button>
          </div>
          <p className="meta job-line" id="gc-status" role="status" data-testid="gc-status">
            {gcRunning
              ? 'Cleanup started.'
              : jobNote?.job === 'gc'
                ? jobNote.text
                : scanRunning
                  ? 'Available when the scan is done.'
                  : lastGc === 'failed'
                    ? 'The last cleanup failed. The server log has the details.'
                    : lastGc
                      ? gcResultLine(lastGc)
                      : ''}
          </p>
          {gcRunning ? (
            <p className="meta" data-testid="gc-progress">
              {gcProgressLine(gcLive)}
            </p>
          ) : null}
        </section>

        <section className="engine__section" aria-labelledby="eng-integ">
          <h2 id="eng-integ">Integrity</h2>
          {!integ ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <KV
              rows={[
                ['Last scan', integ.lastScanAt ? formatAgo(Date.parse(integ.lastScanAt), now) : 'Never'],
                ['Blocks verified', integ.lastScanAt ? integ.verified.toLocaleString('en-US') : 'None yet'],
                [
                  'Failures',
                  integ.lastScanAt ? (
                    <span className={integ.failures === 0 ? 'state-dot state-dot--ok' : 'state-dot state-dot--danger'}>{integ.failures}</span>
                  ) : (
                    'None yet'
                  ),
                ],
              ]}
            />
          )}
          <p className="meta">Downloads already re-check every block against its SHA-256; a scan checks every stored block.</p>
          <div className="actions">
            <Button
              variant="secondary"
              loading={scanRunning}
              icon={<Icon of={ShieldCheck} />}
              aria-disabled={gcRunning || undefined}
              className={gcRunning ? 'btn--unavailable' : undefined}
              aria-describedby="scan-status"
              onClick={() => (gcRunning ? undefined : void runJob('scan'))}
              data-testid="scan-now"
            >
              Scan now
            </Button>
          </div>
          <p className="meta job-line" id="scan-status" role="status" data-testid="scan-status">
            {scanRunning
              ? 'Scan started.'
              : jobNote?.job === 'scan'
                ? jobNote.text
                : gcRunning
                  ? 'Available when cleanup is done.'
                  : lastScan === 'failed'
                    ? 'The last scan could not finish. The server log has the details.'
                    : lastScan
                      ? scanResultLine(lastScan)
                      : integ && integ.failures > 0
                        ? `The last scan found ${count(integ.failures, 'damaged block')}. Scan again to list them.`
                        : ''}
          </p>
          {scanRunning ? (
            <div className="job-progress">
              <Meter percent={scanPercent} label="Integrity scan progress" />
              {/* Not a live region: it changes every 1%. The status line
                  above says "Scan started." once, then the result. */}
              <p className="meta" data-testid="scan-progress">
                {scanProgressLine(scanLive)}
              </p>
            </div>
          ) : null}
          {!scanRunning && lastScan && lastScan !== 'failed' && lastScan.items.length > 0 ? (
            <ul className="scan-failures" data-testid="scan-failures">
              {lastScan.items.map((f) => (
                <li key={`${f.containerId}:${f.offset}`} className="scan-failures__item">
                  <span className="state-dot state-dot--danger">{scanFailureLine(f)}</span>
                  <span className="meta">{scanFilesLine(f)}</span>
                </li>
              ))}
            </ul>
          ) : null}
        </section>

        <section className="engine__section" aria-labelledby="eng-rec">
          <h2 id="eng-rec">Recovery history</h2>
          {!recoveries ? (
            <span className="skeleton skeleton--block" />
          ) : recoveries.length === 0 ? (
            <p className="quiet">No starts recorded yet.</p>
          ) : (
            <div className="table-scroll">
              <table className="table" data-testid="recoveries">
                <thead>
                  <tr>
                    <th scope="col">Started</th>
                    <th scope="col">Records replayed</th>
                    <th scope="col">Transactions redone</th>
                    <th scope="col">Took</th>
                    <th scope="col">Result</th>
                  </tr>
                </thead>
                <tbody>
                  {recoveries.slice(0, RECOVERIES_SHOWN).map((r) => (
                    <tr key={r.at}>
                      <td>{formatAgo(Date.parse(r.at), now)}</td>
                      <td>{r.recordsReplayed.toLocaleString('en-US')}</td>
                      <td>{r.transactionsRedone.toLocaleString('en-US')}</td>
                      <td>{r.durationMs.toLocaleString('en-US')} ms</td>
                      <td>{r.result === 'clean' ? 'Clean start' : 'Recovered'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {recoveries.length > RECOVERIES_SHOWN ? (
                <p className="meta">
                  The latest {RECOVERIES_SHOWN} of {recoveries.length} recorded starts.
                </p>
              ) : null}
            </div>
          )}
        </section>

        <section className="engine__section engine__section--wide" aria-labelledby="eng-log">
          <h2 id="eng-log">Event log</h2>
          <EventLog events={events} />
        </section>
      </div>
    </>
  );
}
