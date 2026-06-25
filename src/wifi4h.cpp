#include "wifi4h.h"
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <log4c.h>

// ─── Internal state ───────────────────────────────────────────────────────────

static const char* DEFAULT_NS = "wifi4h";
static const int   MAX_FIELDS = 16;

struct Wifi4hField {
    char name[32];
    char type[10];   // "string" | "number" | "password"
    bool required;
    String value;    // in-memory value (loaded from NVS or set by portal)
};

static Wifi4hField  _fields[MAX_FIELDS];
static int          _fieldCount  = 0;
static String       _deviceId;
static const char*  _ns          = DEFAULT_NS;
static Wifi4hEventFn _eventFn    = nullptr;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void _emit(const String& event, const String& detail = "") {
    LINFO("wifi4h: %s %s", event.c_str(), detail.c_str());
    if (_eventFn) _eventFn(event, detail);
}

static int _findField(const char* name) {
    for (int i = 0; i < _fieldCount; i++) {
        if (strcmp(_fields[i].name, name) == 0) return i;
    }
    return -1;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void wifi4h_set_device_id(const String& id) { _deviceId = id; }
void wifi4h_set_namespace(const char* ns)   { _ns = ns; }
void wifi4h_on_event(Wifi4hEventFn fn)      { _eventFn = fn; }

void wifi4h_add_field(const char* name, const char* type, bool required) {
    if (_fieldCount >= MAX_FIELDS) return;
    if (_findField(name) >= 0)    return; // no duplicates
    Wifi4hField& f = _fields[_fieldCount++];
    strlcpy(f.name, name, sizeof(f.name));
    strlcpy(f.type, type, sizeof(f.type));
    f.required = required;
    f.value    = "";
}

void wifi4h_load() {
    Preferences prefs;
    prefs.begin(_ns, true);
    for (int i = 0; i < _fieldCount; i++) {
        String val = prefs.getString(_fields[i].name, "");
        // Migration: old firmware stored number fields via putUShort — fall back
        if (val.length() == 0 && strcmp(_fields[i].type, "number") == 0) {
            uint16_t n = prefs.getUShort(_fields[i].name, 0);
            if (n > 0) val = String(n);
        }
        _fields[i].value = val;
    }
    prefs.end();
    LINFO("wifi4h: loaded %d field(s) from NVS ns=%s", _fieldCount, _ns);
}

String wifi4h_get(const char* name) {
    int idx = _findField(name);
    return idx >= 0 ? _fields[idx].value : String("");
}

// ─── Captive-portal hotspot ───────────────────────────────────────────────────

// Build an HTML form dynamically from _fields[].
// Input type mapping: "password" → <input type="password">,
//                     "number"   → <input type="number">,
//                     anything else → <input type="text">
static String _buildForm() {
    String html = F("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Device Setup</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
        "h2{text-align:center}"
        "label{display:block;margin-top:12px;font-size:14px}"
        "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;font-size:15px}"
        "button{width:100%;margin-top:20px;padding:12px;font-size:16px;"
               "background:#333;color:#fff;border:none;border-radius:4px;cursor:pointer}"
        "</style></head><body>"
        "<h2>&#x2699; Device Setup</h2>"
        "<form method='POST' action='/save'>");

    for (int i = 0; i < _fieldCount; i++) {
        const Wifi4hField& f = _fields[i];
        String inputType = "text";
        if (strcmp(f.type, "password") == 0) inputType = "password";
        else if (strcmp(f.type, "number") == 0) inputType = "number";

        html += "<label>";
        html += f.name;
        html += "<input name='";
        html += f.name;
        html += "' type='";
        html += inputType;
        html += "'";
        if (f.value.length() > 0 && strcmp(f.type, "password") != 0) {
            html += " value='";
            html += f.value;
            html += "'";
        }
        if (f.required) html += " required";
        html += "></label>";
    }

    html += F("<button type='submit'>Save &amp; Reboot</button>"
              "</form></body></html>");
    return html;
}

static void _startHotspot() {
    String apName = "Setup-" + (_deviceId.length() >= 6
        ? _deviceId.substring(_deviceId.length() - 6)
        : _deviceId);

    WiFi.softAP(apName.c_str(), "12345678");
    IPAddress apIp = WiFi.softAPIP();
    _emit("ap_start", apName + " " + apIp.toString());

    static DNSServer dns;
    static WebServer server(80);

    dns.start(53, "*", apIp);

    auto sendForm = [&]() {
        server.send(200, "text/html", _buildForm());
    };

    // Main page
    server.on("/",                     HTTP_GET,  sendForm);
    // iOS / Android captive portal probes
    server.on("/generate_204",         HTTP_GET,  [&](){ server.sendHeader("Location", "/"); server.send(302); });
    server.on("/hotspot-detect.html",  HTTP_GET,  sendForm);
    server.on("/connecttest.txt",      HTTP_GET,  [&](){ server.sendHeader("Location", "/"); server.send(302); });
    server.on("/ncsi.txt",             HTTP_GET,  [&](){ server.sendHeader("Location", "/"); server.send(302); });

    server.on("/save", HTTP_POST, [&]() {
        // Validate required fields
        for (int i = 0; i < _fieldCount; i++) {
            if (_fields[i].required && server.arg(_fields[i].name).length() == 0) {
                server.send(400, "text/plain",
                    String("Missing required field: ") + _fields[i].name);
                return;
            }
        }

        // Persist all fields to NVS
        Preferences prefs;
        prefs.begin(_ns, false);
        for (int i = 0; i < _fieldCount; i++) {
            String val = server.arg(_fields[i].name);
            prefs.putString(_fields[i].name, val);
            _fields[i].value = val; // update in-memory too
        }
        prefs.end();

        _emit("saved");
        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
            "<h2>&#x2705; Saved!</h2><p>Rebooting and connecting to your network.</p>"
            "</body></html>");
        delay(1500);
        ESP.restart();
    });

    server.onNotFound([&]() {
        server.sendHeader("Location", "/");
        server.send(302);
    });

    server.begin();
    LINFO("wifi4h: captive portal running on %s", apIp.toString().c_str());

    while (true) {
        dns.processNextRequest();
        server.handleClient();
    }
}

// ─── Connect ──────────────────────────────────────────────────────────────────

void wifi4h_connect(int maxAttempts) {
    String ssid = wifi4h_get("ssid");
    String pass = wifi4h_get("password");

    if (ssid.length() == 0) {
        LWARN("wifi4h: no ssid saved — starting hotspot");
        _startHotspot();
        return;
    }

    LINFO("wifi4h: connecting to %s", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        delay(500);
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        LWARN("wifi4h: connect failed after %d attempts — starting hotspot", attempts);
        _emit("failed", ssid);
        _startHotspot();
    } else {
        _emit("connected", WiFi.localIP().toString());
    }
}

void wifi4h_reconnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    String ssid = wifi4h_get("ssid");
    String pass = wifi4h_get("password");
    if (ssid.length() == 0) return;
    LWARN("wifi4h: reconnecting to %s", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
}
