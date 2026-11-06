// Drawer (DESIGN §6): right side, 480 px, slides in over 320 ms. Closes on
// Escape or a click outside. Focus moves to the drawer panel when it opens
// and back to where it was when it closes. Not modal: the page behind stays
// usable, so it does not trap focus.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { X } from 'lucide-react';
import { Icon } from './icons';

const CLOSE_MS = 320; /* --t-state */

interface Props {
  open: boolean;
  onClose: () => void;
  labelledBy: string;
  children: ReactNode;
}

export function Drawer({ open, onClose, labelledBy, children }: Props) {
  const [mounted, setMounted] = useState(open);
  const [closing, setClosing] = useState(false);
  const panel = useRef<HTMLDivElement>(null);
  const returnTo = useRef<HTMLElement | null>(null);
  const onCloseRef = useRef(onClose);
  useEffect(() => {
    onCloseRef.current = onClose;
  });

  useEffect(() => {
    if (open) {
      returnTo.current = document.activeElement as HTMLElement | null;
      setMounted(true);
      setClosing(false);
      return;
    }
    if (!mounted) return;
    setClosing(true);
    const t = setTimeout(() => {
      setMounted(false);
      setClosing(false);
      // The opener may be gone (a deleted row): then focus stays where the
      // closing action put it.
      if (returnTo.current?.isConnected) returnTo.current.focus();
    }, CLOSE_MS);
    return () => clearTimeout(t);
  }, [open, mounted]);

  useEffect(() => {
    if (!open || !mounted) return;
    // Focus the panel itself: it exists before its content loads, and a
    // screen reader then announces the dialog by its name.
    panel.current?.focus();
    const key = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || e.defaultPrevented) return;
      // Escape in a text field outside the drawer (the search box) belongs
      // to that field.
      const t = e.target as Element;
      if (!panel.current?.contains(t) && t.matches?.('input:not([type=checkbox]), textarea, select')) return;
      onCloseRef.current();
    };
    const down = (e: PointerEvent) => {
      const t = e.target as Node;
      if (panel.current && !panel.current.contains(t) && !(t as Element).closest?.('[data-drawer-keep]'))
        onCloseRef.current();
    };
    document.addEventListener('keydown', key);
    document.addEventListener('pointerdown', down);
    return () => {
      document.removeEventListener('keydown', key);
      document.removeEventListener('pointerdown', down);
    };
  }, [open, mounted]);

  if (!mounted) return null;
  return (
    <div
      ref={panel}
      className={closing ? 'drawer drawer--closing' : 'drawer'}
      role="dialog"
      aria-labelledby={labelledBy}
      aria-modal="false"
      tabIndex={-1}
    >
      <button type="button" className="icon-btn drawer__close" onClick={onClose} aria-label="Close details">
        <Icon of={X} />
      </button>
      {children}
    </div>
  );
}
