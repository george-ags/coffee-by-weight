// scale.cpp — NimBLE implementation of CoffeeScale.
//
// Protocol details ported 1:1 from the project's Python drivers
// (common/scale_acaia.py, common/scale_bookoo.py):
//
//  BooKoo  service 0xFFE0; notify 0xFF11 (20-byte weight packet);
//          write 0xFF12 (fixed command frames). No handshake, no heartbeat.
//
//  Acaia   one characteristic does both notify + write. Two layouts:
//            old   00002a80-...   /  Pyxis/Lunar-2021  49535343-8841-...
//          The enclosing service UUID varies, so we find the service that
//          contains the characteristic. Framing is 0xEF 0xDD <cmd> <len> ...
//          with two trailing checksum bytes. Requires an ID + notification
//          handshake and a ~2 s heartbeat or the weight stream stops.
//
//  Timemore Black Mirror family (model TES017, "DOT"). Service 0xFFF0; notify
//          0xFFF1; write 0xFFF2 (write-no-response). Like BooKoo it streams as
//          soon as you subscribe — no handshake, no heartbeat. Framing is
//          0xA5 0x5A <opcode> <cmd> <len:2 BE> <payload> <crc:2>, where len
//          counts payload bytes only. Weight/flow/timer arrive as cmd 0x01
//          every 100 ms, battery as cmd 0x05. Unlike the other two vendors the
//          command frames carry a real CRC the scale verifies, so we compute it
//          (see crc16_modbus). Spec: doc/BT_Scales/Timemore/protocols.md.

#include "scale.h"
#include <NimBLEDevice.h>
#include <vector>
#include <cstring>
#include <strings.h>

CoffeeScale g_scale;

// Acaia stops streaming weight without a periodic heartbeat (~2 s). This is a
// protocol requirement, not an app setting, so it lives here in the library.
static const uint32_t ACAIA_HEARTBEAT_MS = 2000;

// ---- UUIDs -----------------------------------------------------------------
// Reference only — setupBookoo() matches on the ff11/ff12 substring so it also
// works when a model exposes them under a different (or 128-bit) service UUID.
__attribute__((unused)) static const char* BOOKOO_SVC    = "0000ffe0-0000-1000-8000-00805f9b34fb";
__attribute__((unused)) static const char* BOOKOO_NOTIFY = "0000ff11-0000-1000-8000-00805f9b34fb";
__attribute__((unused)) static const char* BOOKOO_WRITE  = "0000ff12-0000-1000-8000-00805f9b34fb";
static const char* ACAIA_OLD     = "00002a80-0000-1000-8000-00805f9b34fb";
static const char* ACAIA_PYXIS   = "49535343-8841-43f4-a8d4-ecbe34729bb3";
// Reference only — setupTimemore() matches on the fff1/fff2 substring, same as
// the BooKoo path, so a model exposing them under a 128-bit service still works.
__attribute__((unused)) static const char* TIMEMORE_SVC    = "0000fff0-0000-1000-8000-00805f9b34fb";
__attribute__((unused)) static const char* TIMEMORE_NOTIFY = "0000fff1-0000-1000-8000-00805f9b34fb";
__attribute__((unused)) static const char* TIMEMORE_WRITE  = "0000fff2-0000-1000-8000-00805f9b34fb";

// ---- name classification ---------------------------------------------------
static const char* ACAIA_PREFIXES[]  = {"ACAIA", "PYXIS", "UMBRA", "LUNAR", "PROCH"};
static const char* BOOKOO_PREFIXES[] = {"BOOKOO"};
// The Timemore advertised name is user-settable; these cover the brand plus the
// Black Mirror model/type strings (TES017 advertises as "DOT"). If your scale
// shows up under a different name, add its prefix here.
static const char* TIMEMORE_PREFIXES[] = {"TIMEMORE", "TES017", "BLACK MIRROR", "BLACKMIRROR"};

static Vendor classify(const std::string& nameIn) {
  if (nameIn.empty()) return Vendor::NONE;
  std::string n = nameIn;
  for (auto& c : n) c = toupper((unsigned char)c);
  for (auto p : ACAIA_PREFIXES)    if (n.find(p) != std::string::npos) return Vendor::ACAIA;
  for (auto p : BOOKOO_PREFIXES)   if (n.find(p) != std::string::npos) return Vendor::BOOKOO;
  for (auto p : TIMEMORE_PREFIXES) if (n.find(p) != std::string::npos) return Vendor::TIMEMORE;
  return Vendor::NONE;
}

// ===========================================================================
// BooKoo command frames (verbatim from spec, see scale_bookoo.py)
// ===========================================================================
// Command frames, verbatim from the vendor spec
// (doc/BT_Scales/BooKooCode/OpenSource/bookoo_ultra_scale/protocols.md).
// The spec's DATASUM column is internally inconsistent with its own stated XOR
// rule for the timer commands, so — as in common/scale_bookoo.py — send the
// exact bytes the spec lists rather than recomputing them.
static const uint8_t BK_TARE[]        = {0x03,0x0A,0x01,0x00,0x00,0x08};
static const uint8_t BK_RESET_TIMER[] = {0x03,0x0A,0x06,0x00,0x00,0x0C};

