/*
 * STPM_WattTF - BasicReadings example
 *
 * Reads RMS voltage and current from an STPM32 and prints them once a second.
 *
 * Wiring (adjust pins to your board):
 *   STPM SCS  -> CS_PIN
 *   STPM SYN  -> SYN_PIN
 *   STPM EN   -> EN_PIN
 *   STPM SCL  -> SPI SCK
 *   STPM MOSI -> SPI MOSI
 *   STPM MISO -> SPI MISO
 *
 * This example uses the reference-board hardware values. Change the configs
 * below to match YOUR voltage divider and CT.
 */

#include <SPI.h>
#include <STPM_WattTF.h>

// --- Pins (match your wiring) ---
const uint8_t CS_PIN  = 25;
const uint8_t SYN_PIN = 17;
const uint8_t EN_PIN  = 4;

STPM_WattTF meter(CS_PIN, SYN_PIN, EN_PIN);

void setup() {
  Serial.begin(115200);
  delay(500);
  SPI.begin();

  // Voltage divider: 3x133k in series on EACH of line and neutral legs,
  // 470 ohm across VIP-VIN.
  StpmVoltageConfig vcfg;
  vcfg.seriesLine    = 3 * 133000.0f;   // 399 k
  vcfg.seriesNeutral = 3 * 133000.0f;   // 399 k
  vcfg.senseR2       = 470.0f;

  // Current: 2000:1 CT, 10 ohm burden, gain x2.
  StpmCurrentConfig icfg;
  icfg.turnsRatio = 2000.0f;
  icfg.burdenOhms = 10.0f;
  icfg.gain       = StpmGain::X2;

  meter.begin(vcfg, icfg);

  Serial.println("STPM_WattTF BasicReadings");
  Serial.print("Voltage LSB: "); Serial.println(meter.voltageLSB(), 10);
  Serial.print("Current LSB: "); Serial.println(meter.currentLSB(), 10);
}

void loop() {
  float v  = meter.readVoltageRMS();
  float i  = meter.readCurrentRMS();
  float p  = meter.readActivePower();
  float pf = meter.readFundamentalPower();
  float q  = meter.readReactivePower();
  float s  = meter.readApparentRMSPower();

  Serial.print("Vrms: ");  Serial.print(v, 2);
  Serial.print(" V  Irms: "); Serial.print(i, 3);
  Serial.print(" A  P: ");    Serial.print(p, 1);
  Serial.print(" W  Pf: ");   Serial.print(pf, 1);
  Serial.print(" W  Q: ");    Serial.print(q, 1);
  Serial.print(" var  S: ");  Serial.print(s, 1);
  Serial.print(" VA");
  Serial.print("   PF(true): "); Serial.print(meter.readTruePowerFactor(), 3);
  Serial.print("  PF(disp): ");  Serial.print(meter.readDisplacementPowerFactor(), 3);
  Serial.println();

  delay(1000);
}