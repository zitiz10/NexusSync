import { describe, expect, it } from 'vitest';
import { formatBytes } from './format';

// A no-break space joins number and unit, so "5.7 MB" never wraps
// between them (seen in M9: "5.7 / MB" at the end of a line).
const NB = ' ';

describe('formatBytes', () => {
  it('uses bytes with separators below 1 KB', () => {
    expect(formatBytes(0)).toBe(`0${NB}bytes`);
    expect(formatBytes(1023)).toBe(`1,023${NB}bytes`);
  });
  it('uses KB and MB', () => {
    expect(formatBytes(12 * 1024)).toBe(`12${NB}KB`);
    expect(formatBytes(5.8 * 1024 * 1024)).toBe(`5.8${NB}MB`);
  });
  it('uses one decimal for GB', () => {
    expect(formatBytes(214.6 * 1024 ** 3)).toBe(`214.6${NB}GB`);
  });
  it('never puts a breakable space between number and unit', () => {
    for (const n of [0, 5, 2000, 3e6, 7e9, 3e12]) expect(formatBytes(n)).not.toContain(' ');
  });
  it('rejects negative input', () => {
    expect(() => formatBytes(-1)).toThrow();
  });
});
