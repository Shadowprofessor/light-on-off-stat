import { env, noStore, TS_API } from './_lib.js';

/**
 * Reads the newest channel entry, mirroring
 *   GET /channels/:id/feeds/last.json
 * so the dashboard's existing parser needs no changes.
 */
export default async function handler(req, res) {
  noStore(res);

  const { channelId, readKey } = env();
  if (!channelId) {
    return res.status(501).json({ error: 'TS_CHANNEL_ID is not set on the server' });
  }

  const url = new URL(
    TS_API + '/channels/' + encodeURIComponent(channelId) + '/feeds/last.json'
  );
  if (readKey) url.searchParams.set('api_key', readKey);

  try {
    const upstream = await fetch(url, {
      cache: 'no-store',
      headers: { accept: 'application/json' },
    });
    const body = await upstream.text();

    if (!upstream.ok) {
      return res.status(502).json({
        error: 'ThingSpeak returned HTTP ' + upstream.status,
        detail: body.slice(0, 200),
      });
    }

    // ThingSpeak answers "-1" for an empty or unauthorised channel. That is
    // valid JSON, and the dashboard already reads it as "no data", so pass it
    // through untouched rather than inventing an error shape for it.
    res.status(200);
    res.setHeader('Content-Type', 'application/json; charset=utf-8');
    res.send(body);
  } catch (err) {
    res.status(502).json({ error: 'Upstream ThingSpeak read failed: ' + err.message });
  }
}
