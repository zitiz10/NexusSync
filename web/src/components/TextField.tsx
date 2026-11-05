import { useId, useState, type InputHTMLAttributes, type Ref } from 'react';

interface Props extends Omit<InputHTMLAttributes<HTMLInputElement>, 'id'> {
  label: string;
  hint?: string;
  error?: string | undefined;
  /** Adds a Show/Hide control for password fields. */
  revealable?: boolean;
  inputRef?: Ref<HTMLInputElement>;
}

/** Label above, hint and error below (DESIGN §6: never placeholder-only). */
export function TextField({ label, hint, error, revealable, type, inputRef, ...rest }: Props) {
  const id = useId();
  const [shown, setShown] = useState(false);
  const described = [error ? `${id}-error` : '', hint ? `${id}-hint` : ''].filter(Boolean).join(' ');
  const input = (
    <input
      id={id}
      ref={inputRef}
      className="input"
      type={revealable && shown ? 'text' : type}
      aria-invalid={error ? true : undefined}
      aria-describedby={described || undefined}
      {...rest}
    />
  );
  return (
    <div className="field">
      <div className="field__label-row">
        <label className="field__label" htmlFor={id}>
          {label}
        </label>
      </div>
      {revealable ? (
        <div className="input-wrap">
          {input}
          <span className="input-wrap__action">
            <button
              type="button"
              className="text-btn"
              aria-controls={id}
              aria-pressed={shown}
              onClick={() => setShown((s) => !s)}
            >
              {shown ? 'Hide' : 'Show'}
              <span className="visually-hidden"> password</span>
            </button>
          </span>
        </div>
      ) : (
        input
      )}
      {/* The hint (the rule) stays visible next to an error (what went wrong). */}
      {hint ? (
        <p className="field__hint" id={`${id}-hint`}>
          {hint}
        </p>
      ) : null}
      {error ? (
        <p className="field__error" id={`${id}-error`}>
          {error}
        </p>
      ) : null}
    </div>
  );
}
