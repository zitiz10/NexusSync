// Upload tray (DESIGN §6, §8.4): fixed bottom-right, collapsible. One row
// per upload in this tab: name, the phase sentence, a live dedup bar, and
// one action (Cancel until Verifying; Retry after a failure, including
// "session ended": the tray only shows once you are signed in again).
//
// - When every upload has finished, the tray folds to its header after a
//   few seconds; the stored files settle into Recent uploads (§8.4). A new
//   upload or a failure opens it again.
// - It can be closed only when nothing needs attention, so a failed upload
//   never disappears with its Retry.
// - One polite live region, mounted all the time, announces the summary
//   ("Uploading 2 files", "1 upload needs attention").
import { X } from 'lucide-react';
import { useEffect, useState } from 'react';
import { uploadLine } from '../app/copy';
import { formatBytes } from '../app/format';
import { useUploads } from '../app/uploads';
import type { UploadSnapshot } from '../engine/upload';
import { DedupBar, LiveDedupBar } from './DedupBar';
import { Icon } from './icons';

const FOLD_AFTER_MS = 4000;

function running(s: UploadSnapshot): boolean {
  return s.phase !== 'done' && s.phase !== 'failed' && s.phase !== 'cancelled';
}

function summary(list: UploadSnapshot[]): string {
  const active = list.filter(running).length;
  const failed = list.filter((s) => s.phase === 'failed').length;
  if (active) return active === 1 ? 'Uploading 1 file' : `Uploading ${active} files`;
  if (failed) return failed === 1 ? '1 upload needs attention' : `${failed} uploads need attention`;
  const done = list.filter((s) => s.phase === 'done').length;
  if (!done) return 'Uploads cancelled';
  return done === 1 ? '1 upload finished' : `${done} uploads finished`;
}

function Row({ s }: { s: UploadSnapshot }) {
  const { cancel, retry, dismiss } = useUploads();
  const failed = s.phase === 'failed';
  let action: { label: string; run: () => void } | null = null;
  if (s.phase === 'queued' || s.phase === 'reading' || s.phase === 'checking' || s.phase === 'sending')
    action = { label: 'Cancel', run: () => cancel(s.key) };
  else if (failed && s.failure?.kind !== 'disk') action = { label: 'Retry', run: () => retry(s.key) };
  const stored = s.phase === 'sending' || s.phase === 'verifying' ? s.size - s.needBytes : 0;
  return (
    <li className={`tray__row tray__row--${s.phase}`} data-testid="tray-row" data-phase={s.phase}>
      <div className="tray__top">
        <span className="tray__name">{s.name}</span>
        <span className="meta tray__size">{formatBytes(s.size)}</span>
      </div>
      {s.phase === 'done' && s.file ? (
        <DedupBar reusedPercent={s.file.reusedPercent} />
      ) : (
        <LiveDedupBar size={s.size} storedBytes={stored} sentBytes={s.sentBytes} />
      )}
      <div className="tray__bottom">
        <p className={failed ? 'tray__line tray__line--failed' : 'tray__line'}>{uploadLine(s)}</p>
        {action ? (
          <button type="button" className="text-btn" onClick={action.run} aria-label={`${action.label} ${s.name}`}>
            {action.label}
          </button>
        ) : (
          <button type="button" className="text-btn" onClick={() => dismiss(s.key)} aria-label={`Clear ${s.name}`}>
            Clear
          </button>
        )}
      </div>
    </li>
  );
}

export function UploadTray() {
  const { uploads, trayOpen, setTrayOpen, dismiss } = useUploads();
  const [collapsed, setCollapsed] = useState(false);
  const anyRunning = uploads.some(running);
  const anyFailed = uploads.some((s) => s.phase === 'failed');
  const settled = uploads.length > 0 && !anyRunning && !anyFailed;

  // Open on activity or trouble; fold a while after everything finished.
  useEffect(() => {
    if (!settled) {
      setCollapsed(false);
      return;
    }
    const t = setTimeout(() => setCollapsed(true), FOLD_AFTER_MS);
    return () => clearTimeout(t);
  }, [settled, uploads.length]);

  const visible = trayOpen && uploads.length > 0;
  const close = () => {
    uploads.forEach((s) => dismiss(s.key));
    setTrayOpen(false);
  };
  return (
    <>
      <p className="visually-hidden" role="status" aria-live="polite">
        {visible ? summary(uploads) : ''}
      </p>
      {visible ? (
        <section className={collapsed ? 'tray tray--collapsed' : 'tray'} aria-label="Uploads" data-drawer-keep>
          <header className="tray__head">
            <h2 className="tray__title">{summary(uploads)}</h2>
            <button
              type="button"
              className="text-btn"
              aria-expanded={!collapsed}
              aria-controls="tray-list"
              onClick={() => setCollapsed((c) => !c)}
            >
              {collapsed ? 'Show' : 'Hide'}
            </button>
            {settled ? (
              <button type="button" className="icon-btn" onClick={close} aria-label="Close the upload tray">
                <Icon of={X} />
              </button>
            ) : null}
          </header>
          <ul id="tray-list" className="tray__list" hidden={collapsed}>
            {uploads.map((s) => (
              <Row key={s.key} s={s} />
            ))}
          </ul>
        </section>
      ) : null}
    </>
  );
}
