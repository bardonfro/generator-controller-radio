// ================================================================
// GENERATOR SERVER FIRMWARE
// Stage 5 — Full monitoring, heartbeat, battery display
// Board: LilyGo T3 V1.6.1 — Select "TTGO LoRa32 V2.1 (1.6.1)"
// ================================================================

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "secrets.h"
#include "index.h"


// ----------------------------------------------------------------
// PIN CONSTANTS
// ----------------------------------------------------------------
#define LORA_SCK      5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_SS       18
#define LORA_RESET    23
#define LORA_DIO0     26


// ----------------------------------------------------------------
// NETWORK CONSTANTS
// ----------------------------------------------------------------
IPAddress staticIP(172, 17, 0, 10);
IPAddress gateway(172, 17, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(172, 17, 0, 1);

// ----------------------------------------------------------------
// LORA CONSTANTS
// ----------------------------------------------------------------
#define LORA_FREQUENCY    915E6
#define LORA_TX_POWER     17
#define LORA_BANDWIDTH    125E3
#define LORA_SPREAD       8
#define LORA_CODERATE     5

// ----------------------------------------------------------------
// IDENTITY CONSTANTS
// ----------------------------------------------------------------
#define MY_ADDRESS        "SERV"
#define CTRL_ADDRESS      "CTRL"

// ----------------------------------------------------------------
// RELIABILITY CONSTANTS
// ----------------------------------------------------------------
#define ACK_TIMEOUT_MS    15000   // Wait for ACK:START / ACK:STOP from controller
#define MAX_RETRIES       1       // One attempt only - generous timeout makes retries unnecessary

// Two-phase timeout: once the ACK arrives the controller is alive but busy
// confirming generator power (up to GENERATOR_CONFIRM_MS on its side). The
// server then waits for that confirmation plus a margin before declaring
// failure. GENERATOR_CONFIRM_MS must stay in step with controller.ino.
#define GENERATOR_CONFIRM_MS   15000
#define CONFIRM_MARGIN_MS      5000
#define STATUS_TIMEOUT_MS      (GENERATOR_CONFIRM_MS + CONFIRM_MARGIN_MS)

// ----------------------------------------------------------------
// TIMER CONSTANTS
// ----------------------------------------------------------------
#define MAX_TIMER_MINUTES   360   // 6 hours - generator must never run indefinitely
#define SAFETY_TIMER_GRACE  2     // Extra minutes on controller safety timer (must match controller.ino)

// ----------------------------------------------------------------
// WATCHDOG AND WIFI HEALTH CONSTANTS
// ----------------------------------------------------------------
#define WDT_TIMEOUT_MS          30000   // Hardware watchdog
#define WIFI_CHECK_INTERVAL     30000   // WiFi health check period
#define WIFI_MAX_RECONNECTS     3       // Failed reconnects before reboot
#define WIFI_RECONNECT_WAIT_MS  10000   // Max wait per reconnect attempt

// ----------------------------------------------------------------
// DISPLAY AND BATTERY CONSTANTS
// ----------------------------------------------------------------
#define SENSE_UPDATE_INTERVAL     2000   // OLED refresh period
#define BATTERY_WARNING_LOW       12.0
#define BATTERY_WARNING_CRITICAL  11.5

// ----------------------------------------------------------------
// FIRMWARE VERSION
// ----------------------------------------------------------------
#define FW_VERSION        "1.1.1"

// ----------------------------------------------------------------
// HEARTBEAT CONSTANTS
// ----------------------------------------------------------------
#define HEARTBEAT_TIMEOUT_MS  120000   // 2 minutes - missed 2 heartbeats

// ----------------------------------------------------------------
// OLED CONSTANTS
// ----------------------------------------------------------------
#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_RESET        -1
#define OLED_ADDRESS      0x3C
#undef OLED_SDA
#undef OLED_SCL
#define OLED_SDA          21
#define OLED_SCL          22
#define OLED_TIMEOUT_MS   3600000UL

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
unsigned long oledStartTime = 0;
bool oledActive = true;

// ----------------------------------------------------------------
// SEND STATE MACHINE
// ----------------------------------------------------------------
enum SendState {
  SEND_IDLE,
  SEND_WAITING,
};

SendState     sendState      = SEND_IDLE;
String        pendingCommand = "";
String        expectedAck    = "";
int           sendAttempt    = 0;
unsigned long sendTimestamp  = 0;
bool          lastSendSuccess = false;
bool          lastSendDone    = false;
bool          ackReceived     = false;   // True once controller ACKed the pending command

// ----------------------------------------------------------------
// SYSTEM STATE
// ----------------------------------------------------------------
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
unsigned long lastMessageMs = 0;

// ----------------------------------------------------------------
// WEB SERVER
// ----------------------------------------------------------------
AsyncWebServer server(80);

// ----------------------------------------------------------------
// FORWARD DECLARATIONS
// ----------------------------------------------------------------
void setupWiFi();
void setupLoRa();
void setupWebServer();
void checkForLoRaMessage();
void checkSendTimeout();
void checkTimer();
void checkHeartbeatTimeout();
void checkWiFiHealth();
void queueCommand(String command);
void transmitNow();
void sendBootStatusRequest();
void printResetReason();
void restoreStateFromHeartbeat(bool relayOn, bool acOn, long controllerTimerSec);
void updateOLED();

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.print("Server booting... firmware v");
  Serial.println(FW_VERSION);
  printResetReason();

  // OLED first so we can show boot status
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("WARNING: OLED init failed");
  } else {
    oledStartTime = millis();
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 28);
    display.print("GEN SERVER v");
    display.print(FW_VERSION);
    display.display();
    delay(2000);
    Serial.println("OLED initialized.");
  }

  setupWiFi();
  setupLoRa();
  setupWebServer();

  // Enable hardware watchdog
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = WDT_TIMEOUT_MS,  // 30 second timeout
    .idle_core_mask = 0,      // Don't watch idle cores
    .trigger_panic  = true    // Restart on timeout
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog timer enabled.");

  // Ask the controller for its current state so the UI does not sit at
  // "unknown" until the first 60 second heartbeat. Sent after the watchdog
  // is armed; transmit is quick and non-blocking for our purposes.
  sendBootStatusRequest();

  Serial.println("All systems ready.");
  Serial.print("Browse to http://generator.local or http://");
  Serial.println(WiFi.localIP());
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  esp_task_wdt_reset();    // Feed watchdog — proves loop is running
  checkForLoRaMessage();
  checkSendTimeout();
  checkTimer();
  checkHeartbeatTimeout();
  checkWiFiHealth();       // Check WiFi every 30 seconds
  updateOLED();
}

