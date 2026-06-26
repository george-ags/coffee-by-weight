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

// Time-based dosing (alternative mode, selected in the settings screen): grind
// for a fixed number of seconds. Doesn't need a scale.
#define TIME_DEFAULT_S          8.0f
#define TIME_MIN_S              0.5f
#define TIME_MAX_S             30.0f
#define TIME_STEP_S             0.1f   // +/- button step, 0.1 s resolution

// --- Fine-approach dosing -------------------------------------------------
// Coarse phase runs the motor until APPROACH_MARGIN_G *before* the target, then
// stops and lets the weight settle. If still below target, the motor is pulsed
// in short bursts — settle, check, repeat — until the target is reached.
//
// APPROACH_MARGIN_G must be LARGER than the "coast" — the extra coffee that lands
// after the motor stops (scale latency + grinder/chute retention). Measured coast
// was ~0.8 g, so 1.0 g lands the coarse stop ~0.2 g under target.
//
// Fine pulses TAPER with the remaining deficit so the endgame doesn't overshoot:
//   burst_ms = clamp(remaining_g * PULSE_MS_PER_G, PULSE_MIN_MS, PULSE_MS)
// and the grind stops one step early when another pulse would overshoot more than
// stopping now undershoots (judged from the measured gain of the previous pulse).
// If you still land slightly over, lower PULSE_MS_PER_G or PULSE_MIN_MS; if it
// takes too many pulses or lands under, raise them.
#define APPROACH_MARGIN_G       0.9f    // coarse cut this far below target
#define PULSE_MS                 80     // longest fine pulse (far from target)
#define PULSE_MIN_MS             22     // shortest fine pulse (final nudges)
#define PULSE_MS_PER_G          180     // ms of burst per gram remaining (lower = gentler)
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

// How long the DONE result (green flash + "HERE YOU GO") is shown before idle.
#define DONE_HOLD_MS           10000

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