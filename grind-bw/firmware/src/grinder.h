// grinder.h — grind-by-weight state machine + motor control.
//
//   IDLE      motor off, waiting.
//   TARING    scale tared; motor stays off PRE_GRIND_TARE_MS, then -> GRINDING.
//   GRINDING  coarse phase: motor runs until APPROACH_MARGIN_G before target.
//   SETTLING  motor off; wait for the reading to settle, then decide.
//   PULSING   short motor burst (PULSE_MS) to nudge the dose up, then SETTLING.
//   DONE      motor off; final dose held briefly, then back to IDLE.
//
// Net effect: the dose stops at the target, approached precisely by pulsing the
// last fraction of a gram instead of relying on a learned overshoot.
#pragma once

#include <Arduino.h>

enum class GrindState : uint8_t { IDLE, TARING, GRINDING, SETTLING, PULSING, DONE };

class Grinder {
 public:
  void begin();                 // motor safe-low, load saved target
  void start();                 // tare + run (no-op unless idle and scale connected)
  void stop();                  // user abort -> motor off, idle

  void update(float weight, bool scaleConnected);   // call every loop

  GrindState state() const { return _state; }
  float target()      const { return _target; }
  float finalWeight() const { return _finalWeight; }
  bool  timedOut()    const { return _timedOut; }
  float elapsed()     const;    // seconds since the motor first started (0 if idle)

  void  adjustTarget(float deltaG);   // +/- button; clamps, snaps to step, persists

 private:
  GrindState _state = GrindState::IDLE;
  float _target      = 0;
  float _finalWeight = 0;
  bool  _timedOut    = false;

  uint32_t _startMs       = 0;   // motor-on (coarse) — the overall grind clock
  uint32_t _tareMs        = 0;
  uint32_t _doneMs        = 0;
  uint32_t _settleStartMs = 0;
  uint32_t _stableSinceMs = 0;
  float    _stableRefW    = 0;
  uint32_t _pulseStartMs  = 0;
  int      _pulseCount    = 0;

  void motor(bool on);
  void persist();
  void beginSettle(float weight);          // enter SETTLING with fresh stability refs
  void finish(float weight, bool timedOut);
};

extern Grinder g_grinder;