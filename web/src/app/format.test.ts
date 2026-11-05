import { describe, expect, it } from 'vitest';
import { formatBytes } from './format';

describe('formatBytes', () => {
  it('uses bytes with separators below 1 KB', () => {
    expect(formatBytes(0)).toBe('0 bytes');
    expect(formatBytes(1023)).toBe('1,023 bytes');
  });
  it('uses KB and MB', () => {
    expect(formatBytes(12 * 1024)).toBe('12 KB');
    expect(formatBytes(5.8 * 1024 * 1024)).toBe('5.8 MB');
  });
  it('uses one decimal for GB', () => {
    expect(formatBytes(214.6 * 1024 ** 3)).toBe('214.6 GB');
  });
  it('rejects negative input', () => {
    expect(() => formatBytes(-1)).toThrow();
  });
});