// ----------------------------------------------------------------
// RESET REASON (boot diagnostic)
// Printed at every boot so an unexpected restart can be explained from
// the Serial log. Note: ESP32 reports power-up, the EN/reset button and
// the USB serial DTR/RTS toggle (opening the Serial Monitor) all as
// POWERON. BROWNOUT, PANIC and the WDT values indicate a real fault.
// ----------------------------------------------------------------
void printResetReason() {
  Serial.print("Reset reason: ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   Serial.println("POWERON (power-up, reset button or serial monitor connect)"); break;
    case ESP_RST_EXT:       Serial.println("EXT (external reset pin)"); break;
    case ESP_RST_SW:        Serial.println("SW (software restart)"); break;
    case ESP_RST_PANIC:     Serial.println("PANIC (crash)"); break;
    case ESP_RST_INT_WDT:   Serial.println("INT_WDT (interrupt watchdog)"); break;
    case ESP_RST_TASK_WDT:  Serial.println("TASK_WDT (task watchdog)"); break;
    case ESP_RST_WDT:       Serial.println("WDT (other watchdog)"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("DEEPSLEEP (wake)"); break;
    case ESP_RST_BROWNOUT:  Serial.println("BROWNOUT (supply voltage dipped)"); break;
    default:
      Serial.print("other, code ");
      Serial.println((int)esp_reset_reason());
      break;
  }
}

// ----------------------------------------------------------------
// WIFI SETUP
// ----------------------------------------------------------------
void setupWiFi() {
  Serial.print("Connecting to WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  WiFi.config(staticIP, gateway, subnet, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected. IP address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("generator")) {
      Serial.println("mDNS started - http://generator.local");
    } else {
      Serial.println("WARNING: mDNS failed to start");
    }
  } else {
    Serial.println();
    Serial.println("WARNING: WiFi connection failed.");
  }
}

