import { useEffect } from 'react';
import { Link } from 'react-router-dom';

/** Sets the tab title: "Files · NexusSync". */
export function useTitle(title: string): void {
  useEffect(() => {
    document.title = title ? `${title} · NexusSync` : 'NexusSync';
  }, [title]);
}

/**
 * Screens that arrive in M10 and M11 (SPEC §14). Until then each says so
 * plainly; no placeholder numbers (CLAUDE.md: every number on screen comes
 * from the server).
 */
function Pending({ title }: { title: string }) {
  useTitle(title);
  return (
    <div className="page-head page-head--bare">
      <h1>{title}</h1>
      <p className="lead">Not available yet.</p>
    </div>
  );
}

export const Compare = () => <Pending title="Compare" />;
export const Engine = () => <Pending title="Engine" />;
export const Users = () => <Pending title="Users" />;

export function NotFound() {
  useTitle('Not found');
  return (
    <div className="page-head">
      <h1>Nothing here</h1>
      <p className="lead">
        This address does not match a page.{' '}
        <Link className="link" to="/">
          Go to Overview
        </Link>
        .
      </p>
    </div>
  );
}
