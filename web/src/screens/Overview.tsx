// Overview (DESIGN §8.2): what the store holds, the block field, recent
// uploads, and an engine summary (admin) or "Your storage" (student).
// Every number comes from /api/stats, /api/blockmap, or /api/files.
import { useCallback, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router-dom';
import { Upload } from 'lucide-react';
import { ApiError, api } from '../api/client';
import type { BlockMap, FileItem, Stats, StatsUpdate, UploadProgressEvent } from '../api/types';
import { useUser } from '../app/auth';
import { useConnection, useNow, useServerEvent } from '../app/connection';
import { count, dedupNote, percent, uploadLine } from '../app/copy';
import { formatAgo, formatBytes } from '../app/format';
import { useTween } from '../app/motion';
import { useUploads } from '../app/uploads';
import { BlockField, FieldLegend, TILES, type BlockFieldHandle } from '../components/BlockField';
import { Button } from '../components/Button';
import { DedupBar, LiveDedupBar } from '../components/DedupBar';
import { Icon } from '../components/icons';
import type { UploadSnapshot } from '../engine/upload';
import { useTitle } from './Pages';

const RECENT = 8;
const MAP_SETTLE_MS = 2100; /* the server rebuilds the map at most every 2 s (SPEC §12) */
const REFETCH_MS = 600; /* coalesce refetches after a burst of events */

/** Calls fn at most once per `ms`, after the last call (trailing). */
function useDebounced(fn: () => void, ms: number): () => void {
  const t = useRef<ReturnType<typeof setTimeout> | null>(null);
  const f = useRef(fn);
  useEffect(() => {
    f.current = fn;
  });
  useEffect(() => () => void (t.current && clearTimeout(t.current)), []);
  return useCallback(() => {
    if (t.current) clearTimeout(t.current);
    t.current = setTimeout(() => f.current(), ms);
  }, [ms]);
}

function Bytes({ n }: { n: number }) {
  return <>{formatBytes(Math.max(0, Math.round(useTween(n))))}</>;
}

/** Text equivalent of the shades (they are a gradient of meaning, not 3:1 apart). */
function levelSummary(tiles: number[]): string {
  const n = [0, 0, 0, 0, 0];
  for (const l of tiles) n[l] = (n[l] ?? 0) + 1;
  const parts = [
    n[1] ? `${n[1]} written once` : '',
    n[2] ? `${n[2]} shared by 2 files` : '',
    n[3] ? `${n[3]} by 3 to 4` : '',
    n[4] ? `${n[4]} by 5 or more` : '',
  ].filter(Boolean);
  return parts.length ? `Tiles: ${parts.join(', ')}.` : '';
}

function hoverSentence(f: FileItem): string {
  if (f.reusedPercent >= 100) return 'Every block was already stored when it arrived.';
  if (f.reusedPercent <= 0) return 'All new data when it arrived.';
  return `${percent(f.reusedPercent)} was already stored when it arrived.`;
}

export function Overview() {
  useTitle('Overview');
  const user = useUser();
  const isAdmin = user.role === 'admin';
  const { epoch } = useConnection();
  const { uploads, pick, finished } = useUploads();
  const now = useNow(15_000);

  const [stats, setStats] = useState<Stats | null>(null);
  const [map, setMap] = useState<BlockMap | null>(null);
  const [recent, setRecent] = useState<FileItem[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [hover, setHover] = useState<{ file: FileItem; tiles: Set<number> } | null>(null);
  const [entered, setEntered] = useState(false);
  const field = useRef<BlockFieldHandle>(null);
  const tileCache = useRef(new Map<number, Set<number>>());
  const hoverId = useRef<number | null>(null);

  const loadStats = useCallback(async () => {
    setStats(await api.stats());
  }, []);
  const loadMap = useCallback(async () => {
    const m = await api.blockmap();
    tileCache.current.clear(); /* tiles move as the store grows */
    setMap(m);
  }, []);
  const loadRecent = useCallback(async () => {
    const r = await api.files(`?sort=newest&limit=${RECENT}`);
    setRecent(r.items);
  }, []);
  const loadAll = useCallback(async () => {
    try {
      await Promise.all([loadStats(), loadMap(), loadRecent()]);
      setError(null);
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      setError('Could not load the overview.');
    }
  }, [loadStats, loadMap, loadRecent]);

  useEffect(() => {
    void loadAll();
  }, [loadAll, epoch, finished]);

  // Live: numbers now, details and lists shortly after, the map once the
  // server has rebuilt it.
  const refetch = useDebounced(() => void Promise.all([loadStats(), loadRecent()]).catch(() => undefined), REFETCH_MS);
  const refetchMap = useDebounced(() => void loadMap().catch(() => undefined), MAP_SETTLE_MS);
  useServerEvent('stats.updated', (d) => {
    const g = d as StatsUpdate;
    setStats((s) => (s ? { ...s, ...g } : s));
    refetch();
    refetchMap();
  });
  useServerEvent('file.deleted', refetch);
  useServerEvent('file.updated', refetch);
  useServerEvent('upload.progress', (d) => {
    const ev = d as UploadProgressEvent;
    if (ev.newTiles.length) field.current?.pulse(ev.newTiles);
  });

  // The entrance plays once, on the first load with data. After it, the
  // class comes off so later re-renders never replay it. 2.6 s covers the
  // longest tile: 31 × 26 + 11 × 38 ms of delay plus 1,100 ms (DESIGN §9).
  useEffect(() => {
    if (!map || entered) return;
    const t = setTimeout(() => setEntered(true), 2600);
    return () => clearTimeout(t);
  }, [map, entered]);

  const showTiles = useCallback(async (f: FileItem) => {
    hoverId.current = f.id;
    const cached = tileCache.current.get(f.id);
    if (cached) return setHover({ file: f, tiles: cached });
    try {
      const r = await api.fileTiles(f.id);
      const set = new Set(r.tiles);
      tileCache.current.set(f.id, set);
      if (hoverId.current === f.id) setHover({ file: f, tiles: set });
    } catch {
      /* the hover is a nicety; the list still works */
    }
  }, []);
  const hideTiles = useCallback(() => {
    hoverId.current = null;
    setHover(null);
  }, []);

  const loading = !stats || !map || !recent;
  const empty = stats !== null && stats.files === 0 && stats.physicalBytes === 0;
  const active = uploads.filter((s) => s.phase !== 'done' && s.phase !== 'cancelled' && s.phase !== 'failed');

  if (error && loading)
    return (
      <p className="inline-error" role="alert">
        {error}{' '}
        <button type="button" className="text-btn" onClick={() => void loadAll()}>
          Retry
        </button>
      </p>
    );

  const caption = hover
    ? `Tiles holding blocks of ${hover.file.name}. ${hoverSentence(hover.file)}`
    : empty
      ? 'Each tile will hold an equal slice of the stored blocks.'
      : map && map.tileBytes > 0
        ? `Each tile is about ${formatBytes(map.tileBytes)} of stored blocks.`
        : '';

  return (
    <>
      <section
        className="hero"
        {...(loading ? { 'aria-label': 'Overview' } : { 'aria-labelledby': 'hero-title' })}
        aria-busy={loading || undefined}
      >
        <div className="hero__text">
          {loading ? (
            <>
              <span className="skeleton skeleton--display" />
              <span className="skeleton skeleton--line" />
            </>
          ) : empty ? (
            <>
              <h1 id="hero-title" className="display">
                Nothing stored yet.
              </h1>
              <p className="lead">Upload a file to begin. Repeated blocks will be stored once and shared.</p>
            </>
          ) : (
            <>
              <h1 id="hero-title" className="display">
                <span className="display__line">
                  <Bytes n={stats.logicalBytes} /> uploaded.
                </span>{' '}
                <span className="display__line">
                  <Bytes n={stats.physicalBytes} /> on disk.
                </span>
              </h1>
              {stats.writesSkippedBytes === 0 && stats.networkSavedBytes === 0 ? (
                <p className="lead">
                  Repeated blocks are stored once and shared between files. Nothing has repeated yet: every block so far
                  is new.
                </p>
              ) : (
                <p className="lead">
                  Repeated blocks are stored once and shared between files. <Bytes n={stats.writesSkippedBytes} /> of
                  writes skipped, <Bytes n={stats.networkSavedBytes} /> never sent over the network.
                </p>
              )}
            </>
          )}
          <div className="actions">
            <Button icon={<Icon of={Upload} />} onClick={pick}>
              Upload a file
            </Button>
            {!empty ? (
              <Link className="btn btn--secondary" to="/compare">
                Compare two versions
              </Link>
            ) : null}
          </div>
        </div>
        <figure className="hero__field">
          <BlockField
            ref={field}
            levels={loading ? null : (map?.tiles ?? new Array<number>(TILES).fill(0))}
            highlight={hover ? hover.tiles : null}
            enter={!entered}
            label={`Block field: the stored blocks as ${TILES} tiles, shaded by how many files share them. ${
              map && !empty ? `${levelSummary(map.tiles)} ` : ''
            }${caption}`}
          />
          <figcaption className="field-caption">
            <p className="meta" aria-live="polite" data-testid="field-caption">
              {caption}
            </p>
            {!hover && !empty ? <FieldLegend /> : null}
          </figcaption>
        </figure>
      </section>

      <div className="overview-lower">
        <section className="recent" aria-labelledby="recent-title">
          <div className="section-head">
            <h2 id="recent-title">Recent uploads</h2>
            <Link className="link" to="/files">
              See all files
            </Link>
          </div>
          {recent === null ? (
            <RowsSkeleton />
          ) : recent.length === 0 && active.length === 0 ? (
            <p className="quiet">
              {isAdmin ? 'Uploads will appear here.' : 'Your files and files shared with the lab will appear here.'}
            </p>
          ) : (
            <ul className="rows" onMouseLeave={hideTiles}>
              {active.map((s) => (
                <ActiveRow key={s.key} s={s} />
              ))}
              {recent.map((f) => (
                <li
                  key={f.id}
                  className={hover?.file.id === f.id ? 'row row--recent row--lit' : 'row row--recent'}
                  onMouseEnter={() => void showTiles(f)}
                  onFocus={() => void showTiles(f)}
                  onBlur={hideTiles}
                  data-testid="recent-row"
                >
                  <div className="row__main">
                    <Link className="row__name" to={`/files?open=${f.id}`}>
                      {f.name}
                    </Link>
                    <span className="meta">
                      {f.ownerId === user.id ? 'You' : f.ownerName}, {formatAgo(Date.parse(f.createdAt), now)}
                    </span>
                  </div>
                  <span className="row__size">{formatBytes(f.size)}</span>
                  <div className="row__dedup">
                    <DedupBar reusedPercent={f.reusedPercent} />
                    <span className="meta">{dedupNote(f.reusedPercent, f.bytesSent)}</span>
                  </div>
                </li>
              ))}
            </ul>
          )}
        </section>
        <aside className="side" aria-labelledby="side-title">
          {isAdmin ? <EngineSummary stats={stats} now={now} /> : <YourStorage stats={stats} />}
        </aside>
      </div>
    </>
  );
}

function ActiveRow({ s }: { s: UploadSnapshot }) {
  const stored = s.phase === 'sending' || s.phase === 'verifying' ? s.size - s.needBytes : 0;
  return (
    <li className="row row--recent row--active" data-testid="recent-active">
      <div className="row__main">
        <span className="row__name">{s.name}</span>
        <span className="meta">{uploadLine(s)}</span>
      </div>
      <span className="row__size">{formatBytes(s.size)}</span>
      <div className="row__dedup">
        <LiveDedupBar size={s.size} storedBytes={stored} sentBytes={s.sentBytes} />
        <span className="meta">
          {s.phase !== 'sending' && s.phase !== 'verifying'
            ? 'Uploading'
            : stored === 0
              ? 'All new so far'
              : `${percent(s.size ? (100 * stored) / s.size : 100)} already stored`}
        </span>
      </div>
    </li>
  );
}

function RowsSkeleton() {
  return (
    <ul className="rows" aria-hidden="true">
      {[0, 1, 2].map((i) => (
        <li key={i} className="row row--recent">
          <span className="skeleton skeleton--line" />
        </li>
      ))}
    </ul>
  );
}

function KV({ rows }: { rows: [string, string][] }) {
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

function EngineSummary({ stats, now }: { stats: Stats | null; now: number }) {
  return (
    <>
      <div className="section-head">
        <h2 id="side-title">Engine</h2>
        <Link className="link" to="/engine">
          Open engine
        </Link>
      </div>
      {!stats?.wal || !stats.index || !stats.containers || !stats.cleanup || !stats.integrity ? (
        <span className="skeleton skeleton--block" aria-hidden="true" />
      ) : (
        <KV
          rows={[
            ['Write-ahead log', `${count(stats.wal.recordsSinceCheckpoint, 'record')} since the last checkpoint`],
            ['Block index', `${count(stats.index.entries, 'entry', 'entries')}, ${count(stats.index.buckets, 'bucket')}`],
            ['Containers', `${count(stats.containers.count, 'container')}, ${formatBytes(stats.containers.bytes)}`],
            ['Waiting for cleanup', formatBytes(stats.cleanup.pendingBytes)],
            [
              'Integrity scan',
              stats.integrity.lastScanAt
                ? `${formatAgo(Date.parse(stats.integrity.lastScanAt), now)}, ${count(stats.integrity.failures, 'failure')}`
                : 'Not run yet',
            ],
          ]}
        />
      )}
    </>
  );
}

function YourStorage({ stats }: { stats: Stats | null }) {
  return (
    <>
      <div className="section-head">
        <h2 id="side-title">Your storage</h2>
      </div>
      {!stats ? (
        <span className="skeleton skeleton--block" aria-hidden="true" />
      ) : stats.mine.files === 0 ? (
        <p className="quiet">You have not uploaded anything yet.</p>
      ) : (
        <KV
          rows={[
            ['Your files', stats.mine.files.toLocaleString('en-US')],
            ['Total size', formatBytes(stats.mine.bytes)],
            ['Already on the server when you uploaded', percent(stats.mine.reusedPercent)],
          ]}
        />
      )}
    </>
  );
}
