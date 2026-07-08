// main.cpp — GRIND-BW on ESP32-S3.
//
// Boot order matters for safety: the grinder's begin() drives the motor pin
// LOW before anything else can run, so a reset never leaves the burrs spinning.
//
// Concurrency: BLE scan/connect/heartbeat live on their own task (see
// scale.cpp, core 0). This loop (core 1) runs LVGL, refreshes the UI, and runs
// the grind state machine against the latest scale weight.

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include "config.h"
#include "board_display.h"
#include <scale.h>
#include "grinder.h"
#include "ui.h"

static Preferences s_scalePrefs;

static Vendor vendorFromStr(const String& s) {
  if (s.equalsIgnoreCase("bookoo"))   return Vendor::BOOKOO;
  if (s.equalsIgnoreCase("timemore")) return Vendor::TIMEMORE;
  if (s.equalsIgnoreCase("acaia"))    return Vendor::ACAIA;
  return Vendor::NONE;
}
static const char* vendorToStr(Vendor v) {
  return v == Vendor::BOOKOO   ? "bookoo"
       : v == Vendor::TIMEMORE ? "timemore"
       : v == Vendor::ACAIA    ? "acaia" : "";
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== GRIND-BW ===");

  // 1. Motor safe-low + load saved target/overshoot.
  g_grinder.begin();

  // 2. Display + touch + LVGL, then build the UI.
  board_display_init();
  ui_create();
  ui_update();

  // 3. Load the last scale (MAC + vendor) so we reconnect directly; otherwise
  //    the BLE task scans for any supported scale and adopts the first found.
  s_scalePrefs.begin("grindbw_ble", false);
  String   savedMac    = s_scalePrefs.getString("mac", "");
  Vendor   savedVendor = vendorFromStr(s_scalePrefs.getString("vendor", ""));
  g_scale.begin(savedMac, savedVendor, BLE_DEVICE_NAME, SCAN_SECONDS);
}

void loop() {
  lv_timer_handler();

  // Run the grind state machine against the freshest weight + link status.
  g_grinder.update(g_scale.weight, g_scale.connected);

  // Refresh the screen.
  ui_update();

  // Persist the scale identity once connected (so next boot reconnects fast).
  static bool saved = false;
  if (g_scale.connected && !saved && g_scale.macAddress().length()) {
    s_scalePrefs.putString("mac", g_scale.macAddress());
    s_scalePrefs.putString("vendor", vendorToStr(g_scale.vendor));
    saved = true;
    Serial.printf("[main] saved scale %s (%s)\n",
                  g_scale.macAddress().c_str(), g_scale.vendorName());
  }
  if (!g_scale.connected) saved = false;

  delay(5);   // keep LVGL responsive without starving the BLE task
}