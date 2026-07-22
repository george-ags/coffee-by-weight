#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Timemore Bluetooth scale driver (SimplePyBLE).
#
# Mirrors the public interface of common.scale_acaia.AcaiaScale and
# common.scale_bookoo.BookooScale so the rest of the application can use any
# vendor interchangeably:
#   attributes: mac, connected, weight, battery, units
#   methods:    connect(), disconnect(), tare()
#
# Protocol reference:
#   doc/BT_Scale/Timemore/protocols.md   (Timemore Black Mirror family, v1.0.3)
#
# Supports the Timemore "Black Mirror" scale family (device type TES017). The
# wire protocol is closest to BooKoo's (binary framing over two separate
# characteristics — one notify, one write — with no handshake and no heartbeat),
# so this driver is structured like scale_bookoo.py rather than scale_acaia.py.
#
# Framing (see protocols.md §3):
#   Header 0xA55A | Opcode 1B | Cmd 1B | Length 2B (BE, payload only) | Payload | CRC 2B
# Opcodes: Notify 0x01, Read 0x02, Write 0x03. The scale streams a weight frame
# (cmd 0x01) every 100 ms once active; battery (0x05) and unit (0x06) arrive as
# their own notify frames and can also be polled with a Read.
#
# =============================================================================
# IMPORTANT — assumptions that need confirming against real hardware
# =============================================================================
# The published protocol omits two things this driver has to guess. Both are
# isolated to a single named constant so they are trivial to correct once the
# scale can be observed:
#
#   1. WEIGHT_SCALE / FLOW_SCALE — the spec gives the weight as a raw signed
#      Int32 and the flow as a raw Int16 but never states the fixed-point scale.
#      We assume 0.01-unit resolution (raw / 100 -> grams), matching BooKoo and
#      the spec's "minimum step 0.01" hint. If reported weight is off by a power
#      of ten, adjust WEIGHT_SCALE (e.g. 10.0 for 0.1 g resolution, 1000.0 for
#      milligrams). Weight is on the critical path for LM-BBW; verify it first.
#
#   2. CRC on-wire byte order — the spec calls the checksum "CRC-16/IBM
#      (poly 0x8005, init 0xFFFF)", which is the reflected/MODBUS-style CRC-16
#      computed below. The spec says multi-byte fields are big-endian, so we
#      append the CRC MSB-first by default (CRC_BIG_ENDIAN = True). Incoming
#      frames are NOT dropped on a CRC mismatch (it is advisory only), so weight
#      streaming works regardless of this choice. It only matters for OUTGOING
#      commands (tare, timer, power-off): if the scale ignores them, flip
#      CRC_BIG_ENDIAN first, and if that still fails try init 0x0000 in crc16().
# =============================================================================

__version__ = "0.1.0-timemore-blackmirror"

import logging
import time
import threading
import struct
from typing import List, Tuple

from common.ble import adapter_scan_lock

try:
    import simplepyble
except ImportError:
    logging.fatal("SimplePyBLE not installed. Run: pip3 install simplepyble")
    raise

# --- BLE UUIDs ---
# The spec gives 16-bit UUIDs that expand to the standard Bluetooth base UUID:
#   0000xxxx-0000-1000-8000-00805F9B34FB
TIMEMORE_SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"   # custom service 0xFFF0
TIMEMORE_NOTIFY_UUID  = "0000fff1-0000-1000-8000-00805f9b34fb"   # Uplink   (Notify)
TIMEMORE_WRITE_UUID   = "0000fff2-0000-1000-8000-00805f9b34fb"   # Downlink (Write w/o response)

