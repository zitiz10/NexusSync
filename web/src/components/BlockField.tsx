// The block field (DESIGN §8.2): the physical store as 32 × 12 tiles in
// storage order, each shaded by how many files share its blocks.
//
// - Entrance: tiles settle in once, staggered by column and row (§9).
// - Linked hover: `highlight` keeps those tiles and outlines them; the rest
//   dim (the caption, owned by the screen, says which file).
// - Live: pulse(tiles) makes tiles pulse once where new blocks landed.
// All motion is CSS; prefers-reduced-motion removes it.
import { forwardRef, useImperativeHandle, useRef, type CSSProperties } from 'react';

export const COLS = 32;
export const ROWS = 12;
export const TILES = COLS * ROWS;

export interface BlockFieldHandle {
  pulse: (tiles: number[]) => void;
}

interface Props {
  /** Share level 0–4 per tile; null while loading. */
  levels: number[] | null;
  highlight: ReadonlySet<number> | null;
  /** Play the entrance (first load of the page only). */
  enter: boolean;
  label: string;
}

export const BlockField = forwardRef<BlockFieldHandle, Props>(function BlockField(
  { levels, highlight, enter, label },
  ref,
) {
  const tiles = useRef<(HTMLSpanElement | null)[]>([]);
  useImperativeHandle(ref, () => ({
    pulse(list: number[]) {
      for (const i of list) {
        const el = tiles.current[i];
        if (!el) continue;
        el.classList.remove('tile--pulse');
        void el.offsetWidth; /* restart the animation for a second pulse */
        el.classList.add('tile--pulse');
      }
    },
  }));
  const cls = ['bf', enter && levels ? 'bf--enter' : '', highlight ? 'bf--focus' : '', levels ? '' : 'bf--loading']
    .filter(Boolean)
    .join(' ');
  return (
    <div className={cls} role="img" aria-label={label} data-testid="block-field">
      {Array.from({ length: TILES }, (_, i) => {
        const level = levels?.[i] ?? 0;
        const hit = highlight?.has(i) ?? false;
        const style = { '--c': i % COLS, '--r': Math.floor(i / COLS) } as CSSProperties;
        return (
          <span
            key={i}
            ref={(el) => {
              tiles.current[i] = el;
            }}
            className={`tile tile--l${level}${hit ? ' tile--hit' : ''}`}
            style={style}
            onAnimationEnd={(e) => e.currentTarget.classList.remove('tile--pulse')}
          />
        );
      })}
    </div>
  );
});

export function FieldLegend() {
  return (
    <ul className="legend" aria-label="Tile shades">
      <li>
        <span className="legend__swatch tile--l1" aria-hidden="true" />
        Written once
      </li>
      <li>
        <span className="legend__swatch tile--l2" aria-hidden="true" />2 files
      </li>
      <li>
        <span className="legend__swatch tile--l3" aria-hidden="true" />
        3–4 files
      </li>
      <li>
        <span className="legend__swatch tile--l4" aria-hidden="true" />
        5 or more
      </li>
    </ul>
  );
}
