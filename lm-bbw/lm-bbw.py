#!/usr/bin/env python3
import logging
import os
import signal
import sys
import time

from concurrent.futures import ThreadPoolExecutor
from logging import handlers
from multiprocessing import Queue
from timeit import default_timer as timer
from typing import Optional

from app import control
from app.control import ControlManager
from app.display import Display, DisplayData, DisplaySize
from common.scales import Scale
from app.webserver import WebServer

WEB_PORT = 80
WEB_DIR = '/opt/lm-bbw/web'
MIN_GOOD_SHOT_DURATION = 10
MAC_SAVE_FILE = '/opt/lm-bbw/mac.save'

# A finished shot whose final weight is off the target by this many grams or
# more (in either direction) is treated as a bad pour: it is not saved to the
# shot history and is not used to update overshoot learning. Configurable via
# the OFF_TARGET_REJECT_GRAMS env var; default 1.0 g.
try:
    OFF_TARGET_REJECT_GRAMS = float(os.environ.get('OFF_TARGET_REJECT_GRAMS', '1.0'))
except (TypeError, ValueError):
    OFF_TARGET_REJECT_GRAMS = 1.0

stop = False
overshoot_update_executor = ThreadPoolExecutor(max_workers=1)

logLevel = os.environ.get('LOGLEVEL', 'INFO').upper()
logPath = os.environ.get('LOGFILE', '/var/log/lm-bbw.log')

# --- CONFIGURATION SPLIT ---
# 1. Main Loop Speed (Heartbeat)
refreshRate = float(os.environ.get('REFRESH_RATE', '0.1'))

# 2. Graph History Duration (in Seconds)
# Default: 60 seconds. Increase this if you want a longer timeline on screen.
graph_history_seconds = int(os.environ.get('GRAPH_HISTORY_SECONDS', '60'))

# 3. Flow Smoothing Factor (Window Size)
# Default: Calculate dynamically (1 second worth of samples).
# Set 'FLOW_SMOOTHING_FACTOR=5' in env to override manually.
smoothing_env = os.environ.get('FLOW_SMOOTHING_FACTOR')
if smoothing_env:
    smoothing = int(smoothing_env)
else:
    smoothing = round(1 / refreshRate)
# ---------------------------

stdout_handler = logging.StreamHandler(stream=sys.stdout)
stdout_handler.setLevel(logging.INFO)
file_handler = handlers.TimedRotatingFileHandler(filename=logPath, when='midnight', backupCount=4)
file_handler.setLevel(logLevel)
handlers = [stdout_handler, file_handler]
logging.basicConfig(
    level=logLevel,
    format='[%(asctime)s] {%(filename)s:%(lineno)d} %(levelname)s - %(message)s',
    handlers=handlers
)

def save_mac_address(mac, vendor=None):
    try:
        with open(MAC_SAVE_FILE, 'w') as f:
            f.write("%s,%s" % (vendor or 'acaia', mac))
            logging.info(f"Saved Scale {mac} [{vendor}] to disk")
    except Exception as e:
        logging.error(f"Failed to save MAC: {e}")

def load_last_mac():
    """Returns (mac, vendor) or (None, None). Accepts legacy bare-MAC files."""
    try:
        if os.path.exists(MAC_SAVE_FILE):
            with open(MAC_SAVE_FILE, 'r') as f:
                raw = f.read().strip()
            if ',' in raw:
                vendor, mac = raw.split(',', 1)
                vendor, mac = vendor.strip(), mac.strip()
            else:
                vendor, mac = 'acaia', raw
            if len(mac) > 10:
                logging.info(f"Loaded Last Known Scale: {mac} [{vendor}]")
                return mac, vendor
    except Exception as e:
        logging.error(f"Failed to load MAC: {e}")
    return None, None

def update_overshoot(scale, mgr: ControlManager):
    if mgr.shot_time_elapsed() < MIN_GOOD_SHOT_DURATION:
        logging.info("Declining to consider short shot as a good shot. Not updating overshoot value or saving image")
        return
    time.sleep(3)
    target = mgr.current_memory().target
    final_weight = scale.weight
    logging.debug("over scale weight is %.2f, target was %.2f" % (final_weight, target))

    # Reject shots that landed too far off target (in either direction): don't
    # learn from them and don't save them to the shot history.
    if OFF_TARGET_REJECT_GRAMS > 0 and abs(final_weight - target) >= OFF_TARGET_REJECT_GRAMS:
        logging.info(
            "Shot off target by %.2f g (>= %.2f g threshold): final %.2f g vs target %.2f g. "
            "Not updating overshoot value or saving image."
            % (abs(final_weight - target), OFF_TARGET_REJECT_GRAMS, final_weight, target)
        )
        return

    mgr.current_memory().update_overshoot(final_weight)
    mgr.image_needs_save = True
    logging.info("new overshoot on memory %s is %.2f" %(mgr.current_memory().name, mgr.current_memory().overshoot))


SHOT_TIMEOUT_SECONDS = 60.0

def check_target_disable_relay(scale: Scale, mgr: ControlManager):
    if mgr.shot_time_elapsed() < 1.5:
        return

    if mgr.relay_on() and scale.weight > mgr.current_memory().target_minus_overshoot():
        if mgr.shot_time_elapsed() >= SHOT_TIMEOUT_SECONDS:
            logging.info("Shot reached 60s timeout - disabling relay, skipping memory update")
            mgr.disable_relay()
        else:
            mgr.disable_relay()
            overshoot_update_executor.submit(update_overshoot, scale, mgr)
            logging.debug("Scheduling overshoot check and update")


