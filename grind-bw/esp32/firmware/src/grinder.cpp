// grinder.cpp
#include "grinder.h"
#include "config.h"
#include <scale.h>
#include <Preferences.h>

Grinder g_grinder;
static Preferences s_prefs;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Grinder::begin() {
  // Motor safe-low BEFORE anything else can run it.
  pinMode(MOTOR_PIN, OUTPUT);
  motor(false);

  s_prefs.begin("grindbw", false);
  _target     = clampf(s_prefs.getFloat("target", TARGET_DEFAULT_G), TARGET_MIN_G, TARGET_MAX_G);
  _targetTime = clampf(s_prefs.getFloat("ttime",  TIME_DEFAULT_S),   TIME_MIN_S,   TIME_MAX_S);
  _mode       = (s_prefs.getUChar("mode", 0) == 1) ? GrindMode::TIME : GrindMode::WEIGHT;
}

void Grinder::motor(bool on) {
#if MOTOR_ACTIVE_HIGH
  digitalWrite(MOTOR_PIN, on ? HIGH : LOW);
#else
  digitalWrite(MOTOR_PIN, on ? LOW : HIGH);
#endif
}

void Grinder::persist() {
  s_prefs.putFloat("target", _target);
  s_prefs.putFloat("ttime",  _targetTime);
  s_prefs.putUChar("mode",   _mode == GrindMode::TIME ? 1 : 0);
}

void Grinder::setMode(GrindMode m) {
  if (_state != GrindState::IDLE) return;      // don't switch mid-grind
  _mode = m;
  persist();
}

void Grinder::adjustTarget(int dir) {
  if (_state != GrindState::IDLE) return;      // don't move target mid-grind
  if (_mode == GrindMode::WEIGHT) {
    _target = clampf(_target + dir * TARGET_STEP_G, TARGET_MIN_G, TARGET_MAX_G);
    _target = roundf(_target / TARGET_STEP_G) * TARGET_STEP_G;
  } else {
    _targetTime = clampf(_targetTime + dir * TIME_STEP_S, TIME_MIN_S, TIME_MAX_S);
    _targetTime = roundf(_targetTime / TIME_STEP_S) * TIME_STEP_S;
  }
  persist();
}

void Grinder::start() {
  if (_state != GrindState::IDLE) return;
  _timedOut    = false;
  _finalWeight = 0;
  _pulseCount  = 0;

  if (_mode == GrindMode::TIME) {
    // Pure timer — no scale required, no tare.
    _startMs = millis();
    _state   = GrindState::GRINDING;
    motor(true);
    Serial.printf("[grind] timed grind for %.1f s\n", _targetTime);
  } else {
    if (!g_scale.connected) return;            // weight mode needs a scale
    g_scale.tare();                            // count only the dose
    _tareMs = millis();
    _state  = GrindState::TARING;              // motor stays OFF until tare settles
    motor(false);
    Serial.printf("[grind] tare; waiting %d ms before motor\n", PRE_GRIND_TARE_MS);
  }
}

void Grinder::stop() {
  motor(false);
  if (_state != GrindState::IDLE && _state != GrindState::DONE) {
    Serial.println("[grind] aborted by user");
    _state = GrindState::IDLE;
  }
}

float Grinder::elapsed() const {
  if (_state == GrindState::GRINDING || _state == GrindState::SETTLING ||
      _state == GrindState::PULSING)
    return (millis() - _startMs) / 1000.0f;
  return 0.0f;
}

void Grinder::beginSettle(float weight) {
  motor(false);
  _settleStartMs = millis();
  _stableRefW    = weight;
  _stableSinceMs = millis();
  _state         = GrindState::SETTLING;
}

void Grinder::finish(float weight, bool timedOut) {
  motor(false);
  _finalWeight = weight;
  _timedOut    = timedOut;
  _doneMs      = millis();
  _state       = GrindState::DONE;
}

void Grinder::update(float weight, bool scaleConnected) {
  // Global safety for any phase with the motor potentially running.
  bool active = (_state == GrindState::GRINDING ||
                 _state == GrindState::SETTLING ||
                 _state == GrindState::PULSING);
  if (active) {
    if (_mode == GrindMode::WEIGHT && !scaleConnected) {   // weight mode can't run blind
      motor(false);
      Serial.println("[grind] SCALE LOST - emergency stop");
      _state = GrindState::IDLE;
      return;
    }
    if ((millis() - _startMs) >= (uint32_t)(MAX_GRIND_SECONDS * 1000)) {
      Serial.println("[grind] TIMEOUT");
      finish(weight, true);
      return;
    }
  }

  switch (_state) {
    case GrindState::TARING:
      motor(false);
      if (!scaleConnected) { _state = GrindState::IDLE; return; }
      if (millis() - _tareMs >= (uint32_t)PRE_GRIND_TARE_MS) {
        _startMs = millis();
        _state   = GrindState::GRINDING;
        motor(true);
        Serial.printf("[grind] start target=%.1f, coarse cut at %.1f\n",
                      _target, _target - APPROACH_MARGIN_G);
      }
      break;

    case GrindState::GRINDING:
      if (_mode == GrindMode::TIME) {
        if ((millis() - _startMs) >= (uint32_t)(_targetTime * 1000.0f)) {
          Serial.printf("[grind] timed grind complete (%.1f s)\n", _targetTime);
          finish(weight, false);
        }
      } else {
        // Coarse: run until APPROACH_MARGIN_G before target, then let it settle.
        if (weight >= _target - APPROACH_MARGIN_G) {
          Serial.printf("[grind] coarse cut at %.2f g\n", weight);
          beginSettle(weight);
        }
      }
      break;

    case GrindState::SETTLING: {
      uint32_t now = millis();
      if (fabsf(weight - _stableRefW) > SETTLE_STABLE_DELTA_G) {
        _stableRefW = weight;
        _stableSinceMs = now;
      }
      bool minWaited = (now - _settleStartMs) >= (uint32_t)PULSE_SETTLE_MIN_MS;
      bool stable    = (now - _stableSinceMs) >= (uint32_t)SETTLE_STABLE_HOLD_MS;
      bool capped    = (now - _settleStartMs) >= (uint32_t)SETTLE_MAX_MS;
      if (minWaited && (stable || capped)) {
        if (weight >= _target - TARGET_EPSILON_G) {
          Serial.printf("[grind] target reached: %.2f g\n", weight);
          finish(weight, false);
        } else if (_pulseCount >= MAX_FINE_PULSES) {
          Serial.printf("[grind] pulse cap at %.2f g\n", weight);
          finish(weight, false);
        } else {
          _pulseCount++;
          _pulseStartMs = now;
          _state = GrindState::PULSING;
          motor(true);
          Serial.printf("[grind] pulse %d at %.2f g\n", _pulseCount, weight);
        }
      }
      break;
    }

    case GrindState::PULSING:
      if (millis() - _pulseStartMs >= (uint32_t)PULSE_MS) {
        motor(false);
        beginSettle(weight);
      }
      break;

    case GrindState::DONE:
      motor(false);
      if (millis() - _doneMs >= (uint32_t)DONE_HOLD_MS) _state = GrindState::IDLE;
      break;

    case GrindState::IDLE:
    default:
      motor(false);
      break;
  }
}