// ================================================================
// GENERATOR SERVER FIRMWARE
// Stage 4.2 — Updated with static IP, mDNS, and corrected init order, OLED
// Board: LilyGo T3 V1.6.1
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
// WIFI CONSTANTS
// ----------------------------------------------------------------
const char* WIFI_SSID     = "MAG";
const char* WIFI_PASSWORD = "SW2qpuYDCX$64ich";

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
#define ACK_TIMEOUT_MS    15000
#define MAX_RETRIES       2

// ----------------------------------------------------------------
// OLED CONSTANTS
// ----------------------------------------------------------------
#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_RESET        -1      // No reset pin on this board
#define OLED_ADDRESS      0x3C
#define OLED_SDA          21
#define OLED_SCL          22
#define OLED_TIMEOUT_MS   3600000UL  // 60 minutes in milliseconds

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
unsigned long oledStartTime = 0;
bool oledActive = true;

// ----------------------------------------------------------------
// FORWARD DECLARATIONS
// ----------------------------------------------------------------
void setupWiFi();
void setupLoRa();
void setupWebServer();
void checkForLoRaMessage();
void checkSendTimeout();
void checkTimer();
void queueCommand(String command);
void transmitNow();
void updateOLED();

// ----------------------------------------------------------------
// GLOBAL VARIABLES
// ----------------------------------------------------------------

int lastRSSI = 0;
String lastTX = "none";
String lastRX = "none";

// ----------------------------------------------------------------
// SEND STATE MACHINE
// ----------------------------------------------------------------
enum SendState {
  SEND_IDLE,
  SEND_WAITING,
};

SendState   sendState      = SEND_IDLE;
String      pendingCommand = "";
String      expectedAck    = "";
int         sendAttempt    = 0;
unsigned long sendTimestamp = 0;
bool        lastSendSuccess = false;
bool        lastSendDone    = false;

// ----------------------------------------------------------------
// SYSTEM STATE
// ----------------------------------------------------------------
String systemStatus  = "unknown";
String statusDetail  = "Waiting for controller...";
unsigned long timerEndMs = 0;
bool timerActive     = false;

// ----------------------------------------------------------------
// WEB SERVER
// ----------------------------------------------------------------
AsyncWebServer server(80);

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Server booting...");

  // Initialize OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("WARNING: OLED init failed");
  } else {
    oledStartTime = millis();
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 28);
    display.print("GEN SERVER v1.0");
    display.display();
    delay(2000);
    Serial.println("OLED initialized.");
}

  // WiFi initializes first — before LoRa
  setupWiFi();
  setupLoRa();
  setupWebServer();

  Serial.println("All systems ready.");
  Serial.print("Browse to http://generator.local or http://");
  Serial.println(WiFi.localIP());
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  checkForLoRaMessage();
  checkSendTimeout();
  checkTimer();
  updateOLED();
}

// ----------------------------------------------------------------
// WIFI SETUP — First in init order
// ----------------------------------------------------------------
void setupWiFi() {
  Serial.print("Connecting to WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Apply static IP before connecting
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

    // Start mDNS — accessible at http://generator.local
    if (MDNS.begin("generator")) {
      Serial.println("mDNS started - http://generator.local");
    } else {
      Serial.println("WARNING: mDNS failed to start");
    }
  } else {
    Serial.println();
    Serial.println("WARNING: WiFi connection failed. Web server unavailable.");
  }
}

// ----------------------------------------------------------------
// LORA SETUP — After WiFi is established
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
    String cmd = "";
    int timerMinutes = 0;

    if (request->hasParam("cmd")) {
      cmd = request->getParam("cmd")->value();
    }
    if (request->hasParam("timer")) {
      timerMinutes = request->getParam("timer")->value().toInt();
    }

    if (sendState != SEND_IDLE) {
      JsonDocument doc;
      doc["status"]  = systemStatus;
      doc["message"] = "Busy — previous command still in progress";
      doc["pending"] = true;
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
      return;
    }

    if (cmd == "start") {
      if (timerMinutes > 0) {
        timerEndMs  = millis() + (timerMinutes * 60000UL);
        timerActive = true;
        statusDetail = "Running — auto-stop in " + String(timerMinutes) + " min";
      }
      queueCommand("CMD:START");
    } else if (cmd == "stop") {
      timerActive = false;
      timerEndMs  = 0;
      queueCommand("CMD:STOP");
    }

    JsonDocument doc;
    doc["status"]  = "pending";
    doc["message"] = "Command sent — waiting for controller...";
    doc["pending"] = true;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["status"]  = systemStatus;
    doc["detail"]  = statusDetail;
    doc["pending"] = (sendState != SEND_IDLE);
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
  pendingCommand = command;
  expectedAck    = "ACK:" + command.substring(4);
  sendAttempt    = 0;
  lastSendDone   = false;
  transmitNow();
}

