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

// ---- name classification ---------------------------------------------------
static const char* ACAIA_PREFIXES[]  = {"ACAIA", "PYXIS", "UMBRA", "LUNAR", "PROCH"};
static const char* BOOKOO_PREFIXES[] = {"BOOKOO"};

static Vendor classify(const std::string& nameIn) {
  if (nameIn.empty()) return Vendor::NONE;
  std::string n = nameIn;
  for (auto& c : n) c = toupper((unsigned char)c);
  for (auto p : ACAIA_PREFIXES)  if (n.find(p) != std::string::npos) return Vendor::ACAIA;
  for (auto p : BOOKOO_PREFIXES) if (n.find(p) != std::string::npos) return Vendor::BOOKOO;
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

// ===========================================================================
// CoffeeScale
// ===========================================================================
const char* CoffeeScale::vendorName() const {
  switch (vendor) { case Vendor::ACAIA: return "Acaia";
                    case Vendor::BOOKOO: return "BooKoo"; default: return "—"; }
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

  // Pick the saved/target scale if it's present, otherwise the first found.
  int pick = -1;
  FoundDev chosen{};
  portENTER_CRITICAL(&s_foundMux);
  int n = s_foundCount;
  if (_mac.length())
    for (int i = 0; i < n; i++)
      if (strcasecmp(s_found[i].mac, _mac.c_str()) == 0) { pick = i; break; }
  if (pick < 0 && n > 0) pick = 0;
  if (pick >= 0) chosen = s_found[pick];
  portEXIT_CRITICAL(&s_foundMux);

  if (pick < 0) return false;               // no supported scale in range

  _mac   = chosen.mac;
  vendor = chosen.vendor;
  NimBLEAddress addr = s_foundAddr[pick];
  Serial.printf("[scale] connecting to %s (%s)\n", _mac.c_str(), vendorName());

  NimBLEClient* client = NimBLEDevice::createClient();
  _client = client;
  if (!client->connect(addr)) {             // typed address preserves random/public
    Serial.println("[scale] connect failed");
    NimBLEDevice::deleteClient(client); _client = nullptr;
    return false;
  }

  bool ok = (vendor == Vendor::BOOKOO) ? setupBookoo() : setupAcaia();
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
  if (!svc) return false;
  auto* notify = svc->getCharacteristic(BOOKOO_NOTIFY);
  auto* write  = svc->getCharacteristic(BOOKOO_WRITE);
  if (!notify || !write) return false;
  _notifyChar = notify; _writeChar = write;
  if (!notify->subscribe(true, notifyTrampoline)) return false;
  return true;
}

bool CoffeeScale::setupAcaia() {
  auto* client = static_cast<NimBLEClient*>(_client);
  NimBLERemoteCharacteristic* found = nullptr;
  _acaiaPyxis = false;

  // The service UUID is not fixed: scan every service for the known char.
  std::vector<NimBLERemoteService*>* services = client->getServices(true);
  if (!services) return false;
  for (auto* svc : *services) {
    auto* chars = svc->getCharacteristics(true);
    if (!chars) continue;
    for (auto* ch : *chars) {
      std::string u = ch->getUUID().toString();
      for (auto& c : u) c = tolower((unsigned char)c);
      if (u.find("49535343-8841-43f4-a8d4-ecbe34729bb3") != std::string::npos) {
        found = ch; _acaiaPyxis = true; break;
      } else if (u.find("00002a80-0000-1000-8000-00805f9b34fb") != std::string::npos) {
        found = ch; _acaiaPyxis = false; break;
      }
    }
    if (found) break;
  }
  if (!found) return false;

  _notifyChar = found; _writeChar = found;
  if (!found->subscribe(true, notifyTrampoline)) return false;

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
  } else if (vendor == Vendor::ACAIA) {
    auto t = acaiaTare();
    writeRaw(t.data(), t.size());
  }
}

// ---- notification dispatch -------------------------------------------------
void CoffeeScale::_onNotify(const uint8_t* data, size_t len) {
  if (vendor == Vendor::BOOKOO) bookooDecode(data, len);
  else                          acaiaFeed(data, len);
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