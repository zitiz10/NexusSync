// Motion helpers (DESIGN §9). CSS owns every transition and animation; the
// only motion in script is the number tween, which CSS cannot do.
import { useEffect, useRef, useState } from 'react';

/** DESIGN §9 "Overview numbers: tween to new value on change only, 400 ms". */
export const TWEEN_MS = 400;

export function prefersReducedMotion(): boolean {
  return typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
}

/**
 * The value, tweened from the previous one when it changes. The first
 * value is shown as is: no count-up on load (DESIGN §11).
 */
export function useTween(value: number): number {
  const [shown, setShown] = useState(value);
  const from = useRef(value);
  const raf = useRef(0);
  useEffect(() => {
    const start = from.current;
    if (start === value || prefersReducedMotion()) {
      from.current = value;
      setShown(value);
      return;
    }
    const t0 = performance.now();
    const step = (t: number) => {
      const k = Math.min(1, (t - t0) / TWEEN_MS);
      const eased = 1 - Math.pow(1 - k, 3);
      const v = start + (value - start) * eased;
      from.current = v;
      setShown(k < 1 ? v : value);
      if (k < 1) raf.current = requestAnimationFrame(step);
    };
    raf.current = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf.current);
  }, [value]);
  return shown;
}
