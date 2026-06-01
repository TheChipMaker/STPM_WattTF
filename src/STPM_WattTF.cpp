/*
 * STPM_WattTF.cpp
 *
 * Implementation of the STPM_WattTF driver.
 * Reference: STMicroelectronics datasheet DS10272 Rev 14.
 *
 * Author: Abu Nabil <thechipmaker1@gmail.com>
 * MIT licensed.
 */

#include "STPM_WattTF.h"

// SPI settings for the STPM3x:
//   - 1 MHz: well within the device's clock spec and proven reliable across
//     the digital signal isolators on the reference board.
//   - MSB first.
//   - SPI mode 3 (CPOL=1, CPHA=1) as required by the datasheet (section 8.6.2).
static const SPISettings STPM_SPI_SETTINGS(1000000, MSBFIRST, SPI_MODE3);

// ===========================================================================
// Constructor: store pins only. No hardware access here (Arduino convention:
// real setup happens in begin(), after the sketch has called SPI.begin()).
// ===========================================================================
STPM_WattTF::STPM_WattTF(uint8_t csPin, uint8_t synPin, uint8_t enPin)
    : _cs(csPin), _syn(synPin), _en(enPin)
{
}

// ===========================================================================
// Low-level SPI transport
//
// These are file-local helpers for now. They are declared here rather than in
// the header because they are implementation detail; the public API in the
// header stays focused on what the user calls. As the library grows we may
// promote some of these to private class methods.
// ===========================================================================

// Pulse SYN to latch the DSP's live measurement registers into the readable
// transmission latches. A single SYN pulse (while SCS is high) requests a
// latch; see datasheet section 8.6.1, "Synchronization and remote reset".
static void stpmLatch(uint8_t synPin)
{
    digitalWrite(synPin, LOW);
    delayMicroseconds(4); // >= t_lpw latch pulse width (datasheet Table 4)
    digitalWrite(synPin, HIGH);
    delayMicroseconds(4);
}

// Send one 4-byte command frame (no CRC byte; CRC is disabled in v1).
//   readAddr  : address whose contents the chip will return on the NEXT frame
//   writeAddr : 16-bit write target, or STPM_CMD_NO_WRITE (0xFF) for none
//   dataLSB/dataMSB : the 16-bit value to write (ignored if writeAddr=0xFF)
static void stpmSendFrame(uint8_t csPin,
                          uint8_t readAddr, uint8_t writeAddr,
                          uint8_t dataLSB, uint8_t dataMSB)
{
    SPI.beginTransaction(STPM_SPI_SETTINGS);
    digitalWrite(csPin, LOW);
    delayMicroseconds(5);
    SPI.transfer(readAddr);
    SPI.transfer(writeAddr);
    SPI.transfer(dataLSB);
    SPI.transfer(dataMSB);
    delayMicroseconds(5);
    digitalWrite(csPin, HIGH);
    SPI.endTransaction();
}

