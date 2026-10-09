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
// SAFETY TIMER, MONITOR AND DISPLAY CONSTANTS
// ----------------------------------------------------------------
#define MAX_TIMER_MINUTES             360    // 6 hours - generator must never run indefinitely
#define SAFETY_TIMER_GRACE            2      // Extra minutes added to the safety timer (must match server.ino)
#define HEARTBEAT_INTERVAL            60000  // Controller heartbeat period
#define BOOT_HEARTBEAT_DELAY_MS       2000   // Settle time after LoRa init before boot heartbeat
#define SENSE_UPDATE_INTERVAL         2000   // Sense refresh and OLED redraw period
#define SHUTDOWN_CHECK_INTERVAL_MS    200    // Poll rate while confirming shutdown
#define UNCOMMANDED_CHECK_INTERVAL_MS 500    // Poll rate of the uncommanded shutdown monitor
#define UNCOMMANDED_SHUTDOWN_MS       4000   // AC must be absent this long while relay closed

// ----------------------------------------------------------------
// FIRMWARE VERSION AND LOCAL TEST MODE
// LOCAL_TEST_MODE lets you type commands into the Serial Monitor
// (115200 baud) to exercise the controller at the generator site
// without the server or WiFi. Commands are fed through the same
// handleMessage() path a LoRa packet uses, so THE RELAY REALLY
// OPERATES. Disconnect the generator start wire before testing.
// Leave at 0 for normal operation - when 0 none of the test code
// is compiled in.
// ----------------------------------------------------------------
#define FW_VERSION        "1.1"
#define LOCAL_TEST_MODE   1

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
#undef OLED_SDA   // board variant defines different values; use the T3 V1.6.1 pins
#undef OLED_SCL
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
void checkForUncommandedShutdown();
bool isAllDigits(const String &text);
void updateOLED();
#if LOCAL_TEST_MODE
void checkSerialCommands();
void runLocalTestCommand(String line);
void printLocalTestHelp();
void printLocalSense();
#endif

// ----------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.print("Controller booting... firmware v");
  Serial.println(FW_VERSION);
#if LOCAL_TEST_MODE
  Serial.println("*** LOCAL TEST MODE ENABLED - serial commands operate the relay ***");
#endif

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
    display.print("GEN CTRL v");
    display.print(FW_VERSION);
#if LOCAL_TEST_MODE
    display.setCursor(20, 40);
    display.print("LOCAL TEST MODE");
#endif
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

  // Boot status report so the server does not wait for the first periodic
  // heartbeat. readACPresent() needs two readings to confirm a state, so
  // prime it once here; sendHeartbeat() then takes the second reading.
  delay(BOOT_HEARTBEAT_DELAY_MS);
  acPresent = readACPresent();
  sendHeartbeat();

#if LOCAL_TEST_MODE
  printLocalTestHelp();
#endif
}

