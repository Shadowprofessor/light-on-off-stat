// ════════════════════════════════════════════════════════════════════════
//  IoT Smart Automatic Light Control — ESP8266 BRIDGE
//  File: esp8266_thingspeak/esp8266_thingspeak.ino
//
//  ROLE
//    Bridge between the Arduino (real sensor + relay) and ThingSpeak.
//        ThingSpeak  <->  ESP8266  <->  Arduino
//
//  WIRING (NodeMCU / Wemos D1 mini)
//    D5 (GPIO14) = SoftSerial RX  ← Arduino D11 TX  [via 10K/20K divider]
//    D6 (GPIO12) = SoftSerial TX  → Arduino D10 RX  [direct]
//    GND ────────────────────────── Arduino GND     [COMMON GND REQUIRED]
//
//  ════════════════════════════════════════════════════════════════════
//   THINGSPEAK CHANNEL — EXACTLY THREE FIELDS, MEANINGS NEVER CHANGE
//  ════════════════════════════════════════════════════════════════════
//    field1 = LDR      1 = DARK,  0 = BRIGHT
//    field2 = Relay    1 = ON,    0 = OFF
//    field3 = LED      1 = ON,    0 = OFF
//
//    There is NO fourth field and none is ever created, read or written.
//    There is therefore NO field for AUTO/MANUAL, and no string is ever
//    encoded into a numeric field.
//
//  ════════════════════════════════════════════════════════════════════
//   HOW THE ESP LEARNS THE MODE WITHOUT A FOURTH FIELD
//  ════════════════════════════════════════════════════════════════════
//  The control mode lives in RAM here (and independently in the website
//  and the Arduino). The ESP infers what the user asked for using two
//  mechanisms:
//
//  1) ENTRY PROVENANCE.  /update returns the new entry_id in its response
//     body. This sketch remembers the ids of the entries IT wrote (a small
//     ring, because a feed read returns several entries). When it reads the
//     feed, an entry whose id is not one of its own was written by the
//     website. The website only writes when a user clicks a button, so a
//     foreign entry IS a user command.
//
//  2) AUTO-AGREEMENT DECODING.  The website writes
//         field1 = latest known LDR,  field2/field3 = requested state.
//     AUTO would have produced  relay = field1  (dark -> ON). So:
//
//        field2 != field1  ->  the request CONTRADICTS the light level,
//                              which only a manual override can do.
//                              MANUAL ON if field2==1, MANUAL OFF if 0.
//
//        field2 == field1  ->  the request agrees with what AUTO would do.
//                              Interpreted as AUTO: clear the override.
//
//     The website's AUTO button deliberately writes exactly what AUTO
//     would produce, so it always lands in the second row and always
//     releases the override.
//
//  KNOWN LIMITATION (unavoidable with three fields):
//     MANUAL ON while the room is already DARK writes 1,1,1 — identical
//     to AUTO+DARK. MANUAL OFF while BRIGHT writes 0,0,0 — identical to
//     AUTO+BRIGHT. Those two cases are indistinguishable and are treated
//     as AUTO. The light is correct at that instant either way (manual
//     and auto agree there); the only difference is that a later LDR
//     change will move the physical light. The website keeps its own
//     MANUAL mode in RAM regardless, so its display stays latched.
//
//  MANUAL_TIMEOUT_MS additionally releases a stale override so a manual
//  command can never latch the hardware forever.
//
//  ════════════════════════════════════════════════════════════════════
//   CYCLE ORDER (never uploads stale hardware state)
//  ════════════════════════════════════════════════════════════════════
//    read feed / decide mode  ->  command the Arduino  ->  settle  ->
//    request STATUS  ->  receive the ACTUAL state  ->  upload F1/F2/F3
//
//  Uploads are event-driven: only when the actual state CHANGED, or when
//  the liveness heartbeat is due, and never closer together than
//  MIN_UPLOAD_GAP_MS. In steady state the ESP stays silent so the 15 s
//  channel write slot is free for the website's commands.
// ════════════════════════════════════════════════════════════════════════

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h> 
#include <SoftwareSerial.h>

