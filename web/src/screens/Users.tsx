// Users (DESIGN §8.7, admin): the join code with Copy and Rotate, then one
// row per person: name, username, role, files, stored size, last seen,
// status. Actions: Make admin / Make student, Disable / Enable. Your own
// row has none (D-046: an admin cannot lock themself out). Disabling asks
// inline first, because it signs that person out everywhere at once.
import { useCallback, useEffect, useRef, useState } from 'react';
import { ApiError, api } from '../api/client';
import type { AdminUser } from '../api/types';
import { useUser } from '../app/auth';
import { useConnection, useNow } from '../app/connection';
import { formatAgo, formatBytes } from '../app/format';
import { UserX } from 'lucide-react';
import { Button } from '../components/Button';
import { Icon } from '../components/icons';
import { JoinCode } from '../components/JoinCode';
import { useToast } from '../components/Toast';
import { useTitle } from './Pages';

export function Users() {
  useTitle('Users');
  const me = useUser();
  const toast = useToast();
  const now = useNow(15_000);
  const { epoch } = useConnection();
  const [users, setUsers] = useState<AdminUser[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<number | null>(null);
  const [confirm, setConfirm] = useState<number | null>(null);
  // Where keyboard focus goes after an action replaces the focused button
  // (review of M11: confirms and busy buttons used to drop focus to <body>).
  const list = useRef<HTMLUListElement>(null);
  const [focusNext, setFocusNext] = useState<string | null>(null);
  useEffect(() => {
    if (!focusNext) return;
    list.current?.querySelector<HTMLElement>(`[data-focus="${focusNext}"]`)?.focus();
    setFocusNext(null);
  }, [focusNext, users, confirm]);

  const load = useCallback(async () => {
    try {
      const r = await api.adminUsers();
      setUsers(
        [...r.items].sort((a, b) =>
          a.role !== b.role ? (a.role === 'admin' ? -1 : 1) : a.displayName.localeCompare(b.displayName),
        ),
      );
      setError(null);
    } catch (e) {
      if (!(e instanceof ApiError && e.status === 401)) setError('Could not load the users.');
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load, epoch]);

  const change = async (
    u: AdminUser,
    c: { role?: 'admin' | 'student'; status?: 'active' | 'disabled' },
    done: string,
    focus: string,
    undo?: { role?: 'admin' | 'student'; status?: 'active' | 'disabled' },
  ) => {
    setBusy(u.id);
    try {
      const nu = await api.adminUpdateUser(u.id, c);
      setUsers((all) => all?.map((x) => (x.id === u.id ? nu : x)) ?? null);
      setConfirm(null);
      setFocusNext(focus);
      toast({
        text: done,
        ...(undo
          ? {
              action: {
                label: 'Undo',
                run: () => void change(nu, undo, `Changed ${u.displayName} back.`, focus),
              },
            }
          : {}),
      });
    } catch (e) {
      toast({ text: `Could not change ${u.displayName}. ${e instanceof ApiError ? e.message : ''}`.trim() });
    } finally {
      setBusy(null);
    }
  };

  return (
    <>
      <div className="page-head">
        <h1>Users</h1>
        <JoinCode />
      </div>
      {error ? (
        <p className="inline-error" role="alert">
          {error}{' '}
          <button type="button" className="text-btn" onClick={() => void load()}>
            Retry
          </button>
        </p>
      ) : null}
      {users === null ? (
        !error ? (
          <ul className="rows" aria-hidden="true">
            {[0, 1, 2].map((i) => (
              <li key={i} className="row row--user">
                <span className="skeleton skeleton--line" />
              </li>
            ))}
          </ul>
        ) : null
      ) : (
        <ul ref={list} className="rows" aria-label="Users" data-testid="users">
          {users.map((u) => {
            const self = u.id === me.id;
            const disabled = u.status === 'disabled';
            return (
              <li key={u.id} className={disabled ? 'row row--user row--disabled' : 'row row--user'} data-testid="user-row">
                <div className="row__main">
                  <span className="row__name">
                    {u.displayName}
                    {self ? <span className="meta"> (you)</span> : null}
                  </span>
                  <span className="meta mono">{u.username}</span>
                  {/* Below 1200 px the columns fold into this line (DESIGN §8.7 keeps every field). */}
                  <span className="meta row__summary">
                    {u.role === 'admin' ? 'Admin' : 'Student'} · {u.files === 1 ? '1 file' : `${u.files.toLocaleString('en-US')} files`},{' '}
                    {formatBytes(u.bytes)} · {u.lastSeenAt ? `seen ${formatAgo(Date.parse(u.lastSeenAt), now)}` : 'never signed in'}
                  </span>
                </div>
                <span className="row__cell">{u.role === 'admin' ? 'Admin' : 'Student'}</span>
                <span className="row__cell">
                  {u.files === 1 ? '1 file' : `${u.files.toLocaleString('en-US')} files`}
                  <span className="meta">, {formatBytes(u.bytes)}</span>
                </span>
                <span className="row__cell meta">
                  {u.lastSeenAt ? `Seen ${formatAgo(Date.parse(u.lastSeenAt), now)}` : 'Never signed in'}
                </span>
                <span className={disabled ? 'row__cell state-dot state-dot--danger' : 'row__cell state-dot state-dot--ok'}>
                  {disabled ? 'Disabled' : 'Active'}
                </span>
                <div className="row__buttons">
                  {self ? null : confirm === u.id ? (
                    <div
                      className="confirm confirm--inline"
                      role="group"
                      aria-label={`Disable ${u.displayName}`}
                      onKeyDown={(e) => {
                        if (e.key !== 'Escape') return;
                        e.preventDefault();
                        setConfirm(null);
                        setFocusNext(`disable-${u.id}`);
                      }}
                    >
                      <p>Disable {u.displayName}? They are signed out everywhere and cannot sign in. Their files stay.</p>
                      <div className="actions">
                        <Button
                          variant="danger"
                          className="btn--danger-solid"
                          autoFocus
                          loading={busy === u.id}
                          icon={<Icon of={UserX} />}
                          onClick={() =>
                            void change(u, { status: 'disabled' }, `${u.displayName} is disabled and signed out.`, `enable-${u.id}`)
                          }
                        >
                          Disable
                        </Button>
                        <Button
                          variant="secondary"
                          onClick={() => {
                            setConfirm(null);
                            setFocusNext(`disable-${u.id}`);
                          }}
                        >
                          Keep active
                        </Button>
                      </div>
                    </div>
                  ) : (
                    <>
                      <button
                        type="button"
                        className="text-btn"
                        data-focus={`role-${u.id}`}
                        aria-disabled={busy === u.id || undefined}
                        onClick={() => {
                          if (busy === u.id) return;
                          const to = u.role === 'admin' ? 'student' : 'admin';
                          void change(
                            u,
                            { role: to },
                            to === 'student' ? `${u.displayName} is now a student.` : `${u.displayName} is now an admin.`,
                            `role-${u.id}`,
                            { role: u.role },
                          );
                        }}
                      >
                        {u.role === 'admin' ? 'Make student' : 'Make admin'}
                      </button>
                      {disabled ? (
                        <button
                          type="button"
                          className="text-btn"
                          data-focus={`enable-${u.id}`}
                          aria-disabled={busy === u.id || undefined}
                          onClick={() =>
                            busy !== u.id && void change(u, { status: 'active' }, `${u.displayName} can sign in again.`, `disable-${u.id}`)
                          }
                        >
                          Enable
                        </button>
                      ) : (
                        <button type="button" className="text-btn" data-focus={`disable-${u.id}`} onClick={() => setConfirm(u.id)}>
                          Disable
                        </button>
                      )}
                    </>
                  )}
                </div>
              </li>
            );
          })}
        </ul>
      )}
    </>
  );
}
