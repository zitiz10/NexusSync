// Sentences on screen (DESIGN §8.2–§8.4, §10).
import { describe, expect, it } from 'vitest';
import type { UploadSnapshot } from '../engine/upload';
import {
  count,
  dedupNote,
  gcProgressLine,
  gcResultLine,
  nextRunLine,
  percent,
  scanFailureLine,
  scanFilesLine,
  scanProgressLine,
  scanResultLine,
  uploadLine,
} from './copy';

const nb = (s: string) => s.replace(/ /g, ' ');

describe('percent', () => {
  it('never claims 100% or 0% that is not exact', () => {
    expect(percent(100)).toBe('100%');
    expect(percent(99.96)).toBe('99.9%');
    expect(percent(99.4)).toBe('99.4%'); /* one decimal above 99 */
    expect(percent(98.6)).toBe('99%');
    expect(percent(94.2)).toBe('94%');
    expect(percent(0.4)).toBe('0.4%');
    expect(percent(0)).toBe('0%');
    expect(percent(Number.NaN)).toBe('0%');
  });
});

describe('count and dedup notes', () => {
  it('pluralises with separators', () => {
    expect(count(1, 'block')).toBe('1 block');
    expect(count(1482, 'block')).toBe('1,482 blocks');
    expect(count(2, 'entry', 'entries')).toBe('2 entries');
  });
  it('says what was stored and what was sent', () => {
    expect(nb(dedupNote(94.2, 1153434))).toBe('94% already stored, 1.1 MB sent');
    expect(nb(dedupNote(0, 3000000))).toBe('All new, 2.9 MB sent');
    expect(dedupNote(100, 0)).toBe('100% already stored, nothing sent');
  });
});

const base: UploadSnapshot = {
  key: 'u1',
  name: 'notes.csv',
  size: 6_081_740,
  phase: 'queued',
  readBytes: 0,
  chunkCount: 1482,
  needCount: 3,
  needBytes: 12288,
  sentBytes: 0,
  rate: null,
  failure: null,
  file: null,
  bytesSent: null,
  startedAt: 0,
};
const line = (p: Partial<UploadSnapshot>) => nb(uploadLine({ ...base, ...p }));

describe('tray lines (DESIGN §8.4)', () => {
  it('names each phase with real numbers', () => {
    expect(line({})).toBe('Waiting to start.');
    expect(line({ phase: 'reading', readBytes: 2_311_100 })).toBe('Fingerprinting blocks, 38%');
    expect(line({ phase: 'checking' })).toBe('1,482 blocks. Asking the server which it already has.');
    expect(line({ phase: 'sending' })).toBe('1,479 already stored. Sending 3 new blocks, 12 KB.');
    expect(line({ phase: 'sending', rate: 2_500_000 })).toBe('1,479 already stored. Sending 3 new blocks, 12 KB, 2.4 MB/s.');
    expect(line({ phase: 'sending', needCount: 0, needBytes: 0 })).toBe('All 1,482 blocks already stored. Nothing to send.');
    expect(line({ phase: 'sending', needCount: 1482, needBytes: 6_081_740 })).toBe('All new. Sending 1,482 blocks, 5.8 MB.');
    expect(line({ phase: 'verifying' })).toBe('Server is checking the whole file.');
    expect(line({ phase: 'done', bytesSent: 12288 })).toBe('Stored. 12 KB sent instead of 5.8 MB.');
    expect(line({ phase: 'done', bytesSent: 0 })).toBe('Stored. Nothing sent: all 5.8 MB was already on the server.');
    expect(line({ phase: 'cancelled' })).toBe('Cancelled.');
  });
  it('gives each failure a plain reason', () => {
    const f = (failure: UploadSnapshot['failure']) => line({ phase: 'failed', failure });
    expect(f({ kind: 'connection', message: '' })).toBe('Connection lost.');
    expect(f({ kind: 'hash', message: '', rejected: 1 })).toBe('The server rejected 1 block that did not match its fingerprint.');
    expect(f({ kind: 'hash', message: '', rejected: 2 })).toBe('The server rejected 2 blocks that did not match their fingerprints.');
    expect(f({ kind: 'disk', message: '' })).toBe('The server is out of space. Ask the lab admin.');
    expect(f({ kind: 'session', message: '' })).toBe('Your session ended before the upload finished.');
  });
});

