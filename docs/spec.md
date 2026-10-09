# Generator Remote Start System — Coding Agent Specification

## Purpose of This Document

This spec describes the current state of the firmware codebase for a LoRa-based remote generator start system. It is written for a coding agent continuing work on this project. Read the companion summary document (`generator-remote-start-summary.md`) for hardware, architecture, and build history context. This document focuses on what exists, what is incomplete, and what the agent needs to produce or fix.

---

## Repository Structure

```
generator-controller-radio/
├── server/
│   ├── server.ino          — Server board firmware (main sketch)
│   ├── index.h             — Web UI HTML/CSS/JS embedded as a C string
│   └── secrets.h           — WiFi credentials (NOT in version control)
│   └── secrets.h.example   — Credential template (IS in version control)
├── controller/
│   └── controller.ino      — Controller board firmware (main sketch)
└── .gitignore              — Must include secrets.h
```

Both sketches are Arduino IDE projects targeting the **LilyGo TTGO LoRa32 T3 V1.6.1**. Board selection in Arduino IDE must be **"TTGO LoRa32 V2.1 (1.6.1)"**.

---

## Required Libraries

Install via Arduino IDE Library Manager:

| Library | Author | Notes |
|---------|--------|-------|
| arduino-LoRa | Sandeep Mistry | LoRa radio driver |
| Adafruit SSD1306 | Adafruit | OLED display |
| Adafruit GFX | Adafruit | OLED graphics dependency |
| ESP Async WebServer | ESP32Async | Server only — must be this fork |
| AsyncTCP | ESP32Async | Server only — must match above author |
| ArduinoJson | Benoit Blanchon | Server only |
| ESPmDNS | Built into ESP32 core | Server only |
| esp_task_wdt | Built into ESP32 core | Server only |

---

## Pin Constants

Use these exact names and values in both sketches. `LORA_RESET` (not `LORA_RST`) avoids a conflict with the board definition which reserves `LORA_RST` as GPIO 14.

```cpp
#define LORA_SCK      5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_SS       18
#define LORA_RESET    23    // NOT LORA_RST — reserved by board definition
#define LORA_DIO0     26
#define OLED_SDA      21
#define OLED_SCL      22

// Controller only
#define RELAY_PIN           13
#define BATTERY_SENSE_PIN   34
#define AC_SENSE_PIN        35
```

---

## LoRa Settings — Must Match on Both Boards

```cpp
#define LORA_FREQUENCY    915E6
#define LORA_TX_POWER     17
#define LORA_BANDWIDTH    125E3
#define LORA_SPREAD       8
#define LORA_CODERATE     5
```

---

## Message Protocol

Every message is prefixed with the destination address. The receiver ignores messages not addressed to it.

```
CTRL:CMD:START:90       Server → Controller   Start with 90-minute timer
CTRL:CMD:STOP           Server → Controller   Stop generator
CTRL:CMD:STATUS         Server → Controller   Request status update

SERV:ACK:START          Controller → Server   Relay closed (immediate)
SERV:ACK:STOP           Controller → Server   Relay opened (immediate)
SERV:STATUS:ON:12.7     Controller → Server   AC confirmed present, battery voltage
SERV:STATUS:OFF         Controller → Server   AC confirmed absent
SERV:STATUS:ASSUMED_ON  Controller → Server   Relay closed, no voltage sensing
SERV:STATUS:ASSUMED_OFF Controller → Server   Relay open, no voltage sensing
SERV:HB:RELAY:ON:AC:ON:TMR:3240:BAT:12.7          Heartbeat, no warnings
SERV:HB:RELAY:ON:AC:ON:TMR:3240:BAT:11.4:WARN:CRITICAL   Heartbeat with warning
SERV:HB:RELAY:OFF:AC:OFF:TMR:0:BAT:12.7           Heartbeat, relay open, no timer
SERV:ERR:START_FAILED         No AC detected within 15s of start
SERV:ERR:STOP_FAILED          AC still present 15s after stop
SERV:ERR:UNCOMMANDED_SHUTDOWN AC lost for 4s while relay closed
SERV:ERR:SAFETY_TIMEOUT       Controller safety timer expired
SERV:ERR:INVALID_TIMER        CMD:START had a missing or out-of-range timer (relay NOT closed)
SERV:ERR:UNKNOWN_CMD          Unrecognized command received
```

