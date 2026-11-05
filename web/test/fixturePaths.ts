import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = join(here, '..', '..');
export const FIXTURE_DIR = join(REPO_ROOT, 'server', 'tests', 'fixtures');
