// Client-side checks that mirror the server's rules (SPEC §13), so most
// mistakes are caught before a round trip. The server checks again.

const encoder = new TextEncoder();
const bytes = (s: string) => encoder.encode(s).length;

export const JOIN_ALPHABET = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';

export interface JoinInput {
  displayName: string;
  username: string;
  password: string;
  joinCode: string;
}

export type JoinErrors = Partial<Record<keyof JoinInput, string>>;

export function usernameError(u: string): string | undefined {
  if (!u) return 'Enter a username.';
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{2,31}$/.test(u)) return 'That username does not fit the rule above.';
  return undefined;
}

export function displayNameError(d: string): string | undefined {
  const t = d.trim();
  if (!t) return 'Enter your name as others should see it.';
  if (bytes(t) > 64) return 'Use at most 64 characters.';
  // Control characters (C0 range and DEL) are refused by the server too.
  for (const ch of t) {
    const c = ch.codePointAt(0) ?? 0;
    if (c < 0x20 || c === 0x7f) return 'Use letters, digits, spaces, and punctuation only.';
  }
  return undefined;
}

export function passwordError(p: string): string | undefined {
  if (bytes(p) === 0) return 'Enter a password.';
  if (bytes(p) < 10) return 'That password is too short.';
  if (bytes(p) > 256) return 'Use at most 256 characters.';
  return undefined;
}

export function joinCodeError(c: string): string | undefined {
  const t = c.trim().toUpperCase();
  if (!t) return 'Enter the join code from your lab admin.';
  if (t.length !== 8 || [...t].some((ch) => !JOIN_ALPHABET.includes(ch)))
    return 'That does not look like a join code.';
  return undefined;
}

export function validateJoin(v: JoinInput): JoinErrors {
  const e: JoinErrors = {};
  const d = displayNameError(v.displayName);
  const u = usernameError(v.username);
  const p = passwordError(v.password);
  const c = joinCodeError(v.joinCode);
  if (d) e.displayName = d;
  if (u) e.username = u;
  if (p) e.password = p;
  if (c) e.joinCode = c;
  return e;
}

/** Where a server error code belongs on the join form (or the whole form). */
export function joinErrorFromServer(code: string, message: string): { field?: keyof JoinInput; text: string } {
  switch (code) {
    case 'bad_join_code':
      return { field: 'joinCode', text: 'That join code is not valid. Ask the lab admin for the current one.' };
    case 'username_taken':
      return { field: 'username', text: 'That username is taken. Choose another.' };
    case 'invalid_username':
      return { field: 'username', text: 'That username does not fit the rule above.' };
    case 'invalid_display_name':
      return { field: 'displayName', text: 'Use 1 to 64 characters.' };
    case 'weak_password':
      return { field: 'password', text: 'That password is too short.' };
    default:
      return { text: formMessage(code, message) };
  }
}

/** Messages shown above the submit button (DESIGN §8.1, §10). */
export function formMessage(code: string, message: string): string {
  switch (code) {
    case 'too_many_attempts':
      return 'Too many attempts. Try again in a minute.';
    case 'bad_credentials':
      return 'Wrong username or password.';
    case 'account_disabled':
      return 'This account is disabled. Ask the lab admin.';
    case 'network':
      return 'Cannot reach the lab server. Check that you are on the lab network, then try again.';
    case 'recovering':
      return 'The server is starting up. Try again in a moment.';
    default:
      return message || 'Something went wrong. Try again.';
  }
}
