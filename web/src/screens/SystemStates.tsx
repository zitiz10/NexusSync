import type { ReactNode } from 'react';
import type { Health } from '../api/types';
import { useConnection, useNow } from '../app/connection';
import { Mark } from '../components/Mark';

/**
 * Full-page calm states (DESIGN §8.8). The numbers are the server's own:
 * the elapsed time comes from /api/health.
 */
function Calm({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <main className="calm" id="main">
      <div className="calm__col">
        <Mark size={32} />
        <h1 className="calm__title">{title}</h1>
        {children}
      </div>
    </main>
  );
}

export function Recovering({ health }: { health: Health }) {
  const secs = Math.floor((health.progress?.elapsedMs ?? 0) / 1000);
  return (
    <Calm title="The server is recovering after an unexpected stop">
      <p className="lead">It is replaying its write-ahead log so every committed change is back in place.</p>
      {/* Visible progress only: a live region here would speak every second. */}
      <p className="meta">Replaying the log, {secs} s so far.</p>
    </Calm>
  );
}

export function Recovered() {
  return (
    <Calm title="Recovery complete">
      <p className="lead" role="status">
        Every committed change is back in place. Loading your data again.
      </p>
    </Calm>
  );
}

export function Unreachable({ nextRetryAt }: { nextRetryAt: number }) {
  const { retryNow } = useConnection();
  const now = useNow(250);
  const secs = Math.max(0, Math.ceil((nextRetryAt - now) / 1000));
  return (
    <Calm title="Cannot reach the lab server">
      <p className="lead">Check that this laptop is on the lab network and the server is running.</p>
      <p className="meta">
        <span aria-hidden="true">{secs > 0 ? `Retrying in ${secs} s. ` : 'Retrying now. '}</span>
        <button type="button" className="text-btn" onClick={retryNow}>
          Retry now
        </button>
      </p>
    </Calm>
  );
}

export function Unavailable({ status }: { status: Health['status'] }) {
  return status === 'stopping' ? (
    <Calm title="The server is stopping">
      <p className="lead">It will be back when the lab admin starts it again. This page reconnects on its own.</p>
    </Calm>
  ) : (
    <Calm title="The server could not open its data">
      <p className="lead">Ask the lab admin to look at the server terminal. This page reconnects on its own.</p>
    </Calm>
  );
}
