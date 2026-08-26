#pragma once
// ════════════════════════════════════════════════════════════════════════
//  TEMPLATE — safe to commit. Contains no real credentials.
//
//  Copy this file to  secrets.h  in the same folder and fill in your own
//  values. secrets.h is listed in .gitignore, so your credentials never
//  reach GitHub.
//
//      cp secrets.example.h secrets.h
//
//  The sketch prefers secrets.h and falls back to this file, so a fresh
//  clone compiles straight away — it just will not connect until you
//  supply real values.
// ════════════════════════════════════════════════════════════════════════

// ── WiFi ───────────────────────────────────────────────────────────────
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ── ThingSpeak ─────────────────────────────────────────────────────────
// Channel ID and both API keys come from your channel's "API Keys" tab.
// Treat the WRITE key like a password: anyone holding it can write to the
// channel, and this project reads the channel to decide the relay state.
const char* TS_WRITE_KEY = "YOUR_THINGSPEAK_WRITE_KEY";
const char* TS_READ_KEY  = "YOUR_THINGSPEAK_READ_KEY";
const char* TS_CHANNEL   = "YOUR_CHANNEL_ID";