// Auto-shutdown duration, spec row "03 0A 03 00 05~1e checkSum": the scale's
// idle power-off, adjustable 5–30 minutes. This is what actually stops the
// scale disappearing mid-session — BooKoo has no heartbeat, so no amount of
// BLE traffic keeps it awake; the timeout has to be raised on the scale itself.
// Sent once per connection. Set to 0 to leave the scale's own setting alone.
#define BOOKOO_AUTO_OFF_MIN  5

// A periodic write was tried as a keep-alive and did not help, which the spec
// explains: the shutdown is a scale-side timer, not a link-activity timer.
// Leave disabled.
#define BOOKOO_KEEPALIVE_MS  0

// ===========================================================================
// Timemore Black Mirror framing (see doc/BT_Scales/Timemore/protocols.md)
// ===========================================================================
// TWO VALUES THE VENDOR SPEC DOES NOT PIN DOWN. Both are also open assumptions
// in common/scale_timemore.py; neither has been checked against real hardware.
// Verify them on first connect (see TIMEMORE_LOG_PACKETS below) before trusting
// a dose.
//
//   1. Weight scaling. Spec 5.1 gives weight as a 4-byte Int32 with no unit —
//      it states the unit for flow rate (100 mg/s) but not for weight. 0.01 g
//      per count matches BooKoo and is consistent with weight getting 4 bytes
//      while flow gets 2. If the readout is 10x or 100x off the scale's own
//      display, change this divisor (10.0f for 0.1 g/count, 1000.0f for mg).
static const float TIMEMORE_WEIGHT_DIV = 100.0f;

//   2. CRC byte order on the wire. Spec 3 names the checksum "CRC-16/IBM,
//      poly 0x8005, init 0xFFFF" — that is the MODBUS parameter set, whose
//      native wire order is LSB first — but the same section declares all
//      multi-byte fields big-endian. We follow the section and append MSB
//      first. This only affects the COMMAND path: incoming frames are not
//      CRC-checked, so weight can stream perfectly while the scale silently
//      rejects every tare. If tare does nothing, flip this to 0 first.
#define TIMEMORE_CRC_BIG_ENDIAN 1

// Set to a nonzero count to hex-dump that many notifications, to confirm the
// framing and the weight divisor against the scale's own display. Logging from
// the notify context floods the USB-CDC buffer, so return this to 0 afterwards.
#define TIMEMORE_LOG_PACKETS 0

// CRC-16/MODBUS: poly 0x8005 reflected (0xA001), init 0xFFFF. Verified against
// the canonical check value crc("123456789") == 0x4B37.
static uint16_t crc16_modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

// Build a frame: A5 5A | opcode | cmd | len(2 BE) | payload | crc(2).
static std::vector<uint8_t> timemoreFrame(uint8_t opcode, uint8_t cmd,
                                          const uint8_t* payload, size_t n) {
  std::vector<uint8_t> f;
  f.reserve(n + 8);
  f.push_back(0xA5);
  f.push_back(0x5A);
  f.push_back(opcode);
  f.push_back(cmd);
  f.push_back((uint8_t)((n >> 8) & 0xFF));
  f.push_back((uint8_t)(n & 0xFF));
  for (size_t i = 0; i < n; i++) f.push_back(payload[i]);
  uint16_t crc = crc16_modbus(f.data(), f.size());
#if TIMEMORE_CRC_BIG_ENDIAN
  f.push_back((uint8_t)(crc >> 8)); f.push_back((uint8_t)(crc & 0xFF));
#else
  f.push_back((uint8_t)(crc & 0xFF)); f.push_back((uint8_t)(crc >> 8));
#endif
  return f;
}

// Opcodes (spec 4) and the commands we use.
#define TM_OP_NOTIFY  0x01
#define TM_OP_READ    0x02
#define TM_OP_WRITE   0x03
#define TM_CMD_WEIGHT 0x01
#define TM_CMD_BATT   0x05
#define TM_CMD_TARE   0x0D

// ===========================================================================
// Acaia message encoding (ported from scale_acaia.py)
// ===========================================================================
static std::vector<uint8_t> acaiaEncode(uint8_t msgType, const uint8_t* payload, size_t n) {
  std::vector<uint8_t> b;
  b.reserve(n + 5);
  b.push_back(0xEF);
  b.push_back(0xDD);
  b.push_back(msgType);
  uint8_t c1 = 0, c2 = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t v = payload[i] & 0xFF;
    b.push_back(v);
    if (i % 2 == 0) c1 += v; else c2 += v;
  }
  b.push_back(c1 & 0xFF);
  b.push_back(c2 & 0xFF);
  return b;
}
static std::vector<uint8_t> acaiaEventData(const uint8_t* payload, size_t n) {
  std::vector<uint8_t> wrapped;
  wrapped.push_back((uint8_t)(n + 1));
  for (size_t i = 0; i < n; i++) wrapped.push_back(payload[i] & 0xFF);
  return acaiaEncode(12, wrapped.data(), wrapped.size());
}
static std::vector<uint8_t> acaiaNotificationRequest() {
  static const uint8_t p[] = {0,1,1,2,2,5,3,4};
  return acaiaEventData(p, sizeof(p));
}
static std::vector<uint8_t> acaiaId(bool pyxis) {
  if (pyxis) {
    static const uint8_t p[] = {0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
                                0x38,0x39,0x30,0x31,0x32,0x33,0x34};
    return acaiaEncode(11, p, sizeof(p));
  }
  static uint8_t p[15];
  for (auto& v : p) v = 0x2D;
  return acaiaEncode(11, p, sizeof(p));
}
static std::vector<uint8_t> acaiaHeartbeat() { const uint8_t p[]={2,0}; return acaiaEncode(0,p,2); }
static std::vector<uint8_t> acaiaTare()      { const uint8_t p[]={0};   return acaiaEncode(4,p,1); }