// ----------------------------------------------------------------
// LORA SETUP
// ----------------------------------------------------------------
void setupLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RESET, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("ERROR: LoRa init failed.");
    while (true);
  }

  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSpreadingFactor(LORA_SPREAD);
  LoRa.setCodingRate4(LORA_CODERATE);

  Serial.println("LoRa initialized.");
}

// ----------------------------------------------------------------
// WEB SERVER SETUP
// ----------------------------------------------------------------
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", INDEX_HTML);
  });

  server.on("/command", HTTP_GET, [](AsyncWebServerRequest *request) {
    String cmd        = "";
    int timerMinutes  = 0;

    if (request->hasParam("cmd")) {
      cmd = request->getParam("cmd")->value();
    }
    if (request->hasParam("timer")) {
      timerMinutes = request->getParam("timer")->value().toInt();
    }

    if (sendState != SEND_IDLE) {
      JsonDocument doc;
      doc["status"]  = systemStatus;
      doc["message"] = "Busy - previous command still in progress";
      doc["pending"] = true;
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
      return;
    }

    if (cmd == "start") {
      // Safety rule: every start must carry a timer of 1..MAX_TIMER_MINUTES.
      // Enforced here (not just in the web UI) so a direct URL cannot bypass it.
      if (timerMinutes < 1 || timerMinutes > MAX_TIMER_MINUTES) {
        Serial.print("Rejected start - invalid timer: ");
        Serial.println(timerMinutes);
        JsonDocument doc;
        doc["status"]  = systemStatus;
        doc["message"] = "Rejected - timer must be 1 to " + String(MAX_TIMER_MINUTES) + " minutes";
        doc["pending"] = false;
        String json;
        serializeJson(doc, json);
        request->send(400, "application/json", json);
        return;
      }
      timerEndMs   = millis() + (timerMinutes * 60000UL);
      timerActive  = true;
      statusDetail = "Running - auto-stop in " + String(timerMinutes) + " min";
      queueCommand("CMD:START");
    } else if (cmd == "stop") {
      timerActive = false;
      timerEndMs  = 0;
      queueCommand("CMD:STOP");
    } else {
      // Unknown or missing cmd - nothing is queued, so do not report pending
      Serial.println("Rejected command - unknown or missing cmd parameter");
      JsonDocument doc;
      doc["status"]  = systemStatus;
      doc["message"] = "Rejected - unknown command";
      doc["pending"] = false;
      String json;
      serializeJson(doc, json);
      request->send(400, "application/json", json);
      return;
    }

    JsonDocument doc;
    doc["status"]  = "pending";
    doc["message"] = "Command sent - waiting for controller...";
    doc["pending"] = true;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["status"]          = systemStatus;
    doc["detail"]          = statusDetail;
    doc["pending"]         = (sendState != SEND_IDLE);
    doc["timerActive"]     = timerActive;
    doc["timerRemaining"]  = timerActive
      ? (int)((timerEndMs - millis()) / 1000)
      : 0;
    doc["battery"]         = lastBatteryVoltage;
    doc["controllerOnline"] = controllerOnline;
    doc["rssi"]            = lastRSSI;
    doc["uptime"]          = millis() / 1000;
    doc["lastMessageAgo"] = lastMessageMs > 0
      ? (int)((millis() - lastMessageMs) / 1000)
      : -1;   // -1 means no message received yet this session
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Web server started.");
}

// ----------------------------------------------------------------
// QUEUE A LORA COMMAND
// ----------------------------------------------------------------
void queueCommand(String command) {
  if (command == "CMD:START") {
    // Safety rule: never transmit a start without a timer. The /command
    // handler already validates this; this is a backstop.
    if (!timerActive) {
      Serial.println("ERROR: Refusing CMD:START with no active timer");
      systemStatus = "error";
      statusDetail = "Start refused - no timer";
      return;
    }
    // Round UP to whole minutes, then clamp to the allowed range so the
    // controller never sees an out-of-range value.
    unsigned long remainingMs = timerEndMs - millis();
    int remainingMinutes      = (remainingMs + 59999UL) / 60000UL;
    if (remainingMinutes < 1) remainingMinutes = 1;
    if (remainingMinutes > MAX_TIMER_MINUTES) remainingMinutes = MAX_TIMER_MINUTES;
    pendingCommand = "CMD:START:" + String(remainingMinutes);
  } else {
    pendingCommand = command;
  }

  expectedAck  = "ACK:START";
  if (command == "CMD:STOP") expectedAck = "ACK:STOP";
  sendAttempt  = 0;
  lastSendDone = false;
  ackReceived  = false;
  transmitNow();
}

// ----------------------------------------------------------------
// BOOT STATUS REQUEST
// Plain transmit outside the send state machine - the expected reply
// is a heartbeat, not an ACK, so nothing is left pending.
// ----------------------------------------------------------------
void sendBootStatusRequest() {
  String outgoing = String(CTRL_ADDRESS) + ":CMD:STATUS";
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();
  lastTX = "CMD:STATUS";
  Serial.println("Boot status request sent: " + outgoing);
}

void transmitNow() {
  sendAttempt++;
  sendTimestamp = millis();
  sendState     = SEND_WAITING;
  lastTX        = pendingCommand;

  String outgoing = String(CTRL_ADDRESS) + ":" + pendingCommand;
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();

  Serial.print("Sending ");
  Serial.print(pendingCommand);
  Serial.print(" (attempt ");
  Serial.print(sendAttempt);
  Serial.println(")");
}

// ----------------------------------------------------------------
// CHECK SEND TIMEOUT
// ----------------------------------------------------------------
void checkSendTimeout() {
  if (sendState != SEND_WAITING) return;

  // Phase 1 (no ACK yet): ACK_TIMEOUT_MS. Phase 2 (ACK received, waiting for
  // the STATUS/ERR confirmation): STATUS_TIMEOUT_MS. sendTimestamp is reset
  // when the ACK arrives so each phase gets its own full window.
  unsigned long timeoutMs = ackReceived ? STATUS_TIMEOUT_MS : ACK_TIMEOUT_MS;
  if (millis() - sendTimestamp < timeoutMs) return;

  if (ackReceived) {
    // Controller is alive and closed/opened the relay, but never reported
    // generator state. Never retransmit here - a second CMD:START to a
    // controller with the relay already closed would be wrong.
    Serial.println("ERROR: ACK received but no STATUS confirmation in time");
    sendState       = SEND_IDLE;
    lastSendDone    = true;
    lastSendSuccess = false;
    systemStatus    = "error";
    statusDetail    = "Controller acknowledged but did not confirm generator state";
    // Server auto-stop timer is deliberately left running: the relay is
    // known to be in the commanded state, so the timed STOP must still fire.
    return;
  }

  Serial.print("No ACK on attempt ");
  Serial.println(sendAttempt);

  if (sendAttempt < MAX_RETRIES) {
    transmitNow();
  } else {
    Serial.println("ERROR: All retries failed.");
    sendState       = SEND_IDLE;
    lastSendDone    = true;
    lastSendSuccess = false;
    systemStatus    = "error";
    statusDetail    = "No response from controller";

    if (pendingCommand.startsWith("CMD:START")) {
      timerActive = false;
      timerEndMs  = 0;
    }
  }
}

// ----------------------------------------------------------------
// CHECK FOR INCOMING LORA MESSAGES
// ----------------------------------------------------------------
void checkForLoRaMessage() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  lastRSSI = LoRa.packetRssi();
  Serial.print("LoRa received: ");
  Serial.println(incoming);

  if (!incoming.startsWith(MY_ADDRESS)) return;

  // Valid message addressed to us - feeds lastMessageAgo in /status
  lastMessageMs = millis();

  String message = incoming.substring(5);
  lastRX = message;

  // ── ACK messages — relay confirmed ───────────────────────────
  // An ACK updates statusDetail but does NOT clear sendState - that only
  // happens on the full STATUS/ERR message or on timeout. When the ACK is
  // the one we are waiting for, restart the clock (phase 2 of the two-phase
  // timeout) so the controller gets a full STATUS_TIMEOUT_MS to confirm
  // generator state.
  if (message == "ACK:START" || message == "ACK:STOP") {
    bool isStart = (message == "ACK:START");
    if (sendState == SEND_WAITING && message == expectedAck) {
      ackReceived   = true;
      sendTimestamp = millis();
      Serial.println(isStart ? "Relay closed - awaiting power confirmation"
                             : "Relay opened - awaiting shutdown confirmation");
      statusDetail = isStart ? "Relay closed - waiting for generator..."
                             : "Relay opened - confirming shutdown...";
    } else {
      Serial.println("Ignoring unexpected ACK: " + message);
    }
    return;
  }

  // ── STATUS messages ───────────────────────────────────────────
  if (message.startsWith("STATUS:ON")) {
    systemStatus = "running";
    sendState     = SEND_IDLE;
    // Extract battery voltage if present — format STATUS:ON:12.7
    if (message.length() > 10) {
      String batStr = message.substring(10);
      lastBatteryVoltage = batStr.toFloat();
      statusDetail = "Running - battery " + batStr + "V";
    } else {
      statusDetail = "Running - power confirmed";
    }
    return;
  }

  if (message == "STATUS:OFF") {
    systemStatus = "stopped";
    statusDetail  = "Stopped - no power output";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "STATUS:ASSUMED_ON") {
    systemStatus = "running";
    statusDetail  = "Running (unconfirmed - no voltage sensing)";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "STATUS:ASSUMED_OFF") {
    systemStatus = "stopped";
    statusDetail  = "Stopped (unconfirmed - no voltage sensing)";
    sendState     = SEND_IDLE;
    return;
  }

  // ── Error messages ────────────────────────────────────────────
  if (message == "ERR:START_FAILED") {
    systemStatus = "error";
    statusDetail  = "Generator failed to start - no power detected";
    sendState     = SEND_IDLE;
    timerActive   = false;
    timerEndMs    = 0;
    return;
  }

  if (message == "ERR:STOP_FAILED") {
    systemStatus = "error";
    statusDetail  = "Stop command sent but power still present after 15s";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "ERR:SAFETY_TIMEOUT") {
    systemStatus = "stopped";
    statusDetail  = "Generator stopped - controller safety timer expired";
    sendState     = SEND_IDLE;
    timerActive   = false;
    timerEndMs    = 0;
    return;
  }

  if (message == "ERR:UNCOMMANDED_SHUTDOWN") {
    systemStatus = "error";
    statusDetail  = "Generator stopped unexpectedly - check fuel and status";
    sendState     = SEND_IDLE;
    timerActive   = false;
    timerEndMs    = 0;
    Serial.println("ERROR: Uncommanded shutdown reported by controller");
    return;
  }

  if (message == "ERR:INVALID_TIMER") {
    // Controller refused a start with a missing or out-of-range timer.
    // Relay was not closed, so the server timer must not keep running.
    systemStatus = "error";
    statusDetail  = "Controller rejected start - invalid timer";
    sendState     = SEND_IDLE;
    timerActive   = false;
    timerEndMs    = 0;
    Serial.println("ERROR: Controller rejected timer value");
    return;
  }

  if (message.startsWith("ERR:")) {
    systemStatus = "error";
    statusDetail  = message;
    sendState     = SEND_IDLE;
    Serial.print("Controller error: ");
    Serial.println(message);
    return;
  }

  // ── Heartbeat ─────────────────────────────────────────────────
  if (message.startsWith("HB:")) {
    Serial.print("Heartbeat: ");
    Serial.println(message);
    lastHeartbeatMs  = millis();
    controllerOnline = true;

    // Parse relay and AC state
    bool reportedRelay = message.indexOf("RELAY:ON") > 0;
    bool reportedAC    = message.indexOf("AC:ON") > 0;

    // Parse controller safety timer remaining (seconds). -1 means the field
    // is absent (older controller firmware) - state cannot be restored.
    long controllerTimerSec = -1;
    int tmrIdx = message.indexOf(":TMR:");
    if (tmrIdx > 0) {
      String tmrStr = message.substring(tmrIdx + 5);
      int tmrEnd = tmrStr.indexOf(':');
      if (tmrEnd > 0) tmrStr = tmrStr.substring(0, tmrEnd);
      controllerTimerSec = tmrStr.toInt();
    }

    // If we have no idea what state the generator is in (fresh boot or
    // watchdog restart), adopt the controller's reported state
    restoreStateFromHeartbeat(reportedRelay, reportedAC, controllerTimerSec);

    // Uncommanded shutdown detection
    if (systemStatus == "running" && !reportedAC) {
      systemStatus = "error";
      statusDetail  = "Generator stopped unexpectedly - check fuel and status";
      timerActive   = false;
      timerEndMs    = 0;
      Serial.println("ERROR: Uncommanded shutdown detected via heartbeat");
    }

    // Parse battery voltage
    int batIdx = message.indexOf(":BAT:");
    if (batIdx > 0) {
      String batStr = message.substring(batIdx + 5);
      int warnIdx = batStr.indexOf(":WARN:");
      if (warnIdx > 0) {
        String warnType = batStr.substring(warnIdx + 6);
        batStr = batStr.substring(0, warnIdx);
        if (warnType == "CRITICAL") {
          statusDetail = "WARNING - battery critical: " + batStr + "V";
        } else if (warnType == "LOW") {
          statusDetail = "Warning - battery low: " + batStr + "V";
        }
      }
      lastBatteryVoltage = batStr.toFloat();
    }
    return;
  }
}

