import { afterEach, describe, expect, it, vi } from 'vitest';
import { backoffDelayMs, linkStateAfterFailures } from '../api/backoff';
import { ApiError, onSessionEnded, request } from '../api/client';
import { formatAgo, initialOf } from './format';
import { formMessage, joinCodeError, joinErrorFromServer, usernameError, validateJoin } from './validate';

describe('reconnect backoff (DESIGN §8.8)', () => {
  it('waits 1, 2, 4, 8 s, then 10 s forever', () => {
    expect([0, 1, 2, 3, 4, 5, 50].map(backoffDelayMs)).toEqual([1000, 2000, 4000, 8000, 10000, 10000, 10000]);
    expect(backoffDelayMs(-1)).toBe(1000);
  });
  it('is "reconnecting" after one failure and "offline" after two', () => {
    expect(linkStateAfterFailures(1)).toBe('reconnecting');
    expect(linkStateAfterFailures(2)).toBe('offline');
  });
});

describe('formatAgo (DESIGN §10)', () => {
  const now = new Date(2026, 10, 6, 12, 0, 0).getTime();
  it('uses seconds, minutes, hours, then dates', () => {
    expect(formatAgo(now - 300, now)).toBe('just now');
    expect(formatAgo(now - 3000, now)).toBe('3 s ago');
    expect(formatAgo(now - 11 * 60_000, now)).toBe('11 min ago');
    expect(formatAgo(now - 5 * 3_600_000, now)).toBe('5 h ago');
    expect(formatAgo(new Date(2026, 10, 4, 9).getTime(), now)).toBe('4 Nov');
    expect(formatAgo(new Date(2025, 0, 2).getTime(), now)).toBe('2 Jan 2025');
    expect(formatAgo(now + 5000, now)).toBe('just now'); /* clock skew */
  });
  it('takes the first character of any script for the avatar', () => {
    expect(initialOf('riya k')).toBe('R');
    expect(initialOf('  राहुल')).toBe('र');
    expect(initialOf('')).toBe('?');
  });
});

describe('join validation mirrors the server (SPEC §13)', () => {
  it('accepts a good form', () => {
    expect(validateJoin({ displayName: 'Riya K', username: 'Riya.K-2', password: 'long-enough', joinCode: 'abcd2345' })).toEqual({});
  });
  it('flags each field', () => {
    const e = validateJoin({ displayName: ' ', username: 'x', password: 'short', joinCode: 'ABCD1234' });
    expect(Object.keys(e).sort()).toEqual(['displayName', 'joinCode', 'password', 'username']);
    expect(usernameError('-dash')).toBeDefined();
    expect(usernameError('a'.repeat(33))).toBeDefined();
    expect(joinCodeError('ABCD1234')).toBe('That does not look like a join code.'); /* '1' is not in the alphabet */
  });
  it('counts password length in bytes like the server', () => {
    expect(validateJoin({ displayName: 'R', username: 'riya', password: 'éééééé', joinCode: 'ABCD2345' }).password).toBeUndefined(); /* 12 bytes */
  });
  it('puts server errors under the right field', () => {
    expect(joinErrorFromServer('bad_join_code', '').field).toBe('joinCode');
    expect(joinErrorFromServer('username_taken', '').field).toBe('username');
    expect(joinErrorFromServer('too_many_attempts', '')).toEqual({ text: 'Too many attempts. Try again in a minute.' });
    expect(formMessage('mystery', 'Server said so.')).toBe('Server said so.');
  });
});

describe('API client', () => {
  afterEach(() => vi.unstubAllGlobals());

  const reply = (status: number, body?: unknown) =>
    vi.fn(async () => new Response(body === undefined ? null : JSON.stringify(body), { status }));

  it('sends the CSRF header on mutating requests only', async () => {
    const f = reply(200, { ok: 1 });
    vi.stubGlobal('fetch', f);
    await request('/api/x', { method: 'POST', json: { a: 1 } });
    await request('/api/y');
    const post = f.mock.calls[0] as unknown as [string, RequestInit];
    const get = f.mock.calls[1] as unknown as [string, RequestInit];
    expect((post[1].headers as Record<string, string>)['X-NexusSync']).toBe('1');
    expect((post[1].headers as Record<string, string>)['Content-Type']).toBe('application/json');
    expect((get[1].headers as Record<string, string>)['X-NexusSync']).toBeUndefined();
    expect(post[1].credentials).toBe('same-origin');
  });

  it('turns server errors into ApiError with code and message', async () => {
    vi.stubGlobal('fetch', reply(409, { error: { code: 'username_taken', message: 'That username is taken.' } }));
    const e = await request('/api/auth/join', { method: 'POST', quiet401: true }).catch((x: unknown) => x);
    expect(e).toBeInstanceOf(ApiError);
    expect((e as ApiError).code).toBe('username_taken');
    expect((e as ApiError).status).toBe(409);
  });

  it('reports a network failure as code "network"', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => { throw new TypeError('Failed to fetch'); }));
    const e = (await request('/api/me').catch((x: unknown) => x)) as ApiError;
    expect(e.isNetwork).toBe(true);
  });

  it('announces an ended session on 401, except when asked not to', async () => {
    const seen = vi.fn();
    const off = onSessionEnded(seen);
    vi.stubGlobal('fetch', reply(401, { error: { code: 'unauthorized', message: 'Sign in first.' } }));
    await request('/api/files').catch(() => undefined);
    await request('/api/auth/login', { method: 'POST', quiet401: true }).catch(() => undefined);
    off();
    expect(seen).toHaveBeenCalledTimes(1);
  });

  it('returns undefined for 204', async () => {
    vi.stubGlobal('fetch', reply(204));
    await expect(request('/api/auth/logout', { method: 'POST' })).resolves.toBeUndefined();
  });
});
