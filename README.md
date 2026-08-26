# IoT Smart Automatic Light Control

An LDR-driven mains light controller with a cloud dashboard. An Arduino Uno
owns the real automation and switches the relay; an ESP8266 bridges it to
ThingSpeak; a static web dashboard shows live state and sends manual
overrides.

```
   LDR ──► Arduino Uno ──► Relay ──► Light
              │  ▲
       serial │  │ serial
              ▼  │
           ESP8266 ──── WiFi ──── ThingSpeak ──── Dashboard (index.html)
```

## Layout

| Path | Role |
|---|---|
| `arduino_ldr/` | Uno sketch — reads the LDR, decides dark/bright, drives relay + LED |
| `esp8266_thingspeak/` | ESP8266 bridge — Arduino ⇄ ThingSpeak |
| `index.html` | The dashboard — live state, AUTO / ON / OFF buttons |
| `api/` | Optional serverless proxy so the API keys stay off the browser |
| `vercel.json` | Deployment config — cache and security headers |
| `.env.example` | The environment variables `api/` reads |

The Arduino, not the ESP, owns the automation. If WiFi drops, the light keeps
working — the cloud layer is for monitoring and manual override only.

The dashboard is still a single self-contained HTML file with no build step.
`api/` is genuinely optional: delete the folder and everything still works,
just with the keys in the browser instead of on a server.

## Wiring

**Arduino Uno**

| Signal | Pin | Notes |
|---|---|---|
| LDR | `A0` | divider: `5V — LDR — A0 — R — GND`, so the reading *rises* as it gets darker |
| Relay IN | `D7` | **active HIGH** — LOW = off, HIGH = on |
| LED | `D13` | active HIGH (onboard) |
| To ESP RX | `D11` | through a 10 kΩ / 20 kΩ divider down to 3.3 V |
| From ESP TX | `D10` | direct, 3.3 V is a valid HIGH for the Uno |
| GND | — | **must be common with the ESP** |

**ESP8266 (NodeMCU / Wemos D1 mini)**

| Signal | Pin |
|---|---|
| From Arduino `D11` | `D5` (GPIO14) |
| To Arduino `D10` | `D6` (GPIO12) |
| GND | common with Arduino |

If your relay module is the common **active LOW** kind instead, flip one line
in `arduino_ldr/arduino_ldr.ino`:

```c
const bool RELAY_ACTIVE_LOW = true;
```

Both the pin drive and the status report derive from that constant, so it is
the only edit needed.

## Threshold

```
analogRead(A0) >= 550    →  DARK    →  relay ON,  LED ON
analogRead(A0) 546..549  →  hold the current decision   (hysteresis)
analogRead(A0) <= 545    →  BRIGHT  →  relay OFF, LED OFF
```

The 546–549 band stops the relay chattering when the reading jitters on the
trip point. Set `HYSTERESIS = 0` for a hard single-point switch at 550.

Watch `[LDR] raw=` in Serial Monitor at **9600 baud** and confirm your room's
readings straddle 550 with margin before trusting it.

## Setup

### 1. Credentials

Nothing secret is committed. Create your own `secrets.h`:

```sh
cd esp8266_thingspeak
cp secrets.example.h secrets.h
```

Then fill in your WiFi and ThingSpeak values. `secrets.h` is git-ignored.
The sketch prefers it and falls back to `secrets.example.h`, so the project
compiles on a fresh clone — it just will not connect until you supply real
values.

### 2. ThingSpeak channel

Create a channel with **exactly three fields**:

| Field | Meaning |
|---|---|
| `field1` | LDR — `1` = dark, `0` = bright |
| `field2` | Relay — `1` = on, `0` = off |
| `field3` | LED — `1` = on, `0` = off |

There is no fourth field. AUTO/MANUAL mode is held in RAM in all three places
and inferred from whether `field2` agrees with what AUTO would have produced
for `field1` — see the comment block in `esp8266_thingspeak.ino` for the full
reasoning and its one known ambiguity.

### 3. Dashboard

The dashboard finds its ThingSpeak keys at boot and picks a mode to match, so
there is nothing to edit in `index.html`. See **Running the dashboard** below.
It logs which mode and key source it chose in the on-screen console, so you
are never guessing.

### 4. Flash

```sh
arduino-cli compile --fqbn arduino:avr:uno arduino_ldr
arduino-cli upload  --fqbn arduino:avr:uno -p COM3 arduino_ldr

arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 esp8266_thingspeak
arduino-cli upload  --fqbn esp8266:esp8266:nodemcuv2 -p COM5 esp8266_thingspeak
```

Substitute your own ports. Boards using a **CP2102** USB-UART need the
Silicon Labs VCP driver; **CH340** clones need the WCH driver. Neither ships
with the Arduino IDE.

## Running the dashboard

Four ways, no code changes between them. The dashboard resolves its
configuration at startup, highest precedence first:

| # | Source | Mode |
|---|---|---|
| 1 | `?channel=…&read_key=…&write_key=…` in the URL | direct |
| 2 | `localStorage` keys in that browser | direct |
| 3 | `/api/config` reporting server-held keys | **proxy** |
| 4 | nothing found | idle, with setup instructions on screen |

**Proxy mode** keeps the keys in server environment variables. The browser
receives the channel id and nothing else, and calls `/api/feed` and
`/api/update` instead of `api.thingspeak.com`.

**Direct mode** calls ThingSpeak straight from the browser, so the keys have
to be in the browser. Convenient, not secret.

Either way the bytes that reach the channel are identical, so the ESP8266 and
the Arduino need no changes and cannot tell the difference.

### On Vercel

```sh
npm run deploy          # or: git push, with the repo imported on vercel.com
```

Then set the environment variables in **Project → Settings → Environment
Variables**:

| Variable | Required | Notes |
|---|---|---|
| `TS_CHANNEL_ID` | yes | Enables proxy mode. Not secret — it is in the public channel URL |
| `TS_READ_API_KEY` | private channels | Omit for a public channel |
| `TS_WRITE_API_KEY` | to send commands | Without it the dashboard is read-only |
| `DASHBOARD_TOKEN` | recommended | Shared secret required to switch the relay — see below |

Redeploy after changing them; Vercel only injects env vars at build and
invocation time. There is no build step and no dependency to install — the
repo root is served as-is and `api/*.js` become serverless functions.

### Locally, with the proxy

```sh
cp .env.example .env.local     # then fill in your real values
npm run dev                    # vercel dev, on http://localhost:3000
```

### Locally, as a plain static site

No server-side keys, no `api/`. Set the keys once per browser instead:

```sh
npm run static                 # http://localhost:3000
```

```js
// then in the browser console (F12)
localStorage.setItem('ts_channel',   '1234567');
localStorage.setItem('ts_read_key',  'YOURREADKEY');
localStorage.setItem('ts_write_key', 'YOURWRITEKEY');
location.reload();
```

This is also the path for GitHub Pages, Netlify, an SD card, or any other
dumb static host.

### Straight off disk

Double-click `index.html`. It detects `file://`, skips the `/api` probe
entirely, and uses the `localStorage` keys above. Everything works except
proxy mode, which needs a server by definition.

## Security note

Read this before you put the URL anywhere public. The two modes fail in
*different* ways, and proxy mode is not simply the safe one.

**Direct mode leaks the key.** A static page cannot hide an API key — the
write key reaches the browser and is visible to anyone using that machine.
That is inherent to calling ThingSpeak from client-side JavaScript; the
`localStorage` approach only keeps the key out of *this repository*. The ESP
treats channel writes as commands, so **anyone holding the write key can
switch the relay.** If a key is ever exposed, regenerate it on the channel's
*API Keys* tab.

**Proxy mode leaks the ability instead.** The key is safe on the server, but
`/api/update` will switch the relay for anybody who can reach it — and with
no key needed, the deployment URL *is* the credential. A public Vercel URL is
guessable and gets crawled.

So if you deploy publicly, set `DASHBOARD_TOKEN` to a long random string:

```sh
# any long random value
openssl rand -hex 24
```

Then, once per browser:

```js
localStorage.setItem('ts_dashboard_token', 'the-same-value');
location.reload();
```

`/api/update` then rejects commands without it (`403`), while `/api/feed`
stays open — reading is harmless. The dashboard says so in its console when
a token is required but missing. Vercel's own password protection or an
allowlist of one is a fine alternative.

Neither mode is real authentication. Put a proper backend with real accounts
in front of ThingSpeak if this ever switches something that matters.

## Safety

The relay side switches **mains voltage**. Keep mains wiring inside an
enclosure, never work on it live, and keep the low-voltage logic physically
separated from the switched side.
