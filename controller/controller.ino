// ================================================================
// SENSOR TEST SKETCH
// Reads battery voltage divider (GPIO 34) and ZMPT101B (GPIO 35)
// Use this to verify sensing circuits before main firmware integration
// ================================================================

#define BATTERY_SENSE_PIN   34
#define AC_SENSE_PIN        35
#define SAMPLE_COUNT        100   // Samples for AC RMS calculation
#define SAMPLE_INTERVAL_US  200   // Microseconds between samples

// Voltage divider calibration
// R1 = 47k, R2 = 10k, ratio = 10/57 = 0.17543
#define DIVIDER_RATIO       0.17543
#define VOLTAGE_CAL_OFFSET  0.0   // Adjust after comparing to multimeter

void setup() {
  Serial.begin(115200);
  Serial.println("Sensor test starting...");
  Serial.println("Battery sense on GPIO 34");
  Serial.println("AC sense on GPIO 35");
  Serial.println("---");
}

void loop() {
  // ── Battery voltage reading ──────────────────────────────────
  int rawBattery = analogRead(BATTERY_SENSE_PIN);
  float pinVoltage = (rawBattery / 4095.0) * 3.3;
  float batteryVoltage = (pinVoltage / DIVIDER_RATIO) + VOLTAGE_CAL_OFFSET;

  // ── AC presence reading — sample and find peak to peak ───────
  int maxVal = 0;
  int minVal = 4095;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int sample = analogRead(AC_SENSE_PIN);
    if (sample > maxVal) maxVal = sample;
    if (sample < minVal) minVal = sample;
    delayMicroseconds(SAMPLE_INTERVAL_US);
  }

  int peakToPeak = maxVal - minVal;
  bool acPresent = peakToPeak > 100;   // Threshold — adjust during calibration

  // ── Print results ─────────────────────────────────────────────
  Serial.print("Battery raw: ");
  Serial.print(rawBattery);
  Serial.print("  Pin voltage: ");
  Serial.print(pinVoltage, 3);
  Serial.print("V  Battery: ");
  Serial.print(batteryVoltage, 2);
  Serial.println("V");

  Serial.print("AC raw min: ");
  Serial.print(minVal);
  Serial.print("  max: ");
  Serial.print(maxVal);
  Serial.print("  peak-to-peak: ");
  Serial.print(peakToPeak);
  Serial.print("  AC present: ");
  Serial.println(acPresent ? "YES" : "NO");

  Serial.println("---");
  delay(1000);
}