// Files (DESIGN §8.3): search, sort, scope, paging; hover actions; the file
// drawer with versions, block counts, and the SHA-256; delete with undo.
//
// Delete: confirming hides the row and shows "Deleted X. Undo" for 6 s;
// the DELETE request is sent only when that toast expires (or is replaced,
// or the tab closes), so Undo never has to restore anything on the server.
import { useCallback, useEffect, useId, useMemo, useRef, useState } from 'react';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { Check, Download, GitCompare, ListPlus, MoreHorizontal, Upload } from 'lucide-react';
import { ApiError, api } from '../api/client';
import type { FileDetail, FileItem } from '../api/types';
import { useUser } from '../app/auth';
import { copyText } from '../app/clipboard';
import { useConnection, useNow, useServerEvent } from '../app/connection';
import { count, dedupNote, percent } from '../app/copy';
import { formatAgo, formatBytes } from '../app/format';
import { useUploads } from '../app/uploads';
import { Button } from '../components/Button';
import { DedupBar } from '../components/DedupBar';
import { Drawer } from '../components/Drawer';
import { Icon } from '../components/icons';
import { useToast } from '../components/Toast';
import { useTitle } from './Pages';

type Sort = 'newest' | 'name' | 'size' | 'shared';
type Scope = 'all' | 'mine' | 'shared';
const PAGE = 50;
const SEARCH_DELAY = 250;

const SORTS: { value: Sort; label: string }[] = [
  { value: 'newest', label: 'Newest' },
  { value: 'name', label: 'Name' },
  { value: 'size', label: 'Size' },
  { value: 'shared', label: 'Shared first' },
];
const SCOPES: { value: Scope; label: string }[] = [
  { value: 'all', label: 'All' },
  { value: 'mine', label: 'Mine' },
  { value: 'shared', label: 'Shared with lab' },
];

function listQuery(q: string, sort: Sort, scope: Scope, cursor: string | null): string {
  const p = new URLSearchParams({ sort, limit: String(PAGE) });
  if (q) p.set('q', q);
  if (scope !== 'all') p.set('scope', scope);
  if (cursor) p.set('cursor', cursor);
  return `?${p.toString()}`;
}

