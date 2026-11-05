// Make sure the deterministic fixture files exist before tests run. They
// are git-ignored and produced by scripts/make_fixtures.py.
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { FIXTURE_DIR, REPO_ROOT } from './fixturePaths';

export default function setup(): void {
  if (existsSync(join(FIXTURE_DIR, 'data', '.complete'))) return;
  const python = process.platform === 'win32' ? 'python' : 'python3';
  execFileSync(python, [join(REPO_ROOT, 'scripts', 'make_fixtures.py')], { stdio: 'inherit' });
}
