# Tower Chronometer

An ESP32-based cycle timer for hydroponic towers. It switches a pump between
**FLOW** and **IDLE** periods on a schedule you set from a web page, and logs
every cycle to a microSD card.

## Features

- Web dashboard at `http://timer.local` — two-column layout with large,
  high-contrast type
- Adjustable FLOW / IDLE minutes, applied live with the ENGAGE SETTINGS button
- Pausable auto-refresh, so the page never reloads while you're typing
- Manual START / STOP override
- Automatic location + live weather (Open-Meteo, no API key needed),
  with a freeze warning banner at 35°F or below
- Pump hour meter with a 500-hour maintenance reminder
- SD card event log with automatic rotation (100 KB cap, archive viewer)
- Captive-portal WiFi setup, mDNS, and OTA firmware updates

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | 30-pin DevKitC ("ESP32-DevKit-32E Core Board ESP32 WIFI Bluetooth Development Board 2 Core Module"). Note: 38-pin boards label some pins differently — go by GPIO number. Powered via USB 5V |
| 1-channel relay module | 5V optocoupler type ("5V 1 Channel Relay Board Module Optocoupler LED for Arduino PiC ARM AVR"). Active-LOW, signal driven by GPIO 17, 5V from the Vin pin |
| Duplex receptacle (outlet) | **GFCI type required** — the cord runs into the humid tent; pump plugs in here |
| MicroSD card module | SPI interface |
| MicroSD card | FAT32 formatted |
| Double-gang plastic box | Houses the ESP32 + SD module |
| Single-gang plastic box | Houses the relay module + receptacle (tight fit — double-gang recommended) |
| 4-conductor cable | 3 conductors used (5V, GND, signal), strain reliefs at both ends |
| Mains power cord | With strain relief into the single-gang box |

See [wiring-diagram.svg](wiring-diagram.svg) for the hookup.

### Pin connections

**Relay module** (in the single-gang box with the receptacle)

| ESP32 | Relay |
|---|---|
| GPIO 17 | IN (active-LOW: LOW = pump ON) |
| Vin (5V) | VCC |
| GND | GND |

The module has a built-in flyback diode and driver transistor.

**Mains wiring** (single-gang box)

| From | To |
|---|---|
| Mains HOT (black) | Relay COM terminal |
| Relay NO terminal | GFCI LINE HOT terminal |
| Mains neutral (white) | GFCI LINE neutral terminal (straight through) |
| Mains ground (green/bare) | GFCI ground terminal (straight through) |

The relay switches the hot side only, ahead of the GFCI's LINE input —
everything the pump draws is GFCI-protected. If you are not comfortable
with mains wiring, have an electrician check it.

**MicroSD module (SPI)** (in the double-gang box with the ESP32)

| ESP32 | SD module |
|---|---|
| GPIO 23 | MOSI |
| GPIO 19 | MISO |
| GPIO 18 | SCK |
| GPIO 5 | CS |
| 3V3 | VCC (most modules also accept 5V) |
| GND | GND |

### Enclosures

- The ESP32 and SD module live in a **double-gang** plastic box.
- The relay module and duplex receptacle live in a **single-gang**
  plastic box — it fits, but it's cramped; a double-gang box would have
  been the better call.
- A 4-conductor cable (3 conductors used: 5V, GND, GPIO 17 signal)
  runs between the boxes with strain reliefs at both ends.

## Software setup

1. Install the ESP32 board support in the Arduino IDE.
2. Install these libraries (Library Manager):
   - `ESPAsyncWebServer`
   - `AsyncTCP`
   - `WiFiManager` by tzapu
3. Open `tower-chronometer.ino`, select your ESP32 board and port, and upload.
4. On first boot, join the `Tower-Setup` WiFi network on your phone or
   computer and enter your home WiFi credentials.
5. Open `http://timer.local` in a browser.

### Troubleshooting

- **`http://timer.local` doesn't resolve**: some networks and routers
  don't handle mDNS well. Open the serial monitor at 115200 baud and use
  the IP address on the `Connected! IP:` line instead.

## Using the web UI

- **FLOW / IDLE MINUTES** — type new values, then press **ENGAGE SETTINGS**.
  The new cycle starts immediately. If the page refreshes before you can
  press the button, hit **PAUSE UPDATES** first.
- **MANUAL STOP / MANUAL START** — immediate override; the schedule
  resumes from there.
- **VIEW SYSTEM LOGS** — every automatic cycle and manual override,
  newest entries at the bottom. The log rotates automatically at 100 KB
  (older half moves to the archive); **VIEW PREVIOUS LOG (ARCHIVE)**
  shows it.
- **[ RESET SD LOG FILE ]** — asks you to type `DELETE` before erasing,
  so a stray tap can't wipe history.

## Configuration

Constants near the top of the sketch:

| Constant | Default | Meaning |
|---|---|---|
| `FREEZE_WARN_F` | 35.0 | Freeze banner threshold (°F) |
| `MAINT_HOURS` | 500.0 | Pump cleaning reminder interval (hours) |
| `LOG_MAX_BYTES` | 100 KB | SD log size before rotation |
| `WX_INTERVAL` | 15 min | Weather refresh interval |
| `PUMP_SAVE_INTERVAL` | 60 s | How often the hour meter is saved |

## Changelog (highlights)

- v59.9 — weather parser fix, more prominent date/time
- v59.8 — fixed template corruption bug, JSON geolocation, styled log viewer
- v59.7 — two-column layout
- v59.6 — pump hour meter + maintenance reminder
- v59.5 — weather display + freeze warning
- v59.4 — readability pass (larger type)
- v59.3 — SD log rotation
- v59.2 — pausable refresh, prominent ENGAGE SETTINGS

## License

MIT — see [LICENSE](LICENSE).
