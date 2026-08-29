// M5Stack PaperColor — Volumio "Now Playing" display
//
// Polls Volumio's local REST API and, when the playing track changes, renders
// a portrait now-playing card (album art + title/artist/album) to the 400x600
// Spectra 6 e-paper panel. The panel driver (Panel_ED2208, via M5GFX) quantizes
// our plain RGB888 drawing down to its 6-color palette automatically — we just
// draw normally and call display() to commit a refresh.
//
// A full e-paper refresh takes ~15-30s, so redraws are event-driven (only on
// track change), not timer-driven.

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

#include "config.h"

static const char* LINER_VERSION = "1.1.0";
static const char* MDNS_HOSTNAME = "liner"; // also the OTA target: liner.local

static const int PANEL_W = 400;
static const int PANEL_H = 600;
static const int ART_SIZE = 400; // album art occupies the top, full-width square

// Idle screensaver: after this long showing "Nothing Playing", start cycling
// generative Spectra-6 patterns instead, changing on this interval. Kept
// infrequent since each change costs a full 15-30s e-paper refresh.
static const uint32_t SCREENSAVER_DELAY_MS = 5UL * 60 * 1000;
static const uint32_t SCREENSAVER_INTERVAL_MS = 10UL * 60 * 1000;

String lastRenderedKey = "\x01__BOOT__\x01"; // sentinel so the first real state always renders

// ---------------------------------------------------------------------------
// Wi-Fi + Volumio setup
//
// Credentials and the Volumio host live in NVS (via Preferences), not in
// compile-time config — so one built binary works for anyone, configured
// through a small web form served from the device's own access point. This
// is what makes a prebuilt release binary (e.g. for M5Burner) actually usable
// by someone other than the person who built it.
// ---------------------------------------------------------------------------

Preferences prefs;
WebServer setupServer(80);
bool inSetupMode = false;

String savedSsid;
String savedPassword;
String savedVolumioHost;

void loadSettings() {
  prefs.begin("liner", true); // read-only
  savedSsid = prefs.getString("ssid", "");
  savedPassword = prefs.getString("pass", "");
  savedVolumioHost = prefs.getString("vhost", VOLUMIO_HOST_DEFAULT);
  prefs.end();
}

void saveSettings(const String& ssid, const String& password, const String& vhost) {
  String host = vhost.length() ? vhost : String(VOLUMIO_HOST_DEFAULT);
  prefs.begin("liner", false); // read-write
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.putString("vhost", host);
  prefs.end();
  savedSsid = ssid;
  savedPassword = password;
  savedVolumioHost = host;
}