static float acaiaDecodeWeight(const uint8_t* p, size_t n) {
  if (n < 6) return 0.0f;
  uint8_t unit = p[4] & 0xFF;
  float divisor = 10.0f;
  switch (unit) { case 1: divisor=10; break; case 2: divisor=100; break;
                  case 3: divisor=1000; break; case 4: divisor=10000; break; }
  int sign = (p[5] & 0x02) ? -1 : 1;
  uint32_t be = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
  float w = sign * (be / divisor);
  if (fabsf(w) <= 4000.0f) return w;
  uint32_t le = ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0];
  return sign * (le / divisor);
}

// ===========================================================================
// Discovery store — every supported scale seen in the last scan.
// Written from the NimBLE host task (scan callbacks), read from the UI task,
// so the public list is guarded by a spinlock. The parallel address array is
// only touched in the BLE task and holds the properly-typed NimBLEAddress used
// to connect (preserves random vs public addressing that coffee scales use).
// ===========================================================================
struct FoundDev { char name[32]; char mac[18]; Vendor vendor; };
static const int      MAX_FOUND = 10;
static FoundDev       s_found[MAX_FOUND];
static int            s_foundCount = 0;
static portMUX_TYPE   s_foundMux = portMUX_INITIALIZER_UNLOCKED;
static NimBLEAddress  s_foundAddr[MAX_FOUND];

static void addFound(const NimBLEAddress& addr, const char* name, Vendor v) {
  std::string macs = addr.toString();              // heap work BEFORE the lock
  int idx = -1;
  portENTER_CRITICAL(&s_foundMux);
  for (int i = 0; i < s_foundCount; i++)
    if (strcasecmp(s_found[i].mac, macs.c_str()) == 0) { idx = i; break; }
  if (idx < 0 && s_foundCount < MAX_FOUND) idx = s_foundCount++;
  if (idx >= 0) {
    strncpy(s_found[idx].mac,  macs.c_str(), sizeof(s_found[idx].mac) - 1);
    s_found[idx].mac[sizeof(s_found[idx].mac) - 1] = 0;
    strncpy(s_found[idx].name, (name && *name) ? name : "(unnamed)", sizeof(s_found[idx].name) - 1);
    s_found[idx].name[sizeof(s_found[idx].name) - 1] = 0;
    s_found[idx].vendor = v;
  }
  portEXIT_CRITICAL(&s_foundMux);
  if (idx >= 0) s_foundAddr[idx] = addr;            // BLE-task-only, safe outside lock
}

// Collect ALL supported scales; does not stop the scan early.
class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    Vendor v = classify(dev->haveName() ? dev->getName() : std::string());
    if (v == Vendor::NONE) return;
    addFound(dev->getAddress(), dev->haveName() ? dev->getName().c_str() : "", v);
  }
};

// notify trampoline -> instance
static volatile uint32_t s_lastNotifyMs = 0;   // when the scale last sent data
static uint32_t          s_linkUpMs     = 0;   // when the current link came up
static void notifyTrampoline(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  s_lastNotifyMs = millis();
  g_scale._onNotify(data, len);
}

// Client callbacks. NimBLE 1.4.x doesn't pass a disconnect reason (that's a
// 2.x feature), but knowing *when* the link drops relative to the setup traces
// below already localizes the problem (link-level vs post-handshake).
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    Serial.println("[scale] link up");
    Serial.flush();
  }
  void onDisconnect(NimBLEClient*) override {
    Serial.println("[scale] link down");
    Serial.flush();
  }
};
static ClientCB s_clientCB;

// ===========================================================================
// CoffeeScale
// ===========================================================================
const char* CoffeeScale::vendorName() const {
  switch (vendor) { case Vendor::ACAIA:  return "Acaia";
                    case Vendor::BOOKOO: return "BooKoo";
                    case Vendor::TIMEMORE: return "Timemore"; default: return "—"; }
}

static void bleTaskEntry(void* arg) { static_cast<CoffeeScale*>(arg)->_bleTask(); }

