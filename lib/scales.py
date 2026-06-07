#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Vendor-neutral scale layer.
#
# The rest of the application talks to a single `Scale` object using a small,
# vendor-agnostic interface (mac, connected, weight, battery, units, connect(),
# disconnect(), tare()). This module hides which concrete backend
# (Acaia or BooKoo) is actually in use, and provides:
#
#   - find_all_scales(timeout)  -> one BLE scan, classified by vendor
#   - make_scale(vendor, mac)   -> construct the right backend
#   - Scale                     -> delegating wrapper that can switch vendor
#
# To add another vendor later: add a driver module exposing the same interface,
# register its name prefixes and constructor in VENDORS below.

import logging
from typing import List, Tuple

try:
    import simplepyble
except ImportError:
    logging.fatal("SimplePyBLE not installed. Run: pip3 install simplepyble")
    raise

from lib.scale_acaia import AcaiaScale, ACAIA_NAME_PREFIXES
from lib.scale_bookoo import BookooScale, BOOKOO_NAME_PREFIXES
from lib.ble import adapter_scan_lock

# vendor key -> (display label, name-prefix list, constructor)
VENDORS = {
    'acaia':  ('Acaia',  ACAIA_NAME_PREFIXES,  AcaiaScale),
    'bookoo': ('BooKoo', BOOKOO_NAME_PREFIXES, BookooScale),
}

DEFAULT_VENDOR = 'acaia'


def vendor_label(vendor: str) -> str:
    entry = VENDORS.get(vendor)
    return entry[0] if entry else (vendor or 'Unknown')


def classify_vendor(name: str):
    """Return the vendor key for an advertised device name, or None."""
    if not name:
        return None
    u = name.upper()
    # Substring match (more forgiving than prefix-only): catches names like
    # "BOOKOO_SC", "Themis BOOKOO", "ACAIAL-1234", etc.
    for vendor, (_label, prefixes, _ctor) in VENDORS.items():
        if any(p in u for p in prefixes):
            return vendor
    return None


def _scan_raw(timeout):
    """One BLE scan; returns all discovered (name, address), deduped by address."""
    out = []
    seen = set()
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
                name = p.identifier() or ""
                addr = p.address()
                if not addr or addr in seen:
                    continue
                seen.add(addr)
                out.append((name, addr))
            except Exception:
                continue
    except Exception as e:
        if "InProgress" not in str(e):
            logging.error(f"Combined Scan Error: {e}")
    return out


def find_all_scales(timeout=2) -> List[Tuple[str, str, str]]:
    """
    One BLE scan, returning only RECOGNIZED scales as (name, address, vendor),
    deduped by address. Used by automatic background discovery.
    """
    found = []
    for name, addr in _scan_raw(timeout):
        vendor = classify_vendor(name)
        if vendor:
            logging.info(f"Scan Found ({vendor_label(vendor)}): {name} [{addr}]")
            found.append((name, addr, vendor))
    return found


def find_all_devices(timeout=2) -> List[Tuple[str, str, str]]:
    """
    One BLE scan, returning EVERY discovered device as (name, address, vendor),
    where vendor is None for unrecognized devices. Used by the manual setup page
    so a scale can still be selected even if its advertised name isn't matched.
    Logs every device seen, which makes it easy to discover real advertised names.
    """
    results = []
    for name, addr in _scan_raw(timeout):
        vendor = classify_vendor(name)
        shown = name if name else "(unnamed)"
        logging.info(f"Scan saw: '{shown}' [{addr}] -> {vendor_label(vendor) if vendor else 'unrecognized'}")
        results.append((name, addr, vendor))
    return results


def make_scale(vendor: str, mac: str = ''):
    """Construct the concrete backend object for a vendor."""
    entry = VENDORS.get(vendor)
    ctor = entry[2] if entry else VENDORS[DEFAULT_VENDOR][2]
    return ctor(mac=mac)


class Scale:
    """
    Vendor-neutral wrapper holding one concrete backend at a time. Switching
    vendor (or being prepared for a new connection) swaps the backend in place,
    so the single `scale` object used throughout the app never changes identity.
    """

    def __init__(self, mac: str = '', vendor: str = DEFAULT_VENDOR):
        self.vendor = vendor if vendor in VENDORS else DEFAULT_VENDOR
        self._impl = make_scale(self.vendor, mac)

    def prepare(self, mac: str, vendor: str = None):
        """
        Point the wrapper at a target MAC for the given vendor, swapping the
        backend if the vendor changed. Call this immediately before connect().
        """
        vendor = vendor if vendor in VENDORS else self.vendor
        if vendor != self.vendor or self._impl is None:
            # Switching vendor: drop any existing backend cleanly first.
            try:
                if self._impl is not None and self._impl.connected:
                    self._impl.disconnect()
            except Exception:
                pass
            self.vendor = vendor
            self._impl = make_scale(vendor, mac)
            logging.info("Scale backend set to %s" % vendor_label(vendor))
        else:
            self._impl.mac = mac

    # --- delegated interface ---
    @property
    def mac(self):
        return self._impl.mac

    @mac.setter
    def mac(self, value):
        self._impl.mac = value

    @property
    def connected(self):
        return self._impl.connected

    @property
    def weight(self):
        return self._impl.weight

    @property
    def battery(self):
        return self._impl.battery

    @property
    def units(self):
        return self._impl.units

    def connect(self):
        return self._impl.connect()

    def disconnect(self):
        return self._impl.disconnect()

    def tare(self):
        return self._impl.tare()