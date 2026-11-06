// Routes and the gates in front of them (DESIGN §7, §8.1, §8.8):
//   1. server recovering / unavailable -> full-page calm state
//   2. not signed in -> /signin (with "session ended" when a 401 caused it)
//   3. admin-only pages -> Overview for students
import type { ReactNode } from 'react';
import { BrowserRouter, Navigate, Route, Routes, useLocation } from 'react-router-dom';
import { Join } from '../screens/Join';
import { Files } from '../screens/Files';
import { Overview } from '../screens/Overview';
import { Compare, Engine, NotFound, Users, useTitle } from '../screens/Pages';
import { Shell, ShellSkeleton } from '../screens/Shell';
import { SignIn } from '../screens/SignIn';
import { Recovered, Recovering, Unavailable, Unreachable } from '../screens/SystemStates';
import { AuthProvider, useAuth } from './auth';
import { ConnectionProvider, useConnection } from './connection';
import { ThemeProvider } from './theme';
import { ToastProvider } from '../components/Toast';
import { UploadsProvider } from './uploads';

function RequireAuth({ children }: { children: ReactNode }) {
  const { auth } = useAuth();
  const loc = useLocation();
  if (auth.status === 'loading') return <ShellSkeleton />;
  if (auth.status === 'signed-out') return <Navigate to="/signin" replace state={{ from: loc.pathname }} />;
  return <>{children}</>;
}

function SignedOutOnly({ children }: { children: ReactNode }) {
  const { auth } = useAuth();
  if (auth.status === 'loading') return <ShellSkeleton />;
  if (auth.status === 'signed-in') return <Navigate to="/" replace />;
  return <>{children}</>;
}

function RequireAdmin({ children }: { children: ReactNode }) {
  const { auth } = useAuth();
  if (auth.status !== 'signed-in' || auth.user.role !== 'admin') return <Navigate to="/" replace />;
  return <>{children}</>;
}

function Titled({ title, children }: { title: string; children: ReactNode }) {
  useTitle(title);
  return <>{children}</>;
}

function Gate() {
  const { server } = useConnection();
  const { auth } = useAuth();
  if (server.kind === 'recovering') return <Recovering health={server.health} />;
  if (server.kind === 'recovered') return <Recovered />;
  if (server.kind === 'unavailable') return <Unavailable status={server.status} />;
  // Unreachable before we know who you are: say so. Signed in already:
  // keep the app on screen with the offline banner instead.
  if (server.kind === 'unreachable' && auth.status === 'loading')
    return <Unreachable nextRetryAt={server.nextRetryAt} />;
  return (
    <Routes>
      <Route
        path="/signin"
        element={
          <SignedOutOnly>
            <Titled title="Sign in">
              <SignIn />
            </Titled>
          </SignedOutOnly>
        }
      />
      <Route
        path="/join"
        element={
          <SignedOutOnly>
            <Titled title="Create your account">
              <Join />
            </Titled>
          </SignedOutOnly>
        }
      />
      <Route
        element={
          <RequireAuth>
            <Shell />
          </RequireAuth>
        }
      >
        <Route index element={<Overview />} />
        <Route path="files" element={<Files />} />
        <Route path="compare" element={<Compare />} />
        <Route
          path="engine"
          element={
            <RequireAdmin>
              <Engine />
            </RequireAdmin>
          }
        />
        <Route
          path="users"
          element={
            <RequireAdmin>
              <Users />
            </RequireAdmin>
          }
        />
        <Route path="*" element={<NotFound />} />
      </Route>
    </Routes>
  );
}

export function App() {
  return (
    <ThemeProvider>
      <ConnectionProvider>
        <AuthProvider>
          <BrowserRouter>
            <ToastProvider>
              <UploadsProvider>
                <Gate />
              </UploadsProvider>
            </ToastProvider>
          </BrowserRouter>
        </AuthProvider>
      </ConnectionProvider>
    </ThemeProvider>
  );
}
