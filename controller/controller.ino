// ================================================================
// GENERATOR CONTROLLER FIRMWARE
// Stage 5 — Full voltage sensing integrated
// Board: LilyGo T3 V1.6.1 — Select "TTGO LoRa32 V2.1 (1.6.1)"
// ================================================================

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
// SENSING CONSTANTS
// ----------------------------------------------------------------
#define VOLTAGE_SENSING_ENABLED   true
#define BATTERY_SENSE_PIN         34
#define AC_SENSE_PIN              35
#define DIVIDER_RATIO             0.17543
#define VOLTAGE_CAL_OFFSET        1.0
#define AC_THRESHOLD              300
#define BATTERY_SAMPLE_COUNT      16
#define AC_SAMPLE_COUNT           100
#define AC_SAMPLE_INTERVAL_US     200

// ----------------------------------------------------------------
// TIMING CONSTANTS
// ----------------------------------------------------------------
#define GENERATOR_CONFIRM_MS      15000  // Max wait for confirmation
#define SHUTDOWN_SUSTAINED_MS     3000   // Sustained absence required
#define SPINUP_CHECK_INTERVAL_MS  500    // How often to check during startup
#define GENERATOR_SPINUP_MS       10000  // How long to wait for startup if sensing loging is disabled

// ----------------------------------------------------------------
// BATTERY WARNING THRESHOLDS
// ----------------------------------------------------------------
#define BATTERY_WARNING_LOW       12.0
#define BATTERY_WARNING_CRITICAL  11.5

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
// OLED CONSTANTS
// ----------------------------------------------------------------
#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_RESET        -1
#define OLED_ADDRESS      0x3C
#define OLED_SDA          21
#define OLED_SCL          22
#define OLED_TIMEOUT_MS   3600000UL

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
unsigned long oledStartTime = 0;
bool oledActive = true;

// ----------------------------------------------------------------
// SYSTEM STATE
// ----------------------------------------------------------------
bool relayState       = false;
bool acPresent        = false;
float batteryVoltage  = 0.0;
String lastTX         = "none";
String lastRX         = "none";
int lastRSSI          = 0;

// Controller safety timer
unsigned long ctrlTimerEndMs = 0;
bool ctrlTimerActive         = false;

// ----------------------------------------------------------------
// FORWARD DECLARATIONS
// ----------------------------------------------------------------
void checkForLoRaMessage();
void handleMessage(String message);
void confirmGeneratorState(bool expectRunning);
bool readACPresent();
float readBatteryVoltage();
void setRelay(bool state);
void sendMessage(String message);
void sendHeartbeat();
void checkControllerTimer();
void updateOLED();

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Controller booting...");

  // Initialize relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay initialized - OFF");

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
    display.print("GEN CTRL v1.0");
    display.display();
    delay(2000);
    Serial.println("OLED initialized.");
  }

  // Initialize LoRa
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
  checkControllerTimer();
  updateOLED();
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

  lastRSSI = LoRa.packetRssi();
  Serial.print("Received: ");
  Serial.print(incoming);
  Serial.print("  RSSI: ");
  Serial.println(lastRSSI);

  if (!incoming.startsWith(MY_ADDRESS)) return;

  String message = incoming.substring(5);
  lastRX = message;
  handleMessage(message);
}

// ----------------------------------------------------------------
// COMMAND HANDLER
// ----------------------------------------------------------------
void handleMessage(String message) {
  Serial.print("Handling: ");
  Serial.println(message);

  if (message.startsWith("CMD:START")) {
    // Parse optional timer value from CMD:START:90
    int timerMinutes = 0;
    int secondColon = message.indexOf(':', 4);
    if (secondColon > 0) {
      timerMinutes = message.substring(secondColon + 1).toInt();
    }

    setRelay(true);
    sendMessage("ACK:START");

    // Start independent safety timer if value was provided
    if (timerMinutes > 0) {
      ctrlTimerEndMs  = millis() + ((timerMinutes + 2) * 60000UL);
      ctrlTimerActive = true;
      Serial.print("Safety timer set: ");
      Serial.print(timerMinutes + 2);
      Serial.println(" minutes");
    }

    Serial.println("Relay closed - ACK sent. Beginning power confirmation...");
    confirmGeneratorState(true);

  } else if (message == "CMD:STOP") {
    ctrlTimerActive = false;
    ctrlTimerEndMs  = 0;
    setRelay(false);
    sendMessage("ACK:STOP");
    Serial.println("Relay opened - ACK sent. Beginning shutdown confirmation...");
    confirmGeneratorState(false);

  } else {
    Serial.print("Unknown command: ");
    Serial.println(message);
    sendMessage("ERR:UNKNOWN_CMD");
  }
}