# Name tokens Timemore scales advertise with, matched as substrings (see
# scales.classify_vendor). The protocol documents the model string "TES017" and
# manufacturer "TIMEMORE" but not the exact advertised scan-response name, so we
# match both likely tokens. If your unit advertises under a different name, add
# it here (or just pick it on the manual Bluetooth setup page, which can select
# an unrecognized device by MAC regardless of name).
# NB: do NOT add short/generic tokens like "DOT" — they cause false positives on
# unrelated BLE devices, the same lesson scale_bookoo.py learned with "_SC".
TIMEMORE_NAME_PREFIXES = ['TIMEMORE', 'TES017']

# --- Frame framing constants (protocols.md §3/§4) ---
HEADER1 = 0xA5
HEADER2 = 0x5A

OP_NOTIFY = 0x01
OP_READ   = 0x02
OP_WRITE  = 0x03

CMD_WEIGHT        = 0x01   # weight / flow / timer / overload  (notify only)
CMD_TIMER         = 0x02   # timer state: 1 start, 2 pause, 3 reset
CMD_BATTERY       = 0x05   # bars + percent
CMD_UNIT          = 0x06   # 0 g, 1 oz
CMD_POWER_OFF     = 0x0B
CMD_TARE          = 0x0D
CMD_FACTORY_RESET = 0x19
CMD_FORGET        = 0x1A

# Timer sub-commands (data byte for CMD_TIMER writes)
TIMER_START = 0x01
TIMER_PAUSE = 0x02
TIMER_RESET = 0x03

# Fixed-point scales — see the "assumptions" banner above.
WEIGHT_SCALE = 100.0   # raw Int32 / 100 -> grams (0.01 g resolution assumed)
FLOW_SCALE   = 100.0   # raw Int16 / 100 -> g/s   (not on the LM-BBW critical path)

# See "assumptions" banner: True = append CRC big-endian (MSB first).
CRC_BIG_ENDIAN = True


def normalize_uuid(uuid_str):
    return uuid_str.lower().replace('-', '')


def crc16(data) -> int:
    """
    CRC-16 as named in the spec ("CRC-16/IBM", poly 0x8005, init 0xFFFF).

    Implemented in the reflected/MODBUS form (reflected poly 0xA001, init
    0xFFFF, no final XOR), which is the self-consistent reading of that name +
    init value. Covers all frame bytes except the two CRC bytes themselves.
    """
    crc = 0xFFFF
    for b in data:
        crc ^= (b & 0xFF)
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def build_frame(opcode: int, cmd: int, payload: bytes = b'') -> bytes:
    """
    Assemble a full command frame: header + opcode + cmd + length(BE) + payload
    + CRC. Length counts payload bytes only (spec §3).
    """
    payload = bytes(payload)
    body = bytes([HEADER1, HEADER2, opcode & 0xFF, cmd & 0xFF]) + \
        struct.pack('>H', len(payload)) + payload
    crc = crc16(body)
    crc_bytes = struct.pack('>H' if CRC_BIG_ENDIAN else '<H', crc)
    return body + crc_bytes


# Pre-built fixed command frames (all have empty or 1-byte payloads).
def cmd_tare() -> bytes:        return build_frame(OP_WRITE, CMD_TARE)
def cmd_power_off() -> bytes:   return build_frame(OP_WRITE, CMD_POWER_OFF)
def cmd_timer(sub: int) -> bytes:
    return build_frame(OP_WRITE, CMD_TIMER, bytes([sub & 0xFF]))
def read_battery() -> bytes:    return build_frame(OP_READ, CMD_BATTERY)
def read_unit() -> bytes:       return build_frame(OP_READ, CMD_UNIT)