// ── Credentials ────────────────────────────────────────────────────────
// The real WiFi password and ThingSpeak keys live in secrets.h, which is
// git-ignored and never published. A fresh clone has no secrets.h, so it
// falls back to secrets.example.h and still compiles — copy that file to
// secrets.h and fill in your own values before flashing.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.example.h"
#endif

const char* TS_HOST = "api.thingspeak.com";

// ── SoftwareSerial to the Arduino ──────────────────────────────────────
SoftwareSerial ArduSerial(14, 12);            // RX = D5, TX = D6

// ── Timing ─────────────────────────────────────────────────────────────
const unsigned long CYCLE_MS         = 20000UL;  // command poll cycle
const unsigned long MIN_UPLOAD_GAP_MS= 16000UL;  // ThingSpeak limit is 15 s
const unsigned long HEARTBEAT_MS     = 60000UL;  // liveness upload
const unsigned long MANUAL_TIMEOUT_MS= 1800000UL;// 30 min; 0 = never expire
const unsigned long SETTLE_MS        = 400UL;    // Arduino command settle

unsigned long lastCycleAt    = 0;
unsigned long lastUploadAt   = 0;
unsigned long manualSetAt    = 0;

// ── Control mode (RAM only — see header) ───────────────────────────────
enum Mode { MODE_AUTO, MODE_MANUAL_ON, MODE_MANUAL_OFF };
Mode currentMode = MODE_AUTO;

// ── Actual hardware state, as last reported by the Arduino ─────────────
int  ldrStatus   = 0;    // 1 = DARK
int  relayStatus = 0;    // 1 = ON
int  ledStatus   = 0;    // 1 = ON
bool haveStatus  = false;

// Line assembly buffer for unsolicited pushes from the Arduino.
String pushBuf = "";

// ── Last values successfully uploaded (for change detection) ───────────
int  upLdr = -1, upRelay = -1, upLed = -1;

// ── Entry-id bookkeeping for provenance ────────────────────────────────
// A ring of the entry ids WE wrote. It must hold more than one: the feed
// read returns several entries, so if we only remembered the newest write
// we would mistake our own previous upload for a website command.
const uint8_t OWN_ID_SLOTS = 6;
long    ownWriteIds[OWN_ID_SLOTS];
uint8_t ownIdNext      = 0;
long    lastProcessedId = -1;   // newest foreign entry already acted upon

void rememberOwnWrite(long id) {
  ownWriteIds[ownIdNext] = id;
  ownIdNext = (ownIdNext + 1) % OWN_ID_SLOTS;
}

bool isOwnWrite(long id) {
  for (uint8_t i = 0; i < OWN_ID_SLOTS; i++) {
    if (ownWriteIds[i] == id) return true;
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n[ESP] ================================"));
  Serial.println(F("[ESP]  Smart Automatic Light — Bridge"));
  Serial.println(F("[ESP]  Fields: F1=LDR F2=Relay F3=LED"));
  Serial.println(F("[ESP]  (no fourth field is used)"));
  Serial.println(F("[ESP] ================================"));

  ArduSerial.begin(9600);

  connectWiFi();

  // Establish AUTO on the hardware at startup.
  currentMode = MODE_AUTO;
  sendCommandToArduino("AUTO");
  delay(SETTLE_MS);
  readStatusFromArduino();

  // First full cycle fires 2 s from now.
  lastCycleAt = millis() - CYCLE_MS + 2000UL;
}

// ════════════════════════════════════════════════════════════════════════
void loop() {
  // Keep WiFi alive.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WIFI] Lost connection, reconnecting…"));
    connectWiFi();
  }

  // ── EVENT PATH ──────────────────────────────────────────────────────
  // The Arduino pushes a status line the moment the light or the LDR
  // changes. Handle it immediately instead of waiting for the next cycle,
  // so a real light change reaches the cloud in about a second.
  if (drainArduinoPush()) {
    Serial.println(F("[EVENT] Arduino pushed a state change"));
    maybeUpload();
  }

  // ── PERIODIC PATH ───────────────────────────────────────────────────
  // Polls ThingSpeak for website commands and heartbeats the state.
  if (millis() - lastCycleAt >= CYCLE_MS) {
    lastCycleAt = millis();
    runCycle();
  }

  expireManualOverride();
}

