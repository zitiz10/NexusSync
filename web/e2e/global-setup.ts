import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { joinCode, nexusd, startServer, waitHealthy, writeState, type E2EState } from './server';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

function freePort(): Promise<number> {
  return new Promise((ok, fail) => {
    const s = createServer();
    s.listen(0, '127.0.0.1', () => {
      const a = s.address();
      s.close(() => (a && typeof a === 'object' ? ok(a.port) : fail(new Error('no port'))));
    });
  });
}

export default async function globalSetup() {
  // The code under test: a release build of the server and the built app.
  const make = spawnSync('make', ['-C', join(ROOT, 'server')], { encoding: 'utf8' });
  if (make.status !== 0) throw new Error(`make failed:\n${make.stdout}${make.stderr}`);
  const dir = mkdtempSync(join(tmpdir(), 'nexussync-e2e-'));
  const port = await freePort();
  const conf = join(dir, 'nexussync.conf');
  writeFileSync(
    conf,
    [
      `listen = 127.0.0.1:${port}`,
      `data_dir = ${join(dir, 'data')}`,
      `web_root = ${join(ROOT, 'web', 'dist')}`,
      'checkpoint_interval_s = 5',
      '',
    ].join('\n'),
  );
  const bin = join(ROOT, 'server', 'build', process.platform === 'win32' ? 'nexusd.exe' : 'nexusd');
  const admin = { username: 'labadmin', password: 'admin-password-1' };
  nexusd({ bin, conf }, ['--init']);
  nexusd({ bin, conf }, ['--create-admin'], `${admin.username}\nAnita Rao\n${admin.password}\n${admin.password}\n`);
  const url = `http://127.0.0.1:${port}`;
  const state: E2EState = {
    url,
    conf,
    dir,
    bin,
    pid: 0,
    admin,
    student: { username: 'riya', password: 'student-password-1', displayName: 'Riya Kapoor' },
  };
  state.pid = startServer(state);
  process.env['NX_E2E_URL'] = url;
  process.env['NX_E2E_STATE'] = join(dir, 'state.json');
  writeState(state);
  await waitHealthy(url);
  const r = await fetch(`${url}/api/auth/join`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-NexusSync': '1' },
    body: JSON.stringify({ ...state.student, joinCode: joinCode(dir) }),
  });
  if (r.status !== 201) throw new Error(`joining the student failed: ${r.status}`);
}
