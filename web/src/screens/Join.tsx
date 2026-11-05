import { useRef, useState, type FormEvent } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { ApiError } from '../api/client';
import { useAuth } from '../app/auth';
import { joinErrorFromServer, validateJoin, type JoinErrors, type JoinInput } from '../app/validate';
import { Brand } from '../components/Mark';
import { Button } from '../components/Button';
import { TextField } from '../components/TextField';

const ORDER: (keyof JoinInput)[] = ['displayName', 'username', 'password', 'joinCode'];

/** DESIGN §8.1 join form: errors inline under the field they belong to. */
export function Join() {
  const { join } = useAuth();
  const nav = useNavigate();
  const [v, setV] = useState<JoinInput>({ displayName: '', username: '', password: '', joinCode: '' });
  const [errors, setErrors] = useState<JoinErrors>({});
  const [formError, setFormError] = useState('');
  const [busy, setBusy] = useState(false);
  const refs = {
    displayName: useRef<HTMLInputElement>(null),
    username: useRef<HTMLInputElement>(null),
    password: useRef<HTMLInputElement>(null),
    joinCode: useRef<HTMLInputElement>(null),
  };
  const set = (k: keyof JoinInput) => (e: { target: { value: string } }) => setV((o) => ({ ...o, [k]: e.target.value }));

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    setFormError('');
    const errs = validateJoin(v);
    setErrors(errs);
    const first = ORDER.find((k) => errs[k]);
    if (first) {
      refs[first].current?.focus();
      return;
    }
    setBusy(true);
    try {
      await join({
        displayName: v.displayName.trim(),
        username: v.username.trim(),
        password: v.password,
        joinCode: v.joinCode.trim().toUpperCase(),
      });
      nav('/', { replace: true });
    } catch (err) {
      const code = err instanceof ApiError ? err.code : 'unknown';
      const msg = err instanceof ApiError ? err.message : '';
      const placed = joinErrorFromServer(code, msg);
      if (placed.field) {
        setErrors({ [placed.field]: placed.text });
        refs[placed.field].current?.focus();
      } else {
        setFormError(placed.text);
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <main className="auth" id="main">
      <div className="auth__col">
        <Brand />
        <h1 className="auth__title">Create your account</h1>
        <form className="auth__form" onSubmit={submit} noValidate>
          <TextField
            label="Display name"
            name="displayName"
            autoComplete="name"
            value={v.displayName}
            onChange={set('displayName')}
            error={errors.displayName}
            inputRef={refs.displayName}
            autoFocus
          />
          <TextField
            label="Username"
            name="username"
            hint="Letters, digits, dot, dash, or underscore; 3 to 32 characters."
            autoComplete="username"
            autoCapitalize="none"
            spellCheck={false}
            value={v.username}
            onChange={set('username')}
            error={errors.username}
            inputRef={refs.username}
          />
          <TextField
            label="Password"
            name="password"
            type="password"
            hint="At least 10 characters."
            autoComplete="new-password"
            revealable
            value={v.password}
            onChange={set('password')}
            error={errors.password}
            inputRef={refs.password}
          />
          <TextField
            label="Join code"
            name="joinCode"
            hint="8 letters and digits from your lab admin, for example K7MPQ2XD."
            autoComplete="off"
            autoCapitalize="characters"
            spellCheck={false}
            value={v.joinCode}
            onChange={set('joinCode')}
            error={errors.joinCode}
            inputRef={refs.joinCode}
          />
          {formError ? (
            <p className="note note--danger" role="alert">
              {formError}
            </p>
          ) : null}
          <Button type="submit" block loading={busy}>
            Create account
          </Button>
        </form>
        <p className="auth__alt">
          Already have an account?{' '}
          <Link className="link" to="/signin">
            Sign in.
          </Link>
        </p>
      </div>
    </main>
  );
}