// ════════════════════════════════════════════════════════════════════════
//  MAIN CYCLE — read/decide, command, settle, STATUS, upload
// ════════════════════════════════════════════════════════════════════════
void runCycle() {
  Serial.println(F("\n──── cycle ─────────────────────────"));

  // 1. Read the feed and decide what the user wants.
  Mode before = currentMode;
  pollForWebsiteCommand();

  // 2. Command the Arduino. Re-assert the mode every cycle so a reset or
  //    a missed serial byte is self-healing.
  sendCommandToArduino(modeToCommand(currentMode));

  if (currentMode != before) {
    Serial.print(F("[MODE] changed to "));
    Serial.println(modeName(currentMode));
  }

  // 3. Let the relay settle before we ask what actually happened.
  delay(SETTLE_MS);

  // 4/5. Read the ACTUAL state back.
  if (!readStatusFromArduino()) {
    Serial.println(F("[ARDU] No valid response — nothing uploaded"));
    Serial.println(F("────────────────────────────────────"));
    return;
  }

  // 6. Upload the actual state (change-driven + heartbeat).
  maybeUpload();

  Serial.println(F("────────────────────────────────────"));
}

// ════════════════════════════════════════════════════════════════════════
//  ARDUINO LINK
// ════════════════════════════════════════════════════════════════════════

void sendCommandToArduino(const char* cmd) {
  ArduSerial.println(cmd);
  Serial.print(F("[ESP->ARD] "));
  Serial.println(cmd);
}

const char* modeToCommand(Mode m) {
  switch (m) {
    case MODE_MANUAL_ON:  return "ON";
    case MODE_MANUAL_OFF: return "OFF";
    default:              return "AUTO";
  }
}

const char* modeName(Mode m) {
  switch (m) {
    case MODE_MANUAL_ON:  return "MANUAL ON";
    case MODE_MANUAL_OFF: return "MANUAL OFF";
    default:              return "AUTO";
  }
}

/**
 * Asks the Arduino for STATUS and waits up to 1.5 s for the reply.
 * Any line containing "LDR:" counts, so an unsolicited push that arrives
 * first is accepted rather than discarded.
 */
bool readStatusFromArduino() {
  while (ArduSerial.available()) ArduSerial.read();   // drop stale bytes
  pushBuf = "";                 // and drop any half-received pushed line

  sendCommandToArduino("STATUS");

  unsigned long t0 = millis();
  String line = "";

  while (millis() - t0 < 1500UL) {
    while (ArduSerial.available()) {
      char c = ArduSerial.read();
      if (c == '\n') {
        line.trim();
        if (line.indexOf("LDR:") >= 0) {
          applyStatusLine(line);
          return true;
        }
        line = "";                      // not a status line, keep waiting
      } else if (c != '\r') {
        line += c;
      }
    }
    delay(2);
  }
  return false;
}

/**
 * Consumes any unsolicited status line the Arduino pushed.
 * Returns true when the reported state actually differs from what we hold.
 */
bool drainArduinoPush() {
  bool changed = false;

  while (ArduSerial.available()) {
    char c = ArduSerial.read();
    if (c == '\n') {
      pushBuf.trim();
      if (pushBuf.indexOf("LDR:") >= 0) {
        int pl = ldrStatus, pr = relayStatus, pe = ledStatus;
        applyStatusLine(pushBuf);
        if (pl != ldrStatus || pr != relayStatus || pe != ledStatus) {
          changed = true;
        }
      }
      pushBuf = "";
    } else if (c != '\r') {
      pushBuf += c;
      if (pushBuf.length() > 48) pushBuf = "";   // runaway guard
    }
  }
  return changed;
}

void applyStatusLine(const String& line) {
  ldrStatus   = extractInt(line, "LDR:");
  relayStatus = extractInt(line, "RELAY:");
  ledStatus   = extractInt(line, "LED:");
  haveStatus  = true;

  Serial.printf("[ARD->ESP] LDR=%d RELAY=%d LED=%d  (mode=%s)\n",
                ldrStatus, relayStatus, ledStatus, modeName(currentMode));
}

