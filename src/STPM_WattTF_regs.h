/*
 * STPM_WattTF_regs.h
 *
 * Register map for the STMicroelectronics STPM3x energy metering family
 * (STPM32 / STPM33 / STPM34). Addresses are taken from datasheet DS10272
 * Rev 14, register map Figures 54-58 and the DSP_REGx data-register tables.
 *
 * This header is intentionally device-agnostic: it lists addresses for the
 * shared register map only. Which registers actually carry data on a given
 * part (e.g. secondary-channel rows on the STPM33/34) is handled higher up,
 * not here.
 *
 * Addressing rule (datasheet section 8.6, "Communication protocol"):
 *   - The internal memory is organized as 32-bit ROWS.
 *   - Each row is READ at a read-address equal to (row_number * 2).
 *   - Each row is WRITTEN 16 bits at a time, so the lower half and the upper
 *     half of a row have separate write addresses.
 *
 * Part of the STPM_WattTF library. MIT licensed.
 */

#ifndef STPM_WATTTF_REGS_H
#define STPM_WATTTF_REGS_H

#include <stdint.h>

// ===========================================================================
// Configuration registers (read/write)
//
// These hold the chip's settings. On reset they take the default values
// noted in the datasheet. We only name the ones the library needs for v1;
// more will be added as features (calibration, sag/swell, etc.) are built.
// ===========================================================================

// Row 0  - DSP control register 1. Holds, among other things, the Rogowski
//          integrator enable (ROC1) and HPF bypass bits for the primary
//          channel. Default 0x040000A0.
static const uint8_t STPM_REG_DSP_CR1 = 0x00;

// Row 2  - DSP control register 3. Holds latch / reset software bits and the
//          reference-frequency (50/60 Hz) selection. Default 0x000004E0.
static const uint8_t STPM_REG_DSP_CR3 = 0x04;

// Row 4  - DSP control register 5 (DSP_CR5). Low 12 bits hold CHV1, the
//          primary VOLTAGE calibration register. Default 0x800 (mid-scale).
static const uint8_t STPM_REG_DSP_CR5 = 0x08;

// Row 5  - DSP control register 6 (DSP_CR6). Low 12 bits hold CHC1, the
//          primary CURRENT calibration register. Default 0x800 (mid-scale).
static const uint8_t STPM_REG_DSP_CR6 = 0x0A;

// Row 12 - Digital front-end control register 1 (DFE_CR1). Holds the primary
//          current-channel gain selection GAIN1[1:0] in bits [27:26].
//          Default 0x0F270327 (gain field defaults to 0x3 = x16).
static const uint8_t STPM_REG_DFE_CR1 = 0x18;

// Row 18 - UART/SPI control register 1 (US_REG1). Holds the CRC-enable bit
//          (bit 14, default 1) and the CRC polynomial (default 0x07).
//          We will use this to DISABLE CRC at init in v1.
static const uint8_t STPM_REG_US_REG1 = 0x24;

// ===========================================================================
// Measurement registers (read-only)
//
// Continuously updated by the chip's DSP. They must be latched before
// reading (via a SYN pulse or a software-latch bit) to get a coherent
// snapshot. Read address shown; equals (datasheet row number * 2).
// ===========================================================================

// Row 36 - Packed RMS: primary current RMS in the upper 17 bits [16:0 of the
//          C field] and primary voltage RMS in the lower 15 bits.
//          (Datasheet: C1 RMS Data[16:0] + V1 RMS Data[14:0].)
static const uint8_t STPM_REG_V1_C1_RMS = 0x48;

// Row 46 - Primary channel active (wideband) power.
static const uint8_t STPM_REG_PH1_ACTIVE_POWER = 0x5C;

// Row 47 - Primary channel fundamental active power.
static const uint8_t STPM_REG_PH1_FUND_POWER = 0x5E;

// Row 48 - Primary channel reactive power.
static const uint8_t STPM_REG_PH1_REACTIVE_POWER = 0x60;

// Row 49 - Primary channel apparent RMS power (Vrms * Irms).
static const uint8_t STPM_REG_PH1_APPARENT_RMS_POWER = 0x62;

// Row 50 - Primary channel apparent vectorial power (sqrt(P^2 + Q^2)).
static const uint8_t STPM_REG_PH1_APPARENT_VECT_POWER = 0x64;

// Row 66 - Total active energy (== primary active energy on a 1-channel part).
static const uint8_t STPM_REG_TOT_ACTIVE_ENERGY = 0x84;

// Row 67 - Total fundamental energy.
static const uint8_t STPM_REG_TOT_FUND_ENERGY = 0x86;

// Row 68 - Total reactive energy.
static const uint8_t STPM_REG_TOT_REACTIVE_ENERGY = 0x88;

// Row 69 - Total apparent energy.
static const uint8_t STPM_REG_TOT_APPARENT_ENERGY = 0x8A;

// ===========================================================================
// Special command codes (datasheet section 8.6)
// ===========================================================================

// Dummy read address: increments the chip's internal read pointer by one.
static const uint8_t STPM_CMD_DUMMY_READ = 0xFF;

// Dummy write address: tells the chip "no write requested this frame".
static const uint8_t STPM_CMD_NO_WRITE = 0xFF;

#endif // STPM_WATTTF_REGS_H