# Remote Generator Start System — Project Summary

## Overview

A remote start/stop system for a diesel generator, communicating over 915MHz LoRa radio between a control interface in a workshop and a relay controller in the generator shed. The system operates entirely off-grid and over a local WiFi network with no cloud dependency.

---

## Design Philosophy

- **Reliability first** — every design decision prioritizes reliable operation over convenience or cost savings
- **Self-sufficient controller** — the generator-end device operates independently once commanded; it does not depend on continuous server connectivity
- **Honest status reporting** — the UI distinguishes between confirmed states (voltage sensing) and assumed states (relay closed but unconfirmed)
- **Fail-safe defaults** — the generator can never run indefinitely; every start requires a timer, and the controller enforces its own independent safety timer
- **No cloud dependency** — the system operates entirely on the local network; no internet required for any function
- **Progressive enhancement** — voltage sensing was designed as an optional layer; the system functions without it and upgrades gracefully when hardware is present

---

## Physical Layout

- **Generator shed** — remote location, no mains power, powered only by the generator battery (12V DC). Outside WiFi range. Houses the **controller board** and associated hardware.
- **Workshop (shop)** — mains power (120V AC), WiFi connected. Houses the **server board**.
- **Distance** — approximately 500 feet between buildings with potential RF obstruction.

---

## Hardware

### Both Ends
| Item | Notes |
|------|-------|
| LilyGo TTGO LoRa32 T3 V1.6.1 | ESP32-PICO-D4, 915MHz LoRa (SX1276), OLED display, WiFi/BT |
| 915MHz LoRa antenna | External SMA whip antenna |
| RG-58 coax + SMA bulkhead connector | Antenna remote-mounted outside enclosure for RF clearance |

**Critical board note:** Board is the T3 V1.6.1 variant. Arduino IDE board selection must be set to **"TTGO LoRa32 V2.1 (1.6.1)"** — other selections cause WiFi/LoRa conflicts on this chip variant (ESP32-PICO-D4).

### Controller End (Generator Shed)
| Item | Notes |
|------|-------|
| LilyGo LoRa32 T3 V1.6.1 | As above |
| 12V to 5V buck converter (LM2596 or MP1584) | Powers all components from generator battery |
| 470µF electrolytic capacitor | Across 12V input to buck converter |
| 100µF electrolytic capacitor | Across 5V output of buck converter |
| Single channel relay module, 5V coil, optoisolated | H-trigger jumper position; controls generator 12V start signal |
| 70x90mm perfboard | Hosts power bus, voltage divider, ZMPT101B connections |
| Voltage divider circuit | R1=47kΩ, R2=10kΩ, 0.1µF filter cap; scales 12V battery to safe ADC input |
| ZMPT101B AC voltage sensor module | Optoisolated; detects generator AC output presence |
| IP65 waterproof enclosure | Protects all components in generator shed environment |
| Rocker switch | In series on 12V input; cuts all power cleanly |

**Power architecture:** Star topology — buck converter 5V output feeds a common bus on the perfboard. LilyGo board, relay module, and ZMPT101B all draw from this bus independently. LilyGo powered via 5V GPIO header pin, not USB or JST connector.

**Generator control signal:** Sustained contact closure (relay NO+COM). Contacts closed = generator runs. Contacts open = generator stops.

### Server End (Workshop)
| Item | Notes |
|------|-------|
| LilyGo LoRa32 T3 V1.6.1 | As above |
| 5V USB wall adapter | Mains powered |
| Indoor project enclosure | No waterproofing required |

---

## Pin Assignments (T3 V1.6.1)

| Function | GPIO | Notes |
|----------|------|-------|
| LoRa SCK | 5 | SPI |
| LoRa MISO | 19 | SPI |
| LoRa MOSI | 27 | SPI |
| LoRa CS | 18 | SPI |
| LoRa RST | 23 | Use `LORA_RESET` in code to avoid conflict with board definition |
| LoRa DIO0 | 26 | Interrupt |
| OLED SDA | 21 | I2C |
| OLED SCL | 22 | I2C |
| Relay output | 13 | Controller only |
| Battery sense | 34 | Analog input only; voltage divider output |
| AC sense | 35 | Analog input only; ZMPT101B signal output |

