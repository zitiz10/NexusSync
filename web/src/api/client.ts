// Typed client for the HTTP API (SPEC §12).
//
// - Same-origin requests only; the session is the HttpOnly nx_session
//   cookie, which page scripts cannot read (the server checks it).
// - Every mutating request carries X-NexusSync: 1 (the CSRF guard).
// - Errors become ApiError with the server's code and message, or code
//   "network" when the server could not be reached.
// - A 401 on any call except sign-in means the session ended: listeners
//   are told so the app can return to the sign-in screen (DESIGN §8.8).
import type { ErrorBody, FileItem, Health, User } from './types';

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly body: unknown;

  constructor(status: number, code: string, message: string, body?: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.body = body;
  }

  get isNetwork(): boolean {
    return this.code === 'network';
  }
}

type SessionListener = () => void;
const sessionListeners = new Set<SessionListener>();

/** Called when a request finds the session gone (401). Returns unsubscribe. */
export function onSessionEnded(fn: SessionListener): () => void {
  sessionListeners.add(fn);
  return () => sessionListeners.delete(fn);
}

type ResponseListener = () => void;
const responseListeners = new Set<ResponseListener>();

/** Called after any response from the server (proof it is reachable). */
export function onServerResponse(fn: ResponseListener): () => void {
  responseListeners.add(fn);
  return () => responseListeners.delete(fn);
}

interface RequestOptions {
  method?: 'GET' | 'POST' | 'PATCH' | 'DELETE';
  json?: unknown;
  signal?: AbortSignal;
  /** Do not treat 401 as "session ended" (sign-in itself, /api/me probe). */
  quiet401?: boolean;
}

const MUTATING = new Set(['POST', 'PATCH', 'DELETE']);

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const method = opts.method ?? 'GET';
  const headers: Record<string, string> = { Accept: 'application/json' };
  if (MUTATING.has(method)) headers['X-NexusSync'] = '1';
  let body: string | undefined;
  if (opts.json !== undefined) {
    headers['Content-Type'] = 'application/json';
    body = JSON.stringify(opts.json);
  }
  let res: Response;
  try {
    const init: RequestInit = { method, headers, credentials: 'same-origin' };
    if (body !== undefined) init.body = body;
    if (opts.signal) init.signal = opts.signal;
    res = await fetch(path, init);
  } catch (e) {
    if (e instanceof DOMException && e.name === 'AbortError') throw e;
    throw new ApiError(0, 'network', 'Lost connection to the lab server.');
  }
  responseListeners.forEach((fn) => fn());
  if (res.status === 204) return undefined as T;
  let parsed: unknown = null;
  const text = await res.text();
  if (text) {
    try {
      parsed = JSON.parse(text);
    } catch {
      parsed = null;
    }
  }
  if (!res.ok) {
    const err = (parsed as ErrorBody | null)?.error;
    const code = err?.code ?? `http_${res.status}`;
    const message = err?.message ?? `The server answered ${res.status}.`;
    if (res.status === 401 && !opts.quiet401) sessionListeners.forEach((fn) => fn());
    throw new ApiError(res.status, code, message, parsed);
  }
  return parsed as T;
}

// ---- endpoints -------------------------------------------------------------

export const api = {
  health: (signal?: AbortSignal) =>
    request<Health>('/api/health', signal ? { signal, quiet401: true } : { quiet401: true }),
  me: () => request<User>('/api/me', { quiet401: true }),
  login: (username: string, password: string) =>
    request<User>('/api/auth/login', { method: 'POST', json: { username, password }, quiet401: true }),
  join: (input: { username: string; displayName: string; password: string; joinCode: string }) =>
    request<User>('/api/auth/join', { method: 'POST', json: input, quiet401: true }),
  logout: () => request<void>('/api/auth/logout', { method: 'POST', quiet401: true }),
  files: (query = '') =>
    request<{ items: FileItem[]; nextCursor: string | null }>(`/api/files${query}`),
};
