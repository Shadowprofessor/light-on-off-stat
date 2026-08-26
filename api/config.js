import { env, noStore } from './_lib.js';

/**
 * Tells the dashboard whether this deployment holds the ThingSpeak keys.
 *
 * Returns capability flags only — no key material of any kind. `proxy: false`
 * is a perfectly valid answer: the dashboard then falls back to its direct,
 * browser-side mode.
 */
export default function handler(req, res) {
  noStore(res);

  const { channelId, readKey, writeKey, token } = env();

  res.status(200).json({
    proxy:         Boolean(channelId),
    channelId,
    canRead:       Boolean(channelId),
    canWrite:      Boolean(writeKey),
    requiresToken: Boolean(token),
    hasReadKey:    Boolean(readKey),
  });
}