**Protocol v1.1 changes (additive):**

- `ERR:INVALID_TIMER` is new. The controller sends it for `CMD:START` with no timer, a non-numeric timer, or a timer outside 1-360. The relay is not touched.
- Heartbeats gain a `TMR:<seconds>` field before `BAT`. It is the controller safety timer remaining (0 if none armed). A rebooted server uses it to rebuild its countdown and auto-stop timer.
- Parsers find fields by search (`:TMR:`, `:BAT:`, `:WARN:`), so a v1.1 server tolerates a v1.0 controller (no `TMR`) and a v1.0 server ignores `TMR`. The two boards can be flashed one at a time.
- Interim note: a v1.0 controller answers the boot `CMD:STATUS` with `ERR:UNKNOWN_CMD`, so a v1.1 server paired with a v1.0 controller may show an error at boot until the next command. This clears once the controller is flashed.

---

## secrets.h Format

```cpp
#define WIFI_SSID     "your_network_name"
#define WIFI_PASSWORD "your_password"
```

Include in `server.ino` with `#include "secrets.h"`. Do not define credentials anywhere else.

---

## Server Firmware — server.ino

### Initialization Order (Critical)

WiFi MUST initialize before LoRa. Reverse order causes silent WiFi corruption on the ESP32-PICO-D4 chip. The setup sequence must be:

```
Serial.begin()
Wire.begin() + OLED init
setupWiFi()       ← WiFi first
setupLoRa()       ← LoRa second
setupWebServer()
watchdog init
send boot status request to controller
```

### Network Configuration

```cpp
IPAddress staticIP(172, 17, 0, 10);
IPAddress gateway(172, 17, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(172, 17, 0, 1);
```

Apply with `WiFi.config()` before `WiFi.begin()`. Also call `WiFi.mode(WIFI_STA)` and `WiFi.disconnect()` with a 100ms delay before connecting — prevents stale state issues.

mDNS hostname: `generator` (accessible as `http://generator.local`). Start mDNS after WiFi connects. Restart mDNS after WiFi reconnects in the watchdog.

### Watchdog Timer

Use the newer ESP32 API (core 3.x):

```cpp
#include <esp_task_wdt.h>

esp_task_wdt_config_t wdt_config = {
  .timeout_ms     = 30000,
  .idle_core_mask = 0,
  .trigger_panic  = true
};
esp_task_wdt_init(&wdt_config);
esp_task_wdt_add(NULL);
```

Call `esp_task_wdt_reset()` as the first line of `loop()`.

### WiFi Health Watchdog

Check every 30 seconds. On disconnect: attempt reconnection, wait up to 10 seconds, restart mDNS on success. After 3 failed attempts: call `ESP.restart()`.

### Web Server Endpoints

`GET /` — serves `INDEX_HTML` from index.h
`GET /command?cmd=start&timer=90` — queues start command with timer
`GET /command?cmd=stop` — queues stop command
`GET /status` — returns JSON status object

The `/status` response JSON shape:

```json
{
  "status": "running|stopped|error|unknown|pending",
  "detail": "human readable detail string",
  "pending": true|false,
  "timerActive": true|false,
  "timerRemaining": 3240,
  "battery": 12.7,
  "controllerOnline": true|false,
  "rssi": -67,
  "uptime": 7230,
  "lastMessageAgo": 45
}
```

`lastMessageAgo` is seconds since last LoRa message received. Returns -1 if no message received this session. Tracked via `lastMessageMs = millis()` updated in `checkForLoRaMessage()`.

### Send State Machine

The ACK wait is non-blocking. States: `SEND_IDLE`, `SEND_WAITING`.

On command queue:
- Set `pendingCommand`, `expectedAck`, reset `sendAttempt` and `ackReceived`
- For `CMD:START`: the timer is mandatory. If no server timer is active the start is refused and nothing is sent. Otherwise append remaining minutes, rounded up and clamped to 1-`MAX_TIMER_MINUTES` - `CMD:START:90`
- For `CMD:STOP`: `expectedAck = "ACK:STOP"`
- Call `transmitNow()`

