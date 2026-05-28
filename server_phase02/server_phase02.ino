// ================================================================
// GENERATOR SERVER FIRMWARE
// Stage 3 — Web server added
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
// WIFI CONSTANTS — Fill these in
// ----------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

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
// SYSTEM STATE
// Single source of truth for what the system thinks is happening
// ----------------------------------------------------------------
String systemStatus   = "unknown";   // unknown / running / stopped / error
String statusDetail   = "Waiting for controller...";
unsigned long timerEndMs = 0;        // 0 = no timer active
bool timerActive      = false;

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
    Serial.println("WARNING: WiFi connection failed. Web server unavailable.");
  }
}

// ----------------------------------------------------------------
// WEB SERVER SETUP
// ----------------------------------------------------------------
void setupWebServer() {
  // Serve the main HTML page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // Handle start/stop commands from the UI
  server.on("/command", HTTP_GET, [](AsyncWebServerRequest *request) {
    String cmd = "";
    int timerMinutes = 0;

    if (request->hasParam("cmd")) {
      cmd = request->getParam("cmd")->value();
    }
    if (request->hasParam("timer")) {
      timerMinutes = request->getParam("timer")->value().toInt();
    }

    String responseMessage = "";

    if (cmd == "start") {
      systemStatus = "running";
      statusDetail = "Started from web UI";
      responseMessage = "Start command sent";

      if (timerMinutes > 0) {
        timerEndMs = millis() + (timerMinutes * 60000UL);
        timerActive = true;
        statusDetail = "Timer active — " + String(timerMinutes) + " minutes";
        responseMessage = "Started with " + String(timerMinutes) + " minute timer";
      }

      // In Stage 4 we will send LoRa command here
      Serial.println("CMD: START received from web UI");

    } else if (cmd == "stop") {
      systemStatus = "stopped";
      statusDetail = "Stopped from web UI";
      timerActive = false;
      timerEndMs = 0;
      responseMessage = "Stop command sent";

      // In Stage 4 we will send LoRa command here
      Serial.println("CMD: STOP received from web UI");

    } else {
      responseMessage = "Unknown command";
    }

    // Build JSON response
    StaticJsonDocument<128> doc;
    doc["status"]  = systemStatus;
    doc["message"] = responseMessage;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Status endpoint — polled every 10 seconds by the UI
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["status"] = systemStatus;
    doc["detail"] = statusDetail;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Web server started.");
}

// ----------------------------------------------------------------
// LORA — Listen for incoming messages
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

  if (message == "ACK:START") {
    systemStatus = "running";
    Serial.println("Controller confirmed: STARTED");
  } else if (message == "ACK:STOP") {
    systemStatus = "stopped";
    Serial.println("Controller confirmed: STOPPED");
  } else if (message.startsWith("HB:")) {
    // Heartbeat — we will process this properly in Stage 5
    Serial.print("Heartbeat: ");
    Serial.println(message);
  } else if (message.startsWith("ERR:")) {
    systemStatus = "error";
    statusDetail = message;
    Serial.print("Controller error: ");
    Serial.println(message);
  }
}

// ----------------------------------------------------------------
// TIMER — Check if a timed run has expired
// ----------------------------------------------------------------
void checkTimer() {
  if (!timerActive) return;
  if (millis() < timerEndMs) return;

  // Timer expired — send stop
  timerActive = false;
  timerEndMs = 0;
  systemStatus = "stopped";
  statusDetail = "Stopped — timer expired";

  // In Stage 4 we will send LoRa command here
  Serial.println("Timer expired — sending STOP");
}

// ----------------------------------------------------------------
// LORA SEND
// ----------------------------------------------------------------
void sendLoRaMessage(String message) {
  String outgoing = String(CTRL_ADDRESS) + ":" + message;
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();
  Serial.print("LoRa sent: ");
  Serial.println(outgoing);
}