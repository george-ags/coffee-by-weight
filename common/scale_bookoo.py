#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# BooKoo Bluetooth scale driver (SimplePyBLE).
#
# Mirrors the public interface of lib.scale_acaia.AcaiaScale so the rest of the
# application can use either vendor interchangeably:
#   attributes: mac, connected, weight, battery, units
#   methods:    connect(), disconnect(), tare()
#
# Protocol references:
#   doc/BT_Scales/BooKooCode/OpenSource/bookoo_ultra_scale/protocols.md
#   doc/BT_Scales/BooKooCode/OpenSource/bookoo_mini_scale/protocols.md
#
# Supports the BooKoo "Ultra" and "Mini" models. Both use an identical wire
# protocol: the same 0x0FFE service, the same 0xFF11/0xFF12 characteristics,
# the same 20-byte weight packet, and the same command frames. The Mini is
# grams-only and omits a couple of Ultra-only commands (calibration, auto-mode
# stop condition), but nothing this driver reads or sends differs between them,
# so a single driver covers both. Further models that share this 0x0FFE layout
# can be supported by adding their advertised-name prefix below.

__version__ = "0.2.0-bookoo-ultra-mini"

import logging
import time
import threading
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
BOOKOO_SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"      # 0x0FFE service group
BOOKOO_CMD_UUID     = "0000ff12-0000-1000-8000-00805f9b34fb"      # Command characteristic
BOOKOO_WEIGHT_UUID  = "0000ff11-0000-1000-8000-00805f9b34fb"      # Weight-data characteristic

# Name tokens BooKoo scales advertise with. Matched as substrings (see
# classify_vendor). The BooKoo Ultra advertises as "BOOKOO_SC_U" and the Mini
# similarly under a "BOOKOO" name, so matching "BOOKOO" covers both.
# (Do NOT match "GR-AC" or "_SC" alone — those appear on unrelated third-party
# devices and caused false positives.)
BOOKOO_NAME_PREFIXES = ['BOOKOO']

# Receive packet framing
RX_HEADER1 = 0x03      # PRODUCT NUMBER
RX_HEADER2 = 0x0B      # TYPE for weight data
RX_LENGTH  = 20        # full weight packet length

# Command frames are published verbatim in the spec (header, 3 data bytes, then
# the spec's DATASUM byte). The spec's checksum examples are internally
# inconsistent, so we send the exact bytes the spec lists rather than recompute.
CMD_TARE           = bytes([0x03, 0x0A, 0x01, 0x00, 0x00, 0x08])
CMD_START_TIMER    = bytes([0x03, 0x0A, 0x04, 0x00, 0x00, 0x0A])
CMD_STOP_TIMER     = bytes([0x03, 0x0A, 0x05, 0x00, 0x00, 0x0D])
CMD_RESET_TIMER    = bytes([0x03, 0x0A, 0x06, 0x00, 0x00, 0x0C])
CMD_TARE_AND_START = bytes([0x03, 0x0A, 0x07, 0x00, 0x00, 0x00])


def normalize_uuid(uuid_str):
    return uuid_str.lower().replace('-', '')


def xor_checksum(data) -> int:
    """XOR of all bytes, masked to 8 bits (the spec's stated checksum method)."""
    c = 0
    for b in data:
        c ^= b
    return c & 0xFF