The `/command` handler rejects (HTTP 400, `pending:false`, nothing sent) a `start` whose `timer` is missing, below 1 or above `MAX_TIMER_MINUTES` (360), and any unknown or missing `cmd`.

**Two-phase timeout.** On each `loop()` pass, `checkSendTimeout()`:
- Phase 1 (no ACK yet): if `SEND_WAITING` and elapsed > `ACK_TIMEOUT_MS` (15000): retry or fail
- `MAX_RETRIES = 1` (one attempt only - the generous timeout makes retries unnecessary)
- On all retries exhausted: set error state "No response from controller", roll back timer if start command failed
- Phase 2 (ACK received): when the expected `ACK:START`/`ACK:STOP` arrives, set `ackReceived = true` and reset `sendTimestamp`. The server then waits up to `STATUS_TIMEOUT_MS` (`GENERATOR_CONFIRM_MS` 15000 + `CONFIRM_MARGIN_MS` 5000) for the STATUS or ERR confirmation. It never retransmits in this phase.
- If phase 2 times out: `sendState = SEND_IDLE`, systemStatus = error, detail "Controller acknowledged but did not confirm generator state". The server auto-stop timer is deliberately left running, because the relay is known to be in the commanded state.

Why two phases: the controller's confirmation (up to 15s) starts after it receives the command, so a single 15s clock started at transmit always expired first in the worst case and produced a false "no response" error.

`sendState` returns to `SEND_IDLE` only on a STATUS or ERR message, or on timeout. An ACK never clears it.

The boot `CTRL:CMD:STATUS` request is a plain transmit outside this state machine (nothing is left pending); its expected reply is a heartbeat.

### Incoming Message Handling

The server handles these incoming messages from the controller. Note that `ACK:START` and `ACK:STOP` update `statusDetail` but do NOT clear `sendState` — that only clears on the full STATUS message or on timeout.

| Message | Action |
|---------|--------|
| `ACK:START` | If waiting for it: `ackReceived = true`, reset `sendTimestamp`, statusDetail "Relay closed - waiting for generator..." |
| `ACK:STOP` | If waiting for it: `ackReceived = true`, reset `sendTimestamp`, statusDetail "Relay opened - confirming shutdown..." |
| `STATUS:ON:x.x` | systemStatus = running, parse battery voltage, sendState = IDLE |
| `STATUS:OFF` | systemStatus = stopped, sendState = IDLE |
| `STATUS:ASSUMED_ON` | systemStatus = running (with unconfirmed note), sendState = IDLE |
| `STATUS:ASSUMED_OFF` | systemStatus = stopped (with unconfirmed note), sendState = IDLE |
| `ERR:START_FAILED` | systemStatus = error, cancel timer, sendState = IDLE |
| `ERR:STOP_FAILED` | systemStatus = error, sendState = IDLE |
| `ERR:SAFETY_TIMEOUT` | systemStatus = stopped, cancel timer, sendState = IDLE |
| `ERR:UNCOMMANDED_SHUTDOWN` | systemStatus = error, cancel timer, sendState = IDLE |
| `ERR:INVALID_TIMER` | systemStatus = error, cancel timer (relay was not closed), sendState = IDLE |
| `HB:...` | Parse relay state, AC state, `TMR` (if present), battery voltage, warning flags; update lastHeartbeatMs; call `restoreStateFromHeartbeat()`; detect uncommanded shutdown if systemStatus is running but AC:OFF reported |
| Any `ERR:*` not listed | systemStatus = error, statusDetail = message |

### Timer Logic

- `timerEndMs` stored as `millis()` + duration — survives page reloads
- `checkTimer()` in loop: when expired, queue `CMD:STOP`
- `timerRemaining` in status response = `(timerEndMs - millis()) / 1000`
- Maximum timer is `MAX_TIMER_MINUTES` (360), enforced in the web UI, the `/command` handler, `queueCommand()` and the controller

### State Restore After Server Reboot

