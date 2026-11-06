import { useEffect } from 'react';
import { Link } from 'react-router-dom';

/** Sets the tab title: "Files · NexusSync". */
export function useTitle(title: string): void {
  useEffect(() => {
    document.title = title ? `${title} · NexusSync` : 'NexusSync';
  }, [title]);
}

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
