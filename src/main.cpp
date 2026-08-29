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

#include "config.h"

static const int PANEL_W = 400;
static const int PANEL_H = 600;
static const int ART_SIZE = 400; // album art occupies the top, full-width square

String lastRenderedKey = "\x01__BOOT__\x01"; // sentinel so the first real state always renders

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

int currentNetworkIndex = 0;

void showStatus(const char* msg) {
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

void connectWiFi() {
  const KnownNetwork& net = KNOWN_NETWORKS[currentNetworkIndex];

  WiFi.mode(WIFI_STA);
  WiFi.begin(net.ssid, net.password);

  Serial.printf("Connecting to Wi-Fi: %s\n", net.ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin("papercolor"); // also enables outgoing .local resolution
  } else {
    Serial.println("\nWi-Fi connect failed, will keep retrying in the loop.");
  }
}

// Advances to the next known network (wrapping around) and reconnects.
// Triggered by the top button (BtnC).
void cycleToNextNetwork() {
  if (KNOWN_NETWORKS_COUNT <= 1) {
    Serial.println("Only one known network configured, nothing to cycle to.");
    return;
  }
  currentNetworkIndex = (currentNetworkIndex + 1) % KNOWN_NETWORKS_COUNT;
  const KnownNetwork& net = KNOWN_NETWORKS[currentNetworkIndex];
  Serial.printf("Top button: switching to network \"%s\"\n", net.ssid);

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting to %s...", net.ssid);
  showStatus(msg);

  WiFi.disconnect();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(msg, sizeof(msg), "Connected to %s", net.ssid);
    showStatus(msg);
  } else {
    snprintf(msg, sizeof(msg), "Failed: %s", net.ssid);
    showStatus(msg);
  }
  lastRenderedKey = "\x01__BOOT__\x01"; // force a fresh now-playing render after reconnect
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
  String base = "http://" + String(VOLUMIO_HOST) + ":" + String(VOLUMIO_PORT);
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
  String url = "http://" + String(VOLUMIO_HOST) + ":" + String(VOLUMIO_PORT) + "/api/v1/getState";
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
  String url = "http://" + String(VOLUMIO_HOST) + ":" + String(VOLUMIO_PORT) +
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
  Serial.println("\n[boot] Serial up");

  auto cfg = M5.config();
  Serial.println("[boot] calling M5.begin()...");
  M5.begin(cfg);
  Serial.println("[boot] M5.begin() returned");
  Serial.printf("[boot] board = %s\n", M5.getBoard() == m5::board_t::board_M5PaperColor ? "PaperColor (detected correctly)" : "NOT PaperColor!");

  M5.Display.setRotation(0); // native portrait for PaperColor
  Serial.println("[boot] display rotation set, drawing status...");

  showStatus("Connecting to Wi-Fi...");
  Serial.println("[boot] status drawn, connecting wifi...");
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    showStatus("Connected. Waiting for Volumio...");
  } else {
    showStatus("Wi-Fi failed. Retrying...");
  }
}

uint32_t lastPollTime = 0;

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
    cycleToNextNetwork();
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
      } else {
        renderIdle();
      }
      lastRenderedKey = key;
    }
  }

  delay(10);
}
