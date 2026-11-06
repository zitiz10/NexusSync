// Sentences built from real numbers (DESIGN §8.2–§8.4, §10 copy rules:
// sentence case, units always, thousands separators, no exclamation).
import type { GcEvent, GcResult, ScanEvent, ScanFailure, ScanResult } from '../api/types';
import type { UploadSnapshot } from '../engine/upload';
import { formatBytes } from './format';

/** "1 block", "1,482 blocks". */
export function count(n: number, one: string, many = `${one}s`): string {
  return `${n.toLocaleString('en-US')} ${n === 1 ? one : many}`;
}

/** A share of 100 that never rounds up to 100 or down to 0 by accident. */
export function percent(p: number): string {
  if (!Number.isFinite(p)) return '0%';
  if (p >= 100) return '100%';
  if (p <= 0) return '0%';
  if (p > 99 || p < 1) return `${(Math.floor(p * 10) / 10).toLocaleString('en-US')}%`;
  return `${Math.round(p)}%`;
}

/** Note under a dedup bar: "94% already stored, 1.1 MB sent". */
export function dedupNote(reusedPercent: number, bytesSent: number): string {
  const sent = bytesSent === 0 ? 'nothing sent' : `${formatBytes(bytesSent)} sent`;
  if (reusedPercent <= 0) return `All new, ${sent}`;
  return `${percent(reusedPercent)} already stored, ${sent}`;
}

/** The tray line for an upload's current phase (DESIGN §8.4). */
export function uploadLine(s: UploadSnapshot): string {
  switch (s.phase) {
    case 'queued':
      return 'Waiting to start.';
    case 'reading':
      return `Fingerprinting blocks, ${s.size ? Math.floor((100 * s.readBytes) / s.size) : 100}%`;
    case 'checking':
      return `${count(s.chunkCount, 'block')}. Asking the server which it already has.`;
    case 'sending': {
      const stored = s.chunkCount - s.needCount;
      if (s.needCount === 0) return `All ${count(s.chunkCount, 'block')} already stored. Nothing to send.`;
      const rate = s.rate !== null ? `, ${formatBytes(Math.round(s.rate))}/s` : '';
      if (stored === 0) return `All new. Sending ${count(s.needCount, 'block')}, ${formatBytes(s.needBytes)}${rate}.`;
      return `${stored.toLocaleString('en-US')} already stored. Sending ${count(s.needCount, 'new block')}, ${formatBytes(s.needBytes)}${rate}.`;
    }
    case 'verifying':
      return 'Server is checking the whole file.';
    case 'done': {
      const sent = s.bytesSent ?? 0;
      if (sent === 0) return `Stored. Nothing sent: all ${formatBytes(s.size)} was already on the server.`;
      if (sent >= s.size) return `Stored. ${formatBytes(sent)} sent, all of it new.`;
      return `Stored. ${formatBytes(sent)} sent instead of ${formatBytes(s.size)}.`;
    }
    case 'cancelled':
      return 'Cancelled.';
    case 'failed':
      return failureLine(s);
  }
}

function failureLine(s: UploadSnapshot): string {
  const f = s.failure;
  if (!f) return 'The upload failed.';
  switch (f.kind) {
    case 'connection':
      return 'Connection lost.';
    case 'hash':
      return f.rejected
        ? `The server rejected ${count(f.rejected, 'block')} that did not match ${f.rejected === 1 ? 'its fingerprint' : 'their fingerprints'}.`
        : 'The server rebuilt the file and it did not match its fingerprint.';
    case 'disk':
      return 'The server is out of space. Ask the lab admin.';
    case 'session':
      return 'Your session ended before the upload finished.';
    default:
      return f.message;
  }
}

/* ---- cleanup and the integrity scan (Engine, M12) ------------------------------- */

/** The Containers section's status line while cleanup runs. */
export function gcProgressLine(e: GcEvent | null): string {
  if (e?.phase === 'compacting' && e.done < e.total) return `Compacting container ${e.done + 1} of ${e.total}.`;
  if (e?.phase === 'compacting') return 'Deleting the compacted containers.';
  return 'Freeing blocks that no file uses.';
}

/** What a finished cleanup did, in one or two sentences. */
export function gcResultLine(r: GcResult): string {
  const kept =
    r.containersSkipped > 0
      ? ` ${count(r.containersSkipped, 'container')} kept as ${r.containersSkipped === 1 ? 'it is' : 'they are'}: a block in ${r.containersSkipped === 1 ? 'it' : 'them'} is damaged. Run a scan for details.`
      : '';
  if (r.blocksFreed === 0 && r.containersCompacted === 0)
    return `Nothing to clean up. Every stored block is in use.${kept}`;
  if (r.containersCompacted === 0)
    return (
      `Cleanup freed ${count(r.blocksFreed, 'unused block')} (${formatBytes(r.bytesFreed)}). ` +
      `Disk space comes back once a container is less than half full.${kept}`
    );
  const parts = [];
  if (r.blocksFreed > 0) parts.push(`${count(r.blocksFreed, 'unused block')} freed`);
  parts.push(`${count(r.containersCompacted, 'container')} compacted`);
  if (r.blocksMoved > 0) parts.push(`${count(r.blocksMoved, 'block')} moved`);
  return `Cleanup reclaimed ${formatBytes(r.bytesReclaimed)}: ${parts.join(', ')}.${kept}`;
}

/** "Saturday 7 November at 02:00", in the browser's time zone. */
export function nextRunLine(iso: string | null): string {
  if (!iso) return 'Not scheduled';
  const d = new Date(iso);
  const day = d.toLocaleDateString('en-GB', { weekday: 'long', day: 'numeric', month: 'long' });
  const time = d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', hour12: false });
  return `${day} at ${time}`;
}

export function scanProgressLine(e: ScanEvent | null): string {
  if (e?.phase === 'scanning' && e.total > 0)
    return `Checking every stored block: ${e.done.toLocaleString('en-US')} of ${count(e.total, 'block')}.`;
  return 'Checking every stored block.';
}

export function scanResultLine(r: ScanResult): string {
  const took = r.durationMs < 1000 ? 'under a second' : `${Math.round(r.durationMs / 1000).toLocaleString('en-US')} s`;
  if (r.failures === 0) return `Scan finished in ${took}: ${count(r.verified, 'block')} verified, no damage found.`;
  return (
    `Scan finished in ${took}: ${count(r.failures, 'damaged block')} found, ${r.verified.toLocaleString('en-US')} verified. ` +
    'Downloads of the files below stop at the damaged block; restore them from another copy.'
  );
}

const REASONS: Record<ScanFailure['reason'], string> = {
  crc: 'its bytes changed on disk (checksum mismatch)',
  hash: 'its bytes do not match its SHA-256',
  unreadable: 'its container file could not be read',
};

/** "Block 3f9a0c12… in container 4: its bytes changed on disk (checksum mismatch)." */
export function scanFailureLine(f: ScanFailure): string {
  return `Block ${f.sha256.slice(0, 12)}… in container ${f.containerId}: ${REASONS[f.reason]}.`;
}

/** "Used by b.csv and a.csv." / "Used by no file." */
export function scanFilesLine(f: ScanFailure): string {
  const names = f.files.map((x) => x.name);
  if (names.length === 0) return 'Used by no file; cleanup will remove it.';
  if (names.length <= 2) return `Used by ${names.join(' and ')}.`;
  return `Used by ${names.slice(0, 2).join(', ')} and ${count(names.length - 2, 'more file')}.`;
}
