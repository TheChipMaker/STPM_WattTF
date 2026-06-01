/*
 * STPM_WattTF - Calibration example
 *
 * Run this ONCE per board to find its calibration constants. You will need a
 * known reference: a calibrated voltage reading (a good multimeter is enough
 * for voltage) and a known current (a reference ammeter, or a resistive load
 * of accurately known draw).
 *
 * WHAT IT DOES
 *   1. Initializes the meter with your hardware config.
 *   2. Asks you to apply a steady, known load.
 *   3. You type the TRUE voltage and TRUE current (from your reference).
 *   4. It computes the calibrator constants and prints two lines for you to
 *      paste into your main sketch.
 *
 * IMPORTANT: the constants are calibrated for THIS SPECIFIC BOARD. Component
 * tolerances differ board to board, so do not reuse one board's constants on
 * a different board if you need high accuracy.
 *
 * After calibration, your main sketch just does:
 *     meter.begin(vcfg, icfg);
 *     meter.setVoltageCalibrator(0x...);   // <- pasted from this sketch
 *     meter.setCurrentCalibrator(0x...);
 *
 * (Advanced: if your platform has persistent storage, you can instead SAVE
 *  the two values and restore them on boot, so you never hardcode them. See
 *  the README "Persisting calibration" section for ESP32 NVS / AVR EEPROM
 *  snippets. The library itself stores nothing -- that is your app's job.)
 */

#include <SPI.h>
#include <STPM_WattTF.h>

// --- Pins (match your wiring) ---
const uint8_t CS_PIN  = 25;
const uint8_t SYN_PIN = 17;
const uint8_t EN_PIN  = 4;

STPM_WattTF meter(CS_PIN, SYN_PIN, EN_PIN);

// Read a floating-point number typed into the Serial monitor (ends on Enter).
// Blocks until a full line is received. Returns the parsed value.
float readSerialFloat() {
  while (Serial.available()) Serial.read();   // clear any leftover input
  String line = "";
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (line.length() > 0) break;   // got a complete entry
      } else {
        line += c;
      }
    }
  }
  return line.toFloat();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  SPI.begin();

  // ---- Your hardware config (edit to match your board) ----
  StpmVoltageConfig vcfg;
  vcfg.seriesLine    = 3 * 133000.0f;
  vcfg.seriesNeutral = 3 * 133000.0f;
  vcfg.senseR2       = 470.0f;

  StpmCurrentConfig icfg;
  icfg.turnsRatio = 2000.0f;
  icfg.burdenOhms = 10.0f;
  icfg.gain       = StpmGain::X2;

  meter.begin(vcfg, icfg);

  Serial.println();
  Serial.println(F("=== STPM_WattTF Calibration ==="));
  Serial.println(F("Apply a STEADY, KNOWN load now."));
  Serial.println(F("Let readings settle for a few seconds, then continue."));
  Serial.println();

  // Show live (uncalibrated) readings so the user can confirm things are stable.
  for (int i = 0; i < 5; i++) {
    Serial.print(F("  live  Vrms="));
    Serial.print(meter.readVoltageRMS(), 2);
    Serial.print(F(" V   Irms="));
    Serial.print(meter.readCurrentRMS(), 3);
    Serial.println(F(" A"));
    delay(1000);
  }

  // ---- Voltage calibration ----
  Serial.println();
  Serial.println(F("Enter the TRUE voltage from your reference meter, then Enter:"));
  float trueV = readSerialFloat();
  bool okV = meter.calibrateVoltage(trueV);

  // ---- Current calibration ----
  Serial.println(F("Enter the TRUE current (A) from your reference, then Enter:"));
  float trueI = readSerialFloat();
  bool okI = meter.calibrateCurrent(trueI);

  // ---- Results ----
  Serial.println();
  Serial.println(F("=== Calibration complete ==="));
  if (okV && okI) {
    Serial.println(F("Paste these two lines into your main sketch's setup(),"));
    Serial.println(F("right after meter.begin(...):"));
    Serial.println();
    Serial.print(F("  meter.setVoltageCalibrator(0x"));
    Serial.print(meter.getVoltageCalibrator(), HEX);
    Serial.println(F(");"));
    Serial.print(F("  meter.setCurrentCalibrator(0x"));
    Serial.print(meter.getCurrentCalibrator(), HEX);
    Serial.println(F(");"));
  } else {
    Serial.println(F("Calibration FAILED (bad input or zero reading)."));
    Serial.println(F("Check the load is applied and values are non-zero."));
  }
  Serial.println();

  // Show calibrated readings so you can confirm they now match the reference.
  Serial.println(F("Calibrated readings (should match your reference):"));
}

void loop() {
  Serial.print(F("  Vrms="));
  Serial.print(meter.readVoltageRMS(), 2);
  Serial.print(F(" V   Irms="));
  Serial.print(meter.readCurrentRMS(), 3);
  Serial.println(F(" A"));
  delay(2000);
}