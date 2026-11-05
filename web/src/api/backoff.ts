// Reconnect timing (DESIGN.md §8.8): 1, 2, 4, 8 s, then every 10 s.

const STEPS_MS = [1000, 2000, 4000, 8000] as const;
export const MAX_BACKOFF_MS = 10_000;

/** Delay before reconnect attempt number `attempt` (0 = first retry). */
export function backoffDelayMs(attempt: number): number {
  if (!Number.isInteger(attempt) || attempt < 0) return STEPS_MS[0];
  return STEPS_MS[attempt] ?? MAX_BACKOFF_MS;
}

/**
 * Connection state shown by the status indicator and banner.
 * - live: the event socket is open
 * - reconnecting: it dropped moments ago and the first retries are running
 * - offline: retries keep failing (after about 3 s); the banner appears
 */
export type LinkState = 'live' | 'reconnecting' | 'offline';

/** Failed attempts after which "reconnecting" becomes "offline". */
export const OFFLINE_AFTER_FAILURES = 2;

export function linkStateAfterFailures(failures: number): LinkState {
  return failures >= OFFLINE_AFTER_FAILURES ? 'offline' : 'reconnecting';
}