// Read a 32-bit register.
//
// Per the STPM3x protocol (datasheet section 8.6), reading is a two-step
// dance: one frame sets the read pointer to the address you want, and the
// FOLLOWING frame returns that data. So we:
//   1. latch live data,
//   2. send the address we want (a read-request frame),
//   3. clock out 4 bytes, which are the contents of the requested address.
//
// On the wire the least-significant byte arrives first (datasheet Figure 48),
// so we assemble with byte[0] as bits [7:0] up to byte[3] as bits [31:24].
// This function returns the value already in correct order, so callers never
// deal with byte reversal.
static uint32_t stpmReadRegister32(uint8_t csPin, uint8_t synPin, uint8_t addr)
{
    // 1. Latch the live measurement data into the readable latches.
    stpmLatch(synPin);

    // 2. Request the register. The data byte fields are "no write" (0xFF).
    stpmSendFrame(csPin, addr, STPM_CMD_NO_WRITE,
                  STPM_CMD_NO_WRITE, STPM_CMD_NO_WRITE);
    delayMicroseconds(10); // let CS settle fully high before the read frame

    // 3. Clock out the 4 data bytes (LSB first).
    uint8_t b[4];
    SPI.beginTransaction(STPM_SPI_SETTINGS);
    digitalWrite(csPin, LOW);
    for (uint8_t i = 0; i < 4; i++)
    {
        b[i] = SPI.transfer(0xFF);
    }
    digitalWrite(csPin, HIGH);
    SPI.endTransaction();

    // Assemble: first byte received is the least significant.
    return ((uint32_t)b[0]) |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

// ===========================================================================
// Initialization
// ===========================================================================

// Run the STPM3x global startup reset (datasheet section 8.2.4, Figure 29):
//   - drive EN low then high to power-cycle the digital domain,
//   - issue three SYN pulses for a full global reset (config included),
//   - a single CS pulse to complete the documented reset handshake.
static void stpmGlobalReset(uint8_t csPin, uint8_t synPin, uint8_t enPin)
{
    // CS low during reset selects SPI mode at startup; SYN idles high.
    digitalWrite(csPin, LOW);
    digitalWrite(synPin, HIGH);

    // Power-cycle the digital section via EN.
    digitalWrite(enPin, LOW);
    delay(50);
    digitalWrite(enPin, HIGH);
    delay(50);

    // Return CS to its idle (high) state.
    digitalWrite(csPin, HIGH);
    delay(10);

    // Three SYN pulses = global reset (1 pulse latches, 2 resets measurement,
    // 3 resets configuration too). See datasheet section 8.6.1.
    for (uint8_t i = 0; i < 3; i++)
    {
        digitalWrite(synPin, LOW);
        delayMicroseconds(4);
        digitalWrite(synPin, HIGH);
        delayMicroseconds(4);
    }

    // Single CS pulse to finish the documented startup handshake.
    digitalWrite(csPin, LOW);
    delayMicroseconds(4);
    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);
}

bool STPM_WattTF::begin(const StpmVoltageConfig &vConfig,
                        const StpmCurrentConfig &iConfig)
{
    // No raw overrides, default constants.
    return begin(vConfig, iConfig, StpmRawLSB(), StpmConstants());
}

