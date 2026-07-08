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
//  Timemore  Black Mirror family (model TES017). Service 0xFFF0; notify 0xFFF1;
//          write 0xFFF2 (write-no-response). Like BooKoo it streams once you
//          subscribe — no handshake, no heartbeat. Frames are
//          0xA5 0x5A <opcode> <cmd> <len:2 BE> <payload> <crc:2>, len counting
//          payload only. Weight/flow/timer arrive as cmd 0x01 every 100 ms;
//          battery as cmd 0x05. Commands (tare = opcode 0x03 cmd 0x0D) carry a
//          CRC-16/MODBUS the scale checks, so unlike the others we must compute
//          it (see crc16_modbus). See doc/BT_Scale/Timemore/protocols.md.

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
static const char* BOOKOO_SVC    = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char* BOOKOO_NOTIFY = "0000ff11-0000-1000-8000-00805f9b34fb";
static const char* BOOKOO_WRITE  = "0000ff12-0000-1000-8000-00805f9b34fb";
static const char* ACAIA_OLD     = "00002a80-0000-1000-8000-00805f9b34fb";
static const char* ACAIA_PYXIS   = "49535343-8841-43f4-a8d4-ecbe34729bb3";
static const char* TIMEMORE_SVC    = "0000fff0-0000-1000-8000-00805f9b34fb";
static const char* TIMEMORE_NOTIFY = "0000fff1-0000-1000-8000-00805f9b34fb";
static const char* TIMEMORE_WRITE  = "0000fff2-0000-1000-8000-00805f9b34fb";

// ---- name classification ---------------------------------------------------
static const char* ACAIA_PREFIXES[]  = {"ACAIA", "PYXIS", "UMBRA", "LUNAR", "PROCH"};
static const char* BOOKOO_PREFIXES[] = {"BOOKOO"};
// The Timemore advertised name is user-settable; these cover the brand and the
// Black Mirror model/type strings (TES017 = "DOT"). If your scale advertises
// under a different name, add its prefix here.
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
static const uint8_t BK_TARE[]        = {0x03,0x0A,0x01,0x00,0x00,0x08};
// (start/stop/reset timer frames exist but a grinder doesn't need them)

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
// Timemore Black Mirror framing (see doc/BT_Scale/Timemore/protocols.md)
// ===========================================================================
// Weight scaling is NOT stated in the protocol doc. This assumes 0.01 g per
// count (matching BooKoo, and consistent with weight being a 4-byte Int32 while
// flow is only 2 bytes). VERIFY against the scale's own display: if the readout
// is 10x/100x off, change this divisor (e.g. 10.0f for 0.1 g/count).
static const float TIMEMORE_WEIGHT_DIV = 100.0f;

// CRC-16/MODBUS: poly 0x8005 (reflected 0xA001), init 0xFFFF. The doc names it
// "CRC-16/IBM" with init 0xFFFF, which is the Modbus parameter set — the usual
// choice for a UART-style BLE pass-through. Verified against the canonical
// check value crc("123456789") == 0x4B37.
static uint16_t crc16_modbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

// Build a Timemore frame: A5 5A | opcode | cmd | len(2 BE) | payload | crc(2).
// The doc declares multi-byte fields big-endian, so the CRC is appended MSB
// first. If the scale rejects commands (tare has no visible effect), the most
// likely fix is swapping these two CRC bytes — Modbus's own wire order is LSB
// first, and docs sometimes disagree with the transport on this one field.
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
  for (size_t i = 0; i < n; i++) f.push_back(payload[i] & 0xFF);
  uint16_t crc = crc16_modbus(f.data(), f.size());
  f.push_back((uint8_t)((crc >> 8) & 0xFF));
  f.push_back((uint8_t)(crc & 0xFF));
  return f;
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
static void notifyTrampoline(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  g_scale._onNotify(data, len);
}

// Client callbacks. NimBLE 1.4.x doesn't pass a disconnect reason (that's a
// 2.x feature), but knowing *when* the link drops relative to the setup traces
// below already localizes the problem (link-level vs post-handshake).
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    Serial.println("[scale] link up");
  }
  void onDisconnect(NimBLEClient*) override {
    Serial.println("[scale] link down");
  }
};
static ClientCB s_clientCB;

