import { useConnection, useNow } from '../app/connection';

/**
 * Under the header while the server cannot be reached (DESIGN §8.8):
 * "Lost connection to the lab server. Retrying in 4 s." One action.
 */
export function OfflineBanner() {
  const { link, retryNow } = useConnection();
  const now = useNow(250);
  if (link.state !== 'offline') return null;
  const secs = link.nextRetryAt === null ? 0 : Math.max(0, Math.ceil((link.nextRetryAt - now) / 1000));
  return (
    <div className="banner banner--danger">
      <div className="banner__inner">
        <span>
          {/* Announced once; the ticking countdown is not a live region. */}
          <span role="alert">Lost connection to the lab server.</span>{' '}
          <span aria-hidden="true">{secs > 0 ? `Retrying in ${secs} s.` : 'Retrying now.'}</span>
        </span>
        <button type="button" className="text-btn" onClick={retryNow}>
          Retry now
        </button>
      </div>
    </div>
  );
}
