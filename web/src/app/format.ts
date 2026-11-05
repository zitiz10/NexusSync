// Formatting helpers shared by every screen (DESIGN.md §10: units always,
// thousands separators, one decimal for GB).

const UNITS = ['bytes', 'KB', 'MB', 'GB', 'TB'] as const;

/** Formats a byte count, e.g. 0 -> "0 bytes", 12288 -> "12 KB", 1.5e9 -> "1.4 GB". */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) throw new RangeError(`bad byte count: ${n}`);
  if (n < 1024) return `${n.toLocaleString('en-US')} bytes`;
  let v = n;
  let u = 0;
  while (v >= 1024 && u < UNITS.length - 1) {
    v /= 1024;
    u++;
  }
  const unit = UNITS[u] ?? 'TB';
  // KB and MB as whole numbers below 100 get one decimal only when small.
  const digits = unit === 'GB' || unit === 'TB' ? 1 : v < 10 ? 1 : 0;
  const text = v.toLocaleString('en-US', { minimumFractionDigits: 0, maximumFractionDigits: digits });
  return `${text} ${unit}`;
}
