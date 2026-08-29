#pragma once

// Wi-Fi credentials and the Volumio host are NOT configured here — they're
// entered through the device's own setup web page (press the top button, or
// it starts automatically on first boot / if no saved network connects) and
// stored in NVS. That's what lets one built binary work for anyone.

// Seed value only, used the very first time the device boots with nothing
// saved yet. Volumio's default web/API port; change here if yours differs.
#define VOLUMIO_HOST_DEFAULT "volumio.local"
#define VOLUMIO_PORT 3000

// How often to poll Volumio's getState endpoint for changes.
#define POLL_INTERVAL_MS 5000

// Name of the setup Wi-Fi access point the device broadcasts when no network
// is configured, a connection attempt fails, or the top button is pressed.
#define SETUP_AP_NAME "Liner-Setup"
