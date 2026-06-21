// grinder.h — grind-by-weight state machine + motor control.
//
// Mirrors the role of lm-bbw's ControlManager + the target-cutoff logic in
// lm-bbw.py, adapted for a grinder:
//   IDLE     motor off, waiting. start() begins a grind.
//   GRINDING motor on; cut at (target - overshoot), on timeout, or on a lost
//            scale connection.
//   DONE     motor off; after grounds settle, learn a better overshoot and
//            persist it. Auto-returns to IDLE.
#pragma once

#include <Arduino.h>

enum class GrindState : uint8_t { IDLE, GRINDING, DONE };

class Grinder {
 public:
  void begin();                 // motor pin safe-low, load saved target/overshoot
  void start();                 // tare scale + run motor (no-op if not idle/ready)
  void stop();                  // user abort -> motor off, back to idle

  // Call every loop with the current scale weight and link status.
  void update(float weight, bool scaleConnected);

  GrindState state() const { return _state; }
  float target()   const { return _target; }
  float overshoot()const { return _overshoot; }
  float finalWeight() const { return _finalWeight; }
  bool  timedOut() const { return _timedOut; }
  float elapsed()  const;       // seconds since grind start (0 if idle)

  void  adjustTarget(float deltaG);   // +/- button; clamps + persists

 private:
  GrindState _state = GrindState::IDLE;
  float _target    = 0;
  float _overshoot = 0;
  float _finalWeight = 0;
  bool  _timedOut = false;

  uint32_t _startMs = 0;
  uint32_t _doneMs  = 0;
  bool     _settled = false;

  void motor(bool on);
  void persist();
  void learnOvershoot(float settledWeight);
};

extern Grinder g_grinder;
