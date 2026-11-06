import type { ButtonHTMLAttributes, ReactNode } from 'react';

interface Props extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'danger';
  block?: boolean;
  /** Shows a spinner in the icon slot; the label stays (DESIGN §6). */
  loading?: boolean;
  icon?: ReactNode;
}

export function Button({ variant = 'primary', block, loading, icon, children, className, disabled, onClick, ...rest }: Props) {
  const cls = ['btn', `btn--${variant}`, block ? 'btn--block' : '', className ?? ''].filter(Boolean).join(' ');
  // While loading the button stays focusable (a native disabled button would
  // drop keyboard focus for the whole wait, WCAG 2.4.3) and ignores clicks,
  // including the implicit submit of a form (M12 review, D-049).
  return (
    <button
      className={cls}
      disabled={disabled}
      {...rest}
      aria-disabled={loading || rest['aria-disabled'] || undefined}
      aria-busy={loading || undefined}
      onClick={(e) => {
        if (loading) {
          e.preventDefault();
          return;
        }
        onClick?.(e);
      }}
    >
      {icon && !loading ? icon : null}
      {loading ? <span className={icon ? 'spinner' : 'spinner btn__spinner'} aria-hidden="true" /> : null}
      {children}
    </button>
  );
}
