// The join code, with Copy and Rotate (DESIGN §8.6 Server, §8.7 Users).
// Rotating asks inline first: "Anyone with the old code can no longer join."
import { useEffect, useRef, useState } from 'react';
import { ApiError, api } from '../api/client';
import { copyText } from '../app/clipboard';
import { RefreshCw } from 'lucide-react';
import { Button } from './Button';
import { Icon } from './icons';
import { useToast } from './Toast';

export function JoinCode() {
  const toast = useToast();
  const [code, setCode] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState<'yes' | 'no' | null>(null);
  const [rotated, setRotated] = useState(false);
  const copyBtn = useRef<HTMLButtonElement>(null);
  const rotateBtn = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (!copied) return;
    const t = setTimeout(() => setCopied(null), 3000);
    return () => clearTimeout(t);
  }, [copied]);

  useEffect(() => {
    api
      .joinCode()
      .then((r) => setCode(r.joinCode))
      .catch(() => setError('Could not load the join code.'));
  }, []);

  const rotate = async () => {
    setBusy(true);
    try {
      const r = await api.rotateJoinCode();
      setCode(r.joinCode);
      setConfirming(false);
      setError(null);
      setRotated(true);
      requestAnimationFrame(() => copyBtn.current?.focus()); /* next step: copy the new code */
      toast({ text: 'New join code ready. The old one no longer works.' });
    } catch (e) {
      setError(e instanceof ApiError ? e.message : 'Could not rotate the join code.');
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="joincode" data-testid="join-code">
      <div className="joincode__row">
        <span className="field__label">Join code</span>
        {/* Announced only after a rotate, not on every page load. */}
        <span className="mono joincode__value" aria-live={rotated ? 'polite' : 'off'}>
          {code ?? '········'}
        </span>
        <button
          ref={copyBtn}
          type="button"
          className="text-btn"
          disabled={!code}
          onClick={() => code && void copyText(code).then((ok) => setCopied(ok ? 'yes' : 'no'))}
        >
          Copy
        </button>
        {!confirming ? (
          <button ref={rotateBtn} type="button" className="text-btn" disabled={!code} onClick={() => setConfirming(true)}>
            Rotate
          </button>
        ) : null}
      </div>
      <p className="meta" aria-live="polite">
        {copied === 'yes' ? 'Copied.' : copied === 'no' ? 'Could not copy. Select the code and copy it.' : 'Students type this code once, when they create their account.'}
      </p>
      {confirming ? (
        <div
          className="confirm confirm--quiet"
          role="group"
          aria-label="Confirm rotate"
          onKeyDown={(e) => {
            if (e.key !== 'Escape') return;
            e.preventDefault();
            setConfirming(false);
            requestAnimationFrame(() => rotateBtn.current?.focus());
          }}
        >
          <p>Anyone with the old code can no longer join. Accounts already created keep working.</p>
          <div className="actions">
            <Button autoFocus loading={busy} icon={<Icon of={RefreshCw} />} onClick={() => void rotate()}>
              Rotate the code
            </Button>
            <Button
              variant="secondary"
              onClick={() => {
                setConfirming(false);
                requestAnimationFrame(() => rotateBtn.current?.focus());
              }}
            >
              Keep it
            </Button>
          </div>
        </div>
      ) : null}
      {error ? (
        <p className="inline-error" role="alert">
          {error}
        </p>
      ) : null}
    </div>
  );
}
