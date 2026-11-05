import { Link } from 'react-router-dom';

/** Four blocks, one of them shared (DESIGN §5). Colors come from tokens. */
export function Mark({ size = 26 }: { size?: number }) {
  return (
    <svg className="mark" width={size} height={size} viewBox="0 0 26 26" aria-hidden="true">
      <rect className="mark__fill" x="1" y="1" width="11" height="11" rx="2.5" />
      <rect className="mark__outline" x="14" y="1" width="11" height="11" rx="2.5" />
      <rect className="mark__outline" x="1" y="14" width="11" height="11" rx="2.5" />
      <rect className="mark__shared" x="14" y="14" width="11" height="11" rx="2.5" />
    </svg>
  );
}

/** Mark and wordmark; a link to Overview when `to` is given. */
export function Brand({ to }: { to?: string }) {
  const inner = (
    <>
      <Mark />
      <span className="brand__word">NexusSync</span>
    </>
  );
  return to ? (
    <Link className="brand" to={to} aria-label="NexusSync, Overview">
      {inner}
    </Link>
  ) : (
    <span className="brand">{inner}</span>
  );
}
