// grinder.h — grind-by-weight state machine + motor control.
//
//   IDLE      motor off, waiting.
//   TARING    scale tared; motor stays off PRE_GRIND_TARE_MS, then -> GRINDING.
//   GRINDING  weight mode: motor runs until the cut point (see approach below);
//             time mode: motor runs for the set number of seconds.
//   SETTLING  motor off; wait for the reading to settle, then decide.
//   PULSING   short motor burst (PULSE_MS) to nudge the dose up, then SETTLING.
//   DONE      motor off; final dose held briefly, then back to IDLE.
//
// Weight mode has two selectable approaches (ApproachMode):
//   PULSE  coarse cut APPROACH_MARGIN_G before target, then pulse up precisely.
//   LEARN  single cut LEARN_STOP_OFFSET_G before target; after settling, adapt
//          that offset toward the measured error so the next grind lands closer.
#pragma once

#include <Arduino.h>

enum class GrindState   : uint8_t { IDLE, TARING, GRINDING, SETTLING, PULSING, DONE };
enum class GrindMode    : uint8_t { WEIGHT, TIME };
enum class ApproachMode : uint8_t { PULSE, LEARN };   // weight-mode strategy

class Grinder {
 public:
  void begin();                 // motor safe-low, load saved target
  void start();                 // tare + run (no-op unless idle and scale connected)
  void stop();                  // user abort -> motor off, idle

  void update(float weight, bool scaleConnected);   // call every loop

  GrindState state() const { return _state; }
  GrindMode  mode()  const { return _mode; }
  void       setMode(GrindMode m);          // persisted; ignored mid-grind
  ApproachMode approach() const { return _approach; }   // weight-mode strategy
  void       setApproach(ApproachMode a);   // persisted; ignored mid-grind
  float target()      const { return _target; }       // grams (weight mode)
  float targetTime()  const { return _targetTime; }    // seconds (time mode)
  float finalWeight() const { return _finalWeight; }
  float learnedOffset() const { return _learnedOffset; }   // learn-mode cut offset (g)
  bool  timedOut()    const { return _timedOut; }
  float elapsed()     const;    // seconds since the motor first started (0 if idle)

  void  adjustTarget(int dir);   // +/- button (dir = +1 / -1); nudges the active target

 private:
  GrindState _state = GrindState::IDLE;
  GrindMode  _mode  = GrindMode::WEIGHT;
  ApproachMode _approach = ApproachMode::PULSE;   // weight-mode strategy
  float _target      = 0;        // grams
  float _targetTime  = 0;        // seconds
  float _finalWeight = 0;
  bool  _timedOut    = false;
  float _learnedOffset = 0.8f;   // learn-mode cut offset; real default/NVS value set in begin()

  uint32_t _startMs       = 0;   // motor-on (coarse) — the overall grind clock
  uint32_t _tareMs        = 0;
  uint32_t _doneMs        = 0;
  uint32_t _settleStartMs = 0;
  uint32_t _stableSinceMs = 0;
  float    _stableRefW    = 0;
  uint32_t _pulseStartMs  = 0;
  uint32_t _pulseMs       = 0;   // length of the current (tapered) pulse
  int      _pulseCount    = 0;
  float    _prePulseW     = 0;   // weight just before the current pulse
  float    _lastGain      = 0;   // grams the last pulse actually added (after settling)

  void motor(bool on);
  void persist();
  void beginSettle(float weight);          // enter SETTLING with fresh stability refs
  void finish(float weight, bool timedOut);
};

extern Grinder g_grinder;