export function Files() {
  useTitle('Files');
  const user = useUser();
  const isAdmin = user.role === 'admin';
  const { epoch } = useConnection();
  const { pick, finished } = useUploads();
  const toast = useToast();
  const now = useNow(15_000);
  const [params, setParams] = useSearchParams();
  const sort = (SORTS.some((s) => s.value === params.get('sort')) ? params.get('sort') : 'newest') as Sort;
  const scope = (SCOPES.some((s) => s.value === params.get('show')) ? params.get('show') : 'all') as Scope;
  const q = params.get('q') ?? '';
  const openId = Number(params.get('open')) || null;

  const [typed, setTyped] = useState(q);
  const [items, setItems] = useState<FileItem[] | null>(null);
  const [cursor, setCursor] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loadingMore, setLoadingMore] = useState(false);
  const [hidden, setHidden] = useState<Set<number>>(new Set());
  const [confirmDelete, setConfirmDelete] = useState(false);

  const setParam = useCallback(
    (key: string, value: string | null) => {
      setParams(
        (p) => {
          const n = new URLSearchParams(p);
          if (value === null || value === '') n.delete(key);
          else n.set(key, value);
          return n;
        },
        { replace: key === 'q' },
      );
    },
    [setParams],
  );

  // Search as you type, a moment after the last key.
  useEffect(() => {
    if (typed === q) return;
    const t = setTimeout(() => setParam('q', typed.trim()), SEARCH_DELAY);
    return () => clearTimeout(t);
  }, [typed, q, setParam]);

  const load = useCallback(
    async (signal?: AbortSignal) => {
      try {
        const r = await api.files(listQuery(q, sort, scope, null), signal);
        setItems(r.items);
        setCursor(r.nextCursor);
        setError(null);
      } catch (e) {
        if (e instanceof DOMException && e.name === 'AbortError') return;
        if (e instanceof ApiError && e.status === 401) return;
        setError('Could not load the files.');
      }
    },
    [q, sort, scope],
  );
  useEffect(() => {
    const ac = new AbortController();
    void load(ac.signal);
    return () => ac.abort();
  }, [load, epoch, finished]);
  const refresh = useRef(load);
  useEffect(() => {
    refresh.current = load;
  });
  const soon = useRef<ReturnType<typeof setTimeout> | null>(null);
  const refetchSoon = useCallback(() => {
    if (soon.current) clearTimeout(soon.current);
    soon.current = setTimeout(() => void refresh.current(), 600);
  }, []);
  useEffect(() => () => void (soon.current && clearTimeout(soon.current)), []);
  useServerEvent('file.deleted', refetchSoon);
  useServerEvent('file.updated', refetchSoon);
  useServerEvent('upload.committed', refetchSoon);

  const more = async () => {
    if (!cursor) return;
    setLoadingMore(true);
    try {
      const r = await api.files(listQuery(q, sort, scope, cursor));
      setItems((old) => [...(old ?? []), ...r.items]);
      setCursor(r.nextCursor);
    } catch {
      setError('Could not load more files.');
    } finally {
      setLoadingMore(false);
    }
  };

  const open = (id: number | null) => {
    setConfirmDelete(false);
    setParam('open', id === null ? null : String(id));
  };

  const startDelete = (f: FileItem) => {
    open(null);
    setHidden((h) => new Set(h).add(f.id));
    toast({
      text: `Deleted ${f.name}.`,
      focus: true, /* the Delete button just vanished: Undo is one Enter away */
      action: { label: 'Undo', run: () => setHidden((h) => without(h, f.id)) },
      onExpire: () => {
        api
          .deleteFile(f.id)
          .then(() => refresh.current())
          .catch((e: unknown) => {
            setHidden((h) => without(h, f.id));
            const why = e instanceof ApiError ? e.message : 'The server could not be reached.';
            toast({ text: `Could not delete ${f.name}. ${why}` });
          });
      },
    });
  };

  const setShared = async (f: FileItem, shared: boolean) => {
    try {
      const updated = await api.setShared(f.id, shared);
      setItems((list) => list?.map((x) => (x.id === f.id ? { ...x, ...updated } : x)) ?? null);
      toast({ text: shared ? `${f.name} is now shared with the lab.` : `${f.name} is no longer shared.` });
    } catch (e) {
      toast({ text: `Could not change sharing. ${e instanceof ApiError ? e.message : ''}`.trim() });
    }
  };

  const visible = useMemo(() => items?.filter((f) => !hidden.has(f.id)) ?? null, [items, hidden]);

  return (
    <>
      <div className="page-head page-head--files">
        <div className="title-row">
          <h1>Files</h1>
          <Button icon={<Icon of={Upload} />} onClick={pick}>
            Upload a file
          </Button>
        </div>
        <div className="filters" role="search">
          <div className="field filters__search">
            <label className="field__label" htmlFor="file-search">
              Search files
            </label>
            <input
              id="file-search"
              className="input"
              type="search"
              value={typed}
              autoComplete="off"
              onChange={(e) => setTyped(e.target.value)}
            />
          </div>
          <div className="field filters__sort">
            <label className="field__label" htmlFor="file-sort">
              Sort
            </label>
            <select id="file-sort" className="input select" value={sort} onChange={(e) => setParam('sort', e.target.value === 'newest' ? null : e.target.value)}>
              {SORTS.map((s) => (
                <option key={s.value} value={s.value}>
                  {s.label}
                </option>
              ))}
            </select>
          </div>
          <div className="field">
            <span className="field__label" id="show-label">
              Show
            </span>
            <div className="segmented" role="group" aria-labelledby="show-label">
              {SCOPES.map((s) => (
                <button
                  key={s.value}
                  type="button"
                  className="segmented__btn"
                  aria-pressed={scope === s.value}
                  onClick={() => setParam('show', s.value === 'all' ? null : s.value)}
                >
                  {s.label}
                </button>
              ))}
            </div>
          </div>
        </div>
      </div>

      <p className="visually-hidden" role="status" aria-live="polite">
        {visible === null
          ? ''
          : visible.length === 0
            ? q
              ? `No files match "${q}".`
              : 'No files.'
            : `${visible.length.toLocaleString('en-US')} ${visible.length === 1 ? 'file' : 'files'}${cursor ? ' shown, more available' : ''}.`}
      </p>

      {error && visible ? (
        <p className="inline-error" role="alert">
          {error}{' '}
          <button type="button" className="text-btn" onClick={() => void load()}>
            Retry
          </button>
        </p>
      ) : null}

      {visible === null ? (
        error ? (
          <p className="inline-error" role="alert">
            {error}{' '}
            <button type="button" className="text-btn" onClick={() => void load()}>
              Retry
            </button>
          </p>
        ) : (
          <ul className="rows" aria-hidden="true">
            {[0, 1, 2, 3, 4].map((i) => (
              <li key={i} className="row row--file">
                <span className="skeleton skeleton--line" />
              </li>
            ))}
          </ul>
        )
      ) : visible.length === 0 ? (
        <p className="quiet empty-line" data-testid="files-empty">
          {q ? `No files match "${q}".` : scope === 'mine' ? 'You have no files yet. Drop a file anywhere on this page to upload it.' : scope === 'shared' ? 'Nothing is shared with the lab yet.' : 'No files yet. Drop a file anywhere on this page to upload it.'}
        </p>
      ) : (
        <>
          <ul className={isAdmin ? 'rows files files--admin' : 'rows files'} aria-label="Files">
            {visible.map((f) => (
              <FileRow
                key={f.id}
                f={f}
                mine={f.ownerId === user.id}
                canWrite={isAdmin || f.ownerId === user.id}
                showOwner={isAdmin}
                now={now}
                selected={openId === f.id}
                onOpen={() => open(f.id)}
                onDelete={() => {
                  open(f.id);
                  setConfirmDelete(true);
                }}
                onShare={(s) => void setShared(f, s)}
              />
            ))}
          </ul>
          {cursor ? (
            <div className="more">
              <Button variant="secondary" loading={loadingMore} icon={<Icon of={ListPlus} />} onClick={() => void more()}>
                Show more files
              </Button>
            </div>
          ) : null}
        </>
      )}

      <FileDrawer
        id={openId !== null && !hidden.has(openId) ? openId : null}
        onClose={() => open(null)}
        confirmDelete={confirmDelete}
        setConfirmDelete={setConfirmDelete}
        onDelete={startDelete}
        onShare={(f, s) => void setShared(f, s)}
        now={now}
      />
    </>
  );
}

