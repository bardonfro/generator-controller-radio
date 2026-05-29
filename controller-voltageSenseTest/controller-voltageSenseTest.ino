// ================================================================
// SENSOR TEST SKETCH — Updated
// Averaged battery reading, raised AC threshold
// ================================================================

#define BATTERY_SENSE_PIN   34
#define AC_SENSE_PIN        35
#define SAMPLE_COUNT        100
#define SAMPLE_INTERVAL_US  200
#define AC_THRESHOLD        300    // Raised from 100

#define DIVIDER_RATIO       0.17543
#define VOLTAGE_CAL_OFFSET  1.0

void setup() {
  Serial.begin(115200);
  Serial.println("Sensor test starting...");
  Serial.println("---");
}

void loop() {
  // ── Battery voltage — 16 sample average ─────────────────────
  long batterySum = 0;
  for (int i = 0; i < 16; i++) {
    batterySum += analogRead(BATTERY_SENSE_PIN);
    delayMicroseconds(500);
  }
  int rawBattery = batterySum / 16;
  float pinVoltage = (rawBattery / 4095.0) * 3.3;
  float batteryVoltage = (pinVoltage / DIVIDER_RATIO) + VOLTAGE_CAL_OFFSET;

// ── AC presence — confirmed over two consecutive readings ─────
static bool lastACReading = false;
static bool confirmedAC = false;

int maxVal = 0;
int minVal = 4095;

for (int i = 0; i < SAMPLE_COUNT; i++) {
  int sample = analogRead(AC_SENSE_PIN);
  if (sample > maxVal) maxVal = sample;
  if (sample < minVal) minVal = sample;
  delayMicroseconds(SAMPLE_INTERVAL_US);
}

int peakToPeak = maxVal - minVal;
bool currentReading = peakToPeak > AC_THRESHOLD;

// Only confirm state change if two consecutive readings agree
if (currentReading == lastACReading) {
  confirmedAC = currentReading;
}
lastACReading = currentReading;
bool acPresent = confirmedAC;

  // ── Print results ────────────────────────────────────────────
  Serial.print("Battery raw: ");
  Serial.print(rawBattery);
  Serial.print("  Pin voltage: ");
  Serial.print(pinVoltage, 3);
  Serial.print("V  Battery: ");
  Serial.print(batteryVoltage, 2);
  Serial.println("V");

  Serial.print("AC peak-to-peak: ");
  Serial.print(peakToPeak);
  Serial.print("  AC present: ");
  Serial.println(acPresent ? "YES" : "NO");

  Serial.println("---");
  delay(1000);
}