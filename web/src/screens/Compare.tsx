// Compare versions (DESIGN §8.5): how much of version 2 was already stored
// in version 1, with fixed 4 KB blocks against content-defined blocks.
//
// - Opens in Fixed 4 KB mode so the toggle reveals the improvement.
// - Pickers default to the newest file that has a previous version (its
//   last two versions); ?a=&b=&mode= from the Files screen wins.
// - A window of 32 blocks (from the server, starting 3 before the first
//   difference) and a minimap of the whole file that moves the window.
// - Hover or focus a version 2 block: its partner in version 1 lights up.
// - The entrance plays on load and on mode change only (DESIGN §9).
import { useCallback, useEffect, useMemo, useRef, useState, type CSSProperties, type KeyboardEvent } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { ApiError, api } from '../api/client';
import type { CompareBlock, CompareResult, FileItem } from '../api/types';
import { copyText } from '../app/clipboard';
import { useConnection } from '../app/connection';
import { count } from '../app/copy';
import { formatBytes } from '../app/format';
import { prefersReducedMotion } from '../app/motion';
import { useTitle } from './Pages';

type Mode = 'fixed' | 'cdc';
const ENTER_MS = 3600; /* the longest entrance: new blocks after the reused ones (§9) */
const MAX_HEADS = 500;

interface Version {
  id: number;
  version: number;
  size: number;
  createdAt: string;
}
interface Chain {
  name: string;
  ownerName: string;
  head: FileItem;
  versions: Version[]; /* newest first */
}

/** Fixed-mode blocks are calculated, not stored: their colours mean "matches
 * version 1" and "different", not "stored" and "sent" (review of M10). */
const WORDS: Record<Mode, { same: string; other: string }> = {
  fixed: { same: 'Matches version 1', other: 'Different from version 1' },
  cdc: { same: 'Already stored', other: 'New, sent' },
};

/** Fewer blocks on phones so both strips fit without scrolling apart. */
function useWindowCount(): number {
  const query = '(max-width: 767px)';
  const [narrow, setNarrow] = useState(() => window.matchMedia(query).matches);
  useEffect(() => {
    const m = window.matchMedia(query);
    const on = () => setNarrow(m.matches);
    m.addEventListener('change', on);
    return () => m.removeEventListener('change', on);
  }, []);
  return narrow ? 20 : 32;
}

const EXPLAIN: Record<Mode, string> = {
  fixed:
    'Every block is exactly 4 KB. When an edit inserts or removes bytes, all later data shifts along, so every boundary after it lands somewhere new and later blocks stop matching.',
  cdc: 'Boundaries are chosen by a rolling hash over the content itself, not by position. After the edit the cuts fall in the same places again, so only the blocks around the edit change.',
};