`lastMessageMs` is set whenever a valid message addressed to the server arrives. After a reboot or watchdog restart the server sends `CTRL:CMD:STATUS` once LoRa is up, and the controller replies with a heartbeat. `restoreStateFromHeartbeat(relayOn, acOn, controllerTimerSec)` acts only while `systemStatus == "unknown"` and no command is in flight:

- Relay open: status = stopped
- Relay closed and AC on: status = running. Relay closed and AC off: status = error
- If `TMR > 0` and no server timer is active: restore the server timer as `TMR - SAFETY_TIMER_GRACE*60` seconds, which lines up with the original server timer
- If that restored value is 0 or less (original timer already expired): queue `CMD:STOP`
- If the heartbeat has no `TMR` field (v1.0 controller) the timer cannot be restored

### Heartbeat Timeout

Check every loop pass. If `controllerOnline` is true and `millis() - lastHeartbeatMs > 120000`: set `controllerOnline = false`, update statusDetail.

### OLED Display (Server)

60-minute timeout from boot. After timeout: clear display and stop updating.

Display layout (128x64, 8 lines at text size 1):
```
Line 1: [white title bar]  GEN SERVER
Line 2: WiFi: [SSID truncated to 15 chars]
Line 3: IP: 172.17.0.10
Line 4: RSSI: -67dBm ONLINE|OFFLIN
Line 5: Bat: 12.7V [LOW|CRIT if applicable]
Line 6: State: [SYSTEMSTATUS uppercase]
Line 7: [blank or TX line if space]
Line 8: Disp: 58m 30s
```

### Global Variables Required

```cpp
String        systemStatus       = "unknown";
String        statusDetail       = "Waiting for controller...";
unsigned long timerEndMs         = 0;
bool          timerActive        = false;
float         lastBatteryVoltage = 0.0;
unsigned long lastHeartbeatMs    = 0;
bool          controllerOnline   = false;
int           lastRSSI           = 0;
String        lastTX             = "none";
String        lastRX             = "none";
unsigned long lastMessageMs      = 0;

// Send state machine
enum SendState { SEND_IDLE, SEND_WAITING };
SendState     sendState          = SEND_IDLE;
String        pendingCommand     = "";
String        expectedAck        = "";
int           sendAttempt        = 0;
unsigned long sendTimestamp      = 0;
```

---

## Controller Firmware — controller.ino

### Initialization Order

```
Serial.begin()
pinMode(RELAY_PIN, OUTPUT) + digitalWrite LOW
Wire.begin() + OLED init
SPI.begin() + LoRa init
delay(2000)
sendHeartbeat()     ← boot status report
```

### Loop

```cpp
void loop() {
  checkForLoRaMessage();
  checkControllerTimer();
  checkForUncommandedShutdown();  // Only active when relay closed

  // Sense update every 2 seconds for OLED display
  static unsigned long lastSenseUpdate = 0;
  if (millis() - lastSenseUpdate >= 2000) {
    lastSenseUpdate = millis();
    acPresent      = readACPresent();
    batteryVoltage = readBatteryVoltage();
  }

  // Heartbeat every 60 seconds
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 60000) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  updateOLED();
}
```

### Command Handling

On `CMD:START:nn`:
1. Parse timer minutes from after `CMD:START:`. It must be all digits. A missing, non-numeric, zero or greater-than-`MAX_TIMER_MINUTES` (360) value is rejected: send `ERR:INVALID_TIMER` and return. The relay is not touched and no safety timer is armed.
2. Set `ctrlTimerEndMs = millis() + ((minutes + SAFETY_TIMER_GRACE) * 60000UL)`, `ctrlTimerActive = true`. This happens BEFORE the relay closes so the relay is never closed without a timer armed.
3. `setRelay(true)`
4. `sendMessage("ACK:START")`
5. Call `confirmGeneratorState(true)`. If it ends in `ERR:START_FAILED` the relay is reopened and the safety timer is cleared (otherwise it would later fire a spurious `ERR:SAFETY_TIMEOUT`).

On `CMD:STOP`:
1. Clear `ctrlTimerActive` and `ctrlTimerEndMs`
2. `setRelay(false)`
3. `sendMessage("ACK:STOP")`
4. Call `confirmGeneratorState(false)`

On `CMD:STATUS`:
1. Call `sendHeartbeat()`

