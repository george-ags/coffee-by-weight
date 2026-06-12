#!/usr/bin/env python3
#
# GRIND-BW - grind-by-weight controller (skeleton).
#
# Starting point for the grinder project. It already does the shared part:
# scans for a supported Bluetooth scale (Acaia or BooKoo) using the common
# scale stack, connects, and streams the live weight. Grinder-specific logic
# (motor control, target dosing, display screens, web UI) goes where the
# TODO markers are, following the same patterns as lm-bbw.
#
# Deployed layout (assembled by deploy.sh):
#   /opt/grind-bw/
#     grind-bw.py        <- this file
#     app/               <- grinder-specific modules (control, display, web)
#     common/            <- shared package (scales, drivers, ble, lcd, fonts)

import logging
import os
import signal
import sys
import time

from logging import handlers

from common.scales import Scale, find_all_scales, DEFAULT_VENDOR

MAC_SAVE_FILE = '/opt/grind-bw/mac.save'

stop = False

logLevel = os.environ.get('LOGLEVEL', 'INFO').upper()
logPath = os.environ.get('LOGFILE', '/var/log/grind-bw.log')

stdout_handler = logging.StreamHandler(stream=sys.stdout)
stdout_handler.setLevel(logging.INFO)
file_handler = handlers.TimedRotatingFileHandler(filename=logPath, when='midnight', backupCount=4)
file_handler.setLevel(logLevel)
logging.basicConfig(
    level=logLevel,
    format='[%(asctime)s] {%(filename)s:%(lineno)d} %(levelname)s - %(message)s',
    handlers=[stdout_handler, file_handler]
)


def save_mac_address(mac, vendor=None):
    try:
        with open(MAC_SAVE_FILE, 'w') as f:
            f.write("%s,%s" % (vendor or DEFAULT_VENDOR, mac))
        logging.info(f"Saved scale {mac} [{vendor}] to disk")
    except Exception as e:
        logging.error(f"Failed to save MAC: {e}")


def load_last_mac():
    """Returns (mac, vendor) or (None, None)."""
    try:
        if os.path.exists(MAC_SAVE_FILE):
            with open(MAC_SAVE_FILE, 'r') as f:
                raw = f.read().strip()
            if ',' in raw:
                vendor, mac = raw.split(',', 1)
                return mac.strip(), vendor.strip()
    except Exception as e:
        logging.error(f"Failed to load MAC: {e}")
    return None, None


def main():
    scale = Scale()
    mac, vendor = load_last_mac()

    while not stop:
        if not scale.connected:
            if not mac:
                logging.info("Scanning for a supported scale...")
                found = find_all_scales(timeout=2)
                if found:
                    name, mac, vendor = found[0]
                    logging.info("Found %s [%s] (%s)" % (name, mac, vendor))
            if mac:
                scale.prepare(mac, vendor or DEFAULT_VENDOR)
                scale.connect()
                # connect() is non-blocking; give it a moment.
                for _ in range(50):
                    if scale.connected or stop:
                        break
                    time.sleep(0.2)
                if scale.connected:
                    save_mac_address(scale.mac, scale.vendor)
            else:
                time.sleep(3)
                continue

        if scale.connected:
            # TODO grinder logic:
            #  - target dose handling (memory banks like lm-bbw's ControlManager)
            #  - motor relay control: stop grinding at target minus learned overshoot
            #  - display process with grind screens (reuse common.lcd + fonts)
            #  - web UI: config editor + the scan & pin setup page
            logging.info("Weight: %.2f g  (battery %d%%, %s)"
                         % (scale.weight, scale.battery, scale.vendor))
        time.sleep(1.0)

    if scale.connected:
        try:
            scale.disconnect()
        except Exception as ex:
            logging.error("Error during shutdown: %s" % str(ex))
    logging.info("Exiting on stop")


def shutdown(sig, frame):
    global stop
    stop = True


if __name__ == '__main__':
    signal.signal(signal.SIGINT, shutdown)
    main()
