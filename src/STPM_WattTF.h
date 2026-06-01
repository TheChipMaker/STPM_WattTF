/*
 * STPM_WattTF.h
 *
 * Arduino / PlatformIO driver for the STMicroelectronics STPM3x energy
 * metering family. This release targets the single-channel STPM32 with a
 * current-transformer (CT) sensor.
 *
 * Scaling (LSB) values are computed from your board's hardware by default,
 * or you may supply raw LSB values directly to override the computation.
 *
 * Reference: STMicroelectronics datasheet DS10272 Rev 14.
 *
 * Author: Abu Nabil <thechipmaker1@gmail.com>
 * MIT licensed.
 */

#ifndef STPM_WATTTF_H
#define STPM_WATTTF_H

#include <Arduino.h>
#include <SPI.h>
#include "STPM_WattTF_regs.h"

// ===========================================================================
// Current-channel gain (datasheet Table 9, DFE_CR1 GAIN bits [27:26]).
// The current preamplifier gain; voltage channel gain is fixed at x2.
// ===========================================================================
enum class StpmGain : uint8_t {
    X2  = 0b00,   // +-300 mV full scale
    X4  = 0b01,   // +-150 mV
    X8  = 0b10,   // +-75 mV
    X16 = 0b11    // +-37.5 mV  (chip power-on default)
};

// ===========================================================================
// Voltage divider configuration.
//
// The chip senses the differential voltage across the sense resistor R2,
// which sits between the VIP and VIN pins. Mains is attenuated by series
// resistance on the way in.
//
// This struct accepts series resistance PER LEG so that both single-ended
// designs (resistors on the line side only) and balanced/symmetric designs
// (resistors on both line and neutral) are handled by the same fields:
//
//   - Single-ended: put all series resistance in seriesLine, leave
//     seriesNeutral = 0.
//   - Balanced:     put each leg's series resistance in its own field.
//
// Internally the library forms  R1 = seriesLine + seriesNeutral  and uses
// the datasheet ratio (1 + R1/R2).
//
// Example (this library's reference board): three 133k in series on each of
// the line and neutral legs, and a single 470 ohm between VIP and VIN:
//     seriesLine    = 3 * 133000.0   = 399000.0
//     seriesNeutral = 3 * 133000.0   = 399000.0
//     senseR2       = 470.0
// ===========================================================================
struct StpmVoltageConfig {
    float seriesLine;     // total series resistance on the line leg [ohm]
    float seriesNeutral;  // total series resistance on the neutral leg [ohm]
    float senseR2;        // resistor across VIP-VIN [ohm]
};

// ===========================================================================
// Current-channel configuration (CT sensor).
//
// For a current transformer the datasheet defines sensitivity
//     kS = Rb / N      [V/A]
// where N is the CT turns ratio and Rb is the burden resistor across the
// CT secondary. The library computes kS from these two values.
//
// Example (this library's reference board): a 2000:1 CT with a 10 ohm
// burden ->  kS = 10 / 2000 = 0.005 V/A.
//
// (Shunt and Rogowski-coil sensor types are planned for a later release;
//  this release supports CT only.)
// ===========================================================================
struct StpmCurrentConfig {
    float turnsRatio;     // CT turns ratio N (e.g. 2000 for a 2000:1 CT)
    float burdenOhms;     // burden resistor Rb across the CT secondary [ohm]
    StpmGain gain;        // current preamplifier gain (AI)
};

// ===========================================================================
// Optional raw-LSB override.
//
// Most users let the library compute LSBs from the hardware configs above.
// If you have calibrated your meter empirically and prefer to supply the
// scaling factors directly, fill this struct and pass it instead. Any field
// left as a negative value means "compute this one from hardware".
//
//   voltageLSB : volts  per LSB of the Vrms register
//   currentLSB : amps   per LSB of the Irms register
//   powerLSB   : watts  per LSB of a power register
//   energyLSB  : watt-h per LSB of an energy register
// ===========================================================================
struct StpmRawLSB {
    double voltageLSB = -1.0;
    double currentLSB = -1.0;
    double powerLSB   = -1.0;
    double energyLSB  = -1.0;
};

// ===========================================================================
// Advanced electrical constants.
//
// These come from the device architecture and rarely need changing. They are
// exposed so an advanced user can adjust them (e.g. a measured Vref, or a
// non-default calibrator). Defaults are the datasheet values for a CT meter.
// ===========================================================================
struct StpmConstants {
    double vref     = 1.18;       // internal voltage reference [V]
    double calV     = 0.875;      // voltage calibrator mid-value
    double calI     = 0.875;      // current calibrator mid-value
    double avGain    = 2.0;       // fixed voltage-channel gain (AV)
    double kint     = 1.0;        // integrator gain (1.0 for CT/shunt)
    double dclkHz   = 7812.5;     // DSP sample/accumulation rate [Hz]
};

// ===========================================================================
// Main driver class.
// ===========================================================================
class STPM_WattTF {
public:
    // Pins: chip-select, sync, and enable/reset (matches the STPM SPI wiring).
    STPM_WattTF(uint8_t csPin, uint8_t synPin, uint8_t enPin);

    // ----- Initialization -------------------------------------------------
    // Computes LSBs from the supplied hardware configs, resets and configures
    // the chip (sets gain, disables CRC for v1), and prepares it for reading.
    // Returns true on success. SPI.begin() must be called by the sketch first.
    bool begin(const StpmVoltageConfig& vConfig,
               const StpmCurrentConfig& iConfig);

    // Same as above, but with raw-LSB overrides and/or custom constants.
    bool begin(const StpmVoltageConfig& vConfig,
               const StpmCurrentConfig& iConfig,
               const StpmRawLSB& rawLSB,
               const StpmConstants& constants = StpmConstants());

    // ----- Instantaneous measurements ------------------------------------
    // Each returns a freshly-read, scaled value in engineering units.
    float readVoltageRMS();   // [V]
    float readCurrentRMS();   // [A]

    float readActivePower();        // [W]
    float readFundamentalPower();   // [W]
    float readReactivePower();      // [var]
    float readApparentRMSPower();   // [VA]
    float readApparentVectPower();  // [VA]

    float readTruePowerFactor();          // unitless, -1..+1
    float readDisplacementPowerFactor();  // unitless, -1..+1

    // ----- Computed scaling factors (for inspection / debugging) ----------
    double voltageLSB() const { return _lsbV; }
    double currentLSB() const { return _lsbI; }
    double powerLSB()   const { return _lsbP; }
    double energyLSB()  const { return _lsbE; }

private:
    // Pins
    uint8_t _cs, _syn, _en;

    // Computed (or overridden) scaling factors
    double _lsbV = 0.0;
    double _lsbI = 0.0;
    double _lsbP = 0.0;
    double _lsbE = 0.0;

    // Stored configuration
    StpmConstants _k;

    // Low-level SPI transport and helpers are added in the .cpp as we build
    // them (latch, read frame, write frame, etc.). Declared there to keep
    // this public header focused on the user-facing API.
};

#endif // STPM_WATTTF_H