/** Every readable file chain with its versions (heads, then details for chains with history). */
function useChains(epoch: number) {
  const [chains, setChains] = useState<Chain[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(async () => {
    try {
      const heads: FileItem[] = [];
      let cursor: string | null = null;
      do {
        const page: { items: FileItem[]; nextCursor: string | null } = await api.files(
          `?sort=newest&limit=50${cursor ? `&cursor=${encodeURIComponent(cursor)}` : ''}`,
        );
        heads.push(...page.items);
        cursor = page.nextCursor;
      } while (cursor && heads.length < MAX_HEADS);
      const out: Chain[] = heads.map((h) => ({
        name: h.name,
        ownerName: h.ownerName,
        head: h,
        versions: [{ id: h.id, version: h.version, size: h.size, createdAt: h.createdAt }],
      }));
      // Older versions: only chains that have them, a few requests at a time.
      const withHistory = out.filter((c) => c.head.version > 1);
      for (let i = 0; i < withHistory.length; i += 6) {
        await Promise.all(
          withHistory.slice(i, i + 6).map(async (c) => {
            const d = await api.file(c.head.id);
            c.versions = d.versions;
          }),
        );
      }
      setChains(out);
      setError(null);
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      setError('Could not load your files.');
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load, epoch]);
  return { chains, error, reload: load };
}

function defaults(chains: Chain[]): { a: number; b: number } | null {
  const withPrev = chains.find((c) => c.versions.length > 1);
  if (withPrev) return { a: withPrev.versions[1]!.id, b: withPrev.versions[0]!.id };
  if (chains.length >= 2) return { a: chains[1]!.head.id, b: chains[0]!.head.id };
  return null;
}

function shortHash(h: string): string {
  return `${h.slice(0, 16)}…${h.slice(-8)}`;
}

export function Compare() {
  useTitle('Compare');
  const { epoch } = useConnection();
  const [params, setParams] = useSearchParams();
  const { chains, error: listError, reload } = useChains(epoch);
  const mode: Mode = params.get('mode') === 'cdc' ? 'cdc' : 'fixed';
  const pickA = Number(params.get('a')) || null;
  const pickB = Number(params.get('b')) || null;
  const fallback = useMemo(() => (chains ? defaults(chains) : null), [chains]);
  const a = pickA ?? fallback?.a ?? null;
  const b = pickB ?? fallback?.b ?? null;

  const [data, setData] = useState<CompareResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [from, setFrom] = useState<number | undefined>(undefined);
  const [entering, setEntering] = useState(false);
  const [retry, setRetry] = useState(0);
  const windowCount = useWindowCount();
  const [hover, setHover] = useState<CompareBlock | null>(null);
  const [selected, setSelected] = useState<{ side: 'a' | 'b'; block: CompareBlock } | null>(null);
  const story = `${a}-${b}-${mode}`;
  const lastStory = useRef('');

  const set = (patch: Record<string, string | null>) =>
    setParams(
      (p) => {
        const n = new URLSearchParams(p);
        for (const [k, v] of Object.entries(patch)) {
          if (v === null) n.delete(k);
          else n.set(k, v);
        }
        return n;
      },
      { replace: true },
    );

  // A new pair or mode starts at the server's window (3 before the first
  // difference) and replays the entrance; a window move does neither.
  useEffect(() => {
    setFrom(undefined);
    setSelected(null);
    setHover(null);
    setError(null);
  }, [story]);

  useEffect(() => {
    if (a === null || b === null) return;
    const ac = new AbortController();
    api
      .compare(a, b, mode, from, windowCount, ac.signal)
      .then((r) => {
        setData(r);
        setError(null);
        if (lastStory.current !== story) {
          lastStory.current = story;
          if (!prefersReducedMotion()) setEntering(true);
        }
      })
      .catch((e: unknown) => {
        if (e instanceof DOMException && e.name === 'AbortError') return;
        if (e instanceof ApiError && e.status === 401) return;
        setError(
          e instanceof ApiError && e.code === 'too_large_to_compare'
            ? 'These files are over 1 GB, too large to compare here.'
            : e instanceof ApiError && e.status === 404
              ? 'One of these files no longer exists, or is not shared with you.'
              : 'Could not compare these files. Check the connection and try again.',
        );
      });
    return () => ac.abort();
  }, [a, b, mode, from, story, epoch, retry, windowCount]);

  useEffect(() => {
    if (!entering) return;
    const t = setTimeout(() => setEntering(false), ENTER_MS);
    return () => clearTimeout(t);
  }, [entering]);

  if (chains && chains.length < 2 && a === null)
    return (
      <div className="page-head page-head--bare">
        <h1>Compare two versions</h1>
        <p className="lead" data-testid="compare-empty">
          Upload a file and then an edited copy to compare them here.
        </p>
        <p>
          <Link className="link" to="/files">
            Go to Files
          </Link>
        </p>
      </div>
    );

  const win = data?.window;
  const nb = data?.totals.b.chunks ?? 0;
  const showing =
    win && nb ? `Showing blocks ${(win.from + 1).toLocaleString('en-US')}–${Math.min(nb, win.from + win.count).toLocaleString('en-US')} of ${nb.toLocaleString('en-US')}` : '';
  const ready = data && win && data.files.a.id === a && data.files.b.id === b && data.mode === mode;

  return (
    <>
      <div className="page-head page-head--compare">
        <div className="title-row">
          <h1>Compare two versions</h1>
          <div className="segmented" role="group" aria-label="Block method">
            {(
              [
                ['fixed', 'Fixed 4 KB blocks'],
                ['cdc', 'Content-defined'],
              ] as const
            ).map(([m, label]) => (
              <button
                key={m}
                type="button"
                className="segmented__btn"
                aria-pressed={mode === m}
                onClick={() => set({ mode: m === 'fixed' ? null : m })}
              >
                {label}
              </button>
            ))}
          </div>
        </div>
        <div className="pickers">
          <Picker label="Version 1" value={a} chains={chains} onPick={(id) => set({ a: String(id), b: String(b ?? id) })} />
          <Picker label="Version 2" value={b} chains={chains} onPick={(id) => set({ a: String(a ?? id), b: String(id) })} />
        </div>
      </div>

      {listError ? (
        <p className="inline-error" role="alert">
          {listError}{' '}
          <button type="button" className="text-btn" onClick={() => void reload()}>
            Retry
          </button>
        </p>
      ) : null}
      {error ? (
        <p className="inline-error" role="alert">
          {error}{' '}
          <button type="button" className="text-btn" onClick={() => setRetry((n) => n + 1)}>
            Retry
          </button>
        </p>
      ) : null}

      {error ? null : <section className="cmp-panel" aria-label="Blocks of both versions" aria-busy={!ready || undefined}>
        {!ready || !win ? (
          <div className="cmp-skeleton" aria-hidden="true">
            <span className="skeleton skeleton--strip" />
            <span className="skeleton skeleton--strip" />
          </div>
        ) : (
          <>
            <Strips
              mode={mode}
              data={data}
              entering={entering}
              hover={hover}
              setHover={setHover}
              selected={selected}
              setSelected={setSelected}
              clear={() => setSelected(null)}
            />
            <Minimap data={data} onMove={(i) => setFrom(Math.max(0, Math.min(Math.max(0, nb - win.count), i)))} />
            <div className="cmp-foot">
              <span className="meta">{showing}</span>
              <ul className="legend" aria-label="Block colours">
                <li>
                  <span className="legend__swatch legend__swatch--v1" aria-hidden="true" />
                  Version 1 block
                </li>
                <li>
                  <span className="legend__swatch legend__swatch--shared" aria-hidden="true" />
                  {WORDS[mode].same}
                </li>
                <li>
                  <span className="legend__swatch legend__swatch--new" aria-hidden="true" />
                  {WORDS[mode].other}
                </li>
              </ul>
            </div>
          </>
        )}
      </section>}

      {ready ? (
        <div className="cmp-lower">
          <div className="cmp-summary" data-testid="cmp-summary">
            <p className="big-sentence">
              {mode === 'cdc'
                ? `${data.totals.b.reused.toLocaleString('en-US')} of ${count(nb, 'block')} were already stored in version 1.`
                : `${data.totals.b.reused.toLocaleString('en-US')} of ${count(nb, 'block')} match version 1.`}{' '}
              <span className="big-sentence__second">
                New in version 2: {formatBytes(data.totals.b.sentBytes)} of {formatBytes(data.totals.b.bytes)}.
              </span>
            </p>
            <p className="lead">{EXPLAIN[mode]}</p>
          </div>
          <Details selected={selected} mode={mode} total={nb} totalA={data.totals.a.chunks} />
        </div>
      ) : null}
    </>
  );
}

// ---- pickers ---------------------------------------------------------------------------

function Picker({
  label,
  value,
  chains,
  onPick,
}: {
  label: string;
  value: number | null;
  chains: Chain[] | null;
  onPick: (id: number) => void;
}) {
  const id = `pick-${label.replace(' ', '-').toLowerCase()}`;
  const known = chains?.some((c) => c.versions.some((v) => v.id === value)) ?? false;
  return (
    <div className="field picker">
      <label className="field__label" htmlFor={id}>
        {label}
      </label>
      <select
        id={id}
        className="input select"
        value={value ?? ''}
        disabled={!chains}
        onChange={(e) => onPick(Number(e.target.value))}
      >
        {!chains ? <option value="">Loading files…</option> : null}
        {chains && value !== null && !known ? <option value={value}>File {value}</option> : null}
        {chains?.map((c) =>
          c.versions.length > 1 ? (
            <optgroup key={c.head.id} label={`${c.name} (${c.ownerName})`}>
              {c.versions.map((v) => (
                <option key={v.id} value={v.id}>
                  {c.name} v{v.version}, {formatBytes(v.size)}
                </option>
              ))}
            </optgroup>
          ) : (
            <option key={c.head.id} value={c.head.id}>
              {c.name} v{c.head.version}, {formatBytes(c.head.size)}
            </option>
          ),
        )}
      </select>
    </div>
  );
}

// ---- the two strips --------------------------------------------------------------------

interface StripsProps {
  mode: Mode;
  data: CompareResult;
  entering: boolean;
  hover: CompareBlock | null;
  setHover: (b: CompareBlock | null) => void;
  selected: { side: 'a' | 'b'; block: CompareBlock } | null;
  setSelected: (s: { side: 'a' | 'b'; block: CompareBlock }) => void;
}

function Strips({ mode, data, entering, hover, setHover, selected, setSelected, clear }: StripsProps & { clear: () => void }) {
  const { a: wa, b: wb } = data.window;
  const reusedInWindow = wb.filter((x) => x.reused).length;
  let reusedK = 0;
  let newK = 0;
  const partnerShown = hover?.matchIndex != null && wa.some((x) => x.index === hover.matchIndex);
  const v1Label = !hover
    ? 'Hover a version 2 block to find its match'
    : hover.reused
      ? `Matches version 1 block ${(hover.matchIndex! + 1).toLocaleString('en-US')}${partnerShown ? '' : ', outside this window'}`
      : 'No matching block';

  // Arrow keys move along a strip (one Tab stop per strip).
  const keys = (side: 'a' | 'b', list: CompareBlock[]) => (e: KeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'Escape' && selected) {
      e.preventDefault();
      clear();
      return;
    }
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight' && e.key !== 'Home' && e.key !== 'End') return;
    e.preventDefault();
    const btns = Array.from(e.currentTarget.querySelectorAll<HTMLButtonElement>('button'));
    const at = btns.indexOf(document.activeElement as HTMLButtonElement);
    const next =
      e.key === 'Home' ? 0 : e.key === 'End' ? btns.length - 1 : Math.max(0, Math.min(btns.length - 1, at + (e.key === 'ArrowRight' ? 1 : -1)));
    btns[next]?.focus();
    const blk = list[next];
    if (blk) setSelected({ side, block: blk });
  };
  // One Tab stop per strip: the selected block, or the first block when the
  // selection is in the other strip or outside this window.
  const tabIndexFor = (side: 'a' | 'b', blk: CompareBlock, k: number) => {
    const list = side === 'a' ? wa : wb;
    const here = selected?.side === side && list.some((x) => x.index === selected.block.index);
    return here ? (selected!.block.index === blk.index ? 0 : -1) : k === 0 ? 0 : -1;
  };

  return (
    <div className={entering ? 'strips strips--enter' : 'strips'}>
      <div className="strip-row">
        <span className="strip-row__label">Version 1</span>
        <div className="strip-scroll">
          <div className="strip" role="group" aria-label="Version 1 blocks" style={{ '--n': wa.length } as unknown as CSSProperties} onKeyDown={keys('a', wa)} data-testid="strip-a">
            {wa.map((blk, k) => {
              const partner = hover?.reused && hover.matchIndex === blk.index;
              const cls = ['blk', 'blk--v1', hover ? (partner ? 'blk--partner' : 'blk--dim') : '', selected?.side === 'a' && selected.block.index === blk.index ? 'blk--selected' : '']
                .filter(Boolean)
                .join(' ');
              return (
                <button
                  key={blk.index}
                  type="button"
                  className={cls}
                  style={{ flexGrow: blk.size, '--i': k } as CSSProperties}
                  tabIndex={tabIndexFor('a', blk, k)}
                  aria-label={`Version 1, block ${blk.index + 1}, ${formatBytes(blk.size)}${blk.reused ? ', kept in version 2' : ', not in version 2'}`}
                  aria-current={selected?.side === 'a' && selected.block.index === blk.index ? 'true' : undefined}
                  onClick={() => setSelected({ side: 'a', block: blk })}
                  data-testid="cmp-a-block"
                  data-index={blk.index}
                />
              );
            })}
          </div>
        </div>
        <p className="strip-row__note meta" data-testid="cmp-v1-label">
          {v1Label}
        </p>
      </div>
      <div className="strip-row">
        <span className="strip-row__label">Version 2</span>
        <div className="strip-scroll">
          <div
            className="strip"
            role="group"
            aria-label="Version 2 blocks" style={{ '--n': wb.length } as unknown as CSSProperties}
            onKeyDown={keys('b', wb)}
            onMouseLeave={() => setHover(null)}
            data-testid="strip-b"
          >
            {wb.map((blk, k) => {
              // --k: order within its kind; --rn: how many reused blocks rise
              // first (new blocks wait for them, DESIGN §9).
              const style = (
                blk.reused
                  ? { flexGrow: blk.size, '--k': reusedK++ }
                  : { flexGrow: blk.size, '--k': newK++, '--rn': reusedInWindow }
              ) as unknown as CSSProperties;
              const cls = [
                'blk',
                'blk--v2',
                blk.reused ? 'blk--reused' : 'blk--new',
                hover?.index === blk.index ? 'blk--hover' : '',
                selected?.side === 'b' && selected.block.index === blk.index ? 'blk--selected' : '',
              ]
                .filter(Boolean)
                .join(' ');
              return (
                <button
                  key={blk.index}
                  type="button"
                  className={cls}
                  style={style}
                  tabIndex={tabIndexFor('b', blk, k)}
                  aria-label={`Version 2, block ${blk.index + 1}, ${formatBytes(blk.size)}, ${blk.reused ? `${WORDS[mode].same.toLowerCase()}, version 1 block ${blk.matchIndex! + 1}` : WORDS[mode].other.toLowerCase()}`}
                  aria-current={selected?.side === 'b' && selected.block.index === blk.index ? 'true' : undefined}
                  onMouseEnter={() => setHover(blk)}
                  onFocus={() => setHover(blk)}
                  onBlur={() => setHover(null)}
                  onClick={() => setSelected({ side: 'b', block: blk })}
                  data-testid="cmp-b-block"
                  data-index={blk.index}
                />
              );
            })}
          </div>
        </div>
        <p className="strip-row__note meta">
          {selected ? `Block ${(selected.block.index + 1).toLocaleString('en-US')} of version ${selected.side === 'a' ? 1 : 2} selected` : 'Select a block'}
        </p>
      </div>
    </div>
  );
}

// ---- minimap ---------------------------------------------------------------------------

function Minimap({ data, onMove }: { data: CompareResult; onMove: (from: number) => void }) {
  const nb = data.totals.b.chunks;
  const { from, count: n } = data.window;
  if (nb === 0) return null;
  const step = Math.max(1, Math.floor(n / 4));
  const key = (e: KeyboardEvent<HTMLDivElement>) => {
    const moves: Record<string, number> = {
      ArrowLeft: from - step,
      ArrowDown: from - step,
      ArrowRight: from + step,
      ArrowUp: from + step,
      PageUp: from - n,
      PageDown: from + n,
      Home: 0,
      End: nb - n,
    };
    const to = moves[e.key];
    if (to === undefined) return;
    e.preventDefault();
    onMove(to);
  };
  return (
    <div className="minimap-row">
      <span className="strip-row__label">Whole file</span>
      <div
        className="minimap"
        role="slider"
        tabIndex={0}
        aria-label="Window position in version 2"
        aria-valuemin={1}
        aria-valuemax={nb}
        aria-valuenow={from + 1}
        aria-valuetext={`Blocks ${from + 1} to ${Math.min(nb, from + n)} of ${nb}`}
        onKeyDown={key}
        onClick={(e) => {
          const r = e.currentTarget.getBoundingClientRect();
          const at = Math.floor(((e.clientX - r.left) / r.width) * nb);
          onMove(at - Math.floor(n / 2));
        }}
        data-testid="minimap"
      >
        {data.minimap.map(([v, run], i) => (
          <span key={i} className={v ? 'minimap__run minimap__run--shared' : 'minimap__run minimap__run--new'} style={{ flexGrow: run }} />
        ))}
        <span
          className="minimap__window"
          style={{ left: `${(100 * from) / nb}%`, width: `${(100 * Math.min(n, nb - from)) / nb}%` }}
          aria-hidden="true"
        />
      </div>
    </div>
  );
}

// ---- details -----------------------------------------------------------------------------

function Details({
  selected,
  mode,
  total,
  totalA,
}: {
  selected: { side: 'a' | 'b'; block: CompareBlock } | null;
  mode: Mode;
  total: number;
  totalA: number;
}) {
  const [copied, setCopied] = useState<'yes' | 'no' | null>(null);
  useEffect(() => setCopied(null), [selected]);
  if (!selected)
    return (
      <aside className="cmp-details" aria-label="Block details" data-testid="cmp-details">
        <p className="quiet">Select a block to see its fingerprint and where it is stored.</p>
      </aside>
    );
  const { side, block } = selected;
  const of = side === 'a' ? totalA : total;
  const status =
    side === 'b'
      ? block.reused
        ? `${mode === 'cdc' ? 'Already stored' : 'Matches'}: same as version 1 block ${(block.matchIndex! + 1).toLocaleString('en-US')}`
        : mode === 'cdc'
          ? 'New: sent with version 2'
          : 'Different from version 1'
      : block.reused
        ? `Kept in version 2 as block ${(block.matchIndex! + 1).toLocaleString('en-US')}`
        : 'Not in version 2';
  return (
    <aside className="cmp-details" aria-label="Block details" data-testid="cmp-details">
      <h2 className="cmp-details__title">
        Block {(block.index + 1).toLocaleString('en-US')} of {of.toLocaleString('en-US')}
        <span className="meta"> · version {side === 'a' ? 1 : 2}</span>
      </h2>
      <dl className="kv">
        <div className="kv__row">
          <dt>Status</dt>
          <dd>{status}</dd>
        </div>
        <div className="kv__row">
          <dt>SHA-256</dt>
          <dd>
            <span className="mono" title={block.sha256}>
              {shortHash(block.sha256)}
            </span>{' '}
            <button
              type="button"
              className="text-btn"
              onClick={() => void copyText(block.sha256).then((ok) => setCopied(ok ? 'yes' : 'no'))}
            >
              Copy
            </button>
            <span className="meta" aria-live="polite">
              {copied === 'yes' ? ' Copied.' : copied === 'no' ? ' Could not copy.' : ''}
            </span>
          </dd>
        </div>
        <div className="kv__row">
          <dt>Size</dt>
          <dd>{count(block.size, 'byte')}</dd>
        </div>
        {block.stored ? (
          <>
            <div className="kv__row">
              <dt>Stored in</dt>
              <dd>
                Container {block.stored.containerId.toLocaleString('en-US')}, offset {block.stored.offset.toLocaleString('en-US')}
              </dd>
            </div>
            <div className="kv__row">
              <dt>Used</dt>
              <dd>{count(block.stored.sharedBy, 'time')} across stored files</dd>
            </div>
          </>
        ) : null}
      </dl>
      {!block.stored ? (
        <p className="meta cmp-details__note">
          {mode === 'fixed'
            ? 'Not stored. NexusSync stores content-defined blocks; this fixed-size view is calculated for comparison.'
            : 'Not stored as content-defined blocks; this view is calculated for comparison.'}
        </p>
      ) : null}
    </aside>
  );
}

