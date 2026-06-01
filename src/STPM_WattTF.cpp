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
    delayMicroseconds(4);     // >= t_lpw latch pulse width (datasheet Table 4)
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
    delayMicroseconds(10);  // let CS settle fully high before the read frame

    // 3. Clock out the 4 data bytes (LSB first).
    uint8_t b[4];
    SPI.beginTransaction(STPM_SPI_SETTINGS);
    digitalWrite(csPin, LOW);
    for (uint8_t i = 0; i < 4; i++) {
        b[i] = SPI.transfer(0xFF);
    }
    digitalWrite(csPin, HIGH);
    SPI.endTransaction();

    // Assemble: first byte received is the least significant.
    return ((uint32_t)b[0])        |
           ((uint32_t)b[1] << 8)   |
           ((uint32_t)b[2] << 16)  |
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
    for (uint8_t i = 0; i < 3; i++) {
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

bool STPM_WattTF::begin(const StpmVoltageConfig& vConfig,
                        const StpmCurrentConfig& iConfig)
{
    // No raw overrides, default constants.
    return begin(vConfig, iConfig, StpmRawLSB(), StpmConstants());
}

bool STPM_WattTF::begin(const StpmVoltageConfig& vConfig,
                        const StpmCurrentConfig& iConfig,
                        const StpmRawLSB& rawLSB,
                        const StpmConstants& constants)
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
        uint16_t upper = (upperDefault & ~(0x3 << 10))
                       | ((uint16_t)iConfig.gain << 10);
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
    switch (iConfig.gain) {
        case StpmGain::X2:  AI = 2.0;  break;
        case StpmGain::X4:  AI = 4.0;  break;
        case StpmGain::X8:  AI = 8.0;  break;
        case StpmGain::X16: AI = 16.0; break;
    }

    // Computed values (datasheet Tables 13 and 14).
    const double compV = _k.vref * dividerTerm / (_k.calV * _k.avGain * 32768.0);          // 2^15
    const double compI = _k.vref / (_k.calI * AI * kS * _k.kint * 131072.0);               // 2^17
    const double compP = (_k.vref * _k.vref) * dividerTerm
                       / (_k.kint * _k.avGain * AI * kS * _k.calV * _k.calI * 268435456.0); // 2^28
    const double compE = (_k.vref * _k.vref) * dividerTerm
                       / (3600.0 * _k.dclkHz * _k.kint * _k.avGain * AI * kS
                          * _k.calV * _k.calI * 131072.0);                                  // 2^17

    // Apply overrides where provided (negative field => "compute it").
    _lsbV = (rawLSB.voltageLSB >= 0.0) ? rawLSB.voltageLSB : compV;
    _lsbI = (rawLSB.currentLSB >= 0.0) ? rawLSB.currentLSB : compI;
    _lsbP = (rawLSB.powerLSB   >= 0.0) ? rawLSB.powerLSB   : compP;
    _lsbE = (rawLSB.energyLSB  >= 0.0) ? rawLSB.energyLSB  : compE;

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
    uint32_t rawV = raw & 0x7FFF;            // lower 15 bits
    return (float)(rawV * _lsbV);
}

float STPM_WattTF::readCurrentRMS()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_V1_C1_RMS);
    uint32_t rawI = (raw >> 15) & 0x1FFFF;   // upper 17 bits
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
    raw &= 0x1FFFFFFF;             // keep bits [28:0]
    if (raw & 0x10000000) {        // bit 28 set -> negative
        // Subtract 2^29 to sign-extend into a normal signed int32.
        return (int32_t)raw - 0x20000000;
    }
    return (int32_t)raw;
}

float STPM_WattTF::readActivePower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_ACTIVE_POWER);
    int32_t  v   = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readFundamentalPower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_FUND_POWER);
    int32_t  v   = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readReactivePower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_REACTIVE_POWER);
    int32_t  v   = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readApparentRMSPower()
{
    // Apparent power is a magnitude (Vrms * Irms), so it is non-negative by
    // definition; we still read it through the same 29-bit field for
    // consistency. (Datasheet section 8.4.4.)
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_APPARENT_RMS_POWER);
    int32_t  v   = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}

float STPM_WattTF::readApparentVectPower()
{
    uint32_t raw = stpmReadRegister32(_cs, _syn, STPM_REG_PH1_APPARENT_VECT_POWER);
    int32_t  v   = stpmSignExtend29(raw);
    return (float)(v * _lsbP);
}