# --- SCANNING ---
def find_timemore_devices(timeout=1) -> List[Tuple[str, str]]:
    """
    Scan for Timemore scales using SimplePyBLE. Blocking for 'timeout' seconds.
    Returns a list of (name, address) tuples, deduplicated by address.
    (Discovery in the running app goes through common.scales; this mirrors the
    per-vendor helper the other drivers expose.)
    """
    found_devs = []
    seen_addrs = set()
    try:
        adapters = simplepyble.Adapter.get_adapters()
        if not adapters:
            logging.warning("No Bluetooth Adapters found")
            return []

        adapter = adapters[0]
        with adapter_scan_lock:
            adapter.scan_for(timeout * 1000)
            peripherals = adapter.scan_get_results()

        for p in peripherals:
            try:
                name = p.identifier()
                addr = p.address()
                if name and any(t in name.upper() for t in TIMEMORE_NAME_PREFIXES):
                    if addr in seen_addrs:
                        continue
                    seen_addrs.add(addr)
                    logging.info(f"Scan Found (Timemore): {name} [{addr}]")
                    found_devs.append((name, addr))
            except Exception:
                continue

    except Exception as e:
        if "InProgress" not in str(e):
            logging.error(f"SimplePyBLE Scan Error (Timemore): {e}")

    return found_devs


def decode_weight_packet(payload) -> dict:
    """
    Decode the 9-byte payload of a weight frame (protocols.md §5.1):
      [0:4] weight     Int32 (BE, signed) -> weight / WEIGHT_SCALE grams
      [4:6] flow rate  Int16 (BE, signed) -> flow / FLOW_SCALE
      [6:8] timer      uInt16 (BE)        -> seconds (0..3599)
      [8]   overload   uint8              -> 1 if +/- limit exceeded

    Returns {valid, weight, flow_rate, timer_s, overload}.
    """
    if payload is None or len(payload) < 9:
        return {"valid": False}

    raw_weight = struct.unpack('>i', bytes(payload[0:4]))[0]
    weight = raw_weight / WEIGHT_SCALE

    raw_flow = struct.unpack('>h', bytes(payload[4:6]))[0]
    flow_rate = raw_flow / FLOW_SCALE

    timer_s = struct.unpack('>H', bytes(payload[6:8]))[0]
    overload = payload[8] & 0xFF

    return {
        "valid": True,
        "weight": weight,
        "flow_rate": flow_rate,
        "timer_s": timer_s,
        "overload": overload,
    }


def parse_frames(buffer: bytearray):
    """
    Pull complete frames out of a receive buffer. Returns (frames, remainder)
    where each frame is a dict {opcode, cmd, payload} and remainder is the
    leftover bytes (a partial frame awaiting more data).

    Robust to fragmentation and to multiple frames coalesced into one BLE
    notification, and resynchronises past garbage by hunting for the 0xA55A
    header.
    """
    frames = []
    buf = buffer

    while True:
        # Find header.
        start = -1
        for i in range(len(buf) - 1):
            if buf[i] == HEADER1 and buf[i + 1] == HEADER2:
                start = i
                break
        if start < 0:
            # No header; keep only a trailing byte that might be a split 0xA5.
            return frames, bytearray(buf[-1:]) if buf[-1:] == bytes([HEADER1]) else bytearray()

        if start > 0:
            buf = buf[start:]  # drop leading garbage

        if len(buf) < 6:
            return frames, bytearray(buf)  # need header+opcode+cmd+length

        opcode = buf[2]
        cmd = buf[3]
        length = (buf[4] << 8) | buf[5]
        frame_len = 6 + length + 2  # + CRC
        if len(buf) < frame_len:
            return frames, bytearray(buf)  # wait for the rest

        payload = bytes(buf[6:6 + length])
        crc_rx = struct.unpack('>H' if CRC_BIG_ENDIAN else '<H',
                               bytes(buf[6 + length:frame_len]))[0]
        crc_calc = crc16(buf[0:6 + length])
        crc_ok = (crc_rx == crc_calc)

        frames.append({
            "opcode": opcode,
            "cmd": cmd,
            "payload": payload,
            "crc_ok": crc_ok,
        })
        buf = buf[frame_len:]
        if len(buf) < 2:
            return frames, bytearray(buf)


# --- TIMEMORE SCALE CLASS ---