// ----------------------------------------------------------------
// TIMER CHECK
// ----------------------------------------------------------------
void checkTimer() {
  if (!timerActive) return;
  if (millis() < timerEndMs) return;

  timerActive = false;
  timerEndMs  = 0;
  Serial.println("Timer expired - queuing STOP");
  queueCommand("CMD:STOP");
}

// ----------------------------------------------------------------
// RESTORE STATE FROM HEARTBEAT
// After a server reboot or watchdog restart the server knows nothing
// about the generator. The first heartbeat (requested at boot via
// CMD:STATUS) carries relay state, AC state and the controller safety
// timer remaining, which is enough to rebuild the display and the
// server-side auto-stop timer. Only acts while status is "unknown" and
// no command is in flight, so it never overrides a commanded state.
// controllerTimerSec is -1 if the heartbeat had no TMR field.
// ----------------------------------------------------------------
void restoreStateFromHeartbeat(bool relayOn, bool acOn, long controllerTimerSec) {
  if (systemStatus != "unknown") return;
  if (sendState != SEND_IDLE) return;

  if (!relayOn) {
    systemStatus = "stopped";
    statusDetail = "Stopped - state restored from controller";
    Serial.println("State restored from heartbeat: stopped");
    return;
  }

  // Relay is closed
  if (acOn) {
    systemStatus = "running";
    statusDetail = "Running - state restored from controller";
  } else {
    systemStatus = "error";
    statusDetail = "Relay closed but no AC power detected";
  }
  Serial.print("State restored from heartbeat: ");
  Serial.println(systemStatus);

  if (controllerTimerSec <= 0 || timerActive) return;

  // The controller safety timer runs SAFETY_TIMER_GRACE minutes longer than
  // the server timer it was derived from, so subtract the grace to line the
  // server timer back up with the original.
  long serverTimerSec = controllerTimerSec - ((long)SAFETY_TIMER_GRACE * 60L);
  if (serverTimerSec > 0) {
    timerEndMs  = millis() + ((unsigned long)serverTimerSec * 1000UL);
    timerActive = true;
    Serial.print("Server timer restored: ");
    Serial.print(serverTimerSec);
    Serial.println(" seconds remaining");
  } else {
    // Original server timer has already run out - stop the generator now,
    // as it would have been had the server not restarted.
    Serial.println("Restored timer already expired - queuing STOP");
    queueCommand("CMD:STOP");
  }
}