// ----------------------------------------------------------------
// POWER CONFIRMATION
// Startup: two consecutive YES readings within 15 seconds
// Shutdown: 3 seconds sustained absence within 15 seconds
// ----------------------------------------------------------------
void confirmGeneratorState(bool expectRunning) {
  if (VOLTAGE_SENSING_ENABLED) {
    Serial.println("Waiting for power confirmation...");
    unsigned long waitStart = millis();
    bool confirmed = false;

    if (expectRunning) {
      // ── Startup confirmation ──────────────────────────────
      int consecutiveYes = 0;
      while (millis() - waitStart < GENERATOR_CONFIRM_MS) {
        if (readACPresent()) {
          consecutiveYes++;
          Serial.print("AC detected - consecutive count: ");
          Serial.println(consecutiveYes);
          if (consecutiveYes >= 2) {
            confirmed = true;
            break;
          }
        } else {
          if (consecutiveYes > 0) {
            Serial.println("AC lost - resetting consecutive count");
          }
          consecutiveYes = 0;
        }
        delay(SPINUP_CHECK_INTERVAL_MS);
      }

      if (confirmed) {
        batteryVoltage = readBatteryVoltage();
        Serial.println("Power confirmed ON");
        sendMessage("STATUS:ON:" + String(batteryVoltage, 1));
      } else {
        Serial.println("ERROR: Generator failed to start - no power detected");
        sendMessage("ERR:START_FAILED");
        setRelay(false);
      }

    } else {
      // ── Shutdown confirmation ─────────────────────────────
      unsigned long absentSince    = 0;
      bool absenceStarted          = false;

      while (millis() - waitStart < GENERATOR_CONFIRM_MS) {
        if (!readACPresent()) {
          if (!absenceStarted) {
            absenceStarted = true;
            absentSince    = millis();
            Serial.println("AC dropping - waiting for 3s sustained absence...");
          }
          unsigned long absentDuration = millis() - absentSince;
          Serial.print("Absent for: ");
          Serial.print(absentDuration / 1000);
          Serial.println("s");

          if (absentDuration >= SHUTDOWN_SUSTAINED_MS) {
            confirmed = true;
            break;
          }
        } else {
          if (absenceStarted) {
            Serial.println("AC still present - resetting absence timer");
            absenceStarted = false;
            absentSince    = 0;
          }
        }
        delay(200);
      }

      if (confirmed) {
        Serial.println("Power confirmed OFF");
        sendMessage("STATUS:OFF");
      } else {
        Serial.println("ERROR: Generator still producing power after 15 seconds");
        sendMessage("ERR:STOP_FAILED");
      }
    }

  } else {
    // ── Placeholder path ──────────────────────────────────────
    Serial.print("Voltage sensing disabled. Waiting ");
    Serial.print(GENERATOR_SPINUP_MS / 1000);
    Serial.println("s for spinup...");
    delay(10000);
    if (expectRunning) {
      sendMessage("STATUS:ASSUMED_ON");
    } else {
      sendMessage("STATUS:ASSUMED_OFF");
    }
  }
}

// ----------------------------------------------------------------
// READ AC PRESENCE
// Returns confirmed state requiring two consecutive readings
// ----------------------------------------------------------------
bool readACPresent() {
  static bool lastReading = false;

  int maxVal = 0;
  int minVal = 4095;

  for (int i = 0; i < AC_SAMPLE_COUNT; i++) {
    int sample = analogRead(AC_SENSE_PIN);
    if (sample > maxVal) maxVal = sample;
    if (sample < minVal) minVal = sample;
    delayMicroseconds(AC_SAMPLE_INTERVAL_US);
  }

  int peakToPeak     = maxVal - minVal;
  bool currentReading = peakToPeak > AC_THRESHOLD;
  bool confirmed      = (currentReading == lastReading) ? currentReading : lastReading;
  lastReading         = currentReading;
  return confirmed;
}