# While the scale is off / out of range the connect loop retries every few
# seconds. Log the first miss, then stay quiet until it reappears, emitting only
# an occasional reminder at this interval (seconds) so a long outage still
# leaves a breadcrumb without flooding the journal.
SEARCH_REMINDER_INTERVAL = 300.0


class TimemoreScale(object):
    """
    Drop-in counterpart to AcaiaScale / BookooScale for Timemore scales.

    Like BooKoo, uses two characteristics: a write characteristic (downlink)
    and a notify characteristic (uplink). No handshake and no heartbeat are
    required — subscribe to the notify characteristic and weight frames flow at
    ~10 Hz. A light watchdog drops the connection if the peripheral reports it
    has gone away, matching the other drivers.
    """

    def __init__(self, mac=None):
        self.mac = mac
        self.connected = False
        self.weight = 0.0
        self.battery = 0
        self.units = 'grams'
        # Extra fields the app doesn't require but which the protocol provides.
        self.flow_rate = 0.0
        self.overload = False

        self.adapter = None
        self._peripheral = None
        self._service_uuid = None
        self._notify_uuid = None
        self._write_uuid = None

        self._buffer = bytearray()

        self._connect_thread = None
        self._watchdog_thread = None
        self._stop_event = threading.Event()

        # Connect-retry log throttling (see SEARCH_REMINDER_INTERVAL). Counts
        # consecutive failed scans since the scale was last connected; reset to 0
        # on a successful connection so the next disappearance logs fresh.
        self._search_miss_count = 0
        self._search_last_reminder = 0.0

    # ---- connection lifecycle ----

    def connect(self):
        """Start connecting in a background thread (non-blocking)."""
        if self.connected:
            return
        if self._connect_thread and self._connect_thread.is_alive():
            return
        self._stop_event.clear()
        self._connect_thread = threading.Thread(target=self._connect_sync, daemon=True)
        self._connect_thread.start()
        # First attempt of an episode logs normally; quiet retries drop to DEBUG.
        if self._search_miss_count == 0:
            logging.info("Starting Timemore Connection Thread (SimplePyBLE)...")
        else:
            logging.debug("Starting Timemore Connection Thread (SimplePyBLE)... (retry)")

    def _connect_sync(self):
        try:
            time.sleep(0.5)
            adapters = simplepyble.Adapter.get_adapters()
            if not adapters:
                logging.error("No Bluetooth adapters found")
                return
            self.adapter = adapters[0]

            target = None
            for attempt in range(3):
                try:
                    logging.debug(f"Scanning to acquire Timemore {self.mac} (Attempt {attempt + 1})...")
                    with adapter_scan_lock:
                        self.adapter.scan_for(2000)
                        peripherals = self.adapter.scan_get_results()
                    for p in peripherals:
                        if p.address() == self.mac:
                            target = p
                            break
                    if target:
                        break
                except Exception as e:
                    if "InProgress" in str(e):
                        logging.warning("BlueZ busy, waiting...")
                        time.sleep(1.0)
                    else:
                        logging.error(f"Scan Error: {e}")
                        break

            if not target:
                self._search_miss_count += 1
                now = time.monotonic()
                if self._search_miss_count == 1:
                    logging.warning("Device %s not found in scan; will keep retrying "
                                    "quietly until it reappears." % self.mac)
                    self._search_last_reminder = now
                elif now - self._search_last_reminder >= SEARCH_REMINDER_INTERVAL:
                    logging.info("Still searching for Timemore %s (%d attempts since it "
                                 "went missing)." % (self.mac, self._search_miss_count))
                    self._search_last_reminder = now
                else:
                    logging.debug("Timemore %s not found (attempt %d)."
                                  % (self.mac, self._search_miss_count))
                return

            self._peripheral = target
            logging.info(f"Connecting to Timemore {self.mac}...")
            self._peripheral.connect()

            if not self._peripheral.is_connected():
                logging.error("Failed to connect.")
                return

            logging.info(f"Connected to Timemore {self.mac}")
            self.connected = True
            if self._search_miss_count:
                logging.info("Timemore %s reconnected after %d missed scan(s)."
                             % (self.mac, self._search_miss_count))
            self._search_miss_count = 0
            self._search_last_reminder = 0.0
            self._buffer = bytearray()
            time.sleep(1.0)  # let services settle

            if not self._setup_services():
                logging.error("Failed to find Timemore Service/Characteristic UUIDs")
                self.disconnect()
                return

            # Subscribe to uplink notifications.
            try:
                self._peripheral.notify(self._service_uuid, self._notify_uuid,
                                        self._notification_handler)
                logging.info("Subscribed to Timemore notifications")
            except Exception as e:
                logging.error(f"Notify failed: {e}")
                self.disconnect()
                return

            # Weight streams on its own; battery and unit are pushed on change,
            # so poll them once to populate initial state.
            time.sleep(0.2)
            self._write_sync(read_unit())
            time.sleep(0.1)
            self._write_sync(read_battery())

            # Start light keep-alive watchdog.
            self._watchdog_thread = threading.Thread(target=self._watchdog_loop, daemon=True)
            self._watchdog_thread.start()

        except Exception as e:
            logging.error(f"Connection Error: {e}")
            self.connected = False
            self._peripheral = None

    def _setup_services(self) -> bool:
        try:
            services = self._peripheral.services()
            for service in services:
                for char in service.characteristics():
                    u = normalize_uuid(char.uuid())
                    if u == normalize_uuid(TIMEMORE_NOTIFY_UUID):
                        self._notify_uuid = char.uuid()
                        self._service_uuid = service.uuid()
                    elif u == normalize_uuid(TIMEMORE_WRITE_UUID):
                        self._write_uuid = char.uuid()
                        self._service_uuid = service.uuid()
            if self._notify_uuid and self._write_uuid and self._service_uuid:
                logging.info("Detected Timemore service layout")
                return True
        except Exception as e:
            logging.error(f"Service Discovery Error: {e}")
        return False

    def _watchdog_loop(self):
        """
        Timemore needs no heartbeat, but if the peripheral handle reports a lost
        connection we surface it the same way the other drivers do.
        """
        while self.connected and not self._stop_event.is_set():
            try:
                time.sleep(2.0)
                if not self.connected:
                    break
                if self._peripheral is not None and not self._peripheral.is_connected():
                    logging.error("Timemore reports disconnected. Dropping.")
                    self.disconnect()
                    break
            except Exception as e:
                logging.error(f"Timemore watchdog error: {e}")
                self.disconnect()
                break

    # ---- data ----

    def _notification_handler(self, payload):
        self._buffer += bytes(payload)
        # Cap the buffer so a stream of unrecognised bytes can't grow forever.
        if len(self._buffer) > 512:
            self._buffer = self._buffer[-64:]

        frames, self._buffer = parse_frames(self._buffer)
        for f in frames:
            if not f.get("crc_ok", True):
                logging.debug("Timemore frame CRC mismatch (using anyway)")
            self._dispatch(f["cmd"], f["payload"])

    def _dispatch(self, cmd, payload):
        if cmd == CMD_WEIGHT:
            info = decode_weight_packet(payload)
            if info.get("valid"):
                self.weight = info["weight"]
                self.flow_rate = info["flow_rate"]
                self.overload = bool(info["overload"])
                if info["overload"]:
                    logging.debug("Timemore reports overload (weight out of range)")
        elif cmd == CMD_BATTERY and len(payload) >= 2:
            # payload[0] = bar count, payload[1] = percent
            pct = payload[1] & 0xFF
            self.battery = max(0, min(100, pct))
        elif cmd == CMD_UNIT and len(payload) >= 1:
            self.units = 'ounces' if (payload[0] & 0xFF) == 1 else 'grams'
        # Timer notify / write-acks are not needed by the app; ignore quietly.

    # ---- commands ----

    def _write_sync(self, data) -> bool:
        if self.connected and self._peripheral and self._write_uuid:
            try:
                self._peripheral.write_command(self._service_uuid, self._write_uuid, bytes(data))
                return True
            except Exception as e:
                logging.error(f"Timemore Write CMD failed: {e}")
                return False
        return False

    def tare(self):
        self._write_sync(cmd_tare())
        return True

    # Timer helpers for parity with BookooScale (unused by LM-BBW itself, but
    # other apps in the shared common/ package may call them).
    def start_timer(self):
        self._write_sync(cmd_timer(TIMER_START))
        return True

    def stop_timer(self):
        # Timemore's protocol has "pause"; treat it as the BooKoo "stop".
        self._write_sync(cmd_timer(TIMER_PAUSE))
        return True

    def reset_timer(self):
        self._write_sync(cmd_timer(TIMER_RESET))
        return True

    def tare_and_start_timer(self):
        # No combined opcode in the protocol; issue the two commands in order.
        self._write_sync(cmd_tare())
        time.sleep(0.05)
        self._write_sync(cmd_timer(TIMER_START))
        return True

    def power_off(self):
        self._write_sync(cmd_power_off())
        return True

    # ---- teardown ----

    def disconnect(self):
        logging.info("Disconnecting Timemore...")
        self.connected = False
        self._stop_event.set()

        if self._peripheral:
            def _bg_disconnect(peri):
                try:
                    peri.disconnect()
                except Exception:
                    pass
            threading.Thread(target=_bg_disconnect, args=(self._peripheral,), daemon=True).start()
            self._peripheral = None


