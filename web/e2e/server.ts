// Starts, stops, and restarts the real nexusd used by the e2e tests. State
// is kept in a small JSON file so tests (other processes) can restart the
// server and the teardown can stop whichever process is current.
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

export interface E2EState {
  url: string;
  conf: string;
  dir: string;
  bin: string;
  pid: number;
  admin: { username: string; password: string };
  student: { username: string; password: string; displayName: string };
}

export const statePath = () => {
  const p = process.env['NX_E2E_STATE'];
  if (!p) throw new Error('NX_E2E_STATE is not set (run through playwright)');
  return p;
};

export const readState = (): E2EState => JSON.parse(readFileSync(statePath(), 'utf8')) as E2EState;
export const writeState = (s: E2EState) => writeFileSync(statePath(), JSON.stringify(s, null, 2));

export function startServer(s: E2EState): number {
  const child = spawn(s.bin, ['--config', s.conf], {
    stdio: ['ignore', 'ignore', 'ignore'],
    detached: false,
  });
  child.unref();
  if (!child.pid) throw new Error('nexusd did not start');
  return child.pid;
}

export async function waitHealthy(url: string, timeoutMs = 20_000): Promise<void> {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    try {
      const r = await fetch(`${url}/api/health`);
      if (r.ok && ((await r.json()) as { status: string }).status === 'ok') return;
    } catch {
      /* not up yet */
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('nexusd did not become healthy');
}

/** Hard kill (like kill -9). */
export function killServer(s: E2EState): void {
  try {
    process.kill(s.pid, 'SIGKILL');
  } catch {
    /* already gone */
  }
}

export async function restartServer(): Promise<void> {
  const s = readState();
  const pid = startServer(s);
  writeState({ ...s, pid });
  await waitHealthy(s.url);
}

export function nexusd(s: Pick<E2EState, 'bin' | 'conf'>, args: string[], input?: string) {
  const r = spawnSync(s.bin, ['--config', s.conf, ...args], { input, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`nexusd ${args.join(' ')} failed: ${r.stdout}${r.stderr}`);
  return r;
}

export const joinCode = (dir: string) => readFileSync(join(dir, 'data', 'join_code'), 'utf8').trim();
export const exists = existsSync;