function without(s: Set<number>, id: number): Set<number> {
  const n = new Set(s);
  n.delete(id);
  return n;
}

// ---- one row ---------------------------------------------------------------------------

interface RowProps {
  f: FileItem;
  mine: boolean;
  canWrite: boolean;
  showOwner: boolean;
  now: number;
  selected: boolean;
  onOpen: () => void;
  onDelete: () => void;
  onShare: (shared: boolean) => void;
}

function FileRow({ f, mine, canWrite, showOwner, now, selected, onOpen, onDelete, onShare }: RowProps) {
  const [menu, setMenu] = useState(false);
  const menuBtn = useRef<HTMLButtonElement>(null);
  const menuEl = useRef<HTMLDivElement>(null);
  const menuId = useId();

  useEffect(() => {
    if (!menu) return;
    menuEl.current?.querySelector<HTMLElement>('[role^="menuitem"]')?.focus();
    const close = (e: PointerEvent) => {
      if (!menuEl.current?.contains(e.target as Node) && e.target !== menuBtn.current) setMenu(false);
    };
    document.addEventListener('pointerdown', close);
    return () => document.removeEventListener('pointerdown', close);
  }, [menu]);

  const menuKeys = (e: React.KeyboardEvent) => {
    const items = Array.from(menuEl.current?.querySelectorAll<HTMLElement>('[role^="menuitem"]') ?? []);
    const i = items.indexOf(document.activeElement as HTMLElement);
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      setMenu(false);
      menuBtn.current?.focus();
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const n = items.length;
      items[(i + (e.key === 'ArrowDown' ? 1 : n - 1)) % n]?.focus();
    } else if (e.key === 'Tab') setMenu(false);
  };

  return (
    <li
      className={selected ? 'row row--file row--selected' : 'row row--file'}
      data-testid="file-row"
      data-drawer-keep
      onClick={(e) => {
        if ((e.target as Element).closest('a, button, [role="menu"]')) return;
        onOpen();
      }}
    >
      <div className="row__main">
        <button type="button" className="row__name row__open" onClick={onOpen} aria-label={`${f.name}, version ${f.version}. Open details`}>
          {f.name}
        </button>
        <span className="meta">
          v{f.version}
          {f.shared ? ' · shared with lab' : ''}
          {!showOwner && !mine ? ` · ${f.ownerName}` : ''}
        </span>
      </div>
      {showOwner ? <span className="row__owner">{mine ? 'You' : f.ownerName}</span> : null}
      <span className="row__size">{formatBytes(f.size)}</span>
      <div className="row__dedup">
        <DedupBar reusedPercent={f.reusedPercent} />
        <span className="meta">{dedupNote(f.reusedPercent, f.bytesSent)}</span>
      </div>
      <span className="row__date meta">{formatAgo(Date.parse(f.createdAt), now)}</span>
      <div className="row__actions">
        <a className="icon-btn" href={api.contentUrl(f.id)} download={f.name} aria-label={`Download ${f.name}`} title="Download">
          <Icon of={Download} />
        </a>
        {f.prevVersionId ? (
          <Link
            className="icon-btn"
            to={`/compare?a=${f.prevVersionId}&b=${f.id}`}
            aria-label={`Compare ${f.name} with the previous version`}
            title="Compare with previous"
          >
            <Icon of={GitCompare} />
          </Link>
        ) : null}
        {canWrite ? (
          <div className="menu-anchor">
            <button
              ref={menuBtn}
              type="button"
              className="icon-btn"
              aria-label={`More actions for ${f.name}`}
              aria-haspopup="menu"
              aria-expanded={menu}
              aria-controls={menu ? menuId : undefined}
              title="More"
              onClick={() => setMenu((m) => !m)}
            >
              <Icon of={MoreHorizontal} />
            </button>
            {menu ? (
              <div ref={menuEl} id={menuId} className="menu menu--row" role="menu" aria-label={`Actions for ${f.name}`} onKeyDown={menuKeys}>
                <button
                  type="button"
                  className="menu__item"
                  role="menuitemcheckbox"
                  aria-checked={f.shared}
                  onClick={() => {
                    setMenu(false);
                    onShare(!f.shared);
                  }}
                >
                  <span className="menu__check" aria-hidden="true">
                    {f.shared ? <Icon of={Check} /> : null}
                  </span>
                  Share with lab
                </button>
                <button
                  type="button"
                  className="menu__item menu__item--danger"
                  role="menuitem"
                  onClick={() => {
                    setMenu(false);
                    onDelete();
                  }}
                >
                  Delete
                </button>
              </div>
            ) : null}
          </div>
        ) : null}
      </div>
    </li>
  );
}