// ----------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------
void loop() {
  checkForLoRaMessage();
  checkControllerTimer();
  checkForUncommandedShutdown();   // Only active when relay closed
#if LOCAL_TEST_MODE
  checkSerialCommands();
#endif

  // Sense update for OLED display and heartbeat accuracy. Keeping
  // readACPresent() exercised every 2 seconds keeps its two-reading
  // confirmation current, so heartbeats never report a stale AC state.
  static unsigned long lastSenseUpdate = 0;
  if (millis() - lastSenseUpdate >= SENSE_UPDATE_INTERVAL) {
    lastSenseUpdate = millis();
    if (VOLTAGE_SENSING_ENABLED) {
      acPresent      = readACPresent();
      batteryVoltage = readBatteryVoltage();
    }
  }

  updateOLED();

  // Heartbeat every 60 seconds
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

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

  if (message == "CMD:START" || message.startsWith("CMD:START:")) {
    // Safety rule: a start MUST carry a timer of 1..MAX_TIMER_MINUTES
    // (format CMD:START:90). Anything else is refused before the relay is
    // touched, so the generator can never be started with no timer.
    int timerMinutes = 0;
    if (message.length() > 10) {
      String timerText = message.substring(10);   // text after "CMD:START:"
      if (isAllDigits(timerText) && timerText.length() <= 4) {
        timerMinutes = timerText.toInt();
      }
    }

    if (timerMinutes < 1 || timerMinutes > MAX_TIMER_MINUTES) {
      Serial.print("Rejected start - invalid or missing timer: ");
      Serial.println(message);
      sendMessage("ERR:INVALID_TIMER");
      return;
    }

    // Set the safety timer BEFORE closing the relay so there is never a
    // moment where the relay is closed with no timer armed.
    ctrlTimerEndMs  = millis() + ((unsigned long)(timerMinutes + SAFETY_TIMER_GRACE) * 60000UL);
    ctrlTimerActive = true;
    Serial.print("Safety timer set: ");
    Serial.print(timerMinutes + SAFETY_TIMER_GRACE);
    Serial.println(" minutes");

    setRelay(true);
    sendMessage("ACK:START");

    Serial.println("Relay closed - ACK sent. Beginning power confirmation...");
    confirmGeneratorState(true);

  } else if (message == "CMD:STATUS") {
    // Status request (sent by the server at boot) - answer with a heartbeat
    Serial.println("Status requested - sending heartbeat");
    sendHeartbeat();

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
        // Relay is open again, so the safety timer has nothing to guard.
        // Leaving it armed would fire a spurious ERR:SAFETY_TIMEOUT later.
        ctrlTimerActive = false;
        ctrlTimerEndMs  = 0;
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
        delay(SHUTDOWN_CHECK_INTERVAL_MS);
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
    delay(GENERATOR_SPINUP_MS);
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

  // Safety timer seconds remaining (0 if no timer armed). Lets a rebooted
  // server rebuild its countdown and auto-stop timer from the heartbeat.
  unsigned long timerSec = 0;
  if (ctrlTimerActive && ctrlTimerEndMs > millis()) {
    timerSec = (ctrlTimerEndMs - millis()) / 1000;
  }

  sendMessage("HB:" + relayStr + ":" + acStr + ":TMR:" + String(timerSec) +
              ":BAT:" + batStr + warnStr);
}

// ----------------------------------------------------------------
// UNCOMMANDED SHUTDOWN MONITOR
// Only active while the relay is closed and voltage sensing is enabled.
// If AC stays absent for UNCOMMANDED_SHUTDOWN_MS the generator has
// stopped on its own (fuel, fault, etc.) - report it once.
// Uses its own inline ADC read, NOT readACPresent(): that function keeps
// a static two-reading history shared with confirmGeneratorState() and
// sharing it here would corrupt both.
// Not called during confirmGeneratorState() (it blocks), so startup
// spin-up time can never trigger a false alarm.
// ----------------------------------------------------------------
void checkForUncommandedShutdown() {
  static unsigned long lastCheck   = 0;
  static unsigned long absentSince = 0;
  static bool absenceStarted       = false;
  static bool alreadyReported      = false;

  // Relay open (or sensing off): nothing to watch. Reset all state so the
  // next start begins with a clean slate.
  if (!relayState || !VOLTAGE_SENSING_ENABLED) {
    absenceStarted  = false;
    alreadyReported = false;
    return;
  }

  if (millis() - lastCheck < UNCOMMANDED_CHECK_INTERVAL_MS) return;
  lastCheck = millis();

  // Inline peak-to-peak AC read (same method and threshold as readACPresent)
  int maxVal = 0;
  int minVal = 4095;
  for (int i = 0; i < AC_SAMPLE_COUNT; i++) {
    int sample = analogRead(AC_SENSE_PIN);
    if (sample > maxVal) maxVal = sample;
    if (sample < minVal) minVal = sample;
    delayMicroseconds(AC_SAMPLE_INTERVAL_US);
  }
  bool acNow = (maxVal - minVal) > AC_THRESHOLD;

  if (acNow) {
    if (absenceStarted) {
      Serial.println("Monitor: AC returned - absence timer reset");
    }
    absenceStarted  = false;
    alreadyReported = false;
    return;
  }

  // AC absent while relay is closed
  if (!absenceStarted) {
    absenceStarted = true;
    absentSince    = millis();
    Serial.println("Monitor: AC absent while relay closed - timing...");
    return;
  }

  if (!alreadyReported && (millis() - absentSince >= UNCOMMANDED_SHUTDOWN_MS)) {
    alreadyReported = true;
    acPresent       = false;
    Serial.println("ERROR: Uncommanded shutdown - AC lost while relay closed");
    sendMessage("ERR:UNCOMMANDED_SHUTDOWN");
  }
}

// ----------------------------------------------------------------
// STRING HELPER
// True if the text is non-empty and contains only digits 0-9
// ----------------------------------------------------------------
bool isAllDigits(const String &text) {
  if (text.length() == 0) return false;
  for (unsigned int i = 0; i < text.length(); i++) {
    if (text.charAt(i) < '0' || text.charAt(i) > '9') return false;
  }
  return true;
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
  // Throttle redraws - a full I2C refresh on every loop() pass would slow
  // LoRa polling and risk missed commands
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
    // Guard against unsigned underflow in the instant between expiry and
    // checkControllerTimer() clearing the flag
    unsigned long timerRemaining = (ctrlTimerEndMs > millis())
      ? (ctrlTimerEndMs - millis()) / 1000
      : 0;
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

// ================================================================
// LOCAL TEST MODE (compiled only when LOCAL_TEST_MODE is 1)
// Type commands into the Serial Monitor at 115200 baud, line ending
// "Newline". Each command goes through handleMessage(), the same path
// a received LoRa packet takes, so behavior matches field operation.
// THE RELAY REALLY OPERATES - disconnect the generator start wire.
// Replies are also transmitted over LoRa as usual; a server in range
// will see them.
// ================================================================
#if LOCAL_TEST_MODE 

void printLocalTestHelp() {
  Serial.println();
  Serial.println("---- LOCAL TEST MODE commands ----");
  Serial.println("START:<min>  start with timer 1-360 (e.g. START:5)");
  Serial.println("START        start with NO timer - must be rejected");
  Serial.println("STOP         stop");
  Serial.println("STATUS       send heartbeat");
  Serial.println("SENSE        print AC peak-to-peak, AC state, battery");
  Serial.println("RAW <text>   feed any message, e.g. RAW CMD:START:361");
  Serial.println("HELP         show this list");
  Serial.println("Relay really operates - generator start wire must be disconnected.");
  Serial.println("----------------------------------");
}

void printLocalSense() {
  // Inline read so the diagnostic does not disturb readACPresent() history
  int maxVal = 0;
  int minVal = 4095;
  for (int i = 0; i < AC_SAMPLE_COUNT; i++) {
    int sample = analogRead(AC_SENSE_PIN);
    if (sample > maxVal) maxVal = sample;
    if (sample < minVal) minVal = sample;
    delayMicroseconds(AC_SAMPLE_INTERVAL_US);
  }
  int peakToPeak = maxVal - minVal;

  Serial.print("AC peak-to-peak: ");
  Serial.print(peakToPeak);
  Serial.print(" (threshold ");
  Serial.print(AC_THRESHOLD);
  Serial.print(") -> ");
  Serial.println(peakToPeak > AC_THRESHOLD ? "AC PRESENT" : "AC ABSENT");
  Serial.print("Battery: ");
  Serial.print(readBatteryVoltage(), 2);
  Serial.println(" V");
  Serial.print("Relay: ");
  Serial.println(relayState ? "ON" : "OFF");
  Serial.print("Safety timer: ");
  if (ctrlTimerActive && ctrlTimerEndMs > millis()) {
    Serial.print((ctrlTimerEndMs - millis()) / 1000);
    Serial.println(" s remaining");
  } else {
    Serial.println("not armed");
  }
}

void runLocalTestCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();
  Serial.print("[LOCAL TEST] > ");
  Serial.println(line);

  if (upper == "HELP") {
    printLocalTestHelp();
  } else if (upper == "SENSE") {
    printLocalSense();
  } else if (upper == "STATUS") {
    handleMessage("CMD:STATUS");
  } else if (upper == "STOP") {
    handleMessage("CMD:STOP");
  } else if (upper == "START") {
    handleMessage("CMD:START");
  } else if (upper.startsWith("START:")) {
    handleMessage("CMD:" + upper);
  } else if (upper.startsWith("RAW ")) {
    // Pass the text through unchanged (original case preserved)
    handleMessage(line.substring(4));
  } else {
    Serial.println("Unknown test command - type HELP");
  }
}

void checkSerialCommands() {
  // Non-blocking line reader: accumulate characters until newline
  static String lineBuffer = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineBuffer.length() > 0) {
        String line = lineBuffer;
        lineBuffer = "";
        runLocalTestCommand(line);
      }
    } else if (lineBuffer.length() < 64) {
      lineBuffer += c;
    }
  }
}

#endif  // LOCAL_TEST_MODE