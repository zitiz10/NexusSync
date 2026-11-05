import type { ButtonHTMLAttributes, ReactNode } from 'react';

interface Props extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'danger';
  block?: boolean;
  /** Shows a spinner in the icon slot; the label stays (DESIGN §6). */
  loading?: boolean;
  icon?: ReactNode;
}

export function Button({ variant = 'primary', block, loading, icon, children, className, disabled, ...rest }: Props) {
  const cls = ['btn', `btn--${variant}`, block ? 'btn--block' : '', className ?? ''].filter(Boolean).join(' ');
  return (
    <button className={cls} disabled={disabled || loading} aria-busy={loading || undefined} {...rest}>
      {icon && !loading ? icon : null}
      {loading ? <span className={icon ? 'spinner' : 'spinner btn__spinner'} aria-hidden="true" /> : null}
      {children}
    </button>
  );
}
