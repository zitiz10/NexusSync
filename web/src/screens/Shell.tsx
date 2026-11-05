import { NavLink, Outlet } from 'react-router-dom';
import { useUser } from '../app/auth';
import { useConnection } from '../app/connection';
import { Brand } from '../components/Mark';
import { OfflineBanner } from '../components/OfflineBanner';
import { StatusIndicator } from '../components/StatusIndicator';
import { UserMenu } from '../components/UserMenu';

const LINKS = [
  { to: '/', label: 'Overview', end: true, admin: false },
  { to: '/files', label: 'Files', end: false, admin: false },
  { to: '/compare', label: 'Compare', end: false, admin: false },
  { to: '/engine', label: 'Engine', end: false, admin: true },
  { to: '/users', label: 'Users', end: false, admin: true },
];

/** App shell (DESIGN §7): brand, nav, status, user menu; banner; screen. */
export function Shell() {
  const user = useUser();
  const { link } = useConnection();
  const isAdmin = user.role === 'admin';
  return (
    <>
      <a className="skip-link" href="#main">
        Skip to content
      </a>
      <header>
        <div className="shell-header">
          <div className="shell-header__brand">
            <Brand to="/" />
          </div>
          <nav className="shell-header__nav" aria-label="Main">
            <ul className="nav">
              {LINKS.filter((l) => !l.admin || isAdmin).map((l) => (
                <li key={l.to}>
                  <NavLink className="nav__link" to={l.to} end={l.end}>
                    <span className="nav__text">{l.label}</span>
                  </NavLink>
                </li>
              ))}
            </ul>
          </nav>
          <div className="shell-header__end">
            <StatusIndicator />
            <UserMenu />
          </div>
        </div>
        <OfflineBanner />
      </header>
      {/* Data stays visible while offline, dimmed (DESIGN §8.8). */}
      <main id="main" tabIndex={-1} className={link.state === 'offline' ? 'main main--dimmed' : 'main'}>
        <Outlet />
      </main>
    </>
  );
}

/** Shown while we find out who is signed in: shaped like the shell. */
export function ShellSkeleton() {
  return (
    <div aria-busy="true" aria-label="Loading">
      <div className="shell-header">
        <Brand />
        <span className="skeleton skeleton--nav" />
      </div>
      <div className="main">
        <div className="page-head">
          <span className="skeleton skeleton--h1" />
          <span className="skeleton skeleton--line" />
        </div>
      </div>
    </div>
  );
}