# --- Offline self-test (no BLE hardware needed) -----------------------------
# Run:  python scale_timemore.py
# Exercises the pure protocol logic: CRC, frame builder, weight decode, and the
# fragment-tolerant parser. Does NOT touch Bluetooth.
if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)

    # 1. Frame builder round-trips through the parser.
    tare = cmd_tare()
    print("tare frame:", tare.hex(" "))
    frames, rest = parse_frames(bytearray(tare))
    assert rest == b"" and len(frames) == 1, frames
    assert frames[0]["opcode"] == OP_WRITE and frames[0]["cmd"] == CMD_TARE
    assert frames[0]["crc_ok"], "self-built frame must verify against its own CRC"

    # 2. Weight packet decode: -12.34 g, flow 2.50 g/s, 65 s, no overload.
    raw = struct.pack('>i', -1234) + struct.pack('>h', 250) + \
        struct.pack('>H', 65) + bytes([0])
    wframe = build_frame(OP_NOTIFY, CMD_WEIGHT, raw)
    frames, rest = parse_frames(bytearray(wframe))
    info = decode_weight_packet(frames[0]["payload"])
    print("decoded:", info)
    assert abs(info["weight"] - (-12.34)) < 1e-9
    assert abs(info["flow_rate"] - 2.50) < 1e-9
    assert info["timer_s"] == 65 and info["overload"] == 0

    # 3. Fragmentation + coalescing: split one frame across two chunks, then
    #    feed a second whole frame in the same buffer.
    batt = build_frame(OP_NOTIFY, CMD_BATTERY, bytes([4, 87]))
    buf = bytearray()
    buf += wframe[:5]
    frames, buf = parse_frames(buf)
    assert frames == [] and len(buf) == 5              # partial held back
    buf += wframe[5:] + batt
    frames, buf = parse_frames(buf)
    assert len(frames) == 2 and buf == b""             # both recovered
    assert frames[1]["cmd"] == CMD_BATTERY and frames[1]["payload"][1] == 87

    # 4. Resync past leading garbage.
    frames, _ = parse_frames(bytearray(b"\x00\xff\x11" + batt))
    assert len(frames) == 1 and frames[0]["cmd"] == CMD_BATTERY

    print("\nAll self-tests passed.")