# Liner

A "now playing" display for the [M5Stack PaperColor](https://docs.m5stack.com/en/core/PaperColor) — a 4" color e-paper dev board — driven by [Volumio](https://volumio.com/). It shows album art, title, and artist for whatever's currently playing, and refreshes automatically as tracks change.

The name is a nod to *liner notes* — the printed text and art that ship inside physical album packaging. That's essentially what this recreates on e-paper.

<p align="center">
  <img src="docs/liner-demo.svg" alt="Liner cycling through its Connecting, Now Playing, and Idle screens on an M5Stack PaperColor" width="360">
</p>

## Features

- **Now-playing card**: full-width album art on top, title/artist/album text below
- **Idle screen** when nothing is playing
- **Event-driven refresh** — only redraws when the track actually changes, since a full e-paper refresh takes 15–30 seconds
- **Playback controls** — up/down buttons skip to the next/previous track
- **Wi-Fi network cycling** — the top button switches between a configurable list of known networks
- **Album art from any source** — local files, Tidal, and (untested so far) Spotify/other Volumio plugins, via a service-aware resolver

## Hardware

- **Board**: M5Stack PaperColor — ESP32-S3R8 (16MB flash, 8MB octal PSRAM), 400×600 4" E Ink Spectra 6 (6-color) e-paper panel
- **Buttons**: 3 user buttons (top = GPIO1, up = GPIO10, down = GPIO9) + 1 power button
- **Network**: connects to Wi-Fi directly; polls Volumio over your LAN

## How it works

### Firmware

Built with [PlatformIO](https://platformio.org/) on the Arduino framework, using [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX). `M5.begin()` auto-detects the PaperColor board and its `Panel_ED2208` e-paper driver — the firmware just draws normal RGB888 graphics, and the panel driver quantizes everything down to the 6-color Spectra palette automatically on `display()`.

Because a full refresh takes 15–30 seconds, the main loop polls Volumio for state every few seconds but only triggers a screen redraw when the track (title/artist/album) actually changes — not on every poll.

### Talking to Volumio

Volumio exposes a REST API on the local network. Liner polls:

```
GET http://<volumio-host>:3000/api/v1/getState
```

which returns the current `status`, `title`, `artist`, `album`, `albumart`, and `service` (which plugin/source is playing — `mpd` for local files, `spop`/`volspotconnect2` for Spotify Connect, etc.). Playback controls use the same API's commands endpoint:

```
GET http://<volumio-host>:3000/api/v1/commands/?cmd=next
GET http://<volumio-host>:3000/api/v1/commands/?cmd=prev
```

Because Volumio abstracts every backend (local library, Tidal, Spotify, internet radio, ...) into this one interface, the firmware doesn't need to know anything about the specific streaming service — it just reads whatever Volumio reports.

### Album art: the progressive JPEG problem

The one place the source *does* matter is album art. Volumio's `albumart` field is either:

- a **relative path** (`/albumart?...`) — art Volumio serves itself, generally from local file tags — fetched directly, or
- an **absolute URL** to a streaming service's CDN (e.g. Tidal's `resources.tidal.com`)

Tidal (and many other CDNs) serve album art as **progressive JPEG**, which M5GFX's embedded JPEG decoder (TJpgDec) can't decode — it only supports baseline JPEG. Fetching worked fine; `drawJpg()` just silently failed on every image.

The fix: external art URLs are routed through [wsrv.nl](https://wsrv.nl), a free public image-resizing proxy, which re-encodes the image as baseline JPEG and resizes it to the display's art box in one request:

```
https://wsrv.nl/?url=<encoded-source-url>&w=400&h=400&fit=cover&output=jpg
```

This also shrinks the download significantly (a 105KB source image came back as 35KB), which helps given how slow a full-size fetch over Wi-Fi + HTTPS can be. Local Volumio-served art skips the proxy and is fetched directly, since it hasn't shown the same progressive-JPEG issue.

Album art resolution is dispatched by Volumio's `service` field (`resolveAlbumArtUrl()` in `src/main.cpp`), so each source has its own clear spot for future quirks. **Only the Tidal path has actually been exercised** — local file (`mpd`) and Spotify (`spop`/`volspotconnect2`) branches exist as stubs that currently fall through to the same generic logic, untested.

### Controls

| Button | Location | Action |
|---|---|---|
| Up | GPIO10 | Next track (`cmd=next`) |
| Down | GPIO9 | Previous track (`cmd=prev`) |
| Top | GPIO1 | Cycle to the next known Wi-Fi network |

Button polling is non-blocking (`M5.update()` runs every loop iteration; Volumio polling is time-gated separately) — an earlier version blocked on a 5-second `delay()` between polls, which meant a quick tap could start and end entirely between checks and never register.

## Setup

1. Copy the config template and fill in your own values:
   ```
   cp include/config.h.example include/config.h
   ```
   Set your Wi-Fi network(s) (`KNOWN_NETWORKS`) and your Volumio host (`VOLUMIO_HOST`).

2. Build:
   ```
   pio run
   ```

3. Flash. The PaperColor's factory firmware doesn't expose a USB serial console during normal boot, so you'll need to manually put it into download mode first: **hold the reset button** while it's connected over USB, then run:
   ```
   pio run -t upload --upload-port /dev/cu.usbmodemXXXX
   ```

4. **After flashing, press the power button once** to boot into the new firmware. In practice, esptool's software-triggered reset at the end of upload doesn't reliably boot this board into the app — a physical button press is needed. This sometimes takes a couple of tries.

## Known limitations

- Only the Tidal album art path is verified; local/Spotify/other-service art handling is untested (see stubs in `resolveAlbumArtUrl()`).
- Network cycling is a simple compile-time list, not an interactive on-screen picker — given e-paper's refresh time, browsing a menu on-device would be slow. A menu system may come later.
- Flashing and booting both currently require manual button presses (see Setup) — software-triggered resets aren't reliable on this board/toolchain combination.