bool STPM_WattTF::begin(const StpmVoltageConfig &vConfig,
                        const StpmCurrentConfig &iConfig,
                        const StpmRawLSB &rawLSB,
                        const StpmConstants &constants)
{
    _k = constants;

    // ----- Pin directions -------------------------------------------------
    pinMode(_cs, OUTPUT);
    pinMode(_syn, OUTPUT);
    pinMode(_en, OUTPUT);

    // ----- Hardware reset + global reset ----------------------------------
    stpmGlobalReset(_cs, _syn, _en);

    // ----- Configure the chip ---------------------------------------------
    // 1. Set the current-channel gain in DFE_CR1 (row 12, upper half -> write
    //    address 0x19). The other default bits of the upper half (0x0F27) are
    //    preserved; only the GAIN1 field [27:26] is changed.
    {
        const uint16_t upperDefault = 0x0F27;
        uint16_t upper = (upperDefault & ~(0x3 << 10)) | ((uint16_t)iConfig.gain << 10);
        stpmSendFrame(_cs, 0x19, 0x19,
                      (uint8_t)(upper & 0xFF),
                      (uint8_t)(upper >> 8));
    }

    // 2. Disable CRC for v1 (US_REG1 lower half -> write address 0x24).
    //    Frame 24_24_07_00 keeps the default polynomial 0x07 and clears the
    //    CRC-enable bit. (Datasheet Table 28.)
    stpmSendFrame(_cs, STPM_REG_US_REG1, STPM_REG_US_REG1, 0x07, 0x00);

    // ----- Compute (or accept overridden) LSB scaling factors -------------
    // Voltage RMS LSB (datasheet Table 14):
    //   LSB_V = Vref * (1 + R1/R2) / (calV * AV * 2^15)
    // where R1 = total series resistance (line + neutral legs), R2 = sense.
    const double R1 = (double)vConfig.seriesLine + (double)vConfig.seriesNeutral;
    const double R2 = (double)vConfig.senseR2;
    const double dividerTerm = 1.0 + (R1 / R2);

    // Current sensitivity for a CT (datasheet Table 13): kS = Rb / N  [V/A]
    const double kS = (double)iConfig.burdenOhms / (double)iConfig.turnsRatio;

    // Current gain AI as a number.
    double AI = 2.0;
    switch (iConfig.gain)
    {
    case StpmGain::X2:
        AI = 2.0;
        break;
    case StpmGain::X4:
        AI = 4.0;
        break;
    case StpmGain::X8:
        AI = 8.0;
        break;
    case StpmGain::X16:
        AI = 16.0;
        break;
    }

    // Computed values (datasheet Tables 13 and 14).
    const double compV = _k.vref * dividerTerm / (_k.calV * _k.avGain * 32768.0);                                                                 // 2^15
    const double compI = _k.vref / (_k.calI * AI * kS * _k.kint * 131072.0);                                                                      // 2^17
    const double compP = (_k.vref * _k.vref) * dividerTerm / (_k.kint * _k.avGain * AI * kS * _k.calV * _k.calI * 268435456.0);                   // 2^28
    const double compE = (_k.vref * _k.vref) * dividerTerm / (3600.0 * _k.dclkHz * _k.kint * _k.avGain * AI * kS * _k.calV * _k.calI * 131072.0); // 2^17

    // Apply overrides where provided (negative field => "compute it").
    _lsbV = (rawLSB.voltageLSB >= 0.0) ? rawLSB.voltageLSB : compV;
    _lsbI = (rawLSB.currentLSB >= 0.0) ? rawLSB.currentLSB : compI;
    _lsbP = (rawLSB.powerLSB >= 0.0) ? rawLSB.powerLSB : compP;
    _lsbE = (rawLSB.energyLSB >= 0.0) ? rawLSB.energyLSB : compE;

    return true;
}

// ===========================================================================
// RMS measurements
//
// Register STPM_REG_V1_C1_RMS (0x48) packs both RMS values into one 32-bit
// word (datasheet row 36):
//   - bits [14:0]  : voltage RMS (15-bit unsigned)
//   - bits [31:15] : current RMS (17-bit unsigned)
// Each raw count is scaled by its computed LSB to give engineering units.
// ===========================================================================

float STPM_WattTF::readVoltageRMS()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_V1_C1_RMS);
    uint32_t rawV = raw & 0x7FFF; // lower 15 bits
    return (float)(rawV * _lsbV);
}

float STPM_WattTF::readCurrentRMS()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_V1_C1_RMS);
    uint32_t rawI = (raw >> 15) & 0x1FFFF; // upper 17 bits
    return (float)(rawI * _lsbI);
}

// ===========================================================================
// Power measurements
//
// Power registers are 29-bit SIGNED values (datasheet section 8.4.1 and
// Table 13, "Power register normalized"): bit 28 is the sign bit, bits [27:0]
// are the magnitude. A negative result means power flowing back to the grid
// (export / regeneration), per datasheet section 8.4.5.
//
// We read the 32-bit register, keep the low 29 bits, and sign-extend from
// bit 28 so the value carries its correct sign before scaling by the power
// LSB. This is more correct than taking the absolute value: it preserves
// direction, which matters for solar/export and bidirectional metering.
// ===========================================================================

// Extract a signed 29-bit power value from a raw 32-bit register read.
static int32_t stpmSignExtend29(uint32_t raw)
{
    raw &= 0x1FFFFFFF; // keep bits [28:0]
    if (raw & 0x10000000)
    { // bit 28 set -> negative
        // Subtract 2^29 to sign-extend into a normal signed int32.
        return (int32_t)raw - 0x20000000;
    }
    return (int32_t)raw;
}