/** Finds "KEY:x" in s and returns x as an int. */
int extractInt(const String& s, const char* key) {
  int idx = s.indexOf(key);
  if (idx < 0) return 0;
  idx += strlen(key);

  String num = "";
  while (idx < (int)s.length() && (isDigit(s[idx]) || s[idx] == '-')) {
    num += s[idx++];
  }
  return num.toInt();
}

// ════════════════════════════════════════════════════════════════════════
//  UPLOAD — field1 = LDR, field2 = Relay, field3 = LED
//  Event-driven: only on a real change, or when the heartbeat is due.
// ════════════════════════════════════════════════════════════════════════
void maybeUpload() {
  if (!haveStatus) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool changed   = (ldrStatus != upLdr) ||
                   (relayStatus != upRelay) ||
                   (ledStatus != upLed);
  bool heartbeat = (millis() - lastUploadAt >= HEARTBEAT_MS);

  if (!changed && !heartbeat) return;

  // Respect the channel write limit. A change that is throttled here will
  // still be picked up by the next cycle, because upLdr/upRelay/upLed are
  // only updated after a SUCCESSFUL upload.
  if (lastUploadAt != 0 && millis() - lastUploadAt < MIN_UPLOAD_GAP_MS) {
    Serial.println(F("[TS] Throttled (15 s channel limit) — will retry"));
    return;
  }

  uploadToThingSpeak(changed ? "change" : "heartbeat");
}

void uploadToThingSpeak(const char* reason) {
  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + TS_HOST + "/update"
             + "?api_key=" + TS_WRITE_KEY
             + "&field1="  + ldrStatus      // LDR   1 = DARK
             + "&field2="  + relayStatus    // Relay 1 = ON
             + "&field3="  + ledStatus;     // LED   1 = ON

  http.begin(client, url);
  http.setTimeout(6000);
  int    code = http.GET();
  String body = http.getString();
  http.end();

  body.trim();
  long entryId = body.toInt();

  Serial.printf("[TS] Upload (%s) F1=%d F2=%d F3=%d → ",
                reason, ldrStatus, relayStatus, ledStatus);

  if (code == 200 && entryId > 0) {
    // Remember our own entry so we never mistake it for a user command.
    rememberOwnWrite(entryId);
    lastUploadAt   = millis();
    upLdr   = ldrStatus;
    upRelay = relayStatus;
    upLed   = ledStatus;
    Serial.printf("OK (entry %ld)\n", entryId);
  } else {
    Serial.printf("FAILED (HTTP %d, body='%s') — will retry\n",
                  code, body.c_str());
  }
}

