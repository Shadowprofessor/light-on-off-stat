/**
 * Shared helpers for the ThingSpeak proxy functions.
 *
 * The API keys live here, on the server, and are never placed in a response
 * body. The browser learns only *that* a proxy exists, plus the channel id —
 * which is not a secret, it is already in the public channel URL.
 *
 * Files under api/ whose name starts with "_" are not turned into routes by
 * Vercel, so this module is importable but not reachable over HTTP.
 */

export const TS_API = 'https://api.thingspeak.com';

/**
 * Values copied straight out of .env.example. Treating them as absent turns
 * "I deployed before filling in the env vars" into the dashboard's clear
 * not-configured message instead of a puzzling upstream read error.
 *
 * Every entry must be something that can never be a real value — a real
 * channel id is a 7-digit number, so no digit strings belong in this list.
 */
const PLACEHOLDERS = new Set([
  'YOUR_CHANNEL_ID', 'YOUR_READ_API_KEY', 'YOUR_WRITE_API_KEY',
  'YOURREADKEY', 'YOURWRITEKEY',
]);

function cfg(name) {
  const v = (process.env[name] || '').trim();
  return PLACEHOLDERS.has(v) ? '' : v;
}

export function env() {
  return {
    channelId: cfg('TS_CHANNEL_ID'),
    readKey:   cfg('TS_READ_API_KEY'),
    writeKey:  cfg('TS_WRITE_API_KEY'),
    token:     cfg('DASHBOARD_TOKEN'),
  };
}

/**
 * Vercel's Node runtime attaches req.query for us. A plain Node server does
 * not, so fall back to parsing the URL — that keeps these handlers usable
 * outside Vercel too.
 */
export function queryOf(req) {
  if (req.query && typeof req.query === 'object') return req.query;

  const out = {};
  const qs = String(req.url || '').split('?')[1];
  if (qs) {
    for (const [k, v] of new URLSearchParams(qs)) out[k] = v;
  }
  return out;
}

/** Nothing this proxy returns may be cached — it is all live device state. */
export function noStore(res) {
  res.setHeader('Cache-Control', 'no-store, max-age=0, must-revalidate');
}

/**
 * Length-then-XOR compare, so a wrong token does not leak its correct prefix
 * through timing. Network jitter dwarfs the difference, but it costs nothing.
 *
 * An unset expected token means no token was configured, so anyone may write.
 */
export function tokenOk(expected, given) {
  if (!expected) return true;
  if (typeof given !== 'string' || given.length !== expected.length) return false;

  let diff = 0;
  for (let i = 0; i < expected.length; i++) {
    diff |= expected.charCodeAt(i) ^ given.charCodeAt(i);
  }
  return diff === 0;
}

/** Pulls the control token from a header, falling back to the query string. */
export function readToken(req) {
  const header = req.headers && req.headers['x-dashboard-token'];
  if (typeof header === 'string' && header) return header;

  const q = queryOf(req).token;
  return typeof q === 'string' ? q : '';
}