### Power Confirmation Logic

```
confirmGeneratorState(bool expectRunning)
  if VOLTAGE_SENSING_ENABLED:
    if expectRunning (startup):
      loop up to GENERATOR_CONFIRM_MS (15000):
        call readACPresent() every SPINUP_CHECK_INTERVAL_MS (500)
        track consecutiveYes count
        reset to 0 on any NO reading
        if consecutiveYes >= 2: confirmed = true, break
      if confirmed: send STATUS:ON:batteryVoltage
      if not confirmed: send ERR:START_FAILED, setRelay(false)

    if not expectRunning (shutdown):
      loop up to GENERATOR_CONFIRM_MS (15000):
        call readACPresent() every 200ms
        if NO: start/continue absence timer
        if YES: reset absence timer
        if absence timer >= SHUTDOWN_SUSTAINED_MS (3000): confirmed = true, break
      if confirmed: send STATUS:OFF
      if not confirmed: send ERR:STOP_FAILED

  if not VOLTAGE_SENSING_ENABLED:
    delay(10000)
    if expectRunning: send STATUS:ASSUMED_ON
    else: send STATUS:ASSUMED_OFF
```

### Voltage Sensing Constants

```cpp
#define VOLTAGE_SENSING_ENABLED   true
#define DIVIDER_RATIO             0.17543   // R1=47kΩ, R2=10kΩ
#define VOLTAGE_CAL_OFFSET        1.0       // Calibrated against multimeter
#define AC_THRESHOLD              300       // Peak-to-peak counts
#define BATTERY_SAMPLE_COUNT      16
#define AC_SAMPLE_COUNT           100
#define AC_SAMPLE_INTERVAL_US     200
#define GENERATOR_CONFIRM_MS      15000
#define SHUTDOWN_SUSTAINED_MS     3000
#define SPINUP_CHECK_INTERVAL_MS  500
#define GENERATOR_SPINUP_MS       10000     // Placeholder path only
#define BATTERY_WARNING_LOW       12.0
#define BATTERY_WARNING_CRITICAL  11.5
```

### readACPresent()

100-sample peak-to-peak. Two-reading confirmation via static variable. Returns bool.

```cpp
bool readACPresent() {
  static bool lastReading = false;
  int maxVal = 0, minVal = 4095;
  for (int i = 0; i < AC_SAMPLE_COUNT; i++) {
    int s = analogRead(AC_SENSE_PIN);
    if (s > maxVal) maxVal = s;
    if (s < minVal) minVal = s;
    delayMicroseconds(AC_SAMPLE_INTERVAL_US);
  }
  bool current  = (maxVal - minVal) > AC_THRESHOLD;
  bool confirmed = (current == lastReading) ? current : lastReading;
  lastReading   = current;
  return confirmed;
}
```

### readBatteryVoltage()

16-sample average. Returns float voltage.

```cpp
float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    sum += analogRead(BATTERY_SENSE_PIN);
    delayMicroseconds(500);
  }
  float pinV = ((sum / BATTERY_SAMPLE_COUNT) / 4095.0) * 3.3;
  return (pinV / DIVIDER_RATIO) + VOLTAGE_CAL_OFFSET;
}
```

### Uncommanded Shutdown Monitor

`checkForUncommandedShutdown()`, called every `loop()` pass. Only active when `relayState == true` and `VOLTAGE_SENSING_ENABLED`. Uses direct inline ADC read (NOT `readACPresent()` — avoids shared static variable conflict), polled every `UNCOMMANDED_CHECK_INTERVAL_MS` (500). After `UNCOMMANDED_SHUTDOWN_MS` (4000) of sustained absence: send `ERR:UNCOMMANDED_SHUTDOWN` once, update `acPresent = false`. State resets when the relay opens or AC returns. It is not called while `confirmGeneratorState()` is blocking, so start-up spin-up time cannot trigger a false alarm.

### Controller Safety Timer

`checkControllerTimer()` in loop. When `ctrlTimerActive` and `millis() >= ctrlTimerEndMs`: `setRelay(false)`, send `ERR:SAFETY_TIMEOUT`, call `confirmGeneratorState(false)`.

### Heartbeat Format

