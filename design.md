# Eye Ring: NIR Illuminator Controller

Task 1 design writeup. The brief was a USB-powered board that drives six LED illuminators on a 4 cm ring, with per-channel intensity control, on-chip parameter storage, and a programmable 10 to 100 Hz sync output. This is the illuminator ring for an eye-tracking rig, so the six emitters are 850 nm NIR and the sync line is there to hand a timing reference to a camera or a second illuminator.

## What I built

Two boards, joined by an 8-pin 0.1 inch header cable.

The **ring board** is a 50 mm circle with a 20 mm hole in the middle for the camera to look through. Six through-hole NIR LEDs sit on a 40 mm ring at 60 degree spacing, anodes facing inward. That is all it holds. No driver, no logic. It takes in +5V and six cathode lines and lights up.

The **controller board** holds everything with a brain on it: the Raspberry Pi Pico and a TLC5947 constant-current LED driver, plus the handful of passives they need.

I split it this way on purpose. The camera has to sit dead center behind the ring, so the ring board wants to be a clean annulus with nothing crowding the optical axis. Putting the driver IC and the MCU out on a separate board keeps the ring mechanically simple and lets the controller be whatever shape is convenient to mount. It also means the ring can be swapped or re-spun on its own without touching the control electronics.

## Parts and why

**MCU: Raspberry Pi Pico module.** I used the full Pico module rather than a bare RP2040. For a one-off prototype on a tight timeline that removes a pile of risk. The module already has its crystal, flash, regulator, and USB connector laid out and tested, so USB comes in through the Pico's own micro-USB and I do not have to get a 12 MHz crystal or the USB differential pair right on my first board. In production you would drop the bare RP2040 down to shrink the board and cost, and I would call that out to the team as the obvious next step. For proving the design it is the wrong place to spend my risk budget.

**LED driver: TLC5947.** 24 channels of constant-current sink, 12-bit PWM dimming per channel, one SPI-style serial input. I only need six channels, so there is plenty of headroom. Constant current is the reason to use it: LED brightness tracks current, not voltage, so a constant-current sink gives even, repeatable output across all six emitters and across temperature. That matters for an eye tracker, where uneven illumination shows up directly as uneven pupil and glint response. The chip also has an internal grayscale oscillator, so there is no external clock pin to route.

**LEDs: 6x 850 nm NIR.** 850 nm is the usual pick for eye tracking. It sits far enough into the infrared to be invisible-ish to the subject but still inside the silicon sensor's response, so a normal camera with the IR-cut filter removed sees it well.

## Power

USB gives me 5V on VBUS. Two rails come off the Pico:

- **VBUS (5V)** feeds the LED anodes only, over the cable to the ring board.
- **3V3_OUT** feeds the TLC5947's VCC.

I run the driver's logic supply from 3V3 rather than 5V for a specific reason. The Pico's GPIO swings 0 to 3.3V, and the TLC5947's logic-high threshold scales with its VCC. Powering it from 3.3V lines the threshold up with what the Pico actually outputs, so I am not asking a 3.3V signal to clear a 5V-referenced input. The LEDs themselves still get the full 5V on their anodes, and the TLC5947 sinks their cathode current to ground. Common ground across both boards.

Per-channel current is set by one resistor, R2 (about 1.2k) from the IREF pin to ground. The TLC5947 mirrors the current through that resistor onto every output. With 1.2k the per-channel current lands around 15 mA, which is a safe, conservative level for these LEDs and keeps the total draw well inside the USB budget even with all six on.

Decoupling on the driver is a 10 uF bulk cap (C1) and a 0.1 uF (C2) across VCC and ground, close to the chip.

## Control bus

The Pico talks to the TLC5947 over hardware SPI. Using the RP2040's SPI block instead of bit-banging gives clean, jitter-free timing when I shift out the grayscale data, and it costs less CPU. Hardware SPI pins the clock and data lines to fixed GPIOs; the two latch/blank lines are plain GPIO and can go anywhere free. I kept all four adjacent so the header and routing stay tidy.

| Signal | Pico pin | Role |
|--------|----------|------|
| SCLK   | GP2      | SPI0 clock |
| SIN    | GP3      | SPI0 data out to driver |
| XLAT   | GP4      | latch shifted data to the outputs |
| BLANK  | GP5      | high blanks all outputs |
| SYNC   | GP0      | sync square wave to JP1 |

BLANK has a 10k pull-up (R1) to 3V3. That holds it high while the Pico is still booting, so the LEDs stay dark until the firmware is ready and deliberately drives BLANK low. Without it the outputs could flash on at power-up before any code runs. The TLC5947's SOUT pin is for daisy-chaining or readback and I leave it open with a single driver.

## Sync output

GP0 drives a 2-pin header (JP1: SYNC and GND) with a square wave, adjustable from 10 to 100 Hz over the command interface. I generate it with an RP2040 PWM slice at 50% duty. It is a plain timing reference, not gated to the LEDs, so downstream gear can lock to it however it wants.

To hit the low end of the range cleanly I fix the PWM wrap at 62500 counts and vary the clock divider: divider = 2000 / frequency, which stays between 20 and 200 across the whole 10 to 100 Hz span and well inside the hardware's valid divider range.

## Parameter storage

Intensities, on/off state, and the sync setting live in a struct that gets written to the last 4 kB sector of the Pico's flash on a `SAVE` command. The block carries a magic number and a CRC32. At boot the firmware reads it back, checks both, and either restores the saved state or falls back to sane defaults (all LEDs off, sync off, 30 Hz stored). So the board comes up the way you left it, and a corrupt or blank sector fails safe instead of loading garbage.

## Command protocol

Line-based ASCII over USB CDC serial. One command per line, each replies `OK` or `ERR <reason>`. Human-typable in a terminal, trivial to script.

```
IDN?              -> SEETRUE-RING v1.0
LED <1-6> <0-100> -> set one LED intensity, percent
LED ALL <0-100>   -> set all six
ON <1-6>          -> enable one LED at its stored intensity
OFF <1-6>         -> disable one LED
SYNC <10-100>     -> sync output on at the given Hz
SYNC OFF          -> sync output off
GET               -> print current parameters
SAVE              -> persist to flash
```

Intensity is percent on the wire and maps to the driver's 12-bit grayscale internally (0 to 4095). Percent keeps it readable; the firmware handles the scaling.

## Bring-up order

How I would actually test the first board, cautious step by step rather than plugging in and hoping:

1. Visual inspection, then a continuity check for a VBUS-to-ground short before any power goes in.
2. Power up through a current-limited supply. Confirm the 5V and 3.3V rails and a reasonable idle current.
3. USB enumeration, then `IDN?` over the serial link to confirm the firmware is alive.
4. Sync output on a scope, checking frequency accuracy across the 10 to 100 Hz range.
5. LEDs one channel at a time, measuring actual channel current against the IREF calculation.
6. All six at full intensity to confirm the total current sits inside the USB budget.

## Known gaps and what I would do next

Honest about where this stops.

The current level is fixed in hardware by R2. If a channel needs a different absolute current I have to change the resistor, not a register. For this task the per-channel PWM dimming covers the requirement, but a production version might want a digitally trimmed reference.

The sync is a free-running timing reference. It is not phase-locked to the LED PWM or to an external trigger. If the camera integration needs the illumination strobed in lockstep with exposure, that is a firmware and possibly hardware change, and I would want to talk through the exact timing the rig needs before building it.

The Pico module is the prototype choice. The production move is the bare RP2040 to cut size and cost, which brings the crystal, flash, and USB routing back onto my board. Worth doing once the design is proven, not before.
