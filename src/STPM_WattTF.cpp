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