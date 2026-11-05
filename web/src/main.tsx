import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app/App';
import { applyTheme, readTheme } from './app/theme';
import './styles/tokens.css';
import './styles/base.css';
import './styles/components.css';

// Apply the saved theme before the first paint. (An inline script in
// index.html would be earlier, but the CSP allows scripts from 'self' only.)
applyTheme(readTheme());

const root = document.getElementById('root');
if (!root) throw new Error('missing #root');

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