void transmitNow() {
  sendAttempt++;
  sendTimestamp = millis();
  sendState     = SEND_WAITING;
  lastTX = pendingCommand; 

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
  if (millis() - sendTimestamp < ACK_TIMEOUT_MS) return;

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

    if (pendingCommand == "CMD:START") {
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

  Serial.print("LoRa received: ");
  Serial.println(incoming);

  if (!incoming.startsWith(MY_ADDRESS)) return;

  String message = incoming.substring(5);
  lastRSSI = LoRa.packetRssi();
  lastRX = message;

  // ACK messages
  if (message == "ACK:START") {
    Serial.println("Relay closed — awaiting power confirmation");
    statusDetail = "Relay closed — waiting for generator...";
    return;
  }

  if (message == "ACK:STOP") {
    Serial.println("Relay opened — awaiting power confirmation");
    statusDetail = "Relay opened — confirming shutdown...";
    return;
  }

  // Check if this is the ACK we are waiting for
  if (sendState == SEND_WAITING && message == expectedAck) {
    Serial.println("ACK received.");
    sendState       = SEND_IDLE;
    lastSendDone    = true;
    lastSendSuccess = true;

    if (pendingCommand == "CMD:START") {
      systemStatus = "running";
      statusDetail  = timerActive ? "Running — timer active" : "Running";
    } else if (pendingCommand == "CMD:STOP") {
      systemStatus = "stopped";
      statusDetail  = "Stopped";
    }
    return;
  }

  // STATUS messages from controller
  if (message == "STATUS:ON") {
    systemStatus = "running";
    statusDetail  = "Running — power confirmed";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "STATUS:OFF") {
    systemStatus = "stopped";
    statusDetail  = "Stopped — power confirmed off";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "STATUS:ASSUMED_ON") {
    systemStatus = "running";
    statusDetail  = "Running (unconfirmed — no voltage sensing)";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "STATUS:ASSUMED_OFF") {
    systemStatus = "stopped";
    statusDetail  = "Stopped (unconfirmed — no voltage sensing)";
    sendState     = SEND_IDLE;
    return;
  }

  // Error messages
  if (message == "ERR:START_FAILED") {
    systemStatus = "error";
    statusDetail  = "Generator failed to start — no power detected";
    sendState     = SEND_IDLE;
    return;
  }

  if (message == "ERR:STOP_FAILED") {
    systemStatus = "error";
    statusDetail  = "Stop command sent but power still present";
    sendState     = SEND_IDLE;
    return;
  }

  // Heartbeat
  if (message.startsWith("HB:")) {
    Serial.print("Heartbeat: ");
    Serial.println(message);
  }

  // Other errors
  if (message.startsWith("ERR:")) {
    systemStatus = "error";
    statusDetail  = message;
    Serial.print("Controller error: ");
    Serial.println(message);
  }
}

// ----------------------------------------------------------------
// OLED UPDATE
// ----------------------------------------------------------------
void updateOLED() {
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
  display.setCursor(28, 1);
  display.print("GEN SERVER");
  display.setTextColor(SSD1306_WHITE);

  // WiFi SSID — truncate to 15 chars
  display.setCursor(0, 12);
  display.print("WiFi: ");
  String ssid = WiFi.SSID();
  if (ssid.length() > 15) ssid = ssid.substring(0, 15);
  display.print(ssid);

  // IP address
  display.setCursor(0, 21);
  display.print("IP: ");
  display.print(WiFi.localIP());

  // LoRa RSSI
  display.setCursor(0, 30);
  display.print("RSSI: ");
  display.print(lastRSSI);
  display.print(" dBm");

  // System state
  display.setCursor(0, 39);
  display.print("State: ");
  String stateDisplay = systemStatus;
  stateDisplay.toUpperCase();
  display.print(stateDisplay);

  // Last TX
  display.setCursor(0, 48);
  display.print("TX:");
  String txDisplay = lastTX;
  if (txDisplay.length() > 15) txDisplay = txDisplay.substring(0, 15);
  display.print(txDisplay);

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
// TIMER CHECK
// ----------------------------------------------------------------
void checkTimer() {
  if (!timerActive) return;
  if (millis() < timerEndMs) return;

  timerActive = false;
  timerEndMs  = 0;
  Serial.println("Timer expired — queuing STOP");
  queueCommand("CMD:STOP");
}