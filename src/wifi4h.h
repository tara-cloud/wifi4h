#pragma once
#include <Arduino.h>
#include <functional>

// ─── wifi4h — WiFi connection + captive-portal hotspot ───────────────────────
//
// Manages WiFi connect/reconnect and a setup hotspot (captive portal) for
// first-time provisioning.
//
// Consumer declares the fields they want collected (name, type, required).
// The portal renders a form with those fields, validates required ones, saves
// all values to NVS (Preferences), and reboots. Values persist across reboots
// and can be read back via wifi4h_get() after wifi4h_load().
//
// ─── Field schema ────────────────────────────────────────────────────────────
//   wifi4h_add_field("ssid",     "string", true);
//   wifi4h_add_field("password", "string", false);
//   wifi4h_add_field("mqttPort", "number", false);
//
// Built-in fields: "ssid" and "password" are always included and drive the
// WiFi connection. Any extra fields are saved to NVS with the same key.
//
// ─── Typical usage ───────────────────────────────────────────────────────────
//   void setup() {
//       wifi4h_set_device_id("A1B2C3");           // used for AP name suffix
//       wifi4h_add_field("ssid",      "string", true);
//       wifi4h_add_field("password",  "string", false);
//       wifi4h_add_field("host",      "string", true);
//       wifi4h_add_field("mqttPort",  "number", false);
//
//       wifi4h_load();                            // load saved values from NVS
//       wifi4h_connect();                         // connect or start hotspot
//
//       String host = wifi4h_get("host");         // read saved value
//   }

// ─── Callbacks ────────────────────────────────────────────────────────────────
using Wifi4hEventFn = std::function<void(const String& event, const String& detail)>;
// event: "connected", "failed", "ap_start", "saved"
// detail: IP address / AP name / "" / ""

// ─── Init ─────────────────────────────────────────────────────────────────────

// Device ID suffix used in AP name ("Setup-XXXXXX"). Call before wifi4h_connect().
void wifi4h_set_device_id(const String& id);

// NVS namespace to store credentials in (default: "wifi4h")
void wifi4h_set_namespace(const char* ns);

// Register an optional event callback
void wifi4h_on_event(Wifi4hEventFn fn);

// ─── Field schema ─────────────────────────────────────────────────────────────

// Declare a field that the portal should collect.
// type: "string" | "number" | "password"
// required: portal POST is rejected unless this field is non-empty
// max 16 fields
void wifi4h_add_field(const char* name, const char* type, bool required);

// ─── NVS ──────────────────────────────────────────────────────────────────────

// Load all saved field values from NVS into memory. Call once at boot.
void wifi4h_load();

// Read a saved value by field name. Returns "" if not found.
String wifi4h_get(const char* name);

// ─── Connect ──────────────────────────────────────────────────────────────────

// Attempt WiFi connection using saved "ssid"/"password".
// If not provisioned, or connection fails after maxAttempts, starts the
// captive-portal hotspot and blocks until credentials are submitted.
// maxAttempts: number of 500ms polls (default 20 → 10 s)
void wifi4h_connect(int maxAttempts = 20);

// Non-blocking reconnect helper — call in loop() to recover dropped connections.
// Does NOT start the hotspot; use wifi4h_connect() for that.
void wifi4h_reconnect();
