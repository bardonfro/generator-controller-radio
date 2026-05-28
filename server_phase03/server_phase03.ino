// ================================================================
// GENERATOR SERVER FIRMWARE
// Stage 4 revised — Non-blocking ACK with state machine
// Board: LilyGo TTGO LoRa32 V2.1
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "index.h"

// ----------------------------------------------------------------
// PIN CONSTANTS
// ----------------------------------------------------------------
#define LORA_SCK      5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_SS       18
#define LORA_RST      23
#define LORA_DIO0     26

// ----------------------------------------------------------------
// WIFI CONSTANTS
// ----------------------------------------------------------------
const char* WIFI_SSID     = "MAG";
const char* WIFI_PASSWORD = "SW2qpuYDCX$64ich";

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
#define ACK_TIMEOUT_MS    3000
#define MAX_RETRIES       3

// ----------------------------------------------------------------
// SEND STATE MACHINE
// Tracks where we are in the send/wait/retry cycle
// ----------------------------------------------------------------
enum SendState {
  SEND_IDLE,        // Nothing pending
  SEND_WAITING,     // Command sent, waiting for ACK
};

SendState   sendState       = SEND_IDLE;
String      pendingCommand  = "";
String      expectedAck     = "";
int         sendAttempt     = 0;
unsigned long sendTimestamp = 0;

// When a send completes (success or failure), we store the result
// here so the web handler can pick it up on the next status poll
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
  while (!Serial);
  Serial.println("Server booting...");

  setupLoRa();
  setupWiFi();
  setupWebServer();

  Serial.println("All systems ready.");
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  checkForLoRaMessage();
  checkSendTimeout();
  checkTimer();
}

// ----------------------------------------------------------------
// LORA SETUP
// ----------------------------------------------------------------
void setupLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

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
// WIFI SETUP
// ----------------------------------------------------------------
void setupWiFi() {
  Serial.print("Connecting to WiFi");
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
  } else {
    Serial.println();
    Serial.println("WARNING: WiFi connection failed.");
  }
}

// ----------------------------------------------------------------
// WEB SERVER SETUP
// ----------------------------------------------------------------
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
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

    // If a send is already in progress, reject the request
    if (sendState != SEND_IDLE) {
      StaticJsonDocument<128> doc;
      doc["status"]  = "pending";
      doc["message"] = "Command sent — waiting for controller...";
      doc["pending"] = true;   // *** ADD THIS LINE ***
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
      return;
    }

    if (cmd == "start") {
      // Store timer intent so we can apply it when ACK arrives
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

    // Respond immediately — UI will get real status on next poll
    StaticJsonDocument<128> doc;
    doc["status"]  = "pending";
    doc["message"] = "Command sent — waiting for controller...";
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["status"] = systemStatus;
    doc["detail"] = statusDetail;
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
// Starts the send state machine — does not block
// ----------------------------------------------------------------
void queueCommand(String command) {
  pendingCommand = command;
  expectedAck    = "ACK:" + command.substring(4); // CMD:START -> ACK:START
  sendAttempt    = 0;
  lastSendDone   = false;
  transmitNow();
}

void transmitNow() {
  sendAttempt++;
  sendTimestamp = millis();
  sendState     = SEND_WAITING;

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
// Called every loop() — handles retry and failure non-blocking
// ----------------------------------------------------------------
void checkSendTimeout() {
  if (sendState != SEND_WAITING) return;
  if (millis() - sendTimestamp < ACK_TIMEOUT_MS) return;

  // Timed out on this attempt
  Serial.print("No ACK on attempt ");
  Serial.println(sendAttempt);

  if (sendAttempt < MAX_RETRIES) {
    // Retry
    transmitNow();
  } else {
    // All retries exhausted
    Serial.println("ERROR: All retries failed.");
    sendState    = SEND_IDLE;
    lastSendDone    = true;
    lastSendSuccess = false;
    systemStatus = "error";
    statusDetail  = "No response from controller";

    // Roll back timer if start command failed
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

  // Check if this is the ACK we're waiting for
  if (sendState == SEND_WAITING && message == expectedAck) {
    Serial.println("ACK received.");
    sendState       = SEND_IDLE;
    lastSendDone    = true;
    lastSendSuccess = true;

    // Apply state changes now that we know controller acted
    if (pendingCommand == "CMD:START") {
      systemStatus = "running";
      statusDetail  = timerActive
        ? "Running — timer active"
        : "Running";
    } else if (pendingCommand == "CMD:STOP") {
      systemStatus = "stopped";
      statusDetail  = "Stopped";
    }
    return;
  }

  // Other incoming messages
  if (message.startsWith("HB:")) {
    Serial.print("Heartbeat: ");
    Serial.println(message);
  } else if (message.startsWith("ERR:")) {
    systemStatus = "error";
    statusDetail  = message;
    Serial.print("Controller error: ");
    Serial.println(message);
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
  Serial.println("Timer expired — queuing STOP");
  queueCommand("CMD:STOP");
}