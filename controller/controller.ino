// ================================================================
// GENERATOR CONTROLLER FIRMWARE
// Stage 2 revised — Two-stage confirmation, correct board def
// Board: LilyGo T3 V1.6.1 — Select "TTGO LoRa32 V2.1 (1.6.1)"
// ================================================================

#include <SPI.h>
#include <LoRa.h>

// ----------------------------------------------------------------
// PIN CONSTANTS
// ----------------------------------------------------------------
#define LORA_SCK      5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_SS       18
#define LORA_RESET    23
#define LORA_DIO0     26

#define RELAY_PIN     13

// ----------------------------------------------------------------
// VOLTAGE SENSING CONSTANTS
// ----------------------------------------------------------------
#define VOLTAGE_SENSING_ENABLED   false
#define VOLTAGE_SENSE_PIN         34
#define VOLTAGE_START_THRESHOLD   50.0
#define GENERATOR_SPINUP_MS       10000
#define GENERATOR_CONFIRM_MS      15000

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
#define MY_ADDRESS        "CTRL"
#define SERVER_ADDRESS    "SERV"

// ----------------------------------------------------------------
// SYSTEM STATE
// ----------------------------------------------------------------
bool relayState = false;

// ----------------------------------------------------------------
// FORWARD DECLARATIONS
// ----------------------------------------------------------------
void checkForLoRaMessage();
void handleMessage(String message);
void confirmGeneratorState(bool expectRunning);
void setRelay(bool state);
void sendMessage(String message);

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Controller booting...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay initialized - OFF");

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
  Serial.println("Listening for commands...");
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  checkForLoRaMessage();
}

// ----------------------------------------------------------------
// LORA RECEIVE
// ----------------------------------------------------------------
void checkForLoRaMessage() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }

  int rssi = LoRa.packetRssi();
  Serial.print("Received: ");
  Serial.print(incoming);
  Serial.print("  RSSI: ");
  Serial.println(rssi);

  if (!incoming.startsWith(MY_ADDRESS)) return;

  String message = incoming.substring(5);
  handleMessage(message);
}

// ----------------------------------------------------------------
// COMMAND HANDLER
// ----------------------------------------------------------------
void handleMessage(String message) {
  Serial.print("Handling: ");
  Serial.println(message);

  if (message == "CMD:START") {
    setRelay(true);
    sendMessage("ACK:START");
    Serial.println("Relay closed - ACK sent. Beginning power confirmation...");
    confirmGeneratorState(true);

  } else if (message == "CMD:STOP") {
    setRelay(false);
    sendMessage("ACK:STOP");
    Serial.println("Relay opened - ACK sent.");
    confirmGeneratorState(false);

  } else {
    Serial.print("Unknown command: ");
    Serial.println(message);
    sendMessage("ERR:UNKNOWN_CMD");
  }
}

// ----------------------------------------------------------------
// POWER CONFIRMATION
// ----------------------------------------------------------------
void confirmGeneratorState(bool expectRunning) {
  if (VOLTAGE_SENSING_ENABLED) {
    Serial.println("Waiting for voltage confirmation...");
    unsigned long waitStart = millis();
    bool confirmed = false;

    while (millis() - waitStart < GENERATOR_CONFIRM_MS) {
      float reading = analogRead(VOLTAGE_SENSE_PIN);
      if (expectRunning && reading > VOLTAGE_START_THRESHOLD) {
        confirmed = true;
        break;
      }
      if (!expectRunning && reading < VOLTAGE_START_THRESHOLD) {
        confirmed = true;
        break;
      }
      delay(500);
    }

    if (confirmed) {
      if (expectRunning) {
        Serial.println("Power confirmed ON");
        sendMessage("STATUS:ON");
      } else {
        Serial.println("Power confirmed OFF");
        sendMessage("STATUS:OFF");
      }
    } else {
      if (expectRunning) {
        Serial.println("ERROR: Generator failed to start");
        sendMessage("ERR:START_FAILED");
        setRelay(false);
      } else {
        Serial.println("ERROR: Power still present after stop");
        sendMessage("ERR:STOP_FAILED");
      }
    }

  } else {
    Serial.print("Voltage sensing disabled. Waiting ");
    Serial.print(GENERATOR_SPINUP_MS / 1000);
    Serial.println("s for spinup...");

    delay(GENERATOR_SPINUP_MS);

    if (expectRunning) {
      Serial.println("Relay closed - assuming generator started (unconfirmed)");
      sendMessage("STATUS:ASSUMED_ON");
    } else {
      Serial.println("Relay open - assuming generator stopped (unconfirmed)");
      sendMessage("STATUS:ASSUMED_OFF");
    }
  }
}

// ----------------------------------------------------------------
// RELAY CONTROL
// ----------------------------------------------------------------
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);
  Serial.print("Relay: ");
  Serial.println(state ? "ON" : "OFF");
}

// ----------------------------------------------------------------
// LORA SEND
// ----------------------------------------------------------------
void sendMessage(String message) {
  String outgoing = String(SERVER_ADDRESS) + ":" + message;
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();
  Serial.print("Sent: ");
  Serial.println(outgoing);
}