void showStatus(const String& msg) {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.drawString(msg, PANEL_W / 2, PANEL_H / 2);
  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

// Multi-line variant for setup mode, where the AP name + IP need their own lines.
void showStatusLines(const String& line1, const String& line2, const String& line3) {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  int y = line3.length() ? (PANEL_H / 2 - 30) : (PANEL_H / 2 - 15);
  M5.Display.drawString(line1, PANEL_W / 2, y);
  y += 30;
  M5.Display.drawString(line2, PANEL_W / 2, y);
  if (line3.length()) {
    y += 30;
    M5.Display.drawString(line3, PANEL_W / 2, y);
  }
  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

void handleSetupRoot() {
  int n = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    options += "<option value=\"" + htmlEscape(ssid) + "\">" + htmlEscape(ssid) +
               " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String html =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Liner setup</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em;}"
    "label{display:block;margin-top:1em;font-weight:bold;}"
    "input,select{width:100%;padding:0.5em;font-size:1em;box-sizing:border-box;}"
    "button{margin-top:1.5em;padding:0.7em;width:100%;font-size:1em;"
    "background:#1a1a1a;color:#fff;border:none;border-radius:4px;}</style></head><body>"
    "<h2>Liner setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi network</label>"
    "<select onchange=\"document.getElementById('ssid').value=this.value\">"
    "<option value=''>-- choose a network --</option>" + options + "</select>"
    "<label>Or enter manually</label>"
    "<input id='ssid' name='ssid' placeholder='SSID' value='" + htmlEscape(savedSsid) + "'>"
    "<label>Password</label>"
    "<input name='password' type='password' placeholder='leave blank if open'>"
    "<label>Volumio host</label>"
    "<input name='vhost' value='" + htmlEscape(savedVolumioHost) + "'>"
    "<button type='submit'>Save and connect</button>"
    "</form></body></html>";

  setupServer.send(200, "text/html", html);
}

void handleSetupSave() {
  String ssid = setupServer.arg("ssid");
  String password = setupServer.arg("password");
  String vhost = setupServer.arg("vhost");

  if (ssid.length() == 0) {
    setupServer.send(400, "text/plain", "SSID is required — go back and try again.");
    return;
  }

  saveSettings(ssid, password, vhost);
  setupServer.send(200, "text/html",
    "<html><body style='font-family:sans-serif;text-align:center;margin-top:3em;'>"
    "<h2>Saved</h2><p>Liner will now try to connect to \"" + htmlEscape(ssid) + "\".</p>"
    "<p>Check the device screen for status.</p></body></html>");

  delay(500); // let the HTTP response flush before we tear down the AP
  showStatusLines("Connecting to", ssid, "...");
  inSetupMode = false; // loop() will notice and attempt the real connect next
}

void startSetupMode() {
  inSetupMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME);
  delay(100);

  setupServer.on("/", handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.begin();

  String ip = WiFi.softAPIP().toString();
  Serial.printf("Setup mode: connect to Wi-Fi \"%s\", then visit http://%s\n", SETUP_AP_NAME, ip.c_str());
  showStatusLines("Connect to Wi-Fi:", SETUP_AP_NAME, "then visit " + ip);
}

// Attempts to connect using saved credentials. Falls back to setup mode if
// there are none, or if the connection attempt times out.
void connectWiFi() {
  if (savedSsid.length() == 0) {
    Serial.println("No saved Wi-Fi credentials, entering setup mode.");
    startSetupMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

  Serial.printf("Connecting to Wi-Fi: %s\n", savedSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(MDNS_HOSTNAME); // also enables outgoing .local resolution

    // OTA: no password, matching the setup portal's own trust model (local
    // network only, convenience over hardening — see README). Callbacks exist
    // purely for visibility — ArduinoOTA logs nothing on its own otherwise.
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.onStart([]() { Serial.println("OTA: update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: update complete, rebooting"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      Serial.printf("OTA: %u%%\n", (done * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("OTA error [%u]: %s\n", error,
        error == OTA_AUTH_ERROR ? "auth failed" :
        error == OTA_BEGIN_ERROR ? "begin failed" :
        error == OTA_CONNECT_ERROR ? "connect failed" :
        error == OTA_RECEIVE_ERROR ? "receive failed" :
        error == OTA_END_ERROR ? "end failed" : "unknown");
    });
    ArduinoOTA.begin();
    Serial.printf("OTA ready at %s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.println("\nWi-Fi connect failed, entering setup mode.");
    startSetupMode();
  }
}

// Triggered by the top button: force re-entry into setup mode so the network
// (or Volumio host) can be changed at any time, even while already connected.
void enterSetupMode() {
  Serial.println("Top button: entering Wi-Fi setup mode.");
  startSetupMode();
}

// ---------------------------------------------------------------------------
// Volumio
// ---------------------------------------------------------------------------

// Percent-encodes a string for safe use as a URL query parameter value.
String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    bool unreserved = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Generic resolution: external absolute URLs go through the wsrv.nl proxy,
// which re-encodes as baseline JPEG (streaming CDNs commonly serve progressive
// JPEG, which M5GFX's embedded decoder can't handle) and pre-scales to our art
// box size, shrinking the download too. Relative paths are Volumio-local art,
// fetched directly.
String genericResolveArtUrl(const String& albumart) {
  if (albumart.length() == 0) return "";
  if (albumart.startsWith("http://") || albumart.startsWith("https://")) {
    return "https://wsrv.nl/?url=" + urlEncode(albumart) +
           "&w=" + String(ART_SIZE) + "&h=" + String(ART_SIZE) +
           "&fit=cover&output=jpg";
  }
  String base = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT);
  if (!albumart.startsWith("/")) base += "/";
  return base + albumart;
}

// Resolves Volumio's (possibly relative) albumart path into a fetchable URL,
// dispatched by Volumio's "service" field (the active playback plugin) so
// each source has a clear spot for its own quirks. Only Tidal-via-external-URL
// has actually been exercised so far; the rest fall through to the generic
// path untested — verify against real playback before relying on them.
String resolveAlbumArtUrl(const String& albumart, const String& service) {
  if (service == "mpd") {
    // Local library / USB / NAS playback. Volumio typically serves art via
    // its own relative /albumart path, extracted from file tags — generic
    // path should already cover this. TODO: verify once tested; local file
    // tag art with no embedded cover may need a placeholder fallback.
    return genericResolveArtUrl(albumart);
  }
  if (service == "spop" || service == "volspotconnect2") {
    // Spotify Connect. Art normally comes from Spotify's i.scdn.co CDN as an
    // absolute URL — generic external-URL path (wsrv.nl proxy) should apply.
    // TODO: verify once tested; confirm Spotify's JPEGs are baseline (if so,
    // the proxy hop could be skipped for this service to save a round-trip).
    return genericResolveArtUrl(albumart);
  }
  // Default: covers Tidal (verified working) and any other/future service
  // (Qobuz, webradio, AirPlay, etc.) via the same generic logic.
  return genericResolveArtUrl(albumart);
}

bool fetchState(JsonDocument& doc) {
  HTTPClient http;
  String url = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT) + "/api/v1/getState";
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("getState failed, HTTP %d\n", code);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }
  return true;
}

// Fires a Volumio transport command (e.g. "next", "prev") and doesn't wait
// for a re-render — the next poll cycle will pick up the resulting state
// change and redraw normally.
void sendVolumioCommand(const char* cmd) {
  HTTPClient http;
  String url = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT) +
               "/api/v1/commands/?cmd=" + String(cmd);
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  Serial.printf("Command \"%s\" -> HTTP %d\n", cmd, code);
  http.end();
}

// Fetches raw bytes for the album art JPEG into a PSRAM buffer.
// Caller owns the returned buffer and must free() it. Returns nullptr on failure.
uint8_t* fetchAlbumArt(const String& url, size_t& outLen) {
  if (url.length() == 0) return nullptr;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  Serial.printf("  art HTTP GET -> code=%d\n", code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Album art fetch failed, HTTP %d\n", code);
    http.end();
    return nullptr;
  }

  int len = http.getSize();
  Serial.printf("  art content-length=%d\n", len);
  if (len <= 0) {
    http.end();
    return nullptr;
  }

  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) {
    Serial.println("ps_malloc failed for album art buffer");
    http.end();
    return nullptr;
  }

  // Overall cap generous enough for a slow local transfer; per-byte "stall" cap
  // (no progress at all) catches genuine hangs without punishing a slow-but-steady fetch.
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  uint32_t overallStart = millis();
  uint32_t lastProgress = millis();
  while (received < (size_t)len && millis() - overallStart < 25000 && millis() - lastProgress < 8000) {
    // Service OTA here too — this loop can run for several seconds, and an
    // in-progress OTA transfer needs ArduinoOTA.handle() called frequently
    // throughout, not just once per outer loop() iteration, or it stalls.
    if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
    if (stream->available()) {
      int n = stream->read(buf + received, len - received);
      if (n > 0) {
        received += n;
        lastProgress = millis();
      }
    } else {
      delay(5);
    }
  }
  http.end();

  if (received != (size_t)len) {
    Serial.printf("Album art incomplete: %u/%d bytes\n", (unsigned)received, len);
    free(buf);
    return nullptr;
  }
  Serial.printf("  art download complete: %u bytes, first bytes: %02X %02X %02X %02X\n",
                (unsigned)received, buf[0], buf[1], buf[2], buf[3]);

  outLen = received;
  return buf;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Simple word-wrap: draws text within a width, returns the y position after the last line.
int drawWrapped(const String& text, int x, int y, int maxWidth, int lineHeight) {
  if (text.length() == 0) return y;
  String line;
  int cursorY = y;
  int start = 0;
  while (start < (int)text.length()) {
    int spaceIdx = text.indexOf(' ', start);
    String word = (spaceIdx == -1) ? text.substring(start) : text.substring(start, spaceIdx);
    String candidate = line.length() ? (line + " " + word) : word;
    if (M5.Display.textWidth(candidate) > maxWidth && line.length() > 0) {
      M5.Display.drawString(line, x, cursorY);
      cursorY += lineHeight;
      line = word;
    } else {
      line = candidate;
    }
    if (spaceIdx == -1) break;
    start = spaceIdx + 1;
  }
  if (line.length()) {
    M5.Display.drawString(line, x, cursorY);
    cursorY += lineHeight;
  }
  return cursorY;
}

void renderIdle() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString("Nothing Playing", PANEL_W / 2, PANEL_H / 2);
  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

// Idle screensaver: a random abstract pattern in the Spectra 6 palette,
// shown after a long stretch of nothing playing (see SCREENSAVER_DELAY_MS).
// Purely decorative — no text, since it's not conveying information.
void renderScreensaver() {
  static const uint16_t palette[] = {TFT_BLACK, TFT_RED, TFT_YELLOW, TFT_BLUE, TFT_GREEN};
  static const int paletteCount = sizeof(palette) / sizeof(palette[0]);

  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);

  switch (random(3)) {
    case 0: { // scattered shapes
      for (int i = 0; i < 16; i++) {
        int x = random(0, PANEL_W);
        int y = random(0, PANEL_H);
        int size = random(30, 120);
        uint16_t color = palette[random(paletteCount)];
        if (random(2) == 0) {
          M5.Display.fillCircle(x, y, size / 2, color);
        } else {
          M5.Display.fillRect(x - size / 2, y - size / 2, size, size, color);
        }
      }
      break;
    }
    case 1: { // concentric rings from center, largest first so each is visible
      int cx = PANEL_W / 2, cy = PANEL_H / 2;
      for (int r = 340; r > 0; r -= 32) {
        M5.Display.fillCircle(cx, cy, r, palette[random(paletteCount)]);
      }
      break;
    }
    case 2: { // diagonal bands
      int bandWidth = 55;
      for (int x = -PANEL_H; x < PANEL_W; x += bandWidth) {
        uint16_t color = palette[random(paletteCount)];
        M5.Display.fillTriangle(x, 0, x + bandWidth, 0, x + bandWidth + PANEL_H, PANEL_H, color);
        M5.Display.fillTriangle(x, 0, x + bandWidth + PANEL_H, PANEL_H, x + PANEL_H, PANEL_H, color);
      }
      break;
    }
  }

  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

void renderNowPlaying(const String& title, const String& artist, const String& album,
                       const String& artUrl) {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);

  // Album art, top full-width square.
  bool artDrawn = false;
  if (artUrl.length()) {
    size_t artLen = 0;
    uint8_t* art = fetchAlbumArt(artUrl, artLen);
    if (art) {
      bool ok = M5.Display.drawJpg(art, artLen, 0, 0, ART_SIZE, ART_SIZE);
      Serial.printf("  drawJpg(len=%u) -> %s\n", (unsigned)artLen, ok ? "OK" : "FAILED");
      free(art);
      artDrawn = ok;
    }
  }
  if (!artDrawn) {
    M5.Display.fillRect(0, 0, ART_SIZE, ART_SIZE, TFT_LIGHTGRAY);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("No Album Art", ART_SIZE / 2, ART_SIZE / 2);
  }

  // Text block below the art.
  int textX = 16;
  int textWidth = PANEL_W - 32;
  int y = ART_SIZE + 20;

  M5.Display.setTextDatum(textdatum_t::top_left);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  y = drawWrapped(title, textX, y, textWidth, 30);

  y += 6;
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  y = drawWrapped(artist, textX, y, textWidth, 24);

  y += 4;
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawWrapped(album, textX, y, textWidth, 20);

  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void setup() {
  // Serial first, before anything that could hang, so we always get SOME output.
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] Liner v%s\n", LINER_VERSION);
  randomSeed(esp_random()); // hardware TRNG, for the idle screensaver patterns

  auto cfg = M5.config();
  Serial.println("[boot] calling M5.begin()...");
  M5.begin(cfg);
  Serial.println("[boot] M5.begin() returned");
  Serial.printf("[boot] board = %s\n", M5.getBoard() == m5::board_t::board_M5PaperColor ? "PaperColor (detected correctly)" : "NOT PaperColor!");

  M5.Display.setRotation(0); // native portrait for PaperColor
  Serial.println("[boot] display rotation set, drawing status...");

  loadSettings();

  showStatus("Connecting to Wi-Fi...");
  Serial.println("[boot] status drawn, connecting wifi...");
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    showStatus("Connected. Waiting for Volumio...");
  } else if (!inSetupMode) {
    showStatus("Wi-Fi failed. Retrying...");
  }
  // if inSetupMode, connectWiFi() already drew the setup-mode screen
}

uint32_t lastPollTime = 0;
uint32_t idleSince = 0;           // 0 = not currently idle
uint32_t lastScreensaverRender = 0;

void loop() {
  // M5.update() drives button edge-detection and must run on every loop
  // iteration — a blocking delay() here (as this used to have) would swallow
  // any tap that starts and ends between polls, since wasPressed() only sees
  // transitions across successive update() calls.
  M5.update();

  // Button mapping (per M5Unified's PaperColor pin table): BtnA=G10 (up),
  // BtnB=G9 (down), BtnC=G1 (top).
  if (M5.BtnA.wasPressed()) {
    Serial.println("Up button: next track");
    sendVolumioCommand("next");
  }
  if (M5.BtnB.wasPressed()) {
    Serial.println("Down button: previous track");
    sendVolumioCommand("prev");
  }
  if (M5.BtnC.wasPressed()) {
    enterSetupMode();
  }

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

  if (inSetupMode) {
    setupServer.handleClient();
    if (!inSetupMode) {
      // handleSetupSave() just turned this off with fresh credentials saved —
      // tear down the AP/server and try the real connection now.
      setupServer.stop();
      connectWiFi();
      lastRenderedKey = "\x01__BOOT__\x01"; // force a fresh render once connected
    }
    delay(2);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    delay(10);
    return;
  }

  // Volumio polling is time-gated instead of blocking, so buttons stay responsive.
  if (millis() - lastPollTime < POLL_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastPollTime = millis();

  JsonDocument doc;
  if (fetchState(doc)) {
    String status  = doc["status"]  | "";
    String title   = doc["title"]   | "";
    String artist  = doc["artist"]  | "";
    String album   = doc["album"]   | "";
    String art     = doc["albumart"] | "";
    String service = doc["service"] | "";

    bool playing = (status == "play") && title.length() > 0;
    String key = playing ? (title + "|" + artist + "|" + album) : "__IDLE__";

    if (key != lastRenderedKey) {
      Serial.printf("Track change -> %s (service=%s)\n", key.c_str(), service.c_str());
      Serial.printf("  raw albumart field: \"%s\"\n", art.c_str());
      if (playing) {
        String resolvedArt = resolveAlbumArtUrl(art, service);
        Serial.printf("  resolved art URL: \"%s\"\n", resolvedArt.c_str());
        renderNowPlaying(title, artist, album, resolvedArt);
        idleSince = 0;
      } else {
        renderIdle();
        idleSince = millis();
        lastScreensaverRender = 0;
      }
      lastRenderedKey = key;
    }

    // Screensaver: the state-change check above only fires once on entering
    // idle, since "__IDLE__" never itself changes — this timer-based check
    // is what keeps cycling patterns during a long continuous idle stretch.
    if (!playing && idleSince != 0) {
      uint32_t idleFor = millis() - idleSince;
      uint32_t sinceLastSS = millis() - lastScreensaverRender;
      if (idleFor > SCREENSAVER_DELAY_MS &&
          (lastScreensaverRender == 0 || sinceLastSS > SCREENSAVER_INTERVAL_MS)) {
        Serial.println("Idle screensaver: rendering new pattern");
        renderScreensaver();
        lastScreensaverRender = millis();
      }
    }
  }

  delay(10);
}
