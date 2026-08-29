#pragma once

// Wi-Fi — ADD-YOUR-WIFI-HERE is open and MAC-allowlisted, so no password.
#define WIFI_SSID     "ADD-YOUR-WIFI-HERE"
#define WIFI_PASSWORD ""

// Volumio host on the local network.
#define VOLUMIO_HOST  "volumio.local"
#define VOLUMIO_PORT  3000   // Volumio's default web/API port

// How often to poll Volumio's getState endpoint for changes.
#define POLL_INTERVAL_MS 5000
