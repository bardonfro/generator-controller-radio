// ================================================================
// GENERATOR CONTROLLER FIRMWARE
// Stage 2 — Relay Control Added
// Board: LilyGo TTGO LoRa32 V2.1
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
#define LORA_RST      23
#define LORA_DIO0     26

// *** NEW IN STAGE 2 ***
#define RELAY_PIN     13    // GPIO pin connected to relay module input
                            // Update this after checking your board pinout

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

// *** NEW IN STAGE 2 ***
// ----------------------------------------------------------------
// SYSTEM STATE
// Tracks current relay state so we can report it accurately
// ----------------------------------------------------------------
bool relayState = false;   // false = off, true = on

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("Controller booting...");

  // *** NEW IN STAGE 2 ***
  // Initialize relay pin as output and ensure it starts OFF
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay initialized — OFF");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("ERROR: LoRa init failed. Check wiring.");
    while (true);
  }

  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setSpreadingFactor(LORA_SPREAD);
  LoRa.setCodingRate4(LORA_CODERATE);

  Serial.println("LoRa initialized successfully.");
  Serial.println("Listening for commands...");
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  checkForLoRaMessage();
}

// ----------------------------------------------------------------
// FUNCTIONS
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

  if (!incoming.startsWith(MY_ADDRESS)) {
    Serial.println("Message not addressed to us, ignoring.");
    return;
  }

  String message = incoming.substring(5);
  handleMessage(message);
}

// *** UPDATED IN STAGE 2 ***
void handleMessage(String message) {
  Serial.print("Handling message: ");
  Serial.println(message);

  if (message == "CMD:START") {
    setRelay(true);
    sendMessage("ACK:START");
  }
  else if (message == "CMD:STOP") {
    setRelay(false);
    sendMessage("ACK:STOP");
  }
  else {
    // Unknown command — log it and send back an error
    Serial.print("Unknown command: ");
    Serial.println(message);
    sendMessage("ERR:UNKNOWN_CMD");
  }
}

// *** NEW IN STAGE 2 ***
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? HIGH : LOW);

  Serial.print("Relay set to: ");
  Serial.println(state ? "ON" : "OFF");
}

void sendMessage(String message) {
  String outgoing = String(SERVER_ADDRESS) + ":" + message;

  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();

  Serial.print("Sent: ");
  Serial.println(outgoing);
}