def main():
    display_data_queue: Queue[DisplayData] = Queue()
    display = Display(display_data_queue, display_size=DisplaySize.SIZE_2_0, image_save_dir=WEB_DIR)
    display.start()

    # --- UPDATED: Calculate points based on History Seconds ---
    # e.g., 60 seconds / 0.1s rate = 600 points buffer
    max_points = round(graph_history_seconds / refreshRate)
    mgr = ControlManager(max_flow_points=max_points)
    # ----------------------------------------------------------

    scale = Scale()

    # Scale selection: a pinned scale (chosen via the web setup page) wins and
    # is the only device we connect to. Otherwise fall back to the last-known
    # auto-remembered scale (MAC + vendor).
    last_mac = None
    if mgr.pinned_mac:
        logging.info("Pinned scale in use: %s [%s]" % (mgr.pinned_mac, mgr.pinned_vendor))
        last_mac = mgr.pinned_mac
    else:
        last_mac, last_vendor = load_last_mac()
        if last_mac:
            mgr.discovered_mac = last_mac
            mgr.discovered_vendor = last_vendor

    mgr.add_tare_handler(lambda: scale.tare())

    # Start the web server now that mgr + scale exist, so the setup page can
    # trigger scans and pin a scale.
    web_server = WebServer(WEB_DIR, WEB_PORT, control_manager=mgr, scale=scale)
    web_server.start()
    logging.info("Started web server")

    last_sample_time: Optional[float] = None
    last_weight: Optional[float] = None
    
    last_relay_state = False
    shot_started_with_scale = False

    while not stop:
        # Check Auto-Sleep Status
        mgr.check_auto_sleep(scale)
        
        is_connected = control.try_connect_scale(scale, mgr)
        
        if is_connected and scale.mac and scale.mac != last_mac:
            save_mac_address(scale.mac, scale.vendor)
            last_mac = scale.mac

        relay_is_on = mgr.relay_on()

        if relay_is_on and not last_relay_state:
            shot_started_with_scale = is_connected
            if shot_started_with_scale:
                logging.info("Shot Started in GRAVIMETRIC Mode")
            else:
                logging.info("Shot Started in MANUAL Mode (Scale not ready)")

        if is_connected:
            check_target_disable_relay(scale, mgr)
        
        elif relay_is_on:
            if shot_started_with_scale:
                logging.warning("LOST SCALE CONNECTION DURING SHOT - EMERGENCY STOP")
                mgr.disable_relay()
            else:
                pass 

        # Detect timeout: shot has been brewing for 60s or more
        timeout_stop = mgr.shot_time_elapsed() >= SHOT_TIMEOUT_SECONDS

        # Suppress image save if this is a timeout shot
        if timeout_stop:
            mgr.image_needs_save = False

        last_relay_state = relay_is_on

        if scale is not None and scale.connected:
            force_ready = mgr.should_show_ready_screen()
            (last_sample_time, last_weight) = update_display(scale, mgr, display, last_sample_time, last_weight, timeout_stop, force_ready)
        else:
            display.display_off()
            # Reset timing variables on disconnect
            last_sample_time = None
            last_weight = None
            
        time.sleep(refreshRate)
        
    if scale.connected:
        try:
            scale.disconnect()
        except Exception as ex:
            logging.error("Error during shutdown: %s" % str(ex))
    if display is not None:
        display.stop()
    logging.info("Exiting on stop")


TARE_SETTLE_SECONDS = 1.5

def update_display(scale: Scale, mgr: ControlManager, display: Display, last_time: float, last_weight: float, timeout_stop: bool = False, force_ready: bool = False) -> (float, float):
    now = timer()
    weight = scale.weight
    sample_rate = 0.0
    if last_time is not None and last_weight is not None:
        sample_rate = now - last_time
        changed = weight - last_weight
        g_per_s = round(1 / sample_rate * changed, 1)

        shot_elapsed = mgr.shot_time_elapsed()
        if mgr.relay_on() and shot_elapsed < TARE_SETTLE_SECONDS:
            # Shot just started: the auto-tare lands asynchronously a moment
            # later and steps the weight down (e.g. cup weight -> 0), which
            # would register as a large negative flow spike. Skip these samples
            # and let the post-tare weight become the new baseline.
            pass
        else:
            # During a shot, weight only increases; a negative reading here is
            # scale noise or a late tare step, so clamp it to zero.
            if mgr.relay_on() and g_per_s < 0:
                g_per_s = 0.0
            mgr.add_flow_rate_data(g_per_s)
    data = DisplayData(weight, sample_rate, mgr.current_memory(), mgr.flow_rate_data,
                       scale.battery, mgr.relay_on(), mgr.shot_time_elapsed(),
                       mgr.image_needs_save, smoothing, timeout_stop=timeout_stop, force_ready=force_ready,
                       vendor=getattr(scale, 'vendor', None))
    display.display_on()
    display.put_data(data)
    mgr.image_needs_save = False
    return now, weight


def shutdown(sig, frame):
    global stop
    stop = True


if __name__ == '__main__':
    signal.signal(signal.SIGINT, shutdown)
    main()