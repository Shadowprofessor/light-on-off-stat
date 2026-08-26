import { env, noStore, queryOf, readToken, tokenOk, TS_API } from './_lib.js';

/** The three channel fields are booleans, so only "0" and "1" are accepted. */
function bit(v) {
  if (v === undefined || v === null || v === '') return null;   // absent
  const s = String(v).trim();
  if (s === '0' || s === '1') return s;
  return undefined;                                            // present, invalid
}

/**
 * Writes field1/field2/field3 and returns ThingSpeak's entry id as plain text,
 * exactly as api.thingspeak.com/update does — including the "0" that means
 * "the 15 s channel window is not open yet".
 *
 * Returning the real entry id matters: the ESP8266 works out whether a command
 * came from the dashboard by remembering which entry ids it wrote itself, and
 * the dashboard does the same in reverse. Substituting our own id would break
 * that provenance check on both sides.
 */
export default async function handler(req, res) {
  noStore(res);

  if (req.method !== 'GET' && req.method !== 'POST') {
    res.setHeader('Allow', 'GET, POST');
    return res.status(405).json({ error: 'Method not allowed' });
  }

  const { writeKey, token } = env();
  if (!writeKey) {
    return res.status(501).json({ error: 'TS_WRITE_API_KEY is not set on the server' });
  }
  if (!tokenOk(token, readToken(req))) {
    return res.status(403).json({ error: 'Invalid or missing control token' });
  }

  const src = (req.method === 'POST' && req.body && typeof req.body === 'object')
    ? req.body
    : queryOf(req);

  const f1 = bit(src.field1);
  const f2 = bit(src.field2);
  const f3 = bit(src.field3);

  if (f1 === undefined || f2 === undefined || f3 === undefined) {
    return res.status(400).json({ error: 'field1/field2/field3 must be 0 or 1' });
  }
  // field1 is optional — omitting it leaves the LDR reading alone. The relay
  // and LED are the actual command, so they are required.
  if (f2 === null || f3 === null) {
    return res.status(400).json({ error: 'field2 and field3 are required' });
  }

  const url = new URL(TS_API + '/update');
  url.searchParams.set('api_key', writeKey);
  if (f1 !== null) url.searchParams.set('field1', f1);
  url.searchParams.set('field2', f2);
  url.searchParams.set('field3', f3);

  try {
    const upstream = await fetch(url, { cache: 'no-store' });
    const body = (await upstream.text()).trim();

    if (!upstream.ok) {
      return res.status(502).json({
        error: 'ThingSpeak returned HTTP ' + upstream.status,
        detail: body.slice(0, 200),
      });
    }

    res.status(200);
    res.setHeader('Content-Type', 'text/plain; charset=utf-8');
    res.send(body || '0');
  } catch (err) {
    res.status(502).json({ error: 'Upstream ThingSpeak write failed: ' + err.message });
  }
}
