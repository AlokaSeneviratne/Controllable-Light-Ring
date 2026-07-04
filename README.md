# NIR LED Illumination Ring

A near-infrared (850 nm) illumination ring for eye-tracking hardware, built around a Raspberry Pi Pico and a TLC5947 constant-current LED driver. The board provides even, brightness-controlled NIR lighting around a center camera aperture, plus a hardware sync signal so the illumination can be aligned with a camera's frame timing.

This repository holds the design assignment for a Hardware Integration Engineer position: the schematic, the firmware that runs on the Pico, and a host-side control program for a PC.

## What it does

The ring lights the subject's eyes with 850 nm NIR light, which sits just outside the visible range but is bright to most camera sensors. That gives an eye tracker a clean, glare-controlled image without a visible glow. Brightness is set in firmware and driven by a constant-current driver, so all six LEDs stay matched regardless of small forward-voltage differences between parts.

A single sync pin on the Pico outputs a square wave from 10 to 100 Hz. A camera or capture system can lock its frame timing to that signal. The LEDs themselves are not strobe-gated in this version; they stay on at the set brightness while the sync line carries the timing reference.

## Key features

- Six 850 nm NIR LEDs arranged on a 4 cm ring around a center camera hole
- Constant-current drive through a TI TLC5947, so per-LED brightness stays uniform
- Firmware-controlled brightness with no manual current trimming
- Programmable 10 to 100 Hz sync output on a 2-pin header
- Powered and programmed over the Pico's own micro-USB port, no extra connector
- Brightness and sync settings held in the RP2040's flash so they survive a power cycle
- Host control program in C++ with both POSIX and Windows serial backends

## Hardware architecture

The Pico module carries the USB, the RP2040, and the 3.3 V regulator, so the board around it stays simple. The Pico talks to the TLC5947 over a four-wire control bus. The driver sinks current from six of its channels, one per LED. The LED anodes sit on the 5 V USB rail, and the driver pulls their cathodes to ground at a regulated current.

The TLC5947 has 24 channels. Only six are used here, which leaves headroom if the ring later grows.

### Power rails

| Rail | Source | Feeds |
|------|--------|-------|
| 5 V (VBUS) | Pico pin 40 | LED anodes |
| 3.3 V (3V3_OUT) | Pico pin 36 | TLC5947 VCC (logic supply) |
| GND | Pico GND | Common ground for board and driver |

The driver's logic supply runs at 3.3 V on purpose, so its input thresholds line up with the Pico's 3.3 V GPIO. Running the logic at 5 V would risk marginal high/low levels on the control lines.

### Control bus (Pico to TLC5947)

| Signal | Pico pin | Purpose |
|--------|----------|---------|
| SCLK | GP2 | SPI clock |
| SIN | GP3 | Serial data into the driver |
| XLAT | GP4 | Latches shifted data into the outputs |
| BLANK | GP5 | Blanks all outputs when high |

BLANK has a 10 kΩ pull-up (R1) to 3.3 V so the LEDs stay off during power-up, before firmware takes control. That avoids a bright flash at boot.

### Other components

- **R2 (IREF), about 1.2 kΩ** to ground, sets the constant current per channel
- **C1 (10 µF) and C2 (0.1 µF)** decouple the driver's VCC to ground
- **JP1**, a 2-pin header carrying the sync output (GP0) and ground

## Bill of materials (core parts)

| Ref | Part | Notes |
|-----|------|-------|
| U1 | Raspberry Pi Pico module | RP2040, USB, and 3.3 V regulator on board |
| U2 | TLC5947 | 24-channel constant-current LED driver, HTSSOP-32 |
| D1 to D6 | 850 nm NIR LED | 5 mm through-hole, on 4 cm ring |
| R1 | 10 kΩ | BLANK pull-up |
| R2 | approx. 1.2 kΩ | IREF, sets per-channel current |
| C1 | 10 µF | Bulk decoupling |
| C2 | 0.1 µF | High-frequency decoupling |
| JP1 | 2-pin header | Sync output |

## Repository layout

```
.
├── README.md                 This file
├── docs/
│   └── design.md             Full design writeup and rationale
├── hardware/                 Fusion Electronics schematic and exports
├── firmware/
│   ├── CMakeLists.txt
│   └── src/                  Pico C SDK firmware
└── host/
    ├── posix/                Serial control example for Linux and macOS
    └── windows/              Serial control example for Windows
```

## Firmware

The firmware targets the Raspberry Pi Pico C SDK and builds with CMake. It sets up the SPI control bus, pushes 12-bit brightness values to the TLC5947, drives the sync output at the configured frequency, and reads the stored settings back from flash on boot.

### Build

You need the Pico C SDK and its toolchain installed, with `PICO_SDK_PATH` set.

```bash
cd firmware
mkdir build && cd build
cmake ..
make
```

Flash the resulting `.uf2` by holding BOOTSEL while plugging the Pico in, then copying the file to the mounted drive.

If you want to try the logic without hardware in front of you, the design also runs in Wokwi, a browser-based RP2040 simulator.

## Host control tool

The `host/` directory holds a small C++ program that opens the Pico's USB serial port and sends brightness and sync commands. There are two builds of the same tool: a POSIX version for Linux and macOS, and a Windows version using the Win32 serial API. Both take the same command set so a script written for one works on the other.

## Design notes

A few choices are worth calling out, since they shaped the rest of the board.

Using the Pico module instead of a bare RP2040 was deliberate. It brings its own USB port, crystal, regulator, and flash, which removes a large chunk of board-level bring-up risk and lets the design focus on the illumination and driver. The tradeoff is size: the module is 51 mm long, which is tight against a 50 mm ring, and that mechanical fit is the main open problem below.

The TLC5947 was chosen because constant-current sinking keeps the six LEDs matched without hand-trimming resistors per channel. One IREF resistor sets the current for all of them. That matters for eye tracking, where uneven illumination across the ring shows up directly in the camera image.

Keeping the LEDs unstrobed in this version is a scope decision. A sync line is enough to prove the timing interface. Gated strobing can layer on later without changing the power or driver topology.

## Status

The schematic is complete and passes ERC in Autodesk Fusion Electronics. Firmware and the host control tool are part of this deliverable set.

The board outline and mechanical layout are still open. The central question is how to fit the 51 mm Pico module onto or around a 50 mm circular board that also needs a center hole for the camera. Options under consideration include mounting the Pico off the ring plane or reshaping the outline around it. Nothing here has been through PCB fabrication yet.

## Context

This is a design assignment for a Hardware Integration Engineer role at an eye-tracking hardware company. It is meant to show the full path from requirements to a driven, controllable illumination board, including the parts that are still in progress.