void CoffeeScale::begin(const String& savedMac, Vendor savedVendor,
                        const char* deviceName, uint32_t scanSeconds) {
  _mac = savedMac;
  vendor = savedVendor;
  if (deviceName && *deviceName) _deviceName = deviceName;
  if (scanSeconds > 0) _scanSeconds = scanSeconds;
  NimBLEDevice::init(_deviceName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Some scales (BooKoo Ultra among them) ask the central for a security
  // handshake right after connecting. With no I/O capability and just-works
  // pairing configured, NimBLE answers automatically; without this the peer
  // can drop the link mid-discovery, which looks exactly like a GATT failure.
  NimBLEDevice::setSecurityAuth(false, false, false);   // no bonding, no MITM, no SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setMTU(247);                            // 20-byte notifies fit either way
  xTaskCreatePinnedToCore(bleTaskEntry, "ble", 8192, this, 1, nullptr, 0);
}

void CoffeeScale::_bleTask() {
  for (;;) {
    if (_reconnect) {                 // user picked a scale or asked to rescan
      _reconnect = false;
      if (connected) dropConnection();
    }
    if (!connected) {
      scanAndConnect();
      if (!connected) { vTaskDelay(pdMS_TO_TICKS(1200)); continue; }
    }
    serviceLink();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

bool CoffeeScale::scanAndConnect() {
  portENTER_CRITICAL(&s_foundMux);          // fresh discovery list for this scan
  s_foundCount = 0;
  portEXIT_CRITICAL(&s_foundMux);

  NimBLEScan* scan = NimBLEDevice::getScan();
  static ScanCB cb;
  scan->setAdvertisedDeviceCallbacks(&cb, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->start(_scanSeconds, false);         // runs full duration, collecting all
  scan->clearResults();
  _scanGen++;                               // signal the UI that discovery refreshed

  // Device selection:
  //  • If a scale has been chosen (saved MAC set), connect ONLY to that one.
  //    Never adopt a different scale just because it was found first, and don't
  //    connect at all until the chosen one is in range.
  //  • If nothing has ever been chosen (first run), adopt the first supported
  //    scale found; it then becomes the saved/locked scale.
  int pick = -1;
  FoundDev chosen{};
  portENTER_CRITICAL(&s_foundMux);
  int n = s_foundCount;
  if (_mac.length()) {
    for (int i = 0; i < n; i++)
      if (strcasecmp(s_found[i].mac, _mac.c_str()) == 0) { pick = i; break; }
  } else if (n > 0) {
    pick = 0;
  }
  if (pick >= 0) chosen = s_found[pick];
  portEXIT_CRITICAL(&s_foundMux);

  if (pick < 0) {
    if (_mac.length())
      Serial.printf("[scale] chosen scale %s not in range; waiting\n", _mac.c_str());
    return false;
  }

  _mac   = chosen.mac;
  vendor = chosen.vendor;
  NimBLEAddress addr = s_foundAddr[pick];
  Serial.printf("[scale] connecting to %s (%s)\n", _mac.c_str(), vendorName());

  NimBLEClient* client = NimBLEDevice::createClient();
  _client = client;
  client->setClientCallbacks(&s_clientCB, false);
  // Conservative params with generous headroom for link stability.
  // Units: interval x1.25ms (24..40 = 30..50ms), latency 0, timeout x10ms (600 = 6s).
  // 50ms comfortably outpaces the ~10Hz weight stream. The 6s supervision
  // timeout trades slower detection of a genuine drop for far fewer spurious
  // ones — note the grinder's SCALE LOST cut-off inherits that latency.
  client->setConnectionParams(24, 40, 0, 600);
  client->setConnectTimeout(10);            // seconds; default 30 stalls the task too long
  if (!client->connect(addr)) {             // typed address preserves random/public
    Serial.println("[scale] connect failed");
    Serial.flush();
    NimBLEDevice::deleteClient(client); _client = nullptr;
    return false;
  }
  Serial.printf("[scale] connected link (mtu %u), discovering services…\n",
                (unsigned)client->getMTU());
  Serial.flush();

  bool ok;
  switch (vendor) {
    case Vendor::BOOKOO:   ok = setupBookoo();   break;
    case Vendor::TIMEMORE: ok = setupTimemore(); break;
    default:               ok = setupAcaia();    break;
  }
  if (!ok) { dropConnection(); return false; }

  connected = true;
  s_linkUpMs     = millis();
  s_lastNotifyMs = millis();
  Serial.println("[scale] connected");
  Serial.flush();
  return true;
}

// ---- discovery / selection (used by the settings picker) -------------------
int CoffeeScale::discoveredCount() {
  portENTER_CRITICAL(&s_foundMux);
  int n = s_foundCount;
  portEXIT_CRITICAL(&s_foundMux);
  return n;
}

bool CoffeeScale::discoveredAt(int i, String& name, String& mac, Vendor& v) {
  FoundDev tmp{}; bool ok = false;
  portENTER_CRITICAL(&s_foundMux);
  if (i >= 0 && i < s_foundCount) { tmp = s_found[i]; ok = true; }
  portEXIT_CRITICAL(&s_foundMux);
  if (!ok) return false;
  name = tmp.name; mac = tmp.mac; v = tmp.vendor;   // String build outside the lock
  return true;
}

void CoffeeScale::selectScale(const String& mac, Vendor v) {
  _mac = mac; vendor = v; _reconnect = true;        // drop + reconnect to this one
}

void CoffeeScale::rescan() { _reconnect = true; }   // refresh discovery, reconnect

// Standard GATT services that never carry the weight stream — skipped when
// falling back to "first notify characteristic in a vendor service".
// NimBLE prints 16-bit UUIDs short ("0x1800") and 128-bit ones in full, so
// match on the 4 significant hex digits either way.
static bool isGenericService(const std::string& u) {
  static const char* kGeneric[] = {"1800", "1801", "180a", "180f", "fe59"};
  for (auto* g : kGeneric) {
    if (u == std::string("0x") + g) return true;             // short form
    if (u.compare(0, 8, std::string("0000") + g) == 0) return true;  // 128-bit form
  }
  return false;
}

bool CoffeeScale::setupBookoo() {
  auto* client = static_cast<NimBLEClient*>(_client);

  // Give the peer a moment before hammering it with discovery. The Ultra can
  // drop the link if discovery starts the instant the connection completes.
  vTaskDelay(pdMS_TO_TICKS(300));
  if (!client->isConnected()) {
    Serial.println("[scale] BooKoo: peer dropped us before discovery");
    Serial.flush();
    return false;
  }

  // Enumerate everything instead of asking for FFE0 by name: newer BooKoo
  // models don't all use the 0xFFE0 layout, and a plain getService() miss
  // gives no clue what they *do* use.
  std::vector<NimBLERemoteService*>* services = client->getServices(true);
  if (!services || services->empty()) {
    Serial.println("[scale] BooKoo: discovery returned NO services (link dropped)");
    Serial.flush();
    return false;
  }

  NimBLERemoteCharacteristic *notify = nullptr, *write = nullptr;   // exact FF11 / FF12
  NimBLERemoteCharacteristic *anyNotify = nullptr, *anyWrite = nullptr;  // fallback

  Serial.printf("[scale] %u services discovered:\n", (unsigned)services->size());
  Serial.flush();
  for (auto* svc : *services) {
    std::string su = svc->getUUID().toString();
    for (auto& c : su) c = tolower((unsigned char)c);
    Serial.printf("  svc %s\n", su.c_str());
    Serial.flush();
    auto* chars = svc->getCharacteristics(true);
    if (!chars) { Serial.println("    (no characteristics)"); Serial.flush(); continue; }
    for (auto* ch : *chars) {
      std::string u = ch->getUUID().toString();
      for (auto& c : u) c = tolower((unsigned char)c);
      bool canN = ch->canNotify() || ch->canIndicate();
      bool canW = ch->canWrite() || ch->canWriteNoResponse();
      Serial.printf("    chr %s%s%s\n", u.c_str(), canN ? " [notify]" : "", canW ? " [write]" : "");
      Serial.flush();
      vTaskDelay(pdMS_TO_TICKS(5));            // don't outrun the USB-CDC buffer

      if (u.find("ff11") != std::string::npos && canN) notify = ch;
      if (u.find("ff12") != std::string::npos && canW) write  = ch;
      if (!isGenericService(su)) {
        if (!anyNotify && canN) anyNotify = ch;
        if (!anyWrite  && canW) anyWrite  = ch;
      }
    }
  }

  if (!notify && anyNotify) {
    notify = anyNotify;
    Serial.println("[scale] BooKoo: no FF11 — falling back to first vendor notify char");
  }
  if (!write && anyWrite) {
    write = anyWrite;
    Serial.println("[scale] BooKoo: no FF12 — falling back to first vendor write char");
  }
  if (!notify) {
    Serial.println("[scale] BooKoo: no usable notify characteristic in the list above");
    Serial.flush();
    return false;
  }

  // A missing write char is survivable: weight still streams, only tare breaks.
  _notifyChar = notify;
  _writeChar  = write;
  if (!write) Serial.println("[scale] BooKoo: WARNING no write char — tare will not work");

  if (!notify->subscribe(true, notifyTrampoline)) {
    Serial.println("[scale] BooKoo subscribe failed");
    Serial.flush();
    return false;
  }
  Serial.printf("[scale] BooKoo subscribed on %s\n", notify->getUUID().toString().c_str());
  Serial.flush();

#if BOOKOO_AUTO_OFF_MIN
  // Raise the scale's idle power-off timer. The spec gives no example DATASUM
  // for this row (it just says "checkSum"), so compute it with the documented
  // XOR rule — which is the rule the tare frame's published 0x08 confirms.
  if (_writeChar) {
    vTaskDelay(pdMS_TO_TICKS(200));            // let the subscription settle first
    uint8_t mins = BOOKOO_AUTO_OFF_MIN;
    if (mins < 5)  mins = 5;                   // spec range is 5..30 minutes
    if (mins > 30) mins = 30;
    uint8_t f[6] = {0x03, 0x0A, 0x03, 0x00, mins, 0x00};
    f[5] = f[0] ^ f[1] ^ f[2] ^ f[3] ^ f[4];
    writeRaw(f, sizeof(f));
    Serial.printf("[scale] BooKoo: set auto-off to %u min [%02X %02X %02X %02X %02X %02X]\n",
                  (unsigned)mins, f[0], f[1], f[2], f[3], f[4], f[5]);
    Serial.flush();
  }
#endif

  _lastHeartbeat = millis();       // arms the keep-alive clock
  return true;
}

bool CoffeeScale::setupAcaia() {
  auto* client = static_cast<NimBLEClient*>(_client);
  NimBLERemoteCharacteristic* found = nullptr;
  _acaiaPyxis = false;

  // The service UUID is not fixed, so scan every service for the known write/
  // notify characteristic. Dump everything we find so an unrecognized layout is
  // visible in the log.
  std::vector<NimBLERemoteService*>* services = client->getServices(true);
  if (!services || services->empty()) {
    Serial.println("[scale] discovery returned NO services");
    return false;
  }
  Serial.printf("[scale] %u services discovered:\n", (unsigned)services->size());
  for (auto* svc : *services) {
    Serial.printf("  svc %s\n", svc->getUUID().toString().c_str());
    auto* chars = svc->getCharacteristics(true);
    if (!chars) { Serial.println("    (no characteristics)"); continue; }
    for (auto* ch : *chars) {
      std::string u = ch->getUUID().toString();
      Serial.printf("    chr %s%s%s\n", u.c_str(),
                    ch->canNotify() ? " [notify]" : "",
                    (ch->canWrite() || ch->canWriteNoResponse()) ? " [write]" : "");
      for (auto& c : u) c = tolower((unsigned char)c);
      if (u.find("49535343-8841") != std::string::npos) {        // Pyxis/Lunar-2021
        found = ch; _acaiaPyxis = true;
      } else if (!found && u.find("2a80") != std::string::npos) { // older Acaia
        found = ch; _acaiaPyxis = false;
      }
    }
  }
  if (!found) { Serial.println("[scale] no known Acaia characteristic in the list above"); return false; }

  _notifyChar = found; _writeChar = found;
  if (!found->subscribe(true, notifyTrampoline)) { Serial.println("[scale] Acaia subscribe failed"); return false; }
  Serial.printf("[scale] Acaia char found (%s), sending handshake\n", _acaiaPyxis ? "pyxis" : "old");

  // Handshake (ID, notification request x2, heartbeat).
  auto id  = acaiaId(_acaiaPyxis);    writeRaw(id.data(), id.size());     delay(200);
  auto nr1 = acaiaNotificationRequest(); writeRaw(nr1.data(), nr1.size()); delay(200);
  auto nr2 = acaiaNotificationRequest(); writeRaw(nr2.data(), nr2.size()); delay(200);
  auto hb  = acaiaHeartbeat();        writeRaw(hb.data(), hb.size());
  _lastHeartbeat = millis(); _heartbeatCount = 0;
  return true;
}

bool CoffeeScale::setupTimemore() {
  auto* client = static_cast<NimBLEClient*>(_client);

  // Same shape as setupBookoo(): enumerate everything and match on the 4
  // significant hex digits, so a model that publishes FFF1/FFF2 under a
  // 128-bit or non-FFF0 service still resolves. Falls back to the first
  // notify/write pair in a non-generic service, and logs the whole layout so an
  // unrecognized one is visible.
  vTaskDelay(pdMS_TO_TICKS(300));
  if (!client->isConnected()) {
    Serial.println("[scale] Timemore: peer dropped us before discovery");
    Serial.flush();
    return false;
  }

  std::vector<NimBLERemoteService*>* services = client->getServices(true);
  if (!services || services->empty()) {
    Serial.println("[scale] Timemore: discovery returned NO services (link dropped)");
    Serial.flush();
    return false;
  }

  NimBLERemoteCharacteristic *notify = nullptr, *write = nullptr;        // exact FFF1 / FFF2
  NimBLERemoteCharacteristic *anyNotify = nullptr, *anyWrite = nullptr;  // fallback

  Serial.printf("[scale] %u services discovered:\n", (unsigned)services->size());
  Serial.flush();
  for (auto* svc : *services) {
    std::string su = svc->getUUID().toString();
    for (auto& c : su) c = tolower((unsigned char)c);
    Serial.printf("  svc %s\n", su.c_str());
    Serial.flush();
    auto* chars = svc->getCharacteristics(true);
    if (!chars) { Serial.println("    (no characteristics)"); Serial.flush(); continue; }
    for (auto* ch : *chars) {
      std::string u = ch->getUUID().toString();
      for (auto& c : u) c = tolower((unsigned char)c);
      bool canN = ch->canNotify() || ch->canIndicate();
      bool canW = ch->canWrite() || ch->canWriteNoResponse();
      Serial.printf("    chr %s%s%s\n", u.c_str(), canN ? " [notify]" : "", canW ? " [write]" : "");
      Serial.flush();
      vTaskDelay(pdMS_TO_TICKS(5));            // don't outrun the USB-CDC buffer

      if (u.find("fff1") != std::string::npos && canN) notify = ch;
      if (u.find("fff2") != std::string::npos && canW) write  = ch;
      if (!isGenericService(su)) {
        if (!anyNotify && canN) anyNotify = ch;
        if (!anyWrite  && canW) anyWrite  = ch;
      }
    }
  }

  if (!notify && anyNotify) {
    notify = anyNotify;
    Serial.println("[scale] Timemore: no FFF1 — falling back to first vendor notify char");
  }
  if (!write && anyWrite) {
    write = anyWrite;
    Serial.println("[scale] Timemore: no FFF2 — falling back to first vendor write char");
  }
  if (!notify) {
    Serial.println("[scale] Timemore: no usable notify characteristic in the list above");
    Serial.flush();
    return false;
  }

  // A missing write char is survivable: weight still streams, only tare breaks.
  _notifyChar = notify;
  _writeChar  = write;
  if (!write) Serial.println("[scale] Timemore: WARNING no write char — tare will not work");

  // Subscribing starts the 100 ms weight stream: no handshake, no heartbeat.
  if (!notify->subscribe(true, notifyTrampoline)) {
    Serial.println("[scale] Timemore subscribe failed");
    Serial.flush();
    return false;
  }
  Serial.printf("[scale] Timemore subscribed on %s\n", notify->getUUID().toString().c_str());
  Serial.flush();

  // Battery is read/notify and may not push until it changes, so ask once.
  // Harmless if the scale ignores it — and the first thing to look at if the
  // CRC byte order is wrong, since this is a command like any other.
  if (_writeChar) {
    vTaskDelay(pdMS_TO_TICKS(200));            // let the subscription settle first
    auto rb = timemoreFrame(TM_OP_READ, TM_CMD_BATT, nullptr, 0);
    writeRaw(rb.data(), rb.size());
  }
  return true;
}

void CoffeeScale::writeRaw(const uint8_t* data, size_t len) {
  auto* ch = static_cast<NimBLERemoteCharacteristic*>(_writeChar);
  if (!connected && _client == nullptr) return;
  if (!ch) return;
  ch->writeValue(const_cast<uint8_t*>(data), len, false);  // write-no-response
}

void CoffeeScale::serviceLink() {
  auto* client = static_cast<NimBLEClient*>(_client);
  if (!client || !client->isConnected()) {
    uint32_t now = millis();
    Serial.printf("[scale] LINK LOST after %.1fs up; last data %.1fs ago\n",
                  (now - s_linkUpMs) / 1000.0f, (now - s_lastNotifyMs) / 1000.0f);
    Serial.flush();
    dropConnection();
    return;
  }

  // Periodic pulse so the monitor brackets when things stop. The gap between
  // "last data" growing and the link dropping is the tell: if the scale stops
  // streaming well before the link dies, it powered itself down. If data flows
  // right up to an abrupt drop, it is an RF/link problem instead.
  {
    static uint32_t s_lastAliveLog = 0;
    uint32_t now = millis();
    if (now - s_lastAliveLog >= 15000) {
      s_lastAliveLog = now;
      Serial.printf("[scale] alive: up %.0fs, last data %.1fs ago, w=%.2f batt=%d%%\n",
                    (now - s_linkUpMs) / 1000.0f, (now - s_lastNotifyMs) / 1000.0f,
                    weight, battery);
      Serial.flush();
    }
  }

  if (vendor == Vendor::ACAIA && _notifyChar) {
    uint32_t now = millis();
    if (now - _lastHeartbeat >= ACAIA_HEARTBEAT_MS) {
      auto hb = acaiaHeartbeat();
      writeRaw(hb.data(), hb.size());
      _lastHeartbeat = now;
      if (++_heartbeatCount >= 10) {           // periodic re-arm, as in Python
        auto id = acaiaId(_acaiaPyxis); writeRaw(id.data(), id.size());
        auto nr = acaiaNotificationRequest(); writeRaw(nr.data(), nr.size());
        _heartbeatCount = 0;
      }
    }
  }
#if BOOKOO_KEEPALIVE_MS
  else if (vendor == Vendor::BOOKOO && _writeChar) {
    // Not a protocol requirement — purely to stop the scale idling itself off.
    // Reuses _lastHeartbeat as "time of last write to the scale".
    uint32_t now = millis();
    if (now - _lastHeartbeat >= (uint32_t)BOOKOO_KEEPALIVE_MS) {
      writeRaw(BK_RESET_TIMER, sizeof(BK_RESET_TIMER));
      _lastHeartbeat = now;
    }
  }
#endif
}

void CoffeeScale::dropConnection() {
  connected = false;
  weight = 0.0f;
  auto* client = static_cast<NimBLEClient*>(_client);
  // Null the handles first: nothing else may touch the client while it tears down.
  _client = _writeChar = _notifyChar = nullptr;
  if (client) {
    if (client->isConnected()) {
      client->disconnect();
      // Deleting a client whose disconnect is still in flight can wedge the
      // NimBLE host, which makes the *next* connect fail for unrelated reasons.
      for (int i = 0; i < 50 && client->isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    NimBLEDevice::deleteClient(client);
  }
  Serial.println("[scale] disconnected");
  Serial.flush();
}

void CoffeeScale::tare() {
  if (!connected) return;
  if (vendor == Vendor::BOOKOO) {
    writeRaw(BK_TARE, sizeof(BK_TARE));
  } else if (vendor == Vendor::ACAIA) {
    auto t = acaiaTare();
    writeRaw(t.data(), t.size());
  } else if (vendor == Vendor::TIMEMORE) {
    auto t = timemoreFrame(TM_OP_WRITE, TM_CMD_TARE, nullptr, 0);
    writeRaw(t.data(), t.size());
  }
}

// ---- notification dispatch -------------------------------------------------
void CoffeeScale::_onNotify(const uint8_t* data, size_t len) {
  if      (vendor == Vendor::BOOKOO)   bookooDecode(data, len);
  else if (vendor == Vendor::TIMEMORE) timemoreDecode(data, len);
  else                                 acaiaFeed(data, len);
}

// Set to a nonzero count to hex-dump that many notifications in the monitor —
// useful when a new model connects fine but the weight sticks at 0.0.
// Confirmed for the BooKoo Ultra (frames are standard 03 0B ..., 20 bytes), so
// leave at 0: logging from the notify context floods the USB-CDC buffer and
// garbles surrounding output.
#define BOOKOO_LOG_PACKETS 0

void CoffeeScale::bookooDecode(const uint8_t* p, size_t len) {
#if BOOKOO_LOG_PACKETS
  static int s_logged = 0;
  if (s_logged < BOOKOO_LOG_PACKETS) {
    s_logged++;
    Serial.printf("[scale] bookoo pkt (%u): ", (unsigned)len);
    for (size_t i = 0; i < len && i < 24; i++) Serial.printf("%02X ", p[i]);
    Serial.println();
    Serial.flush();
  }
#endif
  if (len < 20) return;
  if (p[0] != 0x03 || p[1] != 0x0B) return;          // product / type
  uint32_t raw = ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
  float w = raw / 100.0f;
  if (p[6] == 0x2D || (p[6] & 0x80)) w = -w;          // sign byte
  weight = w;
  int b = p[13];                                      // battery %
  battery = b < 0 ? 0 : (b > 100 ? 100 : b);

  // The scale reports its own auto-standby setting in p[14..15] (minutes).
  // Log it once per connection: if the scale keeps powering off but this says
  // 60, the culprit is a separate inactivity setting in the BooKoo app, not
  // the standby timer, and no amount of BLE traffic will change it.
  static uint16_t s_lastStandby = 0xFFFF;
  uint16_t standby = ((uint16_t)p[14] << 8) | p[15];
  if (standby != s_lastStandby) {
    s_lastStandby = standby;
    // p[5] is the scale's display unit (0x01 ounce, 0x02 gram). The payload
    // weight is always grams regardless, so this is informational only.
    Serial.printf("[scale] BooKoo auto-off now %u min (battery %d%%, display %s)\n",
                  (unsigned)standby, battery, p[5] == 0x01 ? "oz" : "g");
    Serial.flush();
  }
}

void CoffeeScale::acaiaFeed(const uint8_t* data, size_t len) {
  // Per-connection reassembly buffer (notify runs single-threaded in host ctx).
  static std::vector<uint8_t> buf;
  buf.insert(buf.end(), data, data + len);

  for (;;) {
    int start = -1;
    for (size_t k = 0; k + 1 < buf.size(); k++)
      if (buf[k] == 0xEF && buf[k + 1] == 0xDD) { start = (int)k; break; }
    if (start < 0) {                                  // no header; keep last byte
      if (buf.size() > 1) buf.erase(buf.begin(), buf.end() - 1);
      return;
    }
    if (buf.size() - start < 6) {                     // header but too short
      if (start > 0) buf.erase(buf.begin(), buf.begin() + start);
      return;
    }
    uint8_t payloadLen = buf[start + 3];
    size_t  msgEnd = start + payloadLen + 5;
    if (msgEnd > buf.size()) {                         // wait for the rest
      if (start > 0) buf.erase(buf.begin(), buf.begin() + start);
      return;
    }
    uint8_t cmd = buf[start + 2];
    if (cmd == 12) {                                   // event-data
      uint8_t msgType = buf[start + 4];
      const uint8_t* pl = &buf[start + 5];
      size_t pn = msgEnd - (start + 5);
      if (msgType == 5) {
        weight = acaiaDecodeWeight(pl, pn);
      } else if (msgType == 11 && pn >= 3 && pl[2] == 5) {
        weight = acaiaDecodeWeight(pl + 3, pn - 3);
      }
    } else if (cmd == 8) {                             // settings (battery/units)
      const uint8_t* s = &buf[start + 3];
      if ((size_t)(start + 3 + 2) <= buf.size()) {
        int b = s[1] & 0x7F;
        battery = b < 0 ? 0 : (b > 100 ? 100 : b);
      }
    }
    buf.erase(buf.begin(), buf.begin() + msgEnd);
  }
}

// ---- Timemore Black Mirror decode ------------------------------------------
// Frames: A5 5A | opcode | cmd | len(2 BE) | payload | crc(2), len counting the
// payload only. One notification normally carries one complete frame, but we
// walk the buffer in case two are concatenated and resync on a bad header.
//
// RX frames are NOT CRC-checked: the header + length gate is enough for a
// display value, and being strict here would mean dropping weight updates over
// a checksum whose wire byte order the spec leaves ambiguous (see
// TIMEMORE_CRC_BIG_ENDIAN). Getting the CRC right only matters on the command
// path, where the scale is the one verifying it.
void CoffeeScale::timemoreDecode(const uint8_t* p, size_t len) {
#if TIMEMORE_LOG_PACKETS
  static int s_dumped = 0;
  if (s_dumped < TIMEMORE_LOG_PACKETS) {
    s_dumped++;
    Serial.printf("[scale] TM rx %u:", (unsigned)len);
    for (size_t k = 0; k < len; k++) Serial.printf(" %02X", p[k]);
    Serial.println();
    Serial.flush();
  }
#endif
  size_t i = 0;
  while (i + 8 <= len) {                        // min frame = 6 header + 0 payload + 2 crc
    if (p[i] != 0xA5 || p[i + 1] != 0x5A) { i++; continue; }
    uint8_t  cmd  = p[i + 3];
    uint16_t plen = ((uint16_t)p[i + 4] << 8) | p[i + 5];
    size_t   end  = i + 6 + plen + 2;           // header .. payload .. crc
    if (end > len) break;                       // incomplete frame; wait for more
    const uint8_t* d = &p[i + 6];

    if (cmd == TM_CMD_WEIGHT && plen >= 4) {
      // payload: weight Int32 BE | flow Int16 | timer uInt16 | overload uint8.
      // Flow and timer are decoded by the Python driver but unused here — the
      // grind loop only ever reads `weight`.
      int32_t raw = (int32_t)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                              ((uint32_t)d[2] << 8)  |  (uint32_t)d[3]);
      weight = raw / TIMEMORE_WEIGHT_DIV;
    } else if (cmd == TM_CMD_BATT && plen >= 2) {
      int b = d[1];                             // payload: [bars][percent]
      battery = b < 0 ? 0 : (b > 100 ? 100 : b);
    }
    i = end;
  }
}