---

## Firmware Architecture

### Libraries Required (Both Boards)
- `arduino-LoRa` by Sandeep Mistry
- `Adafruit SSD1306`
- `Adafruit GFX`

### Additional Libraries (Server Only)
- `ESPAsyncWebServer` by ESP32Async
- `AsyncTCP` by ESP32Async
- `ArduinoJson` by Benoit Blanchon
- `ESPmDNS` (built into ESP32 Arduino core)

### Credentials Management
WiFi credentials stored in `secrets.h` (excluded from version control). A `secrets.h.example` template is committed to the repository.

---

## LoRa Communication Protocol

Point-to-point raw LoRa (no Meshtastic). Simple text message protocol with address prefixes:

| Direction | Message | Meaning |
|-----------|---------|---------|
| Server → Controller | `CTRL:CMD:START:90` | Start with 90-minute timer |
| Server → Controller | `CTRL:CMD:STOP` | Stop generator |
| Server → Controller | `CTRL:CMD:STATUS` | Request status update |
| Controller → Server | `SERV:ACK:START` | Relay closed confirmation |
| Controller → Server | `SERV:ACK:STOP` | Relay opened confirmation |
| Controller → Server | `SERV:STATUS:ON:12.7` | Power confirmed on, battery voltage |
| Controller → Server | `SERV:STATUS:OFF` | Power confirmed off |
| Controller → Server | `SERV:STATUS:ASSUMED_ON` | Relay closed, no voltage sensing |
| Controller → Server | `SERV:STATUS:ASSUMED_OFF` | Relay open, no voltage sensing |
| Controller → Server | `SERV:HB:RELAY:ON:AC:ON:BAT:12.7` | Heartbeat |
| Controller → Server | `SERV:ERR:START_FAILED` | No AC detected after start |
| Controller → Server | `SERV:ERR:STOP_FAILED` | AC still present after stop |
| Controller → Server | `SERV:ERR:UNCOMMANDED_SHUTDOWN` | AC lost while relay closed |
| Controller → Server | `SERV:ERR:SAFETY_TIMEOUT` | Controller safety timer expired |

**LoRa settings:** 915MHz, TX power 17dBm, bandwidth 125kHz, spreading factor 8, coding rate 5. Both boards must use identical settings.

---

## Server Firmware Functionality

- Hosts a lightweight async web server on port 80
- Static IP: `172.17.0.10` (DHCP reservation also configured in router)
- mDNS hostname: `generator.local`
- WiFi initializes before LoRa (critical — reverse order causes WiFi corruption on this chip)
- Hardware watchdog timer (30 second timeout) for automatic recovery from lockups
- WiFi health watchdog checks every 30 seconds; attempts reconnection; restarts after 3 failed attempts
- Non-blocking LoRa ACK state machine (blocking approach caused watchdog conflicts with ESPAsyncWebServer)
- ACK timeout: 15 seconds, 1 retry (generous to accommodate controller confirmation delay)
- Heartbeat timeout: 2 minutes (2 missed heartbeats = controller offline warning)
- OLED display: WiFi SSID, IP, RSSI, controller online status, battery voltage, system state, display countdown timer (60 minute timeout)

### Web UI Features
- Preset timed start buttons: 20 min, 60 min, 90 min
- Custom timer: 1–360 minutes (6 hour maximum — no indefinite run permitted)
- Stop button
- Pending state: all buttons disabled with spinner during command in flight
- Status display: running/stopped/error/unknown with detail text
- Battery voltage display with color-coded warnings
- Activity log with timestamps
- Timer countdown display (restored from server on page reload)
- Diagnostics section: server uptime, time since last LoRa message, RSSI with signal quality indicator

---

## Controller Firmware Functionality