float STPM_WattTF::readActivePower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_ACTIVE_POWER);
    int32_t v = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readFundamentalPower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_FUND_POWER);
    int32_t v = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readReactivePower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_REACTIVE_POWER);
    int32_t v = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readApparentRMSPower()
{
    // Apparent power is a magnitude (Vrms * Irms), so it is non-negative by
    // definition; we still read it through the same 29-bit field for
    // consistency. (Datasheet section 8.4.4.)
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_APPARENT_RMS_POWER);
    int32_t v = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readApparentVectPower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_APPARENT_VECT_POWER);
    int32_t v = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

// ===========================================================================
// Power factor
//
// Two definitions, both from the datasheet (section 8.4.4):
//   - True power factor (TPF)         = active power / apparent RMS power
//                                       (includes harmonics; the "real" PF)
//   - Displacement power factor (DPF) = fundamental power / apparent
//                                       vectorial power (fundamental only)
//
// These are computed from the already-scaled power readings, so they carry
// the sign of the active power (a negative PF indicates export). Because the
// LSB cancels in the ratio, the result is independent of calibration scaling.
//
// Guard against divide-by-zero at no load: if the denominator is ~0, return
// 0.0 rather than producing inf/NaN.
// ===========================================================================

float STPM_WattTF::readTruePowerFactor()
{
    float p = readActivePower();
    float s = readApparentRMSPower();
    if (fabsf(s) < 1e-6f)
    {
        return 0.0f;
    }
    return p / s;
}

float STPM_WattTF::readDisplacementPowerFactor()
{
    float pf = readFundamentalPower();
    float sv = readApparentVectPower();
    if (fabsf(sv) < 1e-6f)
    {
        return 0.0f;
    }
    return pf / sv;
}

// ===========================================================================
// Calibration  (datasheet section 9.2.1, Tables 36 & 37)
//
// Single-point calibration corrects this specific board's component
// tolerances and Vref drift. You apply a KNOWN reference voltage/current
// (from a calibrated source or reference meter), tell the library the true
// value, and it computes and writes the chip's calibrator register so that
// future readings are correct.
//
// The calibrator registers (CHV1, CHC1) default to 0x800, which corresponds
// to the calV = calI = 0.875 already used in the LSB formulas. Calibration
// adjusts around that point within +-12.5%. Because the chip applies the
// correction internally before we read the register, our LSB scaling does
// NOT change after calibration -- it stays transparent.
//
// Persistence is intentionally NOT handled here: the library writes the
// calibrator to the chip and exposes get/set of the raw 12-bit value so the
// application can store it (e.g. in NVS) and restore it on boot.
// ===========================================================================

// Read the raw (unscaled) RMS counts from register 0x48, averaged over
// 'samples' reads to reduce noise. Voltage is the low 15 bits, current the
// upper 17 bits.
void STPM_WattTF::readRawRMS(uint16_t samples, uint32_t &vAvg, uint32_t &iAvg)
{
    if (samples == 0)
        samples = 1;

    uint64_t vSum = 0;
    uint64_t iSum = 0;
    for (uint16_t n = 0; n < samples; n++)
    {
        uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_V1_C1_RMS);
        vSum += (raw & 0x7FFF);
        iSum += ((raw >> 15) & 0x1FFFF);
        delay(2); // a few ms between samples; RMS updates every 200 ms,
                  // so this mainly de-correlates SPI noise, not the value
    }
    vAvg = (uint32_t)(vSum / samples);
    iAvg = (uint32_t)(iSum / samples);
}

