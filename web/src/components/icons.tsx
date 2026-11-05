// Lucide icons at the one size and stroke DESIGN §5 allows (18 px, 1.5).
import type { LucideIcon } from 'lucide-react';

export const ICON_SIZE = 18;
export const ICON_STROKE = 1.5;

export function Icon({ of: Of }: { of: LucideIcon }) {
  return <Of size={ICON_SIZE} strokeWidth={ICON_STROKE} aria-hidden="true" />;
}