```
SERV:HB:RELAY:ON:AC:ON:TMR:3240:BAT:12.7
SERV:HB:RELAY:OFF:AC:OFF:TMR:0:BAT:12.7
SERV:HB:RELAY:ON:AC:ON:TMR:3240:BAT:11.4:WARN:CRITICAL
SERV:HB:RELAY:ON:AC:ON:TMR:3240:BAT:11.9:WARN:LOW
```

`TMR` is the safety timer seconds remaining (0 if none armed). Battery voltage formatted to 1 decimal place. Warning appended only when threshold crossed.

### Sense Update and Boot Heartbeat

`loop()` refreshes `acPresent` and `batteryVoltage` every `SENSE_UPDATE_INTERVAL` (2000 ms). This keeps the OLED current and keeps `readACPresent()`'s two-reading confirmation fresh, so heartbeats never report a stale AC state. `updateOLED()` redraws at the same interval on both boards.

At boot, after LoRa init the controller waits `BOOT_HEARTBEAT_DELAY_MS` (2000 ms), primes `readACPresent()` with one reading, then calls `sendHeartbeat()`.

### Local Test Mode (controller, generator site)

The generator shed is outside WiFi range, so the controller can be exercised from a laptop over USB with no server. Set `#define LOCAL_TEST_MODE 1` in `controller.ino`, flash, and open the Serial Monitor at 115200 baud with line ending "Newline". When the flag is `0` (the default) none of this code is compiled in.

Commands (case-insensitive) go through `handleMessage()`, the same path a LoRa packet takes:

| Command | Effect |
|---------|--------|
| `START:<min>` | Same as `CMD:START:<min>`, for example `START:5` |
| `START` | Same as `CMD:START` with no timer. Must be rejected with `ERR:INVALID_TIMER` |
| `STOP` | Same as `CMD:STOP` |
| `STATUS` | Same as `CMD:STATUS` - sends a heartbeat |
| `SENSE` | Print AC peak-to-peak vs threshold, battery voltage, relay state, safety timer |
| `RAW <text>` | Feed any message, for example `RAW CMD:START:361` |
| `HELP` | List commands |

**The relay really operates and replies are transmitted over LoRa as normal. Disconnect the generator start wire before testing.** The OLED splash and boot log show "LOCAL TEST MODE" when enabled. Set the flag back to `0` and reflash before installing for normal use.

### OLED Display (Controller)

60-minute timeout from boot. Layout:

```
Line 1: [white title bar]  GEN CTRL
Line 2: RSSI: -67 dBm
Line 3: Relay:ON  AC:ON   (or OFF)
Line 4: Bat: 12.7V [LOW|CRIT]
Line 5: Stop in: 58m 30s  (if safety timer active)
        OR  RX: [last received message truncated]
Line 6: TX: [last sent message truncated]
Line 7: [blank]
Line 8: Disp: 58m 30s
```

### Global Variables Required

```cpp
bool          relayState        = false;
bool          acPresent         = false;
float         batteryVoltage    = 0.0;
String        lastTX            = "none";
String        lastRX            = "none";
int           lastRSSI          = 0;
unsigned long ctrlTimerEndMs    = 0;
bool          ctrlTimerActive   = false;
```

---

## index.h — Web UI

The HTML/CSS/JS is stored as a raw string literal:

```cpp
const char INDEX_HTML[] PROGMEM = R"rawliteral(
...html content...
)rawliteral";
```

The closing `)rawliteral";` must be the absolute last line of the file with no trailing whitespace or newline after it. This is a common source of "unterminated raw string" compilation errors.

### UI Structure

```
Card
  ├── Title: "Generator Control"
  ├── Status row (dot + status text + detail text)
  ├── Battery display line
  ├── Pending banner (spinner, hidden unless command in flight)
  ├── Controls div (disabled during pending)
  │     ├── Section: "Start with timer"
  │     ├── Preset grid: 20 min | 60 min | 90 min (green buttons)
  │     ├── Custom row: number input (1-360) + "Custom start" (blue button)
  │     ├── Section: "Stop"
  │     └── Stop button (red, full width)
  ├── Section: "Activity log"
  ├── Log div (monospace, scrollable, 160px height)
  ├── Section: "Diagnostics"
  └── Diagnostics table
        ├── Server uptime
        ├── Last message received
        └── Signal strength (RSSI)
```

