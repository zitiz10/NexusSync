// A QR code for a LAN address (DESIGN §8.6 "Open on another laptop").
// Drawn as our own SVG from the module matrix of the `qrcode` package, so
// colours come from tokens (--qr-fg on --qr-bg in both themes: phone
// cameras read dark-on-light codes most reliably) and the CSP needs no
// data: URLs.
import { create } from 'qrcode';
import { useMemo } from 'react';

const QUIET = 4; /* the QR standard's quiet zone; matters on the dark theme's page */

export function QrCode({ text, label }: { text: string; label: string }) {
  const path = useMemo(() => {
    const q = create(text, { errorCorrectionLevel: 'M' });
    const n = q.modules.size;
    let d = '';
    for (let r = 0; r < n; r++)
      for (let c = 0; c < n; c++) if (q.modules.get(r, c)) d += `M${c + QUIET} ${r + QUIET}h1v1h-1z`;
    return { d, size: n + 2 * QUIET };
  }, [text]);
  return (
    <svg className="qr" viewBox={`0 0 ${path.size} ${path.size}`} role="img" aria-label={label} shapeRendering="crispEdges">
      <rect className="qr__bg" width={path.size} height={path.size} />
      <path className="qr__fg" d={path.d} />
    </svg>
  );
}