- Listens for LoRa commands; operates relay accordingly
- Two-stage confirmation: immediate ACK (relay state) followed by STATUS (power confirmed)
- **Startup confirmation:** 2 consecutive AC-present readings within 15 seconds
- **Shutdown confirmation:** 3 seconds sustained AC-absent within 15 seconds; accommodates slow diesel generator voltage decay
- **Uncommanded shutdown detection:** monitors AC presence while relay is closed; reports `ERR:UNCOMMANDED_SHUTDOWN` after 4 seconds sustained absence
- **Independent safety timer:** receives timer value with start command; stops generator 2 minutes after server timer would have expired; protects against server going offline mid-run
- Heartbeat every 60 seconds: relay state, AC state, battery voltage, warnings
- Sends heartbeat immediately on boot; responds to `CMD:STATUS` requests
- OLED display: RSSI, relay state, AC state, battery voltage with warnings, safety timer countdown, last TX/RX messages, display countdown timer (60 minute timeout)

### Voltage Sensing
- **Battery voltage (GPIO 34):** 16-sample averaged ADC read; voltage divider ratio 0.17543 (47kΩ/10kΩ); calibration offset 1.0V; warnings at 12.0V (low) and 11.5V (critical)
- **AC presence (GPIO 35):** 100-sample peak-to-peak measurement; threshold 300 counts; two-reading confirmation to filter transition artifacts
- `VOLTAGE_SENSING_ENABLED` constant toggles between real sensing and timed placeholder

---

## Network Configuration

- Router: Ubiquiti Dream Machine
- Network: `172.17.0.x`, subnet `255.255.255.0`, gateway `172.17.0.1`
- DHCP pool: `172.17.0.100–254`
- Server board static IP: `172.17.0.10` (below DHCP pool, no conflict risk)
- DHCP reservation also set in Dream Machine for server board MAC address
- Controller board: WiFi disabled; LoRa only

---

## Build Phases Completed

### Phase 1 — LoRa Communication
Both boards communicate over raw LoRa. Server sends test messages; controller echoes acknowledgements. RSSI validated at close range (-20 to -30 dBm at bench proximity).

### Phase 2 — Relay Control
Controller firmware updated to parse `CMD:START` and `CMD:STOP` and operate relay GPIO accordingly. Tested with multimeter before connecting to generator.

### Phase 3 — Web Server
Server board hosts HTML control page over WiFi. Start/stop buttons functional. Timer controls implemented. Pending state feedback (spinner, disabled buttons) implemented.

### Phase 4 — Full Integration
Web UI buttons connected to LoRa command pipeline. Non-blocking ACK state machine implemented to prevent watchdog conflicts. Timer value passed to controller for independent safety timer.

### Phase 5 — Voltage Sensing and Monitoring
Voltage divider and ZMPT101B calibrated and validated on bench. Real voltage sensing integrated into controller confirmation logic. Heartbeat system implemented. Uncommanded shutdown detection added. Web UI diagnostics section added.

### Ongoing — Stability and Polish
WiFi watchdog and hardware watchdog added. OLED diagnostics on both boards. Boot status reporting. mDNS hostname. Static IP assignment. Uptime display.

---

## Known Issues and Notes

- ESPAsyncWebServer requires WiFi to initialize before LoRa on this chip variant; reverse order causes silent WiFi corruption
- Board definition must be "TTGO LoRa32 V2.1 (1.6.1)" in Arduino IDE; other selections cause upload failures or runtime WiFi issues on the ESP32-PICO-D4
- `LORA_RST` is reserved by the board definition (defined as GPIO 14); firmware uses `LORA_RESET` (GPIO 23) to avoid conflict
- `StaticJsonDocument` is deprecated in newer ArduinoJson; use `JsonDocument` instead
- `send_P()` is deprecated in newer ESPAsyncWebServer; use `send()` instead
- Occasional web server lockups observed during development; hardware watchdog and WiFi watchdog added as mitigation
- DHCP conflicts were root cause of intermittent WiFi unreachability; resolved with static IP and DHCP reservation

---

## Remaining Work

- Physical installation in generator shed and workshop
- Antenna placement optimization and real-world RSSI validation at 500 foot range
- Calibration verification with actual generator running (AC sensing, battery voltage under load)
- Stage 5 Telegram notification integration (optional future feature)
- Runtime hour tracking and maintenance alerts (optional future feature)
- Field testing of full start/stop/confirmation sequence with real generator