### JavaScript Functions Required

| Function | Purpose |
|----------|---------|
| `setPending(on, message)` | Show/hide pending banner, disable/enable all buttons |
| `sendTimedCommand(minutes)` | Call doStart() |
| `sendCustomCommand()` | Validate input, enforce 360 min max, call doStart() |
| `doStart(minutes)` | POST to /command, start fast poll, start countdown |
| `sendStop()` | POST to /command?cmd=stop, start fast poll |
| `startFastPoll()` | Switch to 2s poll interval |
| `stopFastPoll()` | Return to 10s poll interval |
| `pollStatus()` | Fetch /status, update all UI elements |
| `updateStatus(status, detail)` | Update dot color and status text |
| `startCountdown(seconds)` | Start 1s interval updating timerRemaining |
| `updateCountdownDisplay()` | Update status detail with formatted time |
| `formatDuration(seconds)` | Format as Xh Xm Xs or Xm Xs |
| `formatUptime(seconds)` | Format as Xd Xh Xm or Xh Xm Xs etc |
| `addLog(msg, type)` | Append timestamped entry to log div |

### pollStatus() — Full Update Responsibilities

On each poll response, update:
1. Status dot and text via `updateStatus()`
2. Timer countdown — restore from `data.timerRemaining` if server has active timer and browser doesn't, or drift > 5s
3. Battery display — color coded: red < 11.5V, amber < 12.0V, grey otherwise
4. Pending banner — show if `data.pending`, clear if was pending and now not
5. Diagnostics — uptime, lastMessageAgo (amber if > 120s), RSSI with quality label

### RSSI Quality Labels

| RSSI | Color | Label |
|------|-------|-------|
| Better than -75 | Green | good |
| -75 to -90 | Amber | fair |
| Worse than -90 | Red | weak |

### Init (bottom of script)

```javascript
normalPollInterval = setInterval(pollStatus, 10000);
addLog('Interface ready', 'info');
pollStatus();   // Immediate poll on page load
```

---

## Known Pitfalls

1. **LORA_RST vs LORA_RESET** — the board definition defines `LORA_RST` as GPIO 14. Always use `LORA_RESET` for GPIO 23 in firmware to avoid redefinition conflicts and wrong pin behavior.

2. **WiFi before LoRa** — always initialize WiFi first in `setup()`. LoRa SPI activity during WiFi init causes silent WiFi corruption on the PICO-D4 that persists until full chip erase.

3. **Raw string terminator** — `index.h` must end with `)rawliteral";` as the absolute last characters. Any trailing content causes "unterminated raw string" error.

4. **JsonDocument not StaticJsonDocument** — newer ArduinoJson deprecates `StaticJsonDocument<N>`. Use `JsonDocument` throughout.

5. **send() not send_P()** — newer ESPAsyncWebServer deprecates `send_P()`. Use `send()`.

6. **Watchdog API** — ESP32 Arduino core 3.x uses `esp_task_wdt_config_t` struct. The old two-argument `esp_task_wdt_init(timeout, panic)` call does not compile.

7. **readACPresent() static variable** — the two-reading confirmation uses a static variable. The uncommanded shutdown monitor must NOT call `readACPresent()` — it must do its own inline ADC read to avoid shared state corruption.

8. **Board selection** — must be "TTGO LoRa32 V2.1 (1.6.1)". Other selections cause upload failures (wrong flash config for PICO-D4) or runtime WiFi issues.

9. **Forward declarations** — Arduino IDE requires functions to be declared before they are called, or forward declarations must be added at the top of the sketch. Always include a forward declaration block.

10. **Em dashes in strings** — the `—` character causes string truncation errors in Arduino C++ strings. Use plain hyphens `-` in all `Serial.println()` and other string literals.

---

## Current Outstanding Issues

### 1. index.h Truncation (Blocking)
(Resolved)

### 2. lastMessageMs and lastMessageAgo
(Resolved in v1.1 - `lastMessageMs` is set in `checkForLoRaMessage()` after the address check and feeds `lastMessageAgo`. Needs on-hardware confirmation.)

