// Formatting helpers shared by every screen (DESIGN.md §10: units always,
// thousands separators, one decimal for GB).

const UNITS = ['bytes', 'KB', 'MB', 'GB', 'TB'] as const;

/** Formats a byte count, e.g. 0 -> "0 bytes", 12288 -> "12 KB", 1.5e9 -> "1.4 GB". */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) throw new RangeError(`bad byte count: ${n}`);
  // A no-break space keeps a number and its unit on one line ("5.7 MB").
  if (n < 1024) return `${n.toLocaleString('en-US')}\u00a0bytes`;
  let v = n;
  let u = 0;
  while (v >= 1024 && u < UNITS.length - 1) {
    v /= 1024;
    u++;
  }
  const unit = UNITS[u] ?? 'TB';
  // KB and MB as whole numbers below 100 get one decimal only when small.
  const digits = unit === 'GB' || unit === 'TB' ? 1 : v < 10 ? 1 : 0;
  // Same precision side by side: "4.0 MB" next to "9.7 MB", not "4 MB".
  const text = v.toLocaleString('en-US', { minimumFractionDigits: digits, maximumFractionDigits: digits });
  return `${text}\u00a0${unit}`;
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/**
 * How long ago `then` was, relative to `now` (DESIGN §10): "3 s ago",
 * "11 min ago", "5 h ago" under 24 h, then a date ("5 Nov", or "5 Nov 2025"
 * in another year). Times in the future (clock skew) read as "just now".
 */
export function formatAgo(then: number, now: number): string {
  const s = Math.floor((now - then) / 1000);
  if (s < 1) return 'just now';
  if (s < 60) return `${s} s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} h ago`;
  const d = new Date(then);
  const day = `${d.getDate()} ${MONTHS[d.getMonth()] ?? ''}`;
  return d.getFullYear() === new Date(now).getFullYear() ? day : `${day} ${d.getFullYear()}`;
}

/** First letter of a display name for the avatar circle (any script). */
export function initialOf(name: string): string {
  const first = Array.from(name.trim())[0];
  return first ? first.toLocaleUpperCase() : '?';
}
