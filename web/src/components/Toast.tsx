// Toast (DESIGN §6): bottom-left, one at a time, gone after 6 s. Used for
// "Deleted DBMS_notes.csv. Undo". The timer pauses while the pointer is on
// the toast. A toast shown with `focus: true` takes keyboard focus on its
// action (the delete flow, whose button just disappeared), so Undo is one
// Enter away; when the toast closes, focus goes to the main content.
//
// A toast may carry `onExpire`: it runs when the toast times out or is
// replaced by another one, but not when its action was taken. The delayed
// delete relies on that.
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';

export const TOAST_MS = 6000; /* DESIGN §6; --t-toast in tokens.css */

interface ToastSpec {
  text: string;
  /** Move keyboard focus to the action (when the focused control vanished). */
  focus?: boolean;
  action?: { label: string; run: () => void };
  onExpire?: () => void;
}

interface Live extends ToastSpec {
  id: number;
}

const ToastContext = createContext<((t: ToastSpec) => void) | null>(null);

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toast, setToast] = useState<Live | null>(null);
  const current = useRef<Live | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const remaining = useRef(TOAST_MS);
  const startedAt = useRef(0);
  const nextId = useRef(1);

  const clearTimer = () => {
    if (timer.current) clearTimeout(timer.current);
    timer.current = null;
  };
  const actionRef = useRef<HTMLButtonElement>(null);
  const close = useCallback((expired: boolean) => {
    clearTimer();
    const t = current.current;
    current.current = null;
    if (actionRef.current && actionRef.current === document.activeElement)
      document.getElementById('main')?.focus(); /* do not leave focus on a removed button */
    setToast(null);
    if (expired) t?.onExpire?.();
  }, []);
  const arm = useCallback(() => {
    clearTimer();
    startedAt.current = Date.now();
    timer.current = setTimeout(() => close(true), remaining.current);
  }, [close]);

  const show = useCallback(
    (t: ToastSpec) => {
      if (current.current) close(true); /* the replaced toast's work still happens */
      const live = { ...t, id: nextId.current++ };
      current.current = live;
      remaining.current = TOAST_MS;
      setToast(live);
      arm();
    },
    [arm, close],
  );
  const pause = () => {
    if (!timer.current) return;
    clearTimer();
    remaining.current = Math.max(1000, remaining.current - (Date.now() - startedAt.current));
  };
  const resume = () => {
    if (current.current && !timer.current) arm();
  };

  useEffect(() => {
    if (toast?.focus) actionRef.current?.focus();
  }, [toast]);

  // Leaving the page (tab closed) runs pending work now.
  useEffect(() => {
    const flush = () => current.current?.onExpire?.();
    window.addEventListener('pagehide', flush);
    return () => window.removeEventListener('pagehide', flush);
  }, []);

  const value = useMemo(() => show, [show]);
  return (
    <ToastContext.Provider value={value}>
      {children}
      <div className="toast-region" aria-live="polite">
        {toast ? (
          <div
            key={toast.id}
            className="toast"
            onMouseEnter={pause}
            onMouseLeave={resume}
          >
            <span>{toast.text}</span>
            {toast.action ? (
              <button
                ref={actionRef}
                type="button"
                className="toast__action"
                onClick={() => {
                  const a = toast.action;
                  close(false);
                  a?.run();
                }}
              >
                {toast.action.label}
              </button>
            ) : null}
          </div>
        ) : null}
      </div>
    </ToastContext.Provider>
  );
}

export function useToast(): (t: ToastSpec) => void {
  const v = useContext(ToastContext);
  if (!v) throw new Error('useToast outside ToastProvider');
  return v;
}