### 3. Boot Status Request
(Resolved in v1.1 - server sends `CTRL:CMD:STATUS` after LoRa init; controller handles `CMD:STATUS` and also sends a boot heartbeat. Needs on-hardware confirmation.)

### 4. Known limitation: controller is deaf while confirming
`confirmGeneratorState()` blocks for up to 15 seconds after a start or stop, and the controller does not poll LoRa during that time. A STOP sent in that window is not received. Making the controller loop non-blocking is a possible future refactor.

---

## Testing Checklist

Before considering any build complete, verify:

- [ ] Both sketches compile with zero errors (warnings acceptable)
- [ ] Server board boots, connects to WiFi at 172.17.0.10, starts mDNS
- [ ] `http://generator.local` and `http://172.17.0.10` both load the UI
- [ ] Controller board boots, initializes LoRa, sends boot heartbeat
- [ ] Server receives boot heartbeat and shows controller online
- [ ] Battery voltage and AC state display correctly in UI
- [ ] Preset start buttons show pending state, disable controls
- [ ] Controller receives CMD:START, closes relay, sends ACK then STATUS
- [ ] UI clears pending and shows Running after STATUS:ON received
- [ ] Stop button sends CMD:STOP, controller opens relay, confirms shutdown
- [ ] Timer countdown visible in status detail; restored on page reload
- [ ] Controller safety timer fires 2 minutes after server timer would have
- [ ] Diagnostics section shows uptime, last message time, RSSI
- [ ] OLED on both boards shows correct information
- [ ] OLED goes dark after 60 minutes
- [ ] Heartbeat received every 60 seconds; controller offline warning after 2 missed

### Staged-flash test plan (v1.1)

The boards are flashed one at a time. Flash the server first and test it with the old controller in LoRa range, then flash the controller and test it at the generator site.

**Phase 1 - server v1.1 (WiFi and LoRa range, v1.0 controller)**
- [ ] OLED and Serial show firmware v1.1; WiFi at 172.17.0.10; both URLs load the UI
- [ ] Boot `CMD:STATUS` appears in the Serial log (a v1.0 controller replies `ERR:UNKNOWN_CMD`; expected until the controller is flashed)
- [ ] `/command?cmd=start` (no timer), `timer=0`, `timer=361` and `cmd=bogus` are rejected (HTTP 400) and nothing is sent over LoRa
- [ ] `timer=360` is accepted
- [ ] `lastMessageAgo` is -1 until the first LoRa message, then counts up
- [ ] A valid start gets ACK then STATUS and clears pending
- [ ] Failed start (no AC): the true error shows, with no false "no response" flicker
- [ ] Server reboot while running: no `TMR` field from a v1.0 controller, so state is not restored (expected); from a v1.1 controller see Phase 3

**Phase 2 - controller v1.1 at the generator site (USB Serial only, `LOCAL_TEST_MODE 1`, start wire disconnected)**
- [ ] Boot log and OLED show v1.1 and LOCAL TEST MODE; a heartbeat is sent about 2s after boot
- [ ] `SENSE` shows AC and battery readings that track reality; OLED values refresh every 2s
- [ ] `START` and `RAW CMD:START:361` are rejected with `ERR:INVALID_TIMER` and the relay stays open
- [ ] `START:2` closes the relay, arms the safety timer (4 minutes), sends ACK then STATUS or `ERR:START_FAILED`
- [ ] After `ERR:START_FAILED` the safety timer is cleared (`SENSE` shows "not armed")
- [ ] With relay closed and AC removed for 4s: `ERR:UNCOMMANDED_SHUTDOWN` is sent once
- [ ] `STOP` opens the relay and confirms shutdown; `STATUS` sends a heartbeat with a `TMR` field
- [ ] Set `LOCAL_TEST_MODE` back to 0 and reflash before installing

**Phase 3 - both boards v1.1**
- [ ] Server boot gets a heartbeat reply within seconds and shows controller online
- [ ] Start with a timer, reboot the server: it restores running state and the countdown from the heartbeat `TMR` field
- [ ] Restored timer already expired: server queues STOP