describe('cleanup and scan lines (M12)', () => {
  const base = {
    trigger: 'admin' as const,
    blocksFreed: 0,
    bytesFreed: 0,
    containersCompacted: 0,
    containersSkipped: 0,
    blocksMoved: 0,
    bytesMoved: 0,
    bytesReclaimed: 0,
    durationMs: 40,
  };
  it('says what cleanup did, from the real counts', () => {
    expect(gcResultLine(base)).toBe('Nothing to clean up. Every stored block is in use.');
    expect(gcResultLine({ ...base, blocksFreed: 3, bytesFreed: 12288 })).toBe(
      'Cleanup freed 3 unused blocks (12\u00a0KB). Disk space comes back once a container is less than half full.',
    );
    expect(
      gcResultLine({ ...base, blocksFreed: 1259, containersCompacted: 6, blocksMoved: 1, bytesReclaimed: 1210368 }),
    ).toBe('Cleanup reclaimed 1.2\u00a0MB: 1,259 unused blocks freed, 6 containers compacted, 1 block moved.');
    expect(gcResultLine({ ...base, containersSkipped: 1 })).toBe(
      'Nothing to clean up. Every stored block is in use. 1 container kept as it is: a block in it is damaged. Run a scan for details.',
    );
  });
  it('names the cleanup step in progress', () => {
    expect(gcProgressLine(null)).toBe('Freeing blocks that no file uses.');
    expect(gcProgressLine({ phase: 'compacting', done: 1, total: 5 })).toBe('Compacting container 2 of 5.');
    expect(gcProgressLine({ phase: 'compacting', done: 5, total: 5 })).toBe('Deleting the compacted containers.');
  });
  it('reports scan progress and results', () => {
    expect(scanProgressLine({ phase: 'scanning', done: 1204, total: 2408, percent: 50 })).toBe(
      'Checking every stored block: 1,204 of 2,408 blocks.',
    );
    expect(scanResultLine({ trigger: 'admin', verified: 2408, failures: 0, durationMs: 300, items: [] })).toBe(
      'Scan finished in under a second: 2,408 blocks verified, no damage found.',
    );
    expect(scanResultLine({ trigger: 'nightly', verified: 2407, failures: 1, durationMs: 2600, items: [] })).toMatch(
      /^Scan finished in 3 s: 1 damaged block found, 2,407 verified\. /,
    );
  });
  it('names a damaged block and the files that use it', () => {
    const f = {
      sha256: '3f9a0c12d4e5f60718293a4b5c6d7e8f3f9a0c12d4e5f60718293a4b5c6d7e8f',
      containerId: 4,
      offset: 4136,
      reason: 'crc' as const,
      files: [
        { id: 2, name: 'b.csv' },
        { id: 1, name: 'a.csv' },
      ],
    };
    expect(scanFailureLine(f)).toBe('Block 3f9a0c12d4e5… in container 4: its bytes changed on disk (checksum mismatch).');
    expect(scanFilesLine(f)).toBe('Used by b.csv and a.csv.');
    expect(scanFilesLine({ ...f, files: [...f.files, { id: 3, name: 'c.csv' }, { id: 4, name: 'd.csv' }] })).toBe(
      'Used by b.csv, a.csv and 2 more files.',
    );
    expect(scanFilesLine({ ...f, files: [] })).toBe('Used by no file; cleanup will remove it.');
  });
  it('writes the next nightly run as a date and time', () => {
    const line = nextRunLine('2026-11-07T02:00:00.000Z');
    expect(line).toMatch(/^[A-Z][a-z]+day \d{1,2} [A-Z][a-z]+ at \d\d:\d\d$/);
    expect(nextRunLine(null)).toBe('Not scheduled');
  });
});