// ----------------------------------------------------------------
// READ BATTERY VOLTAGE
// 16 sample average with calibration offset
// ----------------------------------------------------------------
float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    sum += analogRead(BATTERY_SENSE_PIN);
    delayMicroseconds(500);
  }
  int raw          = sum / BATTERY_SAMPLE_COUNT;
  float pinVoltage = (raw / 4095.0) * 3.3;
  return (pinVoltage / DIVIDER_RATIO) + VOLTAGE_CAL_OFFSET;
}

// ----------------------------------------------------------------
// HEARTBEAT
// Sent every 60 seconds — includes battery voltage and AC state
// ----------------------------------------------------------------
void sendHeartbeat() {
  batteryVoltage = readBatteryVoltage();
  acPresent      = readACPresent();

  String batStr   = String(batteryVoltage, 1);
  String acStr    = acPresent ? "AC:ON" : "AC:OFF";
  String relayStr = relayState ? "RELAY:ON" : "RELAY:OFF";

  // Battery warning flag
  String warnStr = "";
  if (batteryVoltage < BATTERY_WARNING_CRITICAL) {
    warnStr = ":WARN:CRITICAL";
  } else if (batteryVoltage < BATTERY_WARNING_LOW) {
    warnStr = ":WARN:LOW";
  }

  sendMessage("HB:" + relayStr + ":" + acStr + ":BAT:" + batStr + warnStr);
}

// ----------------------------------------------------------------
// CONTROLLER SAFETY TIMER
// ----------------------------------------------------------------
void checkControllerTimer() {
  if (!ctrlTimerActive) return;
  if (millis() < ctrlTimerEndMs) return;

  ctrlTimerActive = false;
  ctrlTimerEndMs  = 0;

  Serial.println("Safety timer expired - shutting down relay");
  setRelay(false);
  sendMessage("ERR:SAFETY_TIMEOUT");
  confirmGeneratorState(false);
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
  lastTX          = message;
  String outgoing = String(SERVER_ADDRESS) + ":" + message;
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();
  Serial.print("Sent: ");
  Serial.println(outgoing);
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
  display.print("GEN CTRL");
  display.setTextColor(SSD1306_WHITE);

  // RSSI
  display.setCursor(0, 12);
  display.print("RSSI: ");
  display.print(lastRSSI);
  display.print(" dBm");

  // Relay and AC state on same line
  display.setCursor(0, 21);
  display.print("Relay:");
  display.print(relayState ? "ON " : "OFF");
  display.print(" AC:");
  display.print(acPresent ? "ON" : "OFF");

  // Battery voltage with warning
  display.setCursor(0, 30);
  display.print("Bat: ");
  display.print(batteryVoltage, 1);
  display.print("V");
  if (batteryVoltage > 0 && batteryVoltage < BATTERY_WARNING_CRITICAL) {
    display.print(" CRIT");
  } else if (batteryVoltage > 0 && batteryVoltage < BATTERY_WARNING_LOW) {
    display.print(" LOW");
  }

  // Safety timer countdown if active
  display.setCursor(0, 39);
  if (ctrlTimerActive) {
    unsigned long timerRemaining = (ctrlTimerEndMs - millis()) / 1000;
    int tm = timerRemaining / 60;
    int ts = timerRemaining % 60;
    display.print("Stop in: ");
    display.print(tm);
    display.print("m ");
    if (ts < 10) display.print("0");
    display.print(ts);
    display.print("s");
  } else {
    display.print("RX: ");
    String rxDisplay = lastRX;
    if (rxDisplay.length() > 16) rxDisplay = rxDisplay.substring(0, 16);
    display.print(rxDisplay);
  }

  // Last TX
  display.setCursor(0, 48);
  display.print("TX: ");
  String txDisplay = lastTX;
  if (txDisplay.length() > 16) txDisplay = txDisplay.substring(0, 16);
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