// ----------------------------------------------------------------
// HEARTBEAT TIMEOUT CHECK
// ----------------------------------------------------------------
void checkHeartbeatTimeout() {
  if (!controllerOnline) return;
  if (millis() - lastHeartbeatMs < HEARTBEAT_TIMEOUT_MS) return;

  controllerOnline = false;
  statusDetail     = "WARNING - controller offline";
  Serial.println("WARNING: Controller heartbeat lost");
}

// ----------------------------------------------------------------
// OLED UPDATE
// ----------------------------------------------------------------
void updateOLED() {
  // Throttle redraws - a full I2C refresh on every loop() pass would slow
  // LoRa polling and risk missed packets
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < SENSE_UPDATE_INTERVAL) return;
  lastDraw = millis();

  unsigned long elapsed = millis() - oledStartTime;

  if (elapsed >= OLED_TIMEOUT_MS) {
    if (oledActive) {
      display.clearDisplay();
      display.display();
      oledActive = false;
    }
    return;
  }

  oledActive = true;
  unsigned long remaining = (OLED_TIMEOUT_MS - elapsed) / 1000;
  int remH = remaining / 3600;
  int remM = (remaining % 3600) / 60;
  int remS = remaining % 60;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Title bar
  display.fillRect(0, 0, OLED_WIDTH, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(20, 1);
  display.print("GEN SERVER");
  display.setTextColor(SSD1306_WHITE);

  // WiFi SSID
  display.setCursor(0, 12);
  display.print("WiFi: ");
  String ssid = WiFi.SSID();
  if (ssid.length() > 15) ssid = ssid.substring(0, 15);
  display.print(ssid);

  // IP address
  display.setCursor(0, 21);
  display.print("IP: ");
  display.print(WiFi.localIP());

  // LoRa RSSI and controller status
  display.setCursor(0, 30);
  display.print("RSSI: ");
  display.print(lastRSSI);
  display.print("dBm ");
  display.print(controllerOnline ? "ONLINE" : "OFFLIN");

  // Battery voltage
  display.setCursor(0, 39);
  if (lastBatteryVoltage > 0) {
    display.print("Bat: ");
    display.print(lastBatteryVoltage, 1);
    display.print("V");
    if (lastBatteryVoltage < BATTERY_WARNING_CRITICAL) {
      display.print(" CRIT");
    } else if (lastBatteryVoltage < BATTERY_WARNING_LOW) {
      display.print(" LOW");
    }
  } else {
    display.print("Bat: waiting...");
  }

  // System state
  display.setCursor(0, 48);
  display.print("State: ");
  String stateDisplay = systemStatus;
  stateDisplay.toUpperCase();
  if (stateDisplay.length() > 13) stateDisplay = stateDisplay.substring(0, 13);
  display.print(stateDisplay);

  // Countdown timer
  display.setCursor(0, 57);
  display.print("Disp: ");
  if (remH > 0) {
    display.print(remH);
    display.print("h ");
  }
  if (remM < 10) display.print("0");
  display.print(remM);
  display.print("m ");
  if (remS < 10) display.print("0");
  display.print(remS);
  display.print("s");

  display.display();
}