// Write a 12-bit calibrator value into a calibration register's low 12 bits,
// preserving the other bits already in that register (which hold swell/sag
// thresholds we are not touching). Reads the row first, replaces [11:0].
void STPM_WattTF::writeCalibrator(uint8_t readAddr, uint16_t cal12)
{
    cal12 &= 0x0FFF;

    // Read current 32-bit row contents so we preserve the upper bits.
    uint32_t row = stpmReadRegister32(_cs, _syn, readAddr);
    row = (row & ~0x00000FFFUL) | cal12;

    // The calibrator is in the LOWER 16 bits of the row, so write the lower
    // half (write address == read address). Send the new lower 16 bits.
    uint16_t lower = (uint16_t)(row & 0xFFFF);
    stpmSendFrame(_cs, readAddr, readAddr,
                  (uint8_t)(lower & 0xFF),
                  (uint8_t)(lower >> 8));
}

bool STPM_WattTF::calibrateVoltage(float trueVoltage, uint16_t samples)
{
    if (trueVoltage <= 0.0f)
        return false;

    // Average the raw voltage count the chip currently reports.
    uint32_t vAvg, iAvg;
    readRawRMS(samples, vAvg, iAvg);
    if (vAvg == 0)
        return false;

    // Target raw count for this true voltage (datasheet Table 36):
    //   XV = Vn * AV * calV * 2^15 / (Vref * (1 + R1/R2))
    // We reconstruct (1 + R1/R2) from the stored voltage LSB instead of the
    // raw resistors, since vLSB already encodes it:
    //   vLSB = Vref*(1+R1/R2)/(calV*AV*2^15)  =>  XV = trueVoltage / vLSB
    double XV = (double)trueVoltage / _lsbV;

    // Calibrator (datasheet Table 37):  CHV = 14336 * (XV/VAV) - 12288
    double chv = 14336.0 * (XV / (double)vAvg) - 12288.0;

    // Clamp to the 12-bit register range.
    if (chv < 0.0)
        chv = 0.0;
    if (chv > 4095.0)
        chv = 4095.0;

    _calV = (uint16_t)(chv + 0.5);
    writeCalibrator(STPM_REG_DSP_CR5, _calV); // CHV1 lives in DSP_CR5
    return true;
}

bool STPM_WattTF::calibrateCurrent(float trueCurrent, uint16_t samples)
{
    if (trueCurrent <= 0.0f)
        return false;

    uint32_t vAvg, iAvg;
    readRawRMS(samples, vAvg, iAvg);
    if (iAvg == 0)
        return false;

    // Target raw count (datasheet Table 36):  XI = trueCurrent / iLSB
    // (same reasoning as voltage: iLSB already encodes AI, kS, kint, etc.)
    double XI = (double)trueCurrent / _lsbI;

    double chc = 14336.0 * (XI / (double)iAvg) - 12288.0;
    if (chc < 0.0)
        chc = 0.0;
    if (chc > 4095.0)
        chc = 4095.0;

    _calI = (uint16_t)(chc + 0.5);
    writeCalibrator(STPM_REG_DSP_CR6, _calI); // CHC1 lives in DSP_CR6
    return true;
}

// ----- Raw calibrator get/set (for persistence by the application) --------

uint16_t STPM_WattTF::getVoltageCalibrator() const { return _calV; }
uint16_t STPM_WattTF::getCurrentCalibrator() const { return _calI; }

void STPM_WattTF::setVoltageCalibrator(uint16_t cal12)
{
    _calV = cal12 & 0x0FFF;
    writeCalibrator(STPM_REG_DSP_CR5, _calV);
}

void STPM_WattTF::setCurrentCalibrator(uint16_t cal12)
{
    _calI = cal12 & 0x0FFF;
    writeCalibrator(STPM_REG_DSP_CR6, _calI);
}

// ===========================================================================
// Energy accumulation
//
// The chip's energy registers are 32-bit UP/DOWN counters (datasheet 8.4):
// they count up for imported energy and roll 0xFFFFFFFF -> 0x00000000, and
// count down for exported energy, rolling 0x00000000 -> 0xFFFFFFFF. A raw read
// is therefore a sawtooth, not a usable total.
//
// To get a real total we sample the register periodically, compute the signed
// DELTA since the previous sample (correctly handling wrap in either
// direction), and accumulate those deltas into a 64-bit software total that
// effectively never overflows. The sign is preserved, so a negative total
// means net energy exported to the grid.
//
// USAGE: call updateEnergy() regularly (e.g. once per second, or every loop).
// The register wraps slowly at realistic power, so any cadence of a few
// seconds is safe -- but you must call it often enough that the counter does
// not wrap MORE than once between calls. The readXxxEnergy() methods are cheap
// (no SPI) and just return the scaled accumulated total, so call them freely.
// ===========================================================================

