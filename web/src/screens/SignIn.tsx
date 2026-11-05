import { useRef, useState, type FormEvent } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import { ApiError } from '../api/client';
import { useAuth } from '../app/auth';
import { formMessage } from '../app/validate';
import { Brand } from '../components/Mark';
import { Button } from '../components/Button';
import { TextField } from '../components/TextField';

/** DESIGN §8.1: centered column, max 400 px. */
export function SignIn() {
  const { auth, signIn } = useAuth();
  const nav = useNavigate();
  const loc = useLocation();
  const from = (loc.state as { from?: string } | null)?.from ?? '/';
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [errors, setErrors] = useState<{ username?: string; password?: string; form?: string }>({});
  const [busy, setBusy] = useState(false);
  const userRef = useRef<HTMLInputElement>(null);
  const passRef = useRef<HTMLInputElement>(null);
  const sessionEnded = auth.status === 'signed-out' && auth.sessionEnded;

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const next: typeof errors = {};
    if (!username.trim()) next.username = 'Enter your username.';
    if (!password) next.password = 'Enter your password.';
    setErrors(next);
    if (next.username) return userRef.current?.focus();
    if (next.password) return passRef.current?.focus();
    setBusy(true);
    try {
      await signIn(username.trim(), password);
      nav(from, { replace: true });
    } catch (err) {
      const code = err instanceof ApiError ? err.code : 'unknown';
      const msg = err instanceof ApiError ? err.message : '';
      setErrors({ form: formMessage(code, msg) });
      setPassword('');
      passRef.current?.focus();
    } finally {
      setBusy(false);
    }
  };

  return (
    <main className="auth" id="main">
      <div className="auth__col">
        <Brand />
        <h1 className="auth__title">Sign in to the lab store</h1>
        <form className="auth__form" onSubmit={submit} noValidate>
          {sessionEnded && !errors.form ? (
            <p className="note note--warn" role="status">
              Your session ended. Sign in again.
            </p>
          ) : null}
          <TextField
            label="Username"
            name="username"
            autoComplete="username"
            autoCapitalize="none"
            spellCheck={false}
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            error={errors.username}
            inputRef={userRef}
            autoFocus
          />
          <TextField
            label="Password"
            name="password"
            type="password"
            autoComplete="current-password"
            revealable
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            error={errors.password}
            inputRef={passRef}
          />
          {errors.form ? (
            <p className="note note--danger" role="alert">
              {errors.form}
            </p>
          ) : null}
          <Button type="submit" block loading={busy}>
            Sign in
          </Button>
        </form>
        <p className="auth__alt">
          New here?{' '}
          <Link className="link" to="/join">
            Create an account with your lab's join code.
          </Link>
        </p>
      </div>
    </main>
  );
}
