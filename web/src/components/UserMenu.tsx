import { Check, LogOut } from 'lucide-react';
import { useEffect, useId, useRef, useState, type KeyboardEvent } from 'react';
import { useAuth, useUser } from '../app/auth';
import { initialOf } from '../app/format';
import { useTheme, type ThemeChoice } from '../app/theme';
import { Icon } from './icons';

const THEMES: { value: ThemeChoice; label: string }[] = [
  { value: 'light', label: 'Light' },
  { value: 'dark', label: 'Dark' },
  { value: 'system', label: 'System' },
];

/**
 * Initial in a circle and the display name; the menu holds Theme and Sign
 * out (DESIGN §7). Keyboard: Enter/Space/ArrowDown opens, arrows move,
 * Escape and Tab close, focus returns to the button.
 */
export function UserMenu() {
  const user = useUser();
  const { signOut } = useAuth();
  const { theme, setTheme } = useTheme();
  const [open, setOpen] = useState(false);
  const [signingOut, setSigningOut] = useState(false);
  const [signOutFailed, setSignOutFailed] = useState(false);
  const menuId = useId();
  const root = useRef<HTMLDivElement>(null);
  const button = useRef<HTMLButtonElement>(null);
  const menu = useRef<HTMLDivElement>(null);

  const items = () => Array.from(menu.current?.querySelectorAll<HTMLButtonElement>('[role^="menuitem"]') ?? []);

  useEffect(() => {
    if (!open) return undefined;
    const first = items().find((el) => el.getAttribute('aria-checked') === 'true') ?? items()[0];
    first?.focus();
    const onDown = (e: MouseEvent) => {
      if (root.current && !root.current.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener('mousedown', onDown);
    return () => document.removeEventListener('mousedown', onDown);
  }, [open]);

  const close = (refocus: boolean) => {
    setOpen(false);
    if (refocus) button.current?.focus();
  };

  const onMenuKey = (e: KeyboardEvent) => {
    const list = items();
    const i = list.indexOf(document.activeElement as HTMLButtonElement);
    if (e.key === 'Escape') {
      e.preventDefault();
      close(true);
    } else if (e.key === 'Tab') {
      close(false);
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const next = (i + (e.key === 'ArrowDown' ? 1 : -1) + list.length) % list.length;
      list[next]?.focus();
    } else if (e.key === 'Home' || e.key === 'End') {
      e.preventDefault();
      (e.key === 'Home' ? list[0] : list[list.length - 1])?.focus();
    }
  };

  return (
    <div className="user" ref={root}>
      <button
        ref={button}
        type="button"
        className="user__button"
        aria-haspopup="menu"
        aria-expanded={open}
        aria-controls={open ? menuId : undefined}
        onClick={() => setOpen((o) => !o)}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown' && !open) {
            e.preventDefault();
            setOpen(true);
          }
        }}
      >
        <span className="avatar" aria-hidden="true">
          {initialOf(user.displayName)}
        </span>
        <span className="user__name">{user.displayName}</span>
        <span className="visually-hidden">, account menu</span>
      </button>
      {open ? (
        <div className="menu" id={menuId} role="menu" aria-label="Account" ref={menu} onKeyDown={onMenuKey}>
          <div className="menu__who" role="none">
            <p className="menu__name">{user.displayName}</p>
            <p className="meta">
              {user.username}, {user.role === 'admin' ? 'lab admin' : 'student'}
            </p>
          </div>
          <div role="group" aria-labelledby={`${menuId}-theme`}>
          <p className="menu__label" id={`${menuId}-theme`} role="none">
            Theme
          </p>
          {THEMES.map((t) => (
            <button
              key={t.value}
              type="button"
              role="menuitemradio"
              aria-checked={theme === t.value}
              className="menu__item"
              tabIndex={-1}
              onClick={() => setTheme(t.value)}
            >
              <span className="menu__check" aria-hidden="true">
                {theme === t.value ? <Icon of={Check} /> : null}
              </span>
              {t.label}
            </button>
          ))}
          </div>
          <div className="menu__sep" role="separator" />
          <button
            type="button"
            role="menuitem"
            className="menu__item"
            tabIndex={-1}
            disabled={signingOut}
            onClick={async () => {
              setSigningOut(true);
              setSignOutFailed(false);
              const ok = await signOut();
              if (!ok) {
                setSignOutFailed(true);
                setSigningOut(false);
              }
            }}
          >
            <span className="menu__check" aria-hidden="true">
              <Icon of={LogOut} />
            </span>
            Sign out
          </button>
          {signOutFailed ? (
            <p className="menu__note" role="alert">
              Signing out needs the server. Try again when the connection is back.
            </p>
          ) : null}
        </div>
      ) : null}
    </div>
  );
}
