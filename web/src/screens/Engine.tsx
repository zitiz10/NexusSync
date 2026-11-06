// Engine (DESIGN §8.6, admin): the systems showcase. Two columns on
// desktop, one on compact. Each section: a heading, a key-value list, a
// small visual where it helps, at most one action. Live: stats are polled
// every 2 s while this page is open (for the hit-rate sparkline), engine
// events refresh their sections, and the event log follows the socket.
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ApiError, api } from '../api/client';
import type { Addresses, Recovery, ServerEvent, Stats } from '../api/types';
import { useConnection, useNow, useServerEvent } from '../app/connection';
import { count } from '../app/copy';
import { formatAgo, formatBytes } from '../app/format';
import { Button } from '../components/Button';
import { JoinCode } from '../components/JoinCode';
import { QrCode } from '../components/QrCode';
import { useToast } from '../components/Toast';
import { useTitle } from './Pages';

const POLL_MS = 2000;
const SPARK_POINTS = 30; /* 60 s at one sample per 2 s */
const LOG_MAX = 200;
const QUIET = new Set(['stats.updated', 'upload.progress']);
const RECOVERIES_SHOWN = 10;

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
        if (QUIET.has(e.type)) return;
        setEvents((list) => [...list, tag(e)].slice(-LOG_MAX));
      }),
    [subscribe],
  );
  useServerEvent('engine.recovery', () => void loadRest());

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
  const srv = s?.server;
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
            <Button variant="secondary" loading={checkpointing} onClick={() => void checkpoint()}>
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
          {!ct || !s?.cleanup ? (
            <span className="skeleton skeleton--block" />
          ) : (
            <>
              <KV
                rows={[
                  ['Containers', ct.count.toLocaleString('en-US')],
                  ['Total size', formatBytes(ct.bytes)],
                  ['Reclaimable', formatBytes(ct.reclaimableBytes)],
                  ['Waiting for cleanup', formatBytes(s.cleanup.pendingBytes)],
                ]}
              />
              <h3 className="engine__h3">How full each container is</h3>
              <Histogram bins={ct.liveHistogram} />
            </>
          )}
          <div className="actions">
            <Button variant="secondary" aria-disabled="true" className="btn--unavailable" onClick={(e) => e.preventDefault()} aria-describedby="gc-note">
              Run cleanup
            </Button>
          </div>
          <p className="meta" id="gc-note">
            Cleanup is not part of this server version yet.
          </p>
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
            <Button variant="secondary" aria-disabled="true" className="btn--unavailable" onClick={(e) => e.preventDefault()} aria-describedby="scan-note">
              Scan now
            </Button>
          </div>
          <p className="meta" id="scan-note">
            Scanning is not part of this server version yet.
          </p>
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
