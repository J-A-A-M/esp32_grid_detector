# esp32_grid_detector

ESP32-based electricity grid monitor for Ukraine. Detects power outages via GPIO and reports status to a central WebSocket server. Server-side services live in the RespublikaChatBot repo (`docker-compose.grid.yml`).

## Repository Structure

```
grid_detector/   — Main ESP32 firmware (PlatformIO/Arduino)
flasher/         — One-time ID flasher firmware (PlatformIO/Arduino)
.github/
  workflows/
    compile.yml          — Build firmware on PRs to master
    release.yml          — Create GitHub Release + push OTA to devices
    update_firmware.yml  — Push OTA to devices manually
```

## Firmware Commands

```bash
# Build
cd grid_detector && pio run

# Flash
cd grid_detector && pio run --target upload

# Serial monitor (115200 baud)
cd grid_detector && pio device monitor -b 115200

# Erase NVS settings (factory reset)
cd grid_detector && pio run --target erase
```

## New Device Setup

1. Edit `flasher/src/IdFlasher.cpp` — set `identifier`, `ssid`, `password`, `firmwareUrl`
2. Flash flasher: `cd flasher && pio run --target upload`
3. Flash main firmware: `cd grid_detector && pio run --target upload`
4. On first boot the device opens AP **"GridDetector"** — connect and set WiFi credentials via the portal

## Compile-time Flags (top of GridDetector.cpp)

| Flag | Default | Purpose |
|------|---------|---------|
| `WIFI` | 1 | Enable WiFi (WiFiManager) |
| `ETHERNET` | 0 | Enable LAN8720 ethernet |
| `GRID` | 1 | Enable grid detection logic |
| `ARDUINO_OTA_ENABLED` | 1 | Enable OTA via ArduinoOTA |

The release workflow patches these flags via `sed` before compiling — never toggle both WIFI and ETHERNET to 1.

## Hardware

- **Grid pin**: GPIO2 with `INPUT_PULLUP`
- **Logic is inverted**: LOW = grid **online**, HIGH = grid **offline**
- **Reaction time**: 2000 ms debounce before sending event (configurable via `rt` NVS key)

## Device Settings (NVS namespace `storage`)

| Key | Default | Description |
|-----|---------|-------------|
| `id` | `"test"` | Node identifier (set by flasher) |
| `dn` | `"Grid Detector"` | Device name |
| `bn` | `"griddetector"` | mDNS broadcast name |
| `host` | `"grid.respublika.pp.ua"` | WebSocket server host |
| `wsp` | 39447 | WebSocket port |
| `wsat` | 150000 ms | Reconnect alert threshold |
| `wsrt` | 300000 ms | Auto-reboot threshold |
| `rt` | 2000 ms | Grid state reaction time |

Settings are configurable via WiFiManager web portal (port 80) after connection.

## WebSocket Protocol

**Device → Server** (plain text):
- `node:ID` — node identifier
- `firmware:VER` — current firmware version
- `chip_id:HEXID` — ESP32 chip ID
- `connect_mode:wifi|ethernet`
- `grid:online` / `grid:offline`
- `pong`

**Server → Device** (JSON):
- `{"payload":"ping"}` — heartbeat
- `{"payload":"update","url":"http://...","delay":N}` — OTA update
- `{"payload":"update_cancel"}` — cancel pending update
- `{"payload":"reboot"}` — remote reboot

## Release

Triggered via GitHub Actions → `release.yml` with `release-version` input:
1. Compiles WiFi and Ethernet builds (patches `#define WIFI/ETHERNET` via `sed`)
2. Creates GitHub Release with both `.bin` files
3. Calls web server (`UPDATE_FIRMWARE_URL` secret) to push OTA to all connected devices

## Gotchas

- GPIO2 logic is inverted — don't confuse LOW/HIGH in code
- Release workflow patches `VERSION` string via `sed` — the value in source is only for dev builds
- NVS migration: on first boot of new firmware, if stored `host == "alerts.net.ua"` it auto-migrates to `grid.respublika.pp.ua`
- `nodes_list` in `websocket_server.py` (RespublikaChatBot repo) is hardcoded — add new node identifiers there to track them