// Compute the signed delta between two unsigned 32-bit counter samples,
// correctly accounting for wraparound in both directions. (Idiomatic counter-
// difference: subtract in unsigned space, then reinterpret as signed.)
static int32_t stpmCounterDelta(uint32_t prev, uint32_t curr)
{
    uint32_t d = curr - prev; // wraps modulo 2^32 by C rules
    return (int32_t)d;        // reinterpret as signed delta
}

// Update one accumulator from its register. On the first call it only seeds
// prevRaw (no delta added), matching the reference behavior and avoiding a
// bogus initial jump.
void STPM_WattTF::updateOneEnergy(uint8_t readAddr, StpmEnergyAccumulator &acc)
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, readAddr);

    if (acc.firstRead)
    {
        acc.prevRaw = raw;
        acc.firstRead = false;
        return;
    }

    acc.total64 += (int64_t)stpmCounterDelta(acc.prevRaw, raw);
    acc.prevRaw = raw;
}

// Sample all four energy registers and accumulate. Call this regularly.
void STPM_WattTF::updateEnergy()
{
    updateOneEnergy(STPM_REG_TOT_ACTIVE_ENERGY, _eActive);
    updateOneEnergy(STPM_REG_TOT_FUND_ENERGY, _eFund);
    updateOneEnergy(STPM_REG_TOT_REACTIVE_ENERGY, _eReactive);
    updateOneEnergy(STPM_REG_TOT_APPARENT_ENERGY, _eApparent);
}

// ----- Scaled totals (cheap; no SPI) ----------------------------------------
// Returned in watt-hours (Wh / varh / VAh). Sign preserved: negative = export.

double STPM_WattTF::readActiveEnergy() { return (double)_eActive.total64 * _lsbE; }
double STPM_WattTF::readFundamentalEnergy() { return (double)_eFund.total64 * _lsbE; }
double STPM_WattTF::readReactiveEnergy() { return (double)_eReactive.total64 * _lsbE; }
double STPM_WattTF::readApparentEnergy() { return (double)_eApparent.total64 * _lsbE; }

// ----- Raw accumulator get/set (for persistence by the application) ---------
// Get/set the underlying int64 totals so the application can save them (e.g.
// to NVS/EEPROM) and restore them on boot. After a set, the next updateEnergy()
// re-seeds prevRaw cleanly (firstRead forced true), so restoring a saved total
// does not produce a bogus delta against a stale pre-reboot register value.

int64_t STPM_WattTF::getActiveEnergyRaw() const { return _eActive.total64; }
int64_t STPM_WattTF::getFundamentalEnergyRaw() const { return _eFund.total64; }
int64_t STPM_WattTF::getReactiveEnergyRaw() const { return _eReactive.total64; }
int64_t STPM_WattTF::getApparentEnergyRaw() const { return _eApparent.total64; }

void STPM_WattTF::setActiveEnergyRaw(int64_t v)
{
    _eActive.total64 = v;
    _eActive.firstRead = true;
}
void STPM_WattTF::setFundamentalEnergyRaw(int64_t v)
{
    _eFund.total64 = v;
    _eFund.firstRead = true;
}
void STPM_WattTF::setReactiveEnergyRaw(int64_t v)
{
    _eReactive.total64 = v;
    _eReactive.firstRead = true;
}
void STPM_WattTF::setApparentEnergyRaw(int64_t v)
{
    _eApparent.total64 = v;
    _eApparent.firstRead = true;
}
