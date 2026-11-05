// The live event socket (SPEC §12 "WebSocket events", DESIGN §8.8).
//
// One socket per tab. When it drops, it reconnects with backoff (1, 2, 4,
// 8, 10 s...). Listeners hear about every state change and every event;
// "open" after a drop means data on screen may be stale, so screens
// refetch.
import { backoffDelayMs, linkStateAfterFailures, type LinkState } from './backoff';
import type { ServerEvent } from './types';

export interface LinkSnapshot {
  state: LinkState;
  /** When the next reconnect attempt happens (ms epoch), while not live. */
  nextRetryAt: number | null;
  /** Last time anything arrived from the server on this socket. */
  lastMessageAt: number | null;
}

type StateListener = (s: LinkSnapshot) => void;
type EventListener = (e: ServerEvent) => void;
type OpenListener = (reconnected: boolean) => void;

export class LiveSocket {
  private ws: WebSocket | null = null;
  private failures = 0;
  private everOpened = false;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private stopped = true;
  private snap: LinkSnapshot = { state: 'reconnecting', nextRetryAt: null, lastMessageAt: null };
  private readonly stateListeners = new Set<StateListener>();
  private readonly eventListeners = new Set<EventListener>();
  private readonly openListeners = new Set<OpenListener>();

  constructor(private readonly url: () => string) {}

  get snapshot(): LinkSnapshot {
    return this.snap;
  }

  start(): void {
    if (!this.stopped) return;
    this.stopped = false;
    this.connect();
  }

  /** Closes the socket for good (sign out). */
  stop(): void {
    this.stopped = true;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    const ws = this.ws;
    this.ws = null;
    if (ws) {
      ws.onopen = ws.onclose = ws.onmessage = ws.onerror = null;
      ws.close();
    }
  }

  /** Skip the wait and try now (banner "Retry now"). */
  retryNow(): void {
    if (this.stopped || this.snap.state === 'live') return;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    this.connect();
  }

  onState(fn: StateListener): () => void {
    this.stateListeners.add(fn);
    return () => this.stateListeners.delete(fn);
  }

  onEvent(fn: EventListener): () => void {
    this.eventListeners.add(fn);
    return () => this.eventListeners.delete(fn);
  }

  onOpen(fn: OpenListener): () => void {
    this.openListeners.add(fn);
    return () => this.openListeners.delete(fn);
  }

  private set(next: Partial<LinkSnapshot>): void {
    this.snap = { ...this.snap, ...next };
    this.stateListeners.forEach((fn) => fn(this.snap));
  }

  private connect(): void {
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.url());
    } catch {
      this.scheduleRetry();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      const reconnected = this.everOpened;
      this.everOpened = true;
      this.failures = 0;
      this.set({ state: 'live', nextRetryAt: null, lastMessageAt: Date.now() });
      this.openListeners.forEach((fn) => fn(reconnected));
    };
    ws.onmessage = (m) => {
      this.set({ lastMessageAt: Date.now() });
      if (typeof m.data !== 'string') return;
      let ev: ServerEvent;
      try {
        ev = JSON.parse(m.data) as ServerEvent;
      } catch {
        return; // a malformed frame is ignored, not fatal
      }
      if (ev && typeof ev.type === 'string') this.eventListeners.forEach((fn) => fn(ev));
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      if (!this.stopped) this.scheduleRetry();
    };
    ws.onerror = () => {
      /* onclose follows and schedules the retry */
    };
  }

  private scheduleRetry(): void {
    const delay = backoffDelayMs(this.failures);
    this.failures += 1;
    this.set({ state: linkStateAfterFailures(this.failures), nextRetryAt: Date.now() + delay });
    this.timer = setTimeout(() => {
      this.timer = null;
      if (!this.stopped) this.connect();
    }, delay);
  }
}

export function socketUrl(): string {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${proto}//${location.host}/ws`;
}
