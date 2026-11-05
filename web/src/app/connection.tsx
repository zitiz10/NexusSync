// Connection to the lab server: server health (recovering or not), the
// live event socket, and when data last arrived (DESIGN §6 status
// indicator, §8.8 system states).
//
// One health loop: it checks /api/health at start, every second while the
// server is recovering, with backoff while it cannot be reached, and again
// whenever the event socket is down. The socket reconnects on its own.
//
// Screens use:
//   - useConnection().link        live | reconnecting | offline (+ retry time)
//   - useConnection().lastUpdated  for "updated 3 s ago"
//   - useConnection().epoch        bumps after a reconnect or a recovery:
//                                  refetch when it changes
//   - useServerEvent(type, fn)     subscribe to one event type
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';
import { backoffDelayMs } from '../api/backoff';
import { api, onServerResponse } from '../api/client';
import type { Health, ServerEvent } from '../api/types';
import { LiveSocket, socketUrl, type LinkSnapshot } from '../api/ws';

export type ServerState =
  | { kind: 'checking' }
  | { kind: 'ok' }
  | { kind: 'recovering'; health: Health }
  | { kind: 'recovered' } /* shown briefly after a recovery completes */
  | { kind: 'unreachable'; nextRetryAt: number }
  | { kind: 'unavailable'; status: Health['status'] };

interface ConnectionValue {
  server: ServerState;
  link: LinkSnapshot;
  lastUpdated: number | null;
  epoch: number;
  setSocketEnabled: (on: boolean) => void;
  retryNow: () => void;
  subscribe: (fn: (e: ServerEvent) => void) => () => void;
}

const ConnectionContext = createContext<ConnectionValue | null>(null);

const POLL_MS = 1000; /* while recovering or stopping */
const RECOVERED_NOTICE_MS = 2500;

export function ConnectionProvider({ children }: { children: ReactNode }) {
  const [server, setServer] = useState<ServerState>({ kind: 'checking' });
  const [socketEnabled, setSocketEnabled] = useState(false);
  const [socket] = useState(() => new LiveSocket(socketUrl));
  const [link, setLink] = useState<LinkSnapshot>(socket.snapshot);
  const [lastResponse, setLastResponse] = useState<number | null>(null);
  const [epoch, setEpoch] = useState(0);

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const failures = useRef(0);
  const wasRecovering = useRef(false);
  const alive = useRef(true);
  const inFlight = useRef(false);

  const check = useCallback(async () => {
    if (inFlight.current) return;
    inFlight.current = true;
    if (timer.current) clearTimeout(timer.current);
    timer.current = null;
    let next: ServerState;
    let wait: number | null = null;
    try {
      const h = await api.health();
      failures.current = 0;
      if (h.status === 'recovering') {
        wasRecovering.current = true;
        next = { kind: 'recovering', health: h };
        wait = POLL_MS;
      } else if (h.status === 'ok') {
        if (wasRecovering.current) {
          wasRecovering.current = false;
          next = { kind: 'recovered' };
          setTimeout(() => {
            if (!alive.current) return;
            setServer({ kind: 'ok' });
            setEpoch((e) => e + 1); // reload everything after a recovery
          }, RECOVERED_NOTICE_MS);
        } else {
          next = { kind: 'ok' };
        }
      } else {
        next = { kind: 'unavailable', status: h.status };
        wait = POLL_MS;
      }
    } catch {
      const delay = backoffDelayMs(failures.current++);
      next = { kind: 'unreachable', nextRetryAt: Date.now() + delay };
      wait = delay;
    }
    inFlight.current = false;
    if (!alive.current) return;
    setServer(next);
    if (wait !== null) timer.current = setTimeout(() => void check(), wait);
  }, []);

  useEffect(() => {
    alive.current = true;
    void check();
    return () => {
      alive.current = false;
      if (timer.current) clearTimeout(timer.current);
      socket.stop();
    };
  }, [check, socket]);

  useEffect(() => socket.onState(setLink), [socket]);
  useEffect(
    () =>
      socket.onOpen((reconnected) => {
        if (!reconnected) return;
        setEpoch((e) => e + 1); // data may be stale after a drop
        void check(); // and the server may have restarted
      }),
    [socket, check],
  );
  // Socket down: ask /api/health too, so a restarting server shows its
  // "recovering" page and an unreachable one shows the banner countdown.
  useEffect(() => {
    if (link.state === 'offline' && timer.current === null) void check();
  }, [link.state, link.nextRetryAt, check]);
  useEffect(() => {
    if (socketEnabled && server.kind === 'ok') socket.start();
    else if (!socketEnabled) socket.stop();
  }, [socket, socketEnabled, server.kind]);
  useEffect(() => onServerResponse(() => setLastResponse(Date.now())), []);

  const retryNow = useCallback(() => {
    socket.retryNow();
    void check();
  }, [socket, check]);
  const subscribe = useCallback((fn: (e: ServerEvent) => void) => socket.onEvent(fn), [socket]);
  const lastUpdated =
    link.lastMessageAt !== null && (lastResponse === null || link.lastMessageAt > lastResponse)
      ? link.lastMessageAt
      : lastResponse;

  const value = useMemo<ConnectionValue>(
    () => ({ server, link, lastUpdated, epoch, setSocketEnabled, retryNow, subscribe }),
    [server, link, lastUpdated, epoch, retryNow, subscribe],
  );
  return <ConnectionContext.Provider value={value}>{children}</ConnectionContext.Provider>;
}

export function useConnection(): ConnectionValue {
  const v = useContext(ConnectionContext);
  if (!v) throw new Error('useConnection outside ConnectionProvider');
  return v;
}

/** Runs fn for every server event of this type while mounted. */
export function useServerEvent(type: string, fn: (data: unknown) => void): void {
  const { subscribe } = useConnection();
  const ref = useRef(fn);
  useEffect(() => {
    ref.current = fn;
  });
  useEffect(() => subscribe((e) => e.type === type && ref.current(e.data)), [subscribe, type]);
}

/** Re-renders every `ms` (for "updated 3 s ago" and countdowns). */
export function useNow(ms = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(t);
  }, [ms]);
  return now;
}
