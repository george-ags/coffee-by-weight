// config.h — all pins and tunables for GRIND-BW on ESP32-S3.
//
// Two groups of settings:
//   1. VERIFIED      — motor wiring and grind behaviour. These are correct for
//                      your described setup and safe to trust.
//   2. BOARD-SPECIFIC — the AMOLED + touch pins for the Waveshare
//                      ESP32-S3-Touch-AMOLED-1.64. The values below are a best
//                      guess; you MUST confirm them against Waveshare's wiki /
//                      schematic / Arduino demo for this exact board. They are
//                      isolated in board_display.cpp so a mismatch never
//                      affects the scale or motor logic.
#pragma once

// ===========================================================================
// 1. MOTOR  (VERIFIED — matches your wiring)
// ===========================================================================
// Grinder Pin 3 (motor control signal) -> ESP32-S3 GPIO 18.
// Active-high: the motor RUNS while this pin is driven to ~3.3 V.
#define MOTOR_PIN              18
#define MOTOR_ACTIVE_HIGH      1     // 1 = HIGH runs the motor (your case)

// ===========================================================================
// 2. GRIND BEHAVIOUR  (VERIFIED — tune to taste)
// ===========================================================================
#define TARGET_DEFAULT_G       18.0f  // startup target dose
#define TARGET_MIN_G            1.0f
#define TARGET_MAX_G           60.0f
#define TARGET_STEP_G           0.5f   // +/- button step

// --- Fine-approach dosing -------------------------------------------------
// Coarse phase runs the motor until APPROACH_MARGIN_G *before* the target, then
// stops and lets the weight settle. If the settled weight is still below target,
// the motor is pulsed in short bursts (PULSE_MS) — settle, check, repeat — until
// the target is reached. Precise without relying on a learned overshoot.
#define APPROACH_MARGIN_G       0.5f    // coarse cut this far below target
#define PULSE_MS                100     // motor burst length per fine pulse
#define TARGET_EPSILON_G        0.05f   // target counts as reached within this
#define MAX_FINE_PULSES         40      // safety cap on number of pulses

// "Settled" detection between stops/pulses: wait at least PULSE_SETTLE_MIN_MS,
// then treat the reading as stable once it hasn't moved more than
// SETTLE_STABLE_DELTA_G for SETTLE_STABLE_HOLD_MS (capped at SETTLE_MAX_MS).
#define PULSE_SETTLE_MIN_MS     400
#define SETTLE_STABLE_DELTA_G   0.05f
#define SETTLE_STABLE_HOLD_MS   300
#define SETTLE_MAX_MS           2000

// Safety: hard ceiling on a whole grind (coarse + all pulses). If the target is
// never reached, the motor is cut and the grind is marked timed-out.
#define MAX_GRIND_SECONDS      40.0f

// How long the DONE result is shown before returning to idle.
#define DONE_HOLD_MS            3000

// On START: tare the scale, then wait this long before running the motor, so
// the tare settles and the dose is counted from a true zero.
#define PRE_GRIND_TARE_MS       1000

// ===========================================================================
// 3. BLE SCALE   (passed into the standalone `scale` library via begin())
// ===========================================================================
#define BLE_DEVICE_NAME        "grind-bw"
#define SCAN_SECONDS            4      // per scan attempt while searching
// (The Acaia heartbeat interval is a protocol constant and lives inside the
//  scale library, not here.)

// ===========================================================================
// 4. AMOLED DISPLAY  (BOARD-SPECIFIC — *** VERIFY AGAINST WAVESHARE ***)
// ===========================================================================
// Waveshare ESP32-S3-Touch-AMOLED-1.64: 280 x 456 CO5300 QSPI panel.
// Portrait orientation assumed (tall). If your demo uses a rotation, set it in
// board_display.cpp.
#define LCD_WIDTH              280
#define LCD_HEIGHT             456

// QSPI pins for the CO5300 — PLACEHOLDERS. Copy the real values from the
// Arduino_ESP32QSPI(...) / Arduino_CO5300(...) constructor in Waveshare's demo.
#define LCD_CS                 9
#define LCD_SCK                10
#define LCD_SDIO0              11
#define LCD_SDIO1              12
#define LCD_SDIO2              13
#define LCD_SDIO3              14
#define LCD_RST                21
#define LCD_TE                 -1     // tearing-effect line, -1 if unused
#define LCD_BL                 -1     // backlight/EN; AMOLED often has none, -1

// ===========================================================================
// 5. CAPACITIVE TOUCH  (BOARD-SPECIFIC — *** VERIFY ***)
// ===========================================================================
// FocalTech FT3168 over I2C — values confirmed against a known build for this
// exact board (jaapp/smart-grind-by-weight). Talks directly over Wire, no I/O
// expander or reset line needed.
#define TOUCH_I2C_ADDR         0x38
#define TOUCH_SDA              47
#define TOUCH_SCL              48
#define TOUCH_RST              -1
#define TOUCH_INT              -1
// Set to 1 if X/Y come out mirrored/swapped once you can see the panel.
#define TOUCH_SWAP_XY          0
#define TOUCH_INVERT_X         0
#define TOUCH_INVERT_Y         0