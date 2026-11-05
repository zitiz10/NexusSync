import { formatAgo } from '../app/format';
import { useConnection, useNow } from '../app/connection';

/** 8 px dot plus text (DESIGN §6): Live, updated 3 s ago / Reconnecting / Offline. */
export function StatusIndicator() {
  const { link, lastUpdated } = useConnection();
  const now = useNow(1000);
  const state = link.state;
  const label = state === 'live' ? 'Live' : state === 'reconnecting' ? 'Reconnecting' : 'Offline';
  const detail = state === 'live' && lastUpdated !== null ? `, updated ${formatAgo(lastUpdated, now)}` : '';
  return (
    <span className={`status status--${state}`}>
      <span className="status__dot" aria-hidden="true" />
      <span aria-hidden="true">
        {label}
        <span className="status__detail">{detail}</span>
      </span>
      {/* Announce state changes only, not the ticking "updated" time. */}
      <span className="visually-hidden" role="status">
        Connection: {label}
      </span>
    </span>
  );
}
