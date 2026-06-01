<div align="center">

# STPM_WattTF

<img src="https://raw.githubusercontent.com/TheChipMaker/STPM_WattTF/main/docs/stpm32.png" alt="STPM32 energy metering IC" width="300">

[![PlatformIO Registry](https://badges.registry.platformio.org/packages/thechipmaker/library/STPM_WattTF.svg)](https://registry.platformio.org/libraries/thechipmaker/STPM_WattTF)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Arduino](https://img.shields.io/badge/Arduino-compatible-00979D?logo=arduino&logoColor=white)](https://www.arduino.cc/)
[![ESP32](https://img.shields.io/badge/ESP32-tested-E7352C?logo=espressif&logoColor=white)](https://www.espressif.com/)
![Version](https://img.shields.io/badge/version-0.2.1-blue)

</div>

An open-source **Arduino / PlatformIO** library for the STMicroelectronics
**STPM3x** family of high-accuracy AC energy-metering ICs, communicating over
SPI.

It reads RMS voltage and current, active / fundamental / reactive / apparent
power, true and displacement power factor, and accumulates all four energy
totals. Scaling factors are computed from *your* board's hardware (voltage
divider, CT ratio, gain) — or supplied directly — and the chip's single-point
calibration is supported for high accuracy.

> **Status — v0.2.1 (early but functional).**
> Target in this release: the single-channel **STPM32** with a
> **current-transformer (CT)** sensor, on **ESP32**. The code is structured so
> that shunt / Rogowski sensors and the dual-channel STPM33/34 can be added
> without a rewrite. See the [Roadmap](#roadmap).

---

## Why this library

The STPM32 is powerful but its register interface is fiddly: byte-swapped SPI
frames, a two-step read protocol, packed bit fields, rolling 32-bit energy
counters, and scaling constants that depend on your exact analog front-end.
This library hides that and gives you plain engineering units:

```cpp
float volts  = meter.readVoltageRMS();   // e.g. 236.5
float amps   = meter.readCurrentRMS();   // e.g. 0.080
float watts  = meter.readActivePower();  // signed: negative = export
double wh    = meter.readActiveEnergy(); // accumulated, wraparound-safe
```

No external metering library is required — the library configures the chip
itself (gain, etc.) over SPI.

## Features

- **RMS voltage and current**
- **Power** — active, fundamental, reactive, apparent (all **signed**; negative
  active power means energy exported to the grid)
- **Power factor** — true (with harmonics) and displacement (fundamental only)
- **Energy** — active, fundamental, reactive, apparent, accumulated into 64-bit
  software totals that are immune to the chip's 32-bit counter rollover
- **Hardware-derived scaling** — give the library your resistor divider, CT
  ratio, burden, and gain, and it computes the LSB scaling for you; or pass raw
  LSB values directly if you calibrated empirically
- **Single-point calibration** — correct component tolerances and reference
  drift using a known voltage/current, reaching the chip's class-0.2 accuracy
- **Portable** — pure Arduino framework, no platform-specific storage baked in;
  persistence of calibration and energy totals is left to your application

## Compatibility

| Item            | Supported in v0.2.1           | Planned                     |
|-----------------|-------------------------------|-----------------------------|
| Chip            | STPM32 (single channel)       | STPM33 / STPM34 (2 channels)|
| Current sensor  | Current transformer (CT)      | Shunt, Rogowski coil        |
| Interface       | SPI                           | —                           |
| Tested MCU      | ESP32 (ESP32-WROOM)           | AVR, STM32, RP2040 (likely OK) |
| CRC on SPI      | Disabled at init              | Optional verification       |

> The library uses only the Arduino `SPI` API and standard types, so it should
> work on any Arduino-framework board. ESP32 is what it has been tested on.
> On 8-bit AVR, note that `double` is 32-bit, which slightly reduces *energy*
> precision (the internal accumulator stays exact; only the final Wh conversion
> is affected).

---

## Installation

### PlatformIO

From the PlatformIO Registry (add to your `platformio.ini`):

```ini
lib_deps =
    thechipmaker/STPM_WattTF@^0.2.1
```

Or install the latest development version straight from GitHub:

```ini
lib_deps =
    https://github.com/TheChipMaker/STPM_WattTF.git
```

Or, while developing against a local clone:

```ini
lib_deps =
    file://path/to/STPM_WattTF
```

The `owner/name@^0.2.1` form pulls the published release and automatically
accepts compatible newer versions (0.2.x and later 0.x). Pin an exact version
(`@0.2.1`) if you prefer to lock it.

### Arduino IDE

1. Download this repository as a ZIP (green **Code** button → **Download ZIP**).
2. In the IDE: **Sketch → Include Library → Add .ZIP Library…** and select it.
3. Examples appear under **File → Examples → STPM_WattTF**.

### Dependencies

None beyond the Arduino core (`Arduino.h`, `SPI.h`). No external metering
library is required.

---

## Wiring

The STPM3x is an SPI slave. Connect its SPI pins to your MCU's hardware SPI,
and the three control pins (chip-select, sync, enable) to any free GPIOs.

| STPM3x pin | Connect to          | Notes                                 |
|------------|---------------------|---------------------------------------|
| SCS        | GPIO (chip-select)  | Passed to the constructor as `csPin`  |
| SYN        | GPIO (sync)         | Passed as `synPin`; used for latching |
| EN         | GPIO (enable/reset) | Passed as `enPin`                     |
| SCL        | SPI SCK             | Hardware SPI clock                    |
| MOSI       | SPI MOSI            | Master out, slave in                  |
| MISO       | SPI MISO            | Master in, slave out                  |
| VCC, GND   | 3.3 V, GND          | Decouple per the datasheet            |

The SPI bus runs at **1 MHz, mode 3 (CPOL = 1, CPHA = 1)** — the library
configures this itself. If your design isolates the SPI bus through digital
isolators (common in mains-connected meters), 1 MHz is well within their
bandwidth.

> **⚠ Safety.** The STPM3x sits on the mains side of your design, sensing line
> voltage through a high-value resistor divider and current through a CT. Treat
> the entire analog front-end as live. Galvanic isolation (digital isolators on
> the SPI bus, an isolated supply) between the metering section and your
> MCU/USB side is strongly recommended and is assumed by the example wiring.

### Example pin mapping (used in the examples)

```cpp
const uint8_t CS_PIN  = 25;
const uint8_t SYN_PIN = 17;
const uint8_t EN_PIN  = 4;
// SCL / MOSI / MISO use the board's default hardware-SPI pins via SPI.begin().
```

---

## Hardware configuration

The STPM3x reports raw integer counts. Converting those to volts, amps, watts
and watt-hours requires scaling factors (LSB values) that depend on your
board's analog front-end: the voltage divider, the current sensor, and the
gain. This library computes those factors for you from the component values
you provide — so the same code gives correct readings on any board, as long as
you describe that board's hardware.

![STPM32 analog front-end schematic](https://raw.githubusercontent.com/TheChipMaker/STPM_WattTF/main/docs/analog-frontend.png)

There are three things to configure: the **voltage divider**, the **current
sensor (CT)**, and the **gain**. You can also bypass the computation entirely
and supply **raw LSB values** if you calibrated empirically.

### Voltage divider

Mains voltage is far too high to feed the chip directly, so it is attenuated by
a resistor divider. The chip measures the differential voltage across a small
**sense resistor (R2)** placed between its VIP and VIN inputs; large **series
resistors** drop almost all of the voltage before it.

This library accepts the series resistance **per leg**, which lets it handle
both common topologies with the same fields:

- **Single-ended** — series resistors only on the line side. Put that total in
  `seriesLine` and set `seriesNeutral = 0`.
- **Balanced / symmetric** — series resistors on *both* the line and neutral
  sides (as on the reference board). Put each leg's total in its own field.

```cpp
StpmVoltageConfig vcfg;
vcfg.seriesLine    = 3 * 133000.0f;   // 399 kΩ: three 133 kΩ in series (line)
vcfg.seriesNeutral = 3 * 133000.0f;   // 399 kΩ: three 133 kΩ in series (neutral)
vcfg.senseR2       = 470.0f;          // 470 Ω across VIP–VIN
```

Internally the library forms `R1 = seriesLine + seriesNeutral` and uses the
datasheet ratio `(1 + R1/R2)`. For the values above that is
`(1 + 798000/470) ≈ 1699`, matching the datasheet's reference design.

> **Why per-leg matters.** If you put resistors on both legs but only told the
> library about one, the computed voltage would be off by roughly 2×. Splitting
> the field by leg removes that trap — the neutral-side resistance is part of
> the total series resistance and must be counted.

### Current sensor (current transformer)

This release supports a **current transformer (CT)**. The CT steps the line
current down by its turns ratio **N**, and a **burden resistor (Rb)** across
the CT secondary converts that small current into a voltage the chip can read.
The library needs both:

```cpp
StpmCurrentConfig icfg;
icfg.turnsRatio = 2000.0f;     // a 2000:1 CT
icfg.burdenOhms = 10.0f;       // 10 Ω burden across the CT secondary
icfg.gain       = StpmGain::X2;
```

From these it computes the sensor sensitivity `kS = Rb / N` (V/A) as the
datasheet defines for a CT.

> The **burden resistor is the most accuracy-critical passive** in the current
> path: its tolerance passes almost directly into the current reading. Use a
> tight-tolerance part here, or calibrate it out (see
> [Calibration](#calibration)). The turns ratio N is set by the CT itself and
> is generally very accurate.

### Gain

The chip's current channel has a programmable preamplifier gain that sets the
full-scale current range. Higher gain = smaller full-scale = better resolution
for small currents, but it clips sooner. Pick the smallest gain whose
full-scale comfortably exceeds your maximum current.

| `StpmGain` value | Gain | Differential full-scale |
|------------------|------|-------------------------|
| `StpmGain::X2`   | ×2   | ±300 mV                 |
| `StpmGain::X4`   | ×4   | ±150 mV                 |
| `StpmGain::X8`   | ×8   | ±75 mV                  |
| `StpmGain::X16`  | ×16  | ±37.5 mV (chip default) |

The library writes your chosen gain to the chip during `begin()`, so you do not
need any other library to set it. (Note: the chip powers up at ×16; always set
the gain that matches your design.)

### Putting it together

```cpp
#include <SPI.h>
#include <STPM_WattTF.h>

STPM_WattTF meter(/*cs*/25, /*syn*/17, /*en*/4);

void setup() {
  SPI.begin();

  StpmVoltageConfig vcfg;
  vcfg.seriesLine    = 3 * 133000.0f;
  vcfg.seriesNeutral = 3 * 133000.0f;
  vcfg.senseR2       = 470.0f;

  StpmCurrentConfig icfg;
  icfg.turnsRatio = 2000.0f;
  icfg.burdenOhms = 10.0f;
  icfg.gain       = StpmGain::X2;

  meter.begin(vcfg, icfg);
}
```

### Advanced: raw LSB override

If you have determined your scaling factors empirically (e.g. by comparing
against a reference meter) and prefer to use them directly, supply an
`StpmRawLSB`. Any field left negative is computed from the hardware configs as
usual; any non-negative field overrides the computation.

```cpp
StpmRawLSB raw;
raw.voltageLSB = 0.0349586593;   // V per LSB
raw.currentLSB = 0.0010288783;   // A per LSB
// powerLSB / energyLSB left at -1 => computed from hardware

meter.begin(vcfg, icfg, raw);
```

### Advanced: electrical constants

The device constants (reference voltage, calibrators, decimation rate, fixed
voltage gain, integrator gain) default to the datasheet values for a CT meter
and rarely need changing. To override — for example, a measured V_ref — pass an
`StpmConstants`:

```cpp
StpmConstants k;
k.vref = 1.181;                  // measured reference voltage
meter.begin(vcfg, icfg, StpmRawLSB(), k);
```

---

## Quick start

A complete minimal sketch — initialize the meter, then read and print values
once a second. This is the `BasicReadings` example (**File → Examples →
STPM_WattTF → BasicReadings**).

```cpp
#include 
#include 

// Control pins (match your wiring)
STPM_WattTF meter(/*cs*/25, /*syn*/17, /*en*/4);

void setup() {
  Serial.begin(115200);
  SPI.begin();

  // Describe your board's front-end (see "Hardware configuration")
  StpmVoltageConfig vcfg;
  vcfg.seriesLine    = 3 * 133000.0f;
  vcfg.seriesNeutral = 3 * 133000.0f;
  vcfg.senseR2       = 470.0f;

  StpmCurrentConfig icfg;
  icfg.turnsRatio = 2000.0f;
  icfg.burdenOhms = 10.0f;
  icfg.gain       = StpmGain::X2;

  meter.begin(vcfg, icfg);
}

void loop() {
  // Sample + accumulate the energy counters. Call this regularly so the
  // chip's 32-bit energy registers can't wrap unnoticed between reads.
  meter.updateEnergy();

  float  v = meter.readVoltageRMS();      // V
  float  i = meter.readCurrentRMS();      // A
  float  p = meter.readActivePower();     // W  (signed: negative = export)
  float pf = meter.readTruePowerFactor(); // unitless
  double e = meter.readActiveEnergy();    // Wh (accumulated)

  Serial.print("V=");   Serial.print(v, 2);
  Serial.print(" V  I="); Serial.print(i, 3);
  Serial.print(" A  P="); Serial.print(p, 1);
  Serial.print(" W  PF="); Serial.print(pf, 3);
  Serial.print("  E="); Serial.print(e, 4);
  Serial.println(" Wh");

  delay(1000);
}
```

Expected output under a small load:

```
V=236.50 V  I=0.080 A  P=13.3 W  PF=0.700  E=0.0037 Wh
V=236.57 V  I=0.080 A  P=13.4 W  PF=0.701  E=0.0074 Wh
V=236.50 V  I=0.080 A  P=13.3 W  PF=0.699  E=0.0111 Wh
```

### What to expect

- **The very first reading may be zero.** The chip needs one latch cycle before
  fresh data is available; it self-corrects on the next read.
- **Power can be negative.** A negative active power means current is flowing
  the opposite way to the chip's reference — either real export, or a CT
  mounted/wired in reverse. Flip the CT (or correct it in your own code) if you
  expected positive. The library reports the true sign rather than hiding it.
- **Energy only accumulates while you call `updateEnergy()`.** The
  `readXxxEnergy()` methods just return the running total; they do no I/O. If
  energy stays flat, make sure `updateEnergy()` is being called in your loop.
- **Idle current is not exactly zero.** With no load you will see a small
  residual current/power from noise and offsets. Real loads read correctly.

### Accuracy note

Out of the box, readings are scaled from your *nominal* component values, so
accuracy is limited by resistor and burden tolerances (typically ~0.1–1%). For
high accuracy, run the one-time [calibration](#calibration) against a known
reference — after which the chip reaches its class-0.2 specification.

---

## API reference

All methods are on the `STPM_WattTF` object. Angles/units are noted per method.

### Construction & initialization

```cpp
STPM_WattTF(uint8_t csPin, uint8_t synPin, uint8_t enPin);
```
Create a meter bound to the given control pins. Does no hardware access; call
`SPI.begin()` and then `begin()` from your `setup()`.

```cpp
bool begin(const StpmVoltageConfig& vConfig,
           const StpmCurrentConfig& iConfig);

bool begin(const StpmVoltageConfig& vConfig,
           const StpmCurrentConfig& iConfig,
           const StpmRawLSB& rawLSB,
           const StpmConstants& constants = StpmConstants());
```
Reset and configure the chip (gain, CRC off), and compute the LSB scaling from
the hardware configs. The second form adds raw-LSB overrides and/or custom
electrical constants. Returns `true` (init does not yet read back a
verification register — see [Roadmap](#roadmap)). Call once in `setup()`.

### Instantaneous measurements

| Method | Returns | Unit | Notes |
|--------|---------|------|-------|
| `readVoltageRMS()` | `float` | V | RMS line voltage |
| `readCurrentRMS()` | `float` | A | RMS line current |
| `readActivePower()` | `float` | W | Signed; negative = export |
| `readFundamentalPower()` | `float` | W | Fundamental (50/60 Hz) only |
| `readReactivePower()` | `float` | var | Signed |
| `readApparentRMSPower()` | `float` | VA | Vrms × Irms |
| `readApparentVectPower()` | `float` | VA | √(P² + Q²) |
| `readTruePowerFactor()` | `float` | — | P / S (includes harmonics); 0 at no load |
| `readDisplacementPowerFactor()` | `float` | — | fundamental / vectorial; 0 at no load |

Each instantaneous read performs its own SPI transaction (latch + read).

### Energy

```cpp
void   updateEnergy();             // sample + accumulate all four totals
double readActiveEnergy();         // Wh   (signed: negative = net export)
double readFundamentalEnergy();    // Wh
double readReactiveEnergy();       // varh
double readApparentEnergy();       // VAh
```
Call `updateEnergy()` regularly (every loop, or at least every few seconds) so
the chip's 32-bit counters cannot wrap unnoticed. The `readXxxEnergy()` methods
are cheap — they do no SPI and just return the scaled accumulated total.

### Calibration

```cpp
bool calibrateVoltage(float trueVoltage, uint16_t samples = 100);
bool calibrateCurrent(float trueCurrent, uint16_t samples = 100);
```
Apply a known reference, then call these. They average the chip's raw output
over `samples` reads, compute the calibrator, and write it to the chip.
Return `false` on invalid input (non-positive reference or zero reading).
Require a calibrated reference source/meter. See [Calibration](#calibration).

### Persistence (calibration & energy)

The library stores nothing across reboots — it exposes raw values so your
application can persist them (NVS, EEPROM, a file, …) and restore them on boot.

```cpp
// Calibration factors (raw 12-bit, 0x800 = uncalibrated default)
uint16_t getVoltageCalibrator() const;
uint16_t getCurrentCalibrator() const;
void     setVoltageCalibrator(uint16_t cal12);
void     setCurrentCalibrator(uint16_t cal12);

// Energy totals (raw int64 accumulators; exact, no rounding)
int64_t getActiveEnergyRaw() const;
int64_t getFundamentalEnergyRaw() const;
int64_t getReactiveEnergyRaw() const;
int64_t getApparentEnergyRaw() const;
void    setActiveEnergyRaw(int64_t v);
void    setFundamentalEnergyRaw(int64_t v);
void    setReactiveEnergyRaw(int64_t v);
void    setApparentEnergyRaw(int64_t v);
```
After a `set...`, the next `updateEnergy()` re-seeds cleanly, so restoring a
saved total never produces a bogus delta against a stale register value.

### Inspection / debugging

```cpp
double voltageLSB() const;   // V  per LSB
double currentLSB() const;   // A  per LSB
double powerLSB()   const;   // W  per LSB
double energyLSB()  const;   // Wh per LSB
```
The computed (or overridden) scaling factors, handy for verifying your
hardware config or debugging.

### Configuration types

```cpp
struct StpmVoltageConfig { float seriesLine, seriesNeutral, senseR2; };
struct StpmCurrentConfig { float turnsRatio, burdenOhms; StpmGain gain; };
struct StpmRawLSB        { double voltageLSB, currentLSB, powerLSB, energyLSB; }; // <0 = compute
struct StpmConstants     { double vref, calV, calI, avGain, kint, dclkHz; };       // datasheet defaults
enum class StpmGain      { X2, X4, X8, X16 };
```

---

## Calibration

Out of the box the library scales from nominal component values, so accuracy is
limited by resistor and burden tolerances. A one-time single-point calibration
against a known reference cancels those tolerances (and reference drift),
letting the chip reach its class-0.2 specification.

You need a **known reference**: an accurate voltage reading (a good multimeter
suffices) and a known current (a reference ammeter, or a resistive load of
accurately known draw).

### Step 1 — find your calibration constants

Run the **Calibration** example (**File → Examples → STPM_WattTF →
Calibration**). With a steady, known load applied, it asks you for the true
voltage and current, then prints two lines:
meter.setVoltageCalibrator(0x7A3);
meter.setCurrentCalibrator(0x812);

> Set your serial monitor's line ending to **Newline** (or **Both NL & CR**),
> or the prompt for typed input will wait forever.

### Step 2 — use them in your sketch

Paste those two lines into your `setup()`, right after `begin()`:

```cpp
meter.begin(vcfg, icfg);
meter.setVoltageCalibrator(0x7A3);   // from the Calibration example
meter.setCurrentCalibrator(0x812);
```

That's the simplest workflow: calibrate once, hardcode the constants.

> **The constants are specific to the board they were measured on.** Component
> tolerances differ between boards, so do not reuse one board's constants on a
> different board if you need high accuracy. For a production run, calibrate
> each unit.

### Advanced — persist instead of hardcode

For production, store each board's constants in non-volatile memory and restore
them on boot, rather than hardcoding. The library only does get/set; storage is
your application's job. Example for ESP32 (NVS via the `ArduinoNvs` library):

```cpp
// After calibrating:
nvs.setInt("chv", meter.getVoltageCalibrator());
nvs.setInt("chc", meter.getCurrentCalibrator());

// On every boot, after begin():
meter.setVoltageCalibrator(nvs.getInt("chv", 0x800));  // 0x800 = uncalibrated
meter.setCurrentCalibrator(nvs.getInt("chc", 0x800));
```

On AVR you would use `EEPROM` instead; the principle is identical. The same
pattern applies to persisting energy totals via the `get/setXxxEnergyRaw()`
methods, so a charging session's energy survives a reboot.

---

## Roadmap

- Shunt and Rogowski-coil current sensors (CT is supported today)
- Dual-channel STPM33 / STPM34 support
- Optional SPI CRC verification (disabled in this release)
- Sag / swell / overvoltage / overcurrent event detection (the chip supports
  these natively)
- `begin()` read-back verification to detect a missing or miswired chip
- A combined snapshot read that latches once and returns all values coherently

Contributions and hardware reports (especially on non-ESP32 boards) are
welcome.

---

## A note on billing / "revenue-grade" use

High *accuracy* (class 0.2, after calibration) is achievable with this library,
but legal **revenue-grade billing** is a regulated category (e.g. MID in the
EU, ANSI C12 in the US, IEC 62053 internationally) that certifies an entire
*product* — covering tamper-proofing, sealing, type testing by an accredited
body, and a frozen design — not just measurement accuracy. An accurate reading
from this library is necessary but **not sufficient** for legal billing use.
Check the requirements for your jurisdiction and application.

---

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Abu Nabil.

## Acknowledgements

Built around the STMicroelectronics STPM3x family (datasheet DS10272).