// ===========================================================================
// CoffeeScale
// ===========================================================================
const char* CoffeeScale::vendorName() const {
  switch (vendor) { case Vendor::ACAIA: return "Acaia";
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
  // Conservative params + generous (4 s) supervision timeout for link stability.
  // Units: interval x1.25ms (12..24 = 15..30ms), latency 0, timeout x10ms (400 = 4s).
  client->setConnectionParams(12, 24, 0, 400);
  if (!client->connect(addr)) {             // typed address preserves random/public
    Serial.println("[scale] connect failed");
    NimBLEDevice::deleteClient(client); _client = nullptr;
    return false;
  }
  Serial.println("[scale] connected link, discovering services…");

  bool ok;
  switch (vendor) {
    case Vendor::BOOKOO:   ok = setupBookoo();   break;
    case Vendor::TIMEMORE: ok = setupTimemore(); break;
    default:               ok = setupAcaia();    break;
  }
  if (!ok) { dropConnection(); return false; }

  connected = true;
  Serial.println("[scale] connected");
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

bool CoffeeScale::setupBookoo() {
  auto* client = static_cast<NimBLEClient*>(_client);
  NimBLERemoteService* svc = client->getService(BOOKOO_SVC);
  if (!svc) { Serial.println("[scale] BooKoo service FFE0 not found"); return false; }
  auto* notify = svc->getCharacteristic(BOOKOO_NOTIFY);
  auto* write  = svc->getCharacteristic(BOOKOO_WRITE);
  if (!notify || !write) { Serial.println("[scale] BooKoo chars not found"); return false; }
  _notifyChar = notify; _writeChar = write;
  if (!notify->subscribe(true, notifyTrampoline)) { Serial.println("[scale] BooKoo subscribe failed"); return false; }
  Serial.println("[scale] BooKoo subscribed");
  return true;
}

bool CoffeeScale::setupTimemore() {
  auto* client = static_cast<NimBLEClient*>(_client);
  NimBLERemoteService* svc = client->getService(TIMEMORE_SVC);
  if (!svc) { Serial.println("[scale] Timemore service FFF0 not found"); return false; }
  auto* notify = svc->getCharacteristic(TIMEMORE_NOTIFY);
  auto* write  = svc->getCharacteristic(TIMEMORE_WRITE);
  if (!notify || !write) { Serial.println("[scale] Timemore chars FFF1/FFF2 not found"); return false; }
  _notifyChar = notify; _writeChar = write;
  // Subscribing enables the 0x01 weight stream (every 100 ms) — no handshake or
  // heartbeat, same as BooKoo.
  if (!notify->subscribe(true, notifyTrampoline)) { Serial.println("[scale] Timemore subscribe failed"); return false; }
  Serial.println("[scale] Timemore subscribed");
  // Best-effort: request the current battery level (0x05 is read/notify and may
  // not push until it changes). Harmless if the scale ignores it.
  auto rb = timemoreFrame(0x02, 0x05, nullptr, 0);
  writeRaw(rb.data(), rb.size());
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

void CoffeeScale::writeRaw(const uint8_t* data, size_t len) {
  auto* ch = static_cast<NimBLERemoteCharacteristic*>(_writeChar);
  if (!connected && _client == nullptr) return;
  if (!ch) return;
  ch->writeValue(const_cast<uint8_t*>(data), len, false);  // write-no-response
}

void CoffeeScale::serviceLink() {
  auto* client = static_cast<NimBLEClient*>(_client);
  if (!client || !client->isConnected()) { dropConnection(); return; }

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
}

void CoffeeScale::dropConnection() {
  connected = false;
  weight = 0.0f;
  auto* client = static_cast<NimBLEClient*>(_client);
  if (client) {
    if (client->isConnected()) client->disconnect();
    NimBLEDevice::deleteClient(client);
  }
  _client = _writeChar = _notifyChar = nullptr;
  Serial.println("[scale] disconnected");
}

void CoffeeScale::tare() {
  if (!connected) return;
  if (vendor == Vendor::BOOKOO) {
    writeRaw(BK_TARE, sizeof(BK_TARE));
  } else if (vendor == Vendor::TIMEMORE) {
    auto t = timemoreFrame(0x03, 0x0D, nullptr, 0);   // opcode Write, cmd Tare
    writeRaw(t.data(), t.size());
  } else if (vendor == Vendor::ACAIA) {
    auto t = acaiaTare();
    writeRaw(t.data(), t.size());
  }
}

// ---- notification dispatch -------------------------------------------------
void CoffeeScale::_onNotify(const uint8_t* data, size_t len) {
  switch (vendor) {
    case Vendor::BOOKOO:   bookooDecode(data, len);   break;
    case Vendor::TIMEMORE: timemoreDecode(data, len); break;
    default:               acaiaFeed(data, len);      break;
  }
}

void CoffeeScale::bookooDecode(const uint8_t* p, size_t len) {
  if (len < 20) return;
  if (p[0] != 0x03 || p[1] != 0x0B) return;          // product / type
  uint32_t raw = ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
  float w = raw / 100.0f;
  if (p[6] == 0x2D || (p[6] & 0x80)) w = -w;          // sign byte
  weight = w;
  int b = p[13];                                      // battery %
  battery = b < 0 ? 0 : (b > 100 ? 100 : b);
}

void CoffeeScale::timemoreDecode(const uint8_t* p, size_t len) {
  // Frames: A5 5A | opcode | cmd | len(2 BE) | payload | crc(2). A notification
  // normally carries one complete frame; walk the buffer in case two are
  // concatenated, and resync on a bad header. CRC is not checked on RX — the
  // header + length gate is enough for a display value (the scale computes its
  // own CRC; we only need ours right on the command path).
  size_t i = 0;
  while (i + 8 <= len) {                              // min frame = 6 hdr + 0 payload + 2 crc
    if (p[i] != 0xA5 || p[i + 1] != 0x5A) { i++; continue; }
    uint8_t  cmd  = p[i + 3];
    uint16_t plen = ((uint16_t)p[i + 4] << 8) | p[i + 5];
    size_t   end  = i + 6 + plen + 2;                 // header+len .. payload .. crc
    if (end > len) break;                             // incomplete frame
    const uint8_t* d = &p[i + 6];
    if (cmd == 0x01 && plen >= 4) {                   // weight / flow / timer
      int32_t raw = (int32_t)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                              ((uint32_t)d[2] << 8)  |  (uint32_t)d[3]);
      weight = raw / TIMEMORE_WEIGHT_DIV;
    } else if (cmd == 0x05 && plen >= 2) {            // battery: [bars][percent]
      int bat = d[1];
      battery = bat < 0 ? 0 : (bat > 100 ? 100 : bat);
    }
    i = end;
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