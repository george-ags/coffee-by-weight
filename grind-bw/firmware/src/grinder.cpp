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
  _target    = s_prefs.getFloat("target", TARGET_DEFAULT_G);
  _overshoot = s_prefs.getFloat("overshoot", OVERSHOOT_DEFAULT_G);
  _target    = clampf(_target, TARGET_MIN_G, TARGET_MAX_G);
  _overshoot = clampf(_overshoot, OVERSHOOT_MIN_G, OVERSHOOT_MAX_G);
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
  s_prefs.putFloat("overshoot", _overshoot);
}

void Grinder::adjustTarget(float deltaG) {
  if (_state == GrindState::GRINDING) return;   // don't move target mid-grind
  _target = clampf(_target + deltaG, TARGET_MIN_G, TARGET_MAX_G);
  // round to the step grid to avoid float drift (e.g. 18.0000001)
  _target = roundf(_target / TARGET_STEP_G) * TARGET_STEP_G;
  s_prefs.putFloat("target", _target);
}

void Grinder::start() {
  if (_state != GrindState::IDLE) return;
  if (!g_scale.connected) return;               // need a scale to weigh against
  g_scale.tare();                               // count only the dose
  _timedOut = false;
  _finalWeight = 0;
  _settled = false;
  _tareMs = millis();
  _state = GrindState::TARING;                  // motor stays OFF until tare settles
  motor(false);
  Serial.printf("[grind] tare; waiting %d ms before motor\n", PRE_GRIND_TARE_MS);
}

void Grinder::stop() {
  motor(false);
  if (_state == GrindState::GRINDING || _state == GrindState::TARING) {
    Serial.println("[grind] aborted by user");
    _state = GrindState::IDLE;
  }
}

float Grinder::elapsed() const {
  if (_state != GrindState::GRINDING) return 0.0f;   // grind time only, not the tare wait
  return (millis() - _startMs) / 1000.0f;
}

void Grinder::learnOvershoot(float settledWeight) {
  float err = settledWeight - _target;          // +over, -under
  if (fabsf(err) >= OFF_TARGET_REJECT_G) {
    Serial.printf("[grind] off-target %.2fg, not learning\n", err);
    return;
  }
  // If we overshot, increase the cutoff margin; if we undershot, decrease it.
  _overshoot = clampf(_overshoot + OVERSHOOT_LEARN_RATE * err,
                      OVERSHOOT_MIN_G, OVERSHOOT_MAX_G);
  persist();
  Serial.printf("[grind] settled=%.2f err=%.2f -> overshoot=%.2f\n",
                settledWeight, err, _overshoot);
}

void Grinder::update(float weight, bool scaleConnected) {
  switch (_state) {
    case GrindState::TARING: {
      if (!scaleConnected) {                    // lost the scale during tare -> abort
        motor(false);
        _state = GrindState::IDLE;
        return;
      }
      if (millis() - _tareMs >= (uint32_t)PRE_GRIND_TARE_MS) {
        _startMs = millis();
        _state = GrindState::GRINDING;
        motor(true);
        Serial.printf("[grind] start target=%.1f overshoot=%.2f\n", _target, _overshoot);
      }
      break;
    }

    case GrindState::GRINDING: {
      // Safety: lost the scale mid-grind -> cannot weigh -> stop immediately.
      if (!scaleConnected) {
        motor(false);
        Serial.println("[grind] SCALE LOST - emergency stop");
        _state = GrindState::IDLE;
        return;
      }
      // Hard timeout.
      if (elapsed() >= MAX_GRIND_SECONDS) {
        motor(false);
        _timedOut = true;
        _finalWeight = weight;
        _doneMs = millis();
        _state = GrindState::DONE;
        Serial.println("[grind] TIMEOUT");
        return;
      }
      // Target reached (minus learned in-flight margin).
      if (weight >= _target - _overshoot) {
        motor(false);
        _doneMs = millis();
        _settled = false;
        _state = GrindState::DONE;
        Serial.printf("[grind] cutoff at %.2fg\n", weight);
      }
      break;
    }

    case GrindState::DONE: {
      // Wait for grounds to settle, capture final weight, learn, then idle.
      if (!_settled && (millis() - _doneMs) >= (uint32_t)(SETTLE_SECONDS * 1000)) {
        _settled = true;
        _finalWeight = weight;
        if (!_timedOut) learnOvershoot(_finalWeight);
      }
      // Hold the DONE screen ~3 s after settling, then return to idle.
      if (_settled && (millis() - _doneMs) >= (uint32_t)(SETTLE_SECONDS * 1000 + 3000)) {
        _state = GrindState::IDLE;
      }
      break;
    }

    case GrindState::IDLE:
    default:
      motor(false);   // belt-and-braces: never energised while idle
      break;
  }
}