// Who is signed in. The session cookie is HttpOnly, so the app learns the
// user from /api/me (SPEC §12). A 401 from any request ends the session
// and returns to sign-in with a short explanation (DESIGN §8.8).
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react';
import { ApiError, api, onSessionEnded } from '../api/client';
import type { User } from '../api/types';
import { useConnection } from './connection';

export type AuthState =
  | { status: 'loading' }
  | { status: 'signed-out'; sessionEnded: boolean }
  | { status: 'signed-in'; user: User };

interface AuthValue {
  auth: AuthState;
  signIn: (username: string, password: string) => Promise<void>;
  join: (input: { username: string; displayName: string; password: string; joinCode: string }) => Promise<void>;
  /** Resolves false if the server could not be reached (still signed in). */
  signOut: () => Promise<boolean>;
}

const AuthContext = createContext<AuthValue | null>(null);

export function AuthProvider({ children }: { children: ReactNode }) {
  const { server, setSocketEnabled, link } = useConnection();
  const [auth, setAuth] = useState<AuthState>({ status: 'loading' });

  // Ask who we are once the server can answer (not while it recovers).
  const serverOk = server.kind === 'ok';
  const loading = auth.status === 'loading';
  useEffect(() => {
    if (!serverOk || !loading) return;
    let cancelled = false;
    api
      .me()
      .then((user) => !cancelled && setAuth({ status: 'signed-in', user }))
      .catch((e: unknown) => {
        if (cancelled) return;
        if (e instanceof ApiError && e.status === 401) setAuth({ status: 'signed-out', sessionEnded: false });
        // Network trouble: stay "loading"; the connection layer shows why.
      });
    return () => {
      cancelled = true;
    };
  }, [serverOk, loading]);

  useEffect(
    () =>
      onSessionEnded(() =>
        setAuth((a) => (a.status === 'signed-in' ? { status: 'signed-out', sessionEnded: true } : a)),
      ),
    [],
  );

  const signedIn = auth.status === 'signed-in';
  useEffect(() => setSocketEnabled(signedIn), [signedIn, setSocketEnabled]);

  // The event socket is refused when the session has ended (signed out on
  // another tab, account disabled). If the socket is down while the server
  // answers, ask /api/me: a 401 there means the session is gone, not the
  // network.
  useEffect(() => {
    if (!signedIn || !serverOk || link.state === 'live') return;
    let cancelled = false;
    api.me().catch((e: unknown) => {
      if (!cancelled && e instanceof ApiError && e.status === 401)
        setAuth({ status: 'signed-out', sessionEnded: true });
    });
    return () => {
      cancelled = true;
    };
  }, [signedIn, serverOk, link.state, link.nextRetryAt]);

  const signIn = useCallback(async (username: string, password: string) => {
    const user = await api.login(username, password);
    setAuth({ status: 'signed-in', user });
  }, []);

  const join = useCallback(
    async (input: { username: string; displayName: string; password: string; joinCode: string }) => {
      const user = await api.join(input);
      setAuth({ status: 'signed-in', user });
    },
    [],
  );

  // Only the server can end the session: the cookie is HttpOnly, so a
  // "local" sign-out would leave it valid and the next person at a shared
  // lab PC would be signed in again. So stay signed in if it fails.
  const signOut = useCallback(async () => {
    try {
      await api.logout();
    } catch (e) {
      if (!(e instanceof ApiError && e.status === 401)) return false; /* 401: already ended */
    }
    setAuth({ status: 'signed-out', sessionEnded: false });
    return true;
  }, []);

  const value = useMemo(() => ({ auth, signIn, join, signOut }), [auth, signIn, join, signOut]);
  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth(): AuthValue {
  const v = useContext(AuthContext);
  if (!v) throw new Error('useAuth outside AuthProvider');
  return v;
}

/** The signed-in user; only for screens behind the sign-in gate. */
export function useUser(): User {
  const { auth } = useAuth();
  if (auth.status !== 'signed-in') throw new Error('useUser while signed out');
  return auth.user;
}
