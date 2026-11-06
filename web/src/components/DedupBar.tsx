// Dedup bar (DESIGN §6): blue = already stored, peach = sent over the
// network. The note next to it is the text equivalent, so the bar itself is
// hidden from screen readers.

/** A stored file: track is peach (sent), blue fill from the left. */
export function DedupBar({ reusedPercent }: { reusedPercent: number }) {
  const p = Math.max(0, Math.min(100, reusedPercent));
  return (
    <span className="dedup" aria-hidden="true">
      <span className="dedup__fill" style={{ transform: `scaleX(${p / 100})` }} />
    </span>
  );
}

/**
 * An upload in progress: blue for blocks the server already had, peach for
 * new bytes sent so far, the rest still to send. Two layers that only scale
 * (DESIGN §9): peach covers stored + sent, blue covers stored, on top.
 */
export function LiveDedupBar({ size, storedBytes, sentBytes }: { size: number; storedBytes: number; sentBytes: number }) {
  const stored = size ? Math.min(1, storedBytes / size) : 0;
  const reach = size ? Math.min(1, (storedBytes + sentBytes) / size) : 0;
  return (
    <span className="dedup dedup--live" aria-hidden="true">
      <span className="dedup__sent" style={{ transform: `scaleX(${reach})` }} />
      <span className="dedup__fill" style={{ transform: `scaleX(${stored})` }} />
    </span>
  );
}
