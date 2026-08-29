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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
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

// ---------------------------------------------------------------------------
// Volumio
// ---------------------------------------------------------------------------

// Resolves Volumio's (possibly relative) albumart path into a fetchable URL.
String resolveAlbumArtUrl(const String& albumart) {
  if (albumart.length() == 0) return "";
  if (albumart.startsWith("http://") || albumart.startsWith("https://")) {
    return albumart;
  }
  String base = "http://" + String(VOLUMIO_HOST) + ":" + String(VOLUMIO_PORT);
  if (!albumart.startsWith("/")) base += "/";
  return base + albumart;
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

// Fetches raw bytes for the album art JPEG into a PSRAM buffer.
// Caller owns the returned buffer and must free() it. Returns nullptr on failure.
uint8_t* fetchAlbumArt(const String& url, size_t& outLen) {
  if (url.length() == 0) return nullptr;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Album art fetch failed, HTTP %d\n", code);
    http.end();
    return nullptr;
  }

  int len = http.getSize();
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

  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  uint32_t start = millis();
  while (received < (size_t)len && millis() - start < 8000) {
    if (stream->available()) {
      int n = stream->read(buf + received, len - received);
      if (n > 0) received += n;
    } else {
      delay(10);
    }
  }
  http.end();

  if (received != (size_t)len) {
    Serial.printf("Album art incomplete: %u/%d bytes\n", (unsigned)received, len);
    free(buf);
    return nullptr;
  }

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
      M5.Display.drawJpg(art, artLen, 0, 0, ART_SIZE, ART_SIZE);
      free(art);
      artDrawn = true;
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
  M5.Display.setTextColor(TFT_DARKGRAY, TFT_WHITE);
  drawWrapped(album, textX, y, textWidth, 20);

  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  M5.Display.setRotation(0); // native portrait for PaperColor

  showStatus("Connecting to Wi-Fi...");
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    showStatus("Connected. Waiting for Volumio...");
  } else {
    showStatus("Wi-Fi failed. Retrying...");
  }
}

void loop() {
  M5.update();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    delay(1000);
    return;
  }

  JsonDocument doc;
  if (fetchState(doc)) {
    String status = doc["status"] | "";
    String title  = doc["title"]  | "";
    String artist = doc["artist"] | "";
    String album  = doc["album"]  | "";
    String art    = doc["albumart"] | "";

    bool playing = (status == "play") && title.length() > 0;
    String key = playing ? (title + "|" + artist + "|" + album) : "__IDLE__";

    if (key != lastRenderedKey) {
      Serial.printf("Track change -> %s\n", key.c_str());
      if (playing) {
        renderNowPlaying(title, artist, album, resolveAlbumArtUrl(art));
      } else {
        renderIdle();
      }
      lastRenderedKey = key;
    }
  }

  delay(POLL_INTERVAL_MS);
}
