// Sentences built from real numbers (DESIGN §8.2–§8.4, §10 copy rules:
// sentence case, units always, thousands separators, no exclamation).
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