// ----------------------------------------------------------------
// WIFI WATCHDOG
// Checks WiFi health every 30 seconds and recovers if needed
// ----------------------------------------------------------------
void checkWiFiHealth() {
  static unsigned long lastCheck    = 0;
  static int reconnectAttempts      = 0;

  if (millis() - lastCheck < WIFI_CHECK_INTERVAL) return;
  lastCheck = millis();

  if (WiFi.status() == WL_CONNECTED) {
    // WiFi is fine — reset reconnect counter
    reconnectAttempts = 0;
    return;
  }

  // WiFi dropped
  reconnectAttempts++;
  Serial.print("WiFi lost - reconnect attempt ");
  Serial.println(reconnectAttempts);

  // Same sequence as setupWiFi(): clear stale state, then re-apply the
  // static IP before WiFi.begin()
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.config(staticIP, gateway, subnet, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Wait for reconnection. Total blocking time (< WIFI_RECONNECT_WAIT_MS plus
  // the 100 ms above) stays well inside the WDT_TIMEOUT_MS watchdog window.
  unsigned long waitStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - waitStart < WIFI_RECONNECT_WAIT_MS) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconnected successfully");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    reconnectAttempts = 0;
    // Restart mDNS after reconnection
    MDNS.end();
    MDNS.begin("generator");
  } else {
    Serial.print("Reconnection failed - attempt ");
    Serial.println(reconnectAttempts);

    if (reconnectAttempts >= WIFI_MAX_RECONNECTS) {
      Serial.println("Too many failed reconnects - restarting board");
      delay(1000);
      ESP.restart();
    }
  }
}