// ---- the drawer ----------------------------------------------------------------------

interface DrawerProps {
  id: number | null;
  onClose: () => void;
  confirmDelete: boolean;
  setConfirmDelete: (v: boolean) => void;
  onDelete: (f: FileItem) => void;
  onShare: (f: FileItem, shared: boolean) => void;
  now: number;
}

function FileDrawer({ id, onClose, confirmDelete, setConfirmDelete, onDelete, onShare, now }: DrawerProps) {
  const user = useUser();
  const navigate = useNavigate();
  const [detail, setDetail] = useState<FileDetail | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [picked, setPicked] = useState<number[]>([]);
  const [copied, setCopied] = useState<'yes' | 'no' | null>(null);
  const titleId = useId();

  const load = useCallback(async () => {
    if (id === null) return;
    try {
      setDetail(await api.file(id));
      setError(null);
    } catch (e) {
      setError(e instanceof ApiError && e.status === 404 ? 'This file no longer exists.' : 'Could not load the details.');
    }
  }, [id]);
  useEffect(() => {
    if (id === null) return; /* closing: keep the details on screen while it slides out */
    setDetail(null);
    setError(null);
    setPicked([]);
    setCopied(null);
    void load();
  }, [load, id]);
  useServerEvent('file.updated', () => void load());

  const f = detail && (detail.id === id || id === null) ? detail : null;
  const canWrite = f ? user.role === 'admin' || f.ownerId === user.id : false;
  const togglePick = (v: number) =>
    setPicked((p) => (p.includes(v) ? p.filter((x) => x !== v) : [...p, v].slice(-2)));

  return (
    <Drawer open={id !== null} onClose={onClose} labelledBy={titleId}>
      {!f ? (
        error ? (
          <div className="drawer__body">
            <h2 id={titleId}>
              File details
            </h2>
            <p className="inline-error" role="alert">
              {error}{' '}
              <button type="button" className="text-btn" onClick={() => void load()}>
                Retry
              </button>
            </p>
          </div>
        ) : (
          <div className="drawer__body" aria-busy="true">
            <h2 id={titleId} className="visually-hidden">
              Loading file details
            </h2>
            <span className="skeleton skeleton--line" />
            <span className="skeleton skeleton--block" />
          </div>
        )
      ) : (
        <div className="drawer__body">
          <div className="drawer__head">
            <h2 id={titleId} className="drawer__title">
              {f.name}
            </h2>
            <p className="meta">
              Version {f.version}, uploaded by {f.ownerId === user.id ? 'you' : f.ownerName}{' '}
              {formatAgo(Date.parse(f.createdAt), now)}
            </p>
          </div>

          {confirmDelete ? (
            <div className="confirm" role="group" aria-label="Confirm delete">
              <p>
                Delete {f.name}? Blocks shared with other files are kept.
              </p>
              <div className="actions">
                <Button variant="danger" className="btn--danger-solid" autoFocus onClick={() => onDelete(f)}>
                  Delete
                </Button>
                <Button variant="secondary" onClick={() => setConfirmDelete(false)}>
                  Keep it
                </Button>
              </div>
            </div>
          ) : (
            <div className="actions">
              <a className="btn btn--primary" href={api.contentUrl(f.id)} download={f.name}>
                <Icon of={Download} />
                Download
              </a>
              {canWrite ? (
                <Button variant="danger" onClick={() => setConfirmDelete(true)}>
                  Delete
                </Button>
              ) : null}
            </div>
          )}

          <dl className="kv">
            <div className="kv__row">
              <dt>Size</dt>
              <dd>{formatBytes(f.size)}</dd>
            </div>
            <div className="kv__row">
              <dt>Blocks</dt>
              <dd>{count(f.blocks.total, 'block')}</dd>
            </div>
            <div className="kv__row">
              <dt>Unique to this file</dt>
              <dd>{count(f.blocks.uniqueToFile, 'block')}</dd>
            </div>
            <div className="kv__row">
              <dt>Shared with other files</dt>
              <dd>{count(f.blocks.shared, 'block')}</dd>
            </div>
            <div className="kv__row">
              <dt>Sent at upload</dt>
              <dd>
                {formatBytes(f.bytesSent)}
                <span className="meta"> ({percent(f.reusedPercent)} already stored)</span>
              </dd>
            </div>
            <div className="kv__row">
              <dt>Chunking</dt>
              <dd>{f.chunker === 'cdc' ? 'Content-defined, about 4 KB blocks' : 'Fixed 4 KB blocks'}</dd>
            </div>
          </dl>

          <div className="hash">
            <div className="hash__head">
              <span className="field__label" id={`${titleId}-sha`}>
                SHA-256 of the whole file
              </span>
              <button
                type="button"
                className="text-btn"
                onClick={() => void copyText(f.sha256).then((ok) => setCopied(ok ? 'yes' : 'no'))}
              >
                Copy
              </button>
            </div>
            <p className="mono hash__value" aria-labelledby={`${titleId}-sha`} data-testid="sha256">
              {f.sha256}
            </p>
            <p className="meta" aria-live="polite">
              {copied === 'yes' ? 'Copied.' : copied === 'no' ? 'Could not copy. Select the text and copy it.' : ''}
            </p>
          </div>

          {canWrite ? (
            <label className="check">
              <input type="checkbox" checked={f.shared} onChange={(e) => onShare(f, e.target.checked)} />
              Shared with the lab (others can read and download it)
            </label>
          ) : null}

          <section aria-labelledby={`${titleId}-versions`}>
            <h3 id={`${titleId}-versions`} className="drawer__h3">
              Versions
            </h3>
            <ul className="versions">
              {f.versions.map((v) => (
                <li key={v.id} className="versions__row">
                  <label className="check">
                    <input type="checkbox" checked={picked.includes(v.id)} onChange={() => togglePick(v.id)} />
                    Version {v.version}
                    {v.id === f.id ? ' (this one)' : ''}
                  </label>
                  <span className="meta">
                    {formatBytes(v.size)}, {formatAgo(Date.parse(v.createdAt), now)}
                  </span>
                </li>
              ))}
            </ul>
            {f.versions.length > 1 ? (
              picked.length === 2 ? (
                <Button
                  variant="secondary"
                  onClick={() => {
                    const [a, b] = [...picked].sort((x, y) => x - y);
                    navigate(`/compare?a=${a}&b=${b}`);
                  }}
                >
                  Compare these two
                </Button>
              ) : (
                <p className="meta">Select two versions to compare them.</p>
              )
            ) : null}
          </section>
        </div>
      )}
    </Drawer>
  );
}