// ════════════════════════════════════════════════════════════════════════
//  COMMAND POLL — find a foreign entry and decode the requested mode
// ════════════════════════════════════════════════════════════════════════
void pollForWebsiteCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  // Two entries, so a command is still seen if one of our own uploads
  // landed on top of it. Reads are not rate limited.
  String url = String("http://") + TS_HOST
             + "/channels/" + TS_CHANNEL + "/feeds.json"
             + "?api_key=" + TS_READ_KEY + "&results=2";

  http.begin(client, url);
  http.setTimeout(6000);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[TS] Feed read failed: HTTP %d — keeping mode %s\n",
                  code, modeName(currentMode));
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  // Walk every entry in the response, oldest first as returned, and keep
  // the newest foreign one.
  long bestId = -1;
  int  bestF1 = -1, bestF2 = -1, bestF3 = -1;

  int scan = 0;
  while (true) {
    int at = body.indexOf("\"entry_id\":", scan);
    if (at < 0) break;

    // Bound this entry's text at the start of the next one.
    int next = body.indexOf("\"entry_id\":", at + 11);
    String chunk = (next < 0) ? body.substring(at)
                              : body.substring(at, next);
    scan = at + 11;

    long id = jsonNumber(chunk, "\"entry_id\":");
    if (id <= 0)               continue;
    if (isOwnWrite(id))        continue;   // our own upload, not a command
    if (id <= lastProcessedId) continue;   // already acted on
    if (id <= bestId)          continue;

    bestId = id;
    bestF1 = jsonField(chunk, "\"field1\":");
    bestF2 = jsonField(chunk, "\"field2\":");
    bestF3 = jsonField(chunk, "\"field3\":");
  }

  if (bestId < 0) {
    Serial.printf("[TS] No new website command — mode stays %s\n",
                  modeName(currentMode));
    return;
  }

  lastProcessedId = bestId;
  Serial.printf("[TS] Website entry %ld: F1=%d F2=%d F3=%d\n",
                bestId, bestF1, bestF2, bestF3);

  if (bestF2 < 0) {
    Serial.println(F("[TS] field2 missing — cannot decode, ignoring"));
    return;
  }

  // The LDR the website believed at the time it wrote. If it wrote null,
  // fall back to our own last known reading.
  int refLdr = (bestF1 >= 0) ? bestF1 : ldrStatus;

  // What AUTO would have produced for that LDR: dark (1) -> relay ON (1).
  int autoRelay = refLdr;

  if (bestF2 == autoRelay) {
    // Agrees with AUTO -> treat as AUTO and release any override.
    if (currentMode != MODE_AUTO) {
      Serial.println(F("[DECODE] F2 == auto(F1) → AUTO, override released"));
    } else {
      Serial.println(F("[DECODE] F2 == auto(F1) → AUTO (already AUTO)"));
    }
    currentMode = MODE_AUTO;
    manualSetAt = 0;

  } else {
    // Contradicts the light level -> only a manual override explains it.
    currentMode = (bestF2 == 1) ? MODE_MANUAL_ON : MODE_MANUAL_OFF;
    manualSetAt = millis();
    Serial.printf("[DECODE] F2(%d) != auto(%d) → %s\n",
                  bestF2, autoRelay, modeName(currentMode));
  }
}

/** Reads a plain JSON number that follows `key` (e.g. "entry_id":). */
long jsonNumber(const String& s, const char* key) {
  int idx = s.indexOf(key);
  if (idx < 0) return -1;
  idx += strlen(key);

  while (idx < (int)s.length() && (s[idx] == ' ' || s[idx] == '"')) idx++;

  String num = "";
  while (idx < (int)s.length() && (isDigit(s[idx]) || s[idx] == '-')) {
    num += s[idx++];
  }
  return num.length() ? num.toInt() : -1;
}

/**
 * Reads a ThingSpeak field, which arrives as "1" (quoted string) or null.
 * Returns -1 for null / missing / unparsable so the caller can preserve
 * whatever it already knows instead of reading null as zero.
 */
int jsonField(const String& s, const char* key) {
  int idx = s.indexOf(key);
  if (idx < 0) return -1;
  idx += strlen(key);

  while (idx < (int)s.length() && s[idx] == ' ') idx++;
  if (s.startsWith("null", idx)) return -1;
  if (idx < (int)s.length() && s[idx] == '"') idx++;

  String num = "";
  while (idx < (int)s.length() && (isDigit(s[idx]) || s[idx] == '-')) {
    num += s[idx++];
  }
  return num.length() ? num.toInt() : -1;
}

// ════════════════════════════════════════════════════════════════════════
//  MANUAL OVERRIDE SAFETY NET
//  A manual command can never latch the hardware forever.
// ════════════════════════════════════════════════════════════════════════
void expireManualOverride() {
  if (MANUAL_TIMEOUT_MS == 0)      return;   // disabled
  if (currentMode == MODE_AUTO)    return;
  if (manualSetAt == 0)            return;
  if (millis() - manualSetAt < MANUAL_TIMEOUT_MS) return;

  Serial.println(F("[MODE] Manual override expired → AUTO"));
  currentMode = MODE_AUTO;
  manualSetAt = 0;
  sendCommandToArduino("AUTO");
}

// ════════════════════════════════════════════════════════════════════════
//  WIFI
// ════════════════════════════════════════════════════════════════════════
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("[WIFI] Connecting"));
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print('.');
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // NOTE: F() yields a __FlashStringHelper*, which cannot be added to a
    // String. Print the two parts separately.
    Serial.print(F("[WIFI] Connected — IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WIFI] Failed — will retry"));
  }
}