# --- SCANNING ---
def find_bookoo_devices(timeout=1) -> List[Tuple[str, str]]:
    """
    Scan for BooKoo scales using SimplePyBLE. Blocking for 'timeout' seconds.
    Returns a list of (name, address) tuples, deduplicated by address.
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
                if name and any(name.upper().startswith(t) for t in BOOKOO_NAME_PREFIXES):
                    if addr in seen_addrs:
                        continue
                    seen_addrs.add(addr)
                    logging.info(f"Scan Found (BooKoo): {name} [{addr}]")
                    found_devs.append((name, addr))
            except Exception:
                continue

    except Exception as e:
        if "InProgress" not in str(e):
            logging.error(f"SimplePyBLE Scan Error (BooKoo): {e}")

    return found_devs


def decode_weight_packet(packet) -> dict:
    """
    Decode a 20-byte BooKoo weight notification.

    Layout (spec BYTE#, 1-indexed -> array index, 0-indexed):
      BYTE1  [0]   product number (0x03)
      BYTE2  [1]   type (0x0B for weight)
      BYTE3-5 [2..4]  milliseconds (BE24)            -> timer
      BYTE6  [5]   unit of weight (0x01 ounce, 0x02 gram)
      BYTE7  [6]   weight sign (+/-)
      BYTE8-10 [7..9]  grams * 100 (BE24, unsigned)  -> weight
      BYTE11 [10]  flow-rate sign (+/-)
      BYTE12-13 [11..12]  flow rate * 100 (BE16)      -> flow rate
      BYTE14 [13]  percentage of remaining power      -> battery
      BYTE15-16 [14..15]  standby time (min, BE16)
      BYTE17 [16]  buzzer gear
      BYTE18 [17]  flow-rate smoothing switch
      BYTE19 [18]  reserved (00)
      BYTE20 [19]  DATASUM (XOR checksum)

    Returns dict: {valid, weight, units, battery, flow_rate, timer_ms, checksum_ok}.
    """
    if packet is None or len(packet) < RX_LENGTH:
        return {"valid": False}
    if packet[0] != RX_HEADER1 or packet[1] != RX_HEADER2:
        return {"valid": False}

    # Checksum is informational: we log a mismatch but still use the data, since
    # the spec's checksum examples are not internally consistent.
    checksum_ok = (xor_checksum(packet[0:RX_LENGTH - 1]) == packet[RX_LENGTH - 1])

    timer_ms = (packet[2] << 16) | (packet[3] << 8) | packet[4]
    # BYTE6 unit: Ultra reports 0x01=ounce / 0x02=gram. The Mini is grams-only
    # and always sends the gram code, so this maps correctly for both models.
    unit_byte = packet[5]
    units = 'ounces' if unit_byte == 0x01 else 'grams'

    raw_weight = (packet[7] << 16) | (packet[8] << 8) | packet[9]
    weight = raw_weight / 100.0
    # Sign byte: ASCII '-' (0x2D) or a set high bit indicates negative.
    if packet[6] == 0x2D or (packet[6] & 0x80):
        weight = -weight

    # Flow rate: BYTE12-13 (index 11..12) = flow * 100, BE16; BYTE11 = sign.
    raw_flow = (packet[11] << 8) | packet[12]
    flow_rate = raw_flow / 100.0
    if packet[10] == 0x2D or (packet[10] & 0x80):
        flow_rate = -flow_rate

    # Battery is BYTE14 (index 13) — "percentage of remaining power".
    # (Earlier this read index 12, the flow-rate low byte, which made the
    # battery track the weight and exceed 100%.)
    battery = packet[13]
    if battery < 0:
        battery = 0
    elif battery > 100:
        battery = 100

    return {
        "valid": True,
        "weight": weight,
        "units": units,
        "battery": battery,
        "flow_rate": flow_rate,
        "timer_ms": timer_ms,
        "checksum_ok": checksum_ok,
    }


# --- BOOKOO SCALE CLASS ---
class BookooScale(object):
    """
    Drop-in counterpart to AcaiaScale for BooKoo scales.

    Unlike Acaia, BooKoo uses two separate characteristics: a command
    characteristic (write) and a weight characteristic (notify). There is no
    handshake — subscribe to the weight characteristic and data flows. There is
    also no heartbeat requirement, but we run a light keep-alive watchdog that
    drops the connection if writes start failing, matching AcaiaScale behavior.
    """

    def __init__(self, mac=None):
        self.mac = mac
        self.connected = False
        self.weight = 0.0
        self.battery = 0
        self.units = 'grams'

        self.adapter = None
        self._peripheral = None
        self._service_uuid = None
        self._cmd_uuid = None
        self._weight_uuid = None

        self._connect_thread = None
        self._watchdog_thread = None
        self._stop_event = threading.Event()

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
        logging.info("Starting BooKoo Connection Thread (SimplePyBLE)...")

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
                    logging.info(f"Scanning to acquire BooKoo {self.mac} (Attempt {attempt + 1})...")
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
                logging.warning(f"Device {self.mac} not found in scan.")
                return

            self._peripheral = target
            logging.info(f"Connecting to BooKoo {self.mac}...")
            self._peripheral.connect()

            if not self._peripheral.is_connected():
                logging.error("Failed to connect.")
                return

            logging.info(f"Connected to BooKoo {self.mac}")
            self.connected = True
            time.sleep(1.0)  # let services settle

            if not self._setup_services():
                logging.error("Failed to find BooKoo Service/Characteristic UUIDs")
                self.disconnect()
                return

            # Subscribe to weight notifications.
            try:
                self._peripheral.notify(self._service_uuid, self._weight_uuid,
                                        self._notification_handler)
                logging.info("Subscribed to BooKoo weight notifications")
            except Exception as e:
                logging.error(f"Notify failed: {e}")
                self.disconnect()
                return

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
                    if u == normalize_uuid(BOOKOO_CMD_UUID):
                        self._service_uuid = service.uuid()
                        self._cmd_uuid = char.uuid()
                    elif u == normalize_uuid(BOOKOO_WEIGHT_UUID):
                        self._weight_uuid = char.uuid()
                        # weight + command live under the same 0x0FFE service
                        self._service_uuid = service.uuid()
            if self._cmd_uuid and self._weight_uuid and self._service_uuid:
                logging.info("Detected BooKoo Ultra service layout")
                return True
        except Exception as e:
            logging.error(f"Service Discovery Error: {e}")
        return False

    def _watchdog_loop(self):
        """
        BooKoo needs no heartbeat, but if the peripheral handle reports a lost
        connection we surface it the same way AcaiaScale does.
        """
        while self.connected and not self._stop_event.is_set():
            try:
                time.sleep(2.0)
                if not self.connected:
                    break
                if self._peripheral is not None and not self._peripheral.is_connected():
                    logging.error("BooKoo reports disconnected. Dropping.")
                    self.disconnect()
                    break
            except Exception as e:
                logging.error(f"BooKoo watchdog error: {e}")
                self.disconnect()
                break

    # ---- data ----

    def _notification_handler(self, payload):
        info = decode_weight_packet(payload)
        if not info.get("valid"):
            return
        if not info.get("checksum_ok", True):
            logging.debug("BooKoo packet checksum mismatch (using anyway)")
        self.weight = info["weight"]
        self.units = info["units"]
        self.battery = info["battery"]

    # ---- commands ----

    def _write_sync(self, data) -> bool:
        if self.connected and self._peripheral and self._cmd_uuid:
            try:
                self._peripheral.write_command(self._service_uuid, self._cmd_uuid, bytes(data))
                return True
            except Exception as e:
                logging.error(f"BooKoo Write CMD failed: {e}")
                return False
        return False

    def tare(self):
        self._write_sync(CMD_TARE)
        return True

    def tare_and_start_timer(self):
        self._write_sync(CMD_TARE_AND_START)
        return True

    def start_timer(self):
        self._write_sync(CMD_START_TIMER)
        return True

    def stop_timer(self):
        self._write_sync(CMD_STOP_TIMER)
        return True

    def reset_timer(self):
        self._write_sync(CMD_RESET_TIMER)
        return True

    # ---- teardown ----

    def disconnect(self):
        logging.info("Disconnecting BooKoo...")
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