// scale.h — vendor-neutral Bluetooth scale client for ESP32 (NimBLE).
//
// Mirrors the role of common/scales.py from the Raspberry Pi project: one
// object the rest of the firmware talks to, hiding whether an Acaia, BooKoo, or
// Timemore is connected. Connection management runs on its own FreeRTOS task so
// the LVGL UI loop never blocks on a BLE scan.
//
// Standalone library: it has NO dependency on the application's config.h. The
// few app-tunable values (BLE central name, scan duration) have built-in
// defaults and can be overridden via begin(); protocol facts (the Acaia
// heartbeat interval) live privately in scale.cpp.
//
// Exposed (read from the UI/grinder loop, written from the BLE task):
//   connected, weight (g), battery (%), vendor name
//   begin(), tare()
#pragma once

#include <Arduino.h>

enum class Vendor : uint8_t { NONE, ACAIA, BOOKOO, TIMEMORE };

class CoffeeScale {
 public:
  // Live state. Simple 32-bit/byte values; written in the BLE/notify context,
  // read in the main loop. A torn read is harmless for a display value.
  volatile bool  connected = false;
  volatile float weight    = 0.0f;   // grams
  volatile int   battery   = 0;      // percent
  Vendor         vendor    = Vendor::NONE;

  // Start the BLE stack and the background connection task. Pass the last
  // saved MAC + vendor to reconnect directly; empty mac => scan for any
  // supported scale and adopt the first found. deviceName / scanSeconds have
  // sensible defaults so the library is usable on its own; the app passes its
  // config.h values to keep tuning in one place.
  void begin(const String& savedMac, Vendor savedVendor,
             const char* deviceName = "grind-bw",
             uint32_t scanSeconds = 4);

  // Zero the scale. Safe to call from the main loop.
  void tare();

  // The address we are connected to (for persisting). Empty if none.
  String macAddress() const { return _mac; }

  const char* vendorName() const;

  // --- discovery / selection (for the settings picker) ----------------------
  // Every supported scale seen in the most recent scan.
  int  discoveredCount();
  bool discoveredAt(int i, String& name, String& mac, Vendor& v);
  void selectScale(const String& mac, Vendor v);  // choose one; drops + reconnects
  void rescan();                                   // refresh discovery + reconnect
  uint32_t scanGeneration() const { return _scanGen; }  // bumps after each scan

  // Called internally by the BLE task — public only so the C-style NimBLE
  // callbacks can reach them.
  void _onNotify(const uint8_t* data, size_t len);
  void _bleTask();

 private:
  String _mac;          // target / connected address
  String _deviceName = "grind-bw";   // BLE central name (overridable in begin)
  uint32_t _scanSeconds = 4;         // per scan attempt (overridable in begin)

  // resolved characteristics for the active vendor
  void* _client     = nullptr;   // NimBLEClient*
  void* _writeChar  = nullptr;   // NimBLERemoteCharacteristic* (commands)
  void* _notifyChar = nullptr;   // NimBLERemoteCharacteristic* (weight)

  // Acaia state
  bool          _acaiaPyxis = false;
  uint32_t      _lastHeartbeat = 0;
  uint32_t      _heartbeatCount = 0;

  // discovery / selection
  volatile bool     _reconnect = false;   // request: drop link + scan/reconnect
  volatile uint32_t _scanGen   = 0;        // increments each completed scan

  bool scanAndConnect();
  bool setupAcaia();
  bool setupBookoo();
  bool setupTimemore();
  void writeRaw(const uint8_t* data, size_t len);
  void serviceLink();      // heartbeat etc, called periodically while connected
  void dropConnection();

  // protocol decoders
  void acaiaFeed(const uint8_t* data, size_t len);
  void bookooDecode(const uint8_t* data, size_t len);
  void timemoreDecode(const uint8_t* data, size_t len);
};

extern CoffeeScale g_scale;