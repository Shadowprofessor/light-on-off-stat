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
| `index.html` | Static dashboard — live state, AUTO / ON / OFF buttons |

The Arduino, not the ESP, owns the automation. If WiFi drops, the light keeps
working — the cloud layer is for monitoring and manual override only.

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

Open `index.html` in a browser, then set your keys once from the console (F12):

```js
localStorage.setItem('ts_channel',   '1234567');
localStorage.setItem('ts_read_key',  'YOURREADKEY');
localStorage.setItem('ts_write_key', 'YOURWRITEKEY');
location.reload();
```

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

## Security note

A static page cannot hide an API key — the write key reaches the browser and
is therefore visible to whoever uses that machine. That is inherent to talking
to ThingSpeak directly from client-side JavaScript, and the `localStorage`
approach above only keeps the key out of *this repository*.

It matters here because the ESP treats channel writes as commands: **anyone
holding the write key can switch the relay.** If a key is ever exposed,
regenerate it on the channel's *API Keys* tab. Put a real backend in front of
ThingSpeak if this ever controls anything that matters.

## Safety

The relay side switches **mains voltage**. Keep mains wiring inside an
enclosure, never work on it live, and keep the low-voltage logic physically
separated from the switched side.
