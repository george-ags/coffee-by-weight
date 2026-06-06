import math
import logging
import pickle
import time
import copy
import threading
import os
from collections import deque
from timeit import default_timer as timer
from typing import Optional, Callable

from gpiozero import Button, DigitalOutputDevice

from lib.scale_acaia import AcaiaScale, find_acaia_devices

default_target = 36.0
default_overshoot = 1.0
memory_save_file = "memory.save"

# --- Overshoot learning (adaptive "cut early" margin) ---
OVERSHOOT_LEARNING_RATE = 0.5    # fraction of each shot's error absorbed (EMA gain)
MAX_OVERSHOOT_ERROR     = 5.0    # ignore shots that miss by more than this (anomaly)
MIN_OVERSHOOT           = 0.0    # never cut after the target
MAX_OVERSHOOT           = 5.0    # physical ceiling on drip-out margin

# Weight delta (g) that counts as user activity for the auto-sleep timer
ACTIVITY_WEIGHT_THRESHOLD = float(os.environ.get('ACTIVITY_WEIGHT_THRESHOLD', '0.3'))


class TargetMemory:
    def __init__(self, name: str, color="#ff1303"):
        self.name: str = name
        self.target: float = default_target
        self.overshoot: float = default_overshoot
        self.color: str = color
        self.shot_count: int = 0

    def target_minus_overshoot(self) -> float:
        return self.target - self.overshoot

    def update_overshoot(self, weight: float):
        error = weight - self.target

        # 1. Reject anomalous shots — guard the ERROR, not the accumulated value.
        #    A huge miss means a bumped scale / wrong basket / missed cutoff, not
        #    a real change in drip-out, so we refuse to learn from it.
        if abs(error) > MAX_OVERSHOOT_ERROR:
            logging.error(
                "Shot ended %.2fg off target (%.2f vs %.2f) - outside sanity range, not learning"
                % (error, weight, self.target))
            return

        # 2. Adaptive rate: trust the first shot fully, then settle to a smoothing
        #    gain so a single odd shot can't yank the value around.
        #    getattr() guards memories pickled before shot_count existed.
        self.shot_count = getattr(self, 'shot_count', 0) + 1
        alpha = max(OVERSHOOT_LEARNING_RATE, 1.0 / self.shot_count)

        # 3. Move a fraction of the way toward correcting the error (EMA).
        new_overshoot = self.overshoot + alpha * error

        # 4. Clamp the RESULT to a physically sensible range.
        new_overshoot = min(MAX_OVERSHOOT, max(MIN_OVERSHOOT, new_overshoot))

        logging.info("Overshoot %s: %.2f -> %.2f (err %.2fg, a=%.2f, shot #%d)"
                     % (self.name, self.overshoot, new_overshoot, error, alpha, self.shot_count))
        self.overshoot = new_overshoot


class ControlManager:
    TARE_GPIO = 4
    MEM_GPIO = 21
    SCALE_CONNECT_GPIO = 5
    TGT_INC_GPIO = 12
    TGT_DEC_GPIO = 16
    PADDLE_GPIO = 20
    RELAY_GPIO = 26

    def __init__(self, max_flow_points=500):
        self.flow_rate_data = deque([])
        self.flow_rate_max_points = max_flow_points
        self.relay_off_time = timer()
        self.shot_timer_start: Optional[float] = None
        self.image_needs_save = False
        self.running = True

        # Serializes all relay state transitions (start vs stop) across the
        # paddle callback thread, the watchdog thread, and the main loop.
        self._relay_lock = threading.Lock()

        # Tare callback, wired up later via add_tare_handler().
        self._tare_callback: Optional[Callable] = None

        # --- AUTO-SLEEP CONFIG ---
        self.idle_timeout = int(os.environ.get('IDLE_TIMEOUT', 300))
        self.sleep_pause = int(os.environ.get('SLEEP_PAUSE', 360))

        # Seconds of inactivity after which the screen reverts to the Ready/logo
        # view (softer than full sleep: stays connected and the screen stays on).
        # Should be <= idle_timeout to be visible before the system sleeps.
        self.ready_screen_timeout = int(os.environ.get('READY_SCREEN_TIMEOUT', 180))

        self.last_activity = timer()
        self.is_sleeping = False
        self.sleep_end_time = 0.0
        self.last_weight_check = 0.0

        # Latched Ready/logo state. Set True once READY_SCREEN_TIMEOUT of idle
        # passes; stays True (sticky) through weight wiggle and button presses,
        # and is only cleared when a new shot starts. This keeps the logo on
        # screen until the next pour rather than flipping back to the old graph.
        self.ready_screen_active = False

        # --- WATCHDOG LATCH ---
        # If True, we ignore the paddle until it is toggled OFF and back ON.
        self.paddle_release_required = False

        # ASYNC SCANNER VARIABLES
        self.discovered_mac: Optional[str] = None
        self.scale_is_connected_flag = False

        self.load_memory()

        self.relay = DigitalOutputDevice(ControlManager.RELAY_GPIO)

        # TARGET BUTTONS
        self.tgt_inc_button = Button(ControlManager.TGT_INC_GPIO, hold_time=0.5, hold_repeat=True, pull_up=True, bounce_time=0.02)
        self.tgt_inc_button.when_released = lambda: (self._activity_detected(), self._change_target(0.1))
        self.tgt_inc_button.when_held = lambda: (self._activity_detected(), self._change_target_held(1))

        self.tgt_dec_button = Button(ControlManager.TGT_DEC_GPIO, hold_time=0.5, hold_repeat=True, pull_up=True, bounce_time=0.02)
        self.tgt_dec_button.when_released = lambda: (self._activity_detected(), self._change_target(-0.1))
        self.tgt_dec_button.when_held = lambda: (self._activity_detected(), self._change_target_held(-1))

        # PADDLE SWITCH
        self.paddle_switch = Button(ControlManager.PADDLE_GPIO, pull_up=True, bounce_time=0.05)
        self.paddle_switch.when_pressed = lambda: (self._activity_detected(), self._start_shot())

        # --- TARE BUTTON (5s long-press restarts the service) ---
        self.tare_button = Button(ControlManager.TARE_GPIO, pull_up=True, hold_time=5.0)
        self.tare_button.when_held = lambda: self._restart_service()

        self.memory_button = Button(ControlManager.MEM_GPIO, pull_up=True)
        self.memory_button.when_pressed = lambda: (self._activity_detected(), self._rotate_memory())

        self.scale_connect_button = Button(ControlManager.SCALE_CONNECT_GPIO, pull_up=True)
        self.scale_connect_button.when_pressed = lambda: self._activity_detected()

        self.tgt_button_was_held = False

        # START THREADS
        self.wd_thread = threading.Thread(target=self._watchdog_loop)
        self.wd_thread.daemon = True
        self.wd_thread.start()

        self.scan_thread = threading.Thread(target=self._bg_scan_loop)
        self.scan_thread.daemon = True
        self.scan_thread.start()

    def _restart_service(self):
        logging.warning("Tare button held for 5 seconds! Force restarting service...")
        # Service runs as root, so we can restart it directly. The '&' backgrounds
        # the command so it doesn't block python while systemd kills us.
        os.system("systemctl restart lm-bbw &")

    # --- AUTO-SLEEP LOGIC ---
    def _activity_detected(self):
        self.last_activity = timer()
        if self.is_sleeping:
            logging.info("Activity Detected -> Waking Up from Sleep Mode")
            self.is_sleeping = False

    def check_auto_sleep(self, scale: AcaiaScale):
        now = timer()

        # Check for weight change (Activity)
        if scale.connected:
            if abs(scale.weight - self.last_weight_check) > ACTIVITY_WEIGHT_THRESHOLD:
                self._activity_detected()
            self.last_weight_check = scale.weight

            # Logic: Enter Sleep
            if not self.is_sleeping:
                if (now - self.last_activity) > self.idle_timeout:
                    logging.info(f"No activity for {self.idle_timeout}s -> Sleep Mode Active (Scanner Paused)")
                    self.is_sleeping = True
                    self.sleep_end_time = now + self.sleep_pause

                    # Disconnect scale if connected
                    logging.info("Disconnecting scale for sleep...")
                    scale.disconnect()

        # Logic: Auto-Wake after pause
        elif self.is_sleeping:
            if now > self.sleep_end_time:
                logging.info("Sleep Pause Timeout Reached -> Auto-Waking System")
                self._activity_detected()  # Resets flags and timers
    # ------------------------

    def should_show_ready_screen(self) -> bool:
        """
        Returns the latched Ready/logo state.

        Becomes True once there has been no activity for READY_SCREEN_TIMEOUT
        seconds (and not mid-shot). Once latched it STAYS True through weight
        changes and button presses — it is only cleared when a new shot starts
        (see _start_shot). This makes the logo sticky until the next pour rather
        than reverting to the previous flow graph on any activity.
        """
        if self.relay_on():
            # Mid-shot: never show the ready screen. (The latch is cleared at
            # shot start anyway; this is just belt-and-suspenders.)
            return False

        if not self.ready_screen_active:
            if (timer() - self.last_activity) > self.ready_screen_timeout:
                self.ready_screen_active = True
                logging.info("Idle for %ds -> reverting to Ready/logo screen" % self.ready_screen_timeout)

        return self.ready_screen_active

    def _watchdog_loop(self):
        logging.info("Paddle Watchdog Started")

        OPEN_CONFIRM_READS = 4      # consecutive open reads needed to stop
        OPEN_READ_INTERVAL = 0.04   # ~160 ms total confirmation window

        while self.running:

            # 1. HANDLE MANUAL STOP (Paddle moved to OFF)
            #    Require the paddle to read open across several samples so a brief
            #    electrical transient (e.g. pump spin-up coupling) can't kill a shot.
            if self.relay_on() and not self.paddle_switch.is_pressed:
                confirmed_open = True
                for _ in range(OPEN_CONFIRM_READS - 1):
                    time.sleep(OPEN_READ_INTERVAL)
                    if self.paddle_switch.is_pressed:
                        confirmed_open = False   # glitch — paddle came back
                        break
                # re-check relay too, in case the target-weight stop already ran
                if confirmed_open and self.relay_on() and not self.paddle_switch.is_pressed:
                    logging.info("Watchdog detected paddle OPEN - Stopping shot")
                    self.disable_relay()

            # 2. HANDLE LATCH RESET (Paddle is OFF)
            #    If paddle is OFF, we are allowed to start a new shot next time.
            if not self.paddle_switch.is_pressed:
                self.paddle_release_required = False

            # 3. HANDLE START (Paddle CLOSED)
            if not self.relay_on() and self.paddle_switch.is_pressed:

                # Safety latch: paddle is ON but we haven't seen it go OFF yet.
                # Ignore it to prevent an auto-restart loop after an auto-stop.
                if self.paddle_release_required:
                    time.sleep(0.05)
                    continue

                time.sleep(0.05)  # Debounce
                if self.paddle_switch.is_pressed:
                    if not self.relay_on():
                        logging.info("Watchdog detected paddle CLOSED - Force Starting shot")
                        self._activity_detected()
                        self._start_shot()

            time.sleep(0.05)

    def _bg_scan_loop(self):
        logging.info("Bluetooth Background Scanner Started")
        while self.running:

            # --- PAUSE SCANNING IF SLEEPING ---
            if self.is_sleeping:
                time.sleep(1)
                continue
            # ----------------------------------

            if self.should_scale_connect() and not self.scale_is_connected_flag and self.discovered_mac is None:
                try:
                    devices = find_acaia_devices(timeout=1)
                    if devices:
                        self.discovered_mac = devices[0]
                        logging.info("Scanner found Scale: %s (Handing over to Main Thread)" % self.discovered_mac)
                        time.sleep(1)
                    else:
                        time.sleep(6)
                except Exception as e:
                    # --- SAFETY NET: AUTO-RESTART IF D-BUS IS DEAD ---
                    err_str = str(e)
                    if "AccessDenied" in err_str or "registered" in err_str or "Hello" in err_str:
                        logging.fatal(f"CRITICAL: D-Bus Connection Limit Reached. Restarting Service... Error: {err_str}")
                        # Failsafe: never leave the pump energised across a hard exit.
                        try:
                            self.relay.off()
                        except Exception:
                            pass
                        os._exit(1)  # Kill Process. Systemd will restart it.
                    # -------------------------------------------------

                    logging.error("Scanner Error: %s" % e)
                    time.sleep(10)
            else:
                time.sleep(1)

    def save_memory(self):
        self._save_worker(self.memories)

    def _save_worker(self, data_to_save):
        try:
            with open(memory_save_file, 'wb') as savefile:
                pickle.dump(data_to_save, savefile)
                logging.info("Saved shot data to memory")
        except Exception as e:
            logging.error("Error persisting memory: %s" % e)

    def load_memory(self):
        # Read colors from env, falling back to the current defaults.
        color_a = os.environ.get('MEMORY_A_COLOR', '#ff0000')  # Red
        color_b = os.environ.get('MEMORY_B_COLOR', '#00ff00')  # Green
        color_c = os.environ.get('MEMORY_C_COLOR', '#0000ff')  # Blue

        def fresh_memories():
            return deque([TargetMemory("A", color_a),
                          TargetMemory("B", color_b),
                          TargetMemory("C", color_c)])

        if not os.path.exists(memory_save_file):
            logging.info("No saved memory found - initializing defaults")
            self.memories = fresh_memories()
            return

        try:
            with open(memory_save_file, 'rb') as savefile:
                self.memories = pickle.load(savefile)
        except Exception as e:
            # The file exists but couldn't be read/unpickled. Don't silently
            # discard it — preserve it for inspection, then start fresh.
            logging.error("Failed to load memory (%s). Backing up and resetting to defaults." % e)
            try:
                backup = memory_save_file + ".corrupt"
                os.replace(memory_save_file, backup)
                logging.error("Moved unreadable memory file to %s" % backup)
            except Exception as be:
                logging.error("Could not back up bad memory file: %s" % be)
            self.memories = fresh_memories()
            return

        # Backfill new fields on objects pickled by older versions, and apply
        # color overrides from env (lets colors change without wiping memory).
        for mem in self.memories:
            if not hasattr(mem, 'shot_count'):
                mem.shot_count = 0
            if mem.name == "A":
                mem.color = color_a
            elif mem.name == "B":
                mem.color = color_b
            elif mem.name == "C":
                mem.color = color_c

    def add_tare_handler(self, callback: Callable):
        # Store the callback and wire the physical button to it (with activity).
        self._tare_callback = callback
        self.tare_button.when_pressed = lambda: (self._activity_detected(), self._do_tare())

    def _do_tare(self):
        if self._tare_callback is not None:
            self._tare_callback()

    def should_scale_connect(self) -> bool:
        return self.scale_connect_button.value

    def relay_on(self) -> bool:
        return self.relay.value

    def add_flow_rate_data(self, data_point: float):
        if self.relay_on() or self.relay_off_time + 3.0 > timer():
            self.flow_rate_data.append(data_point)
            if len(self.flow_rate_data) > self.flow_rate_max_points:
                self.flow_rate_data.popleft()

    def disable_relay(self):
        # Take the lock only around the state transition. The memory save and the
        # thread start happen afterwards so we never hold the lock across I/O.
        do_save = False
        memories_snapshot = None

        with self._relay_lock:
            if not self.relay_on():
                return
            logging.info("disable relay")
            self.relay_off_time = timer()

            # If the paddle is still ON when we stop (i.e. target-weight auto-stop),
            # require it to be released before another shot can start.
            if self.paddle_switch.is_pressed:
                self.paddle_release_required = True

            self.relay.off()

            do_save = self.scale_is_connected_flag
            if do_save:
                memories_snapshot = copy.deepcopy(self.memories)

        if do_save:
            save_thread = threading.Thread(target=self._save_worker, args=(memories_snapshot,))
            save_thread.start()
        else:
            logging.info("Scale disconnected - Skipping memory save")

    def current_memory(self):
        return self.memories[0]

    def shot_time_elapsed(self):
        if self.shot_timer_start is None:
            return 0.0
        elif self.relay_on():
            return timer() - self.shot_timer_start
        else:
            return self.relay_off_time - self.shot_timer_start

    def _change_target(self, amount):
        if not self.tgt_button_was_held:
            self.memories[0].target += amount
        else:
            self.tgt_button_was_held = False

    def _change_target_held(self, amount):
        self.tgt_button_was_held = True
        if amount > 0:
            self.memories[0].target = math.floor(self.memories[0].target) + math.floor(amount)
        if amount < 0:
            self.memories[0].target = math.ceil(self.memories[0].target) + math.ceil(amount)

    def _rotate_memory(self):
        prev = self.memories[0].name
        self.memories.rotate(-1)
        new = self.memories[0]
        logging.info("Switched memory bank %s -> %s (target %.1fg, overshoot %.2fg)"
                      % (prev, new.name, new.target, new.overshoot))

    def _start_shot(self):
        # Take the lock only around the relay transition. The auto-tare (a BLE
        # write that can block) happens AFTER releasing the lock, so a slow or
        # hung tare can never delay an emergency stop on disable_relay().
        with self._relay_lock:
            if self.relay_on() or self.paddle_release_required:
                return

            logging.info("Start shot")
            self.flow_rate_data = deque([])

            # Leaving the Ready/logo screen for a live shot.
            self.ready_screen_active = False

            # Priority to relay (coffee first), then tare.
            self.shot_timer_start = timer()
            self.relay.on()

        if self.scale_is_connected_flag:
            try:
                logging.info("Scale Connected -> Auto-Taring...")
                self._do_tare()
            except Exception as e:
                logging.error(f"Auto-Tare failed (Shot continuing): {e}")
        else:
            logging.info("Scale Not Connected -> Skipping Tare")


def try_connect_scale(scale: AcaiaScale, mgr: ControlManager) -> bool:
    try:
        mgr.scale_is_connected_flag = scale.connected

        if not mgr.should_scale_connect():
            if scale.connected:
                logging.info("Scale connect switch detected OFF -> Disconnecting...")
                scale.disconnect()
            return False

        if scale.connected:
            return True

        if mgr.discovered_mac:
            logging.info("Main Thread connecting to found MAC: %s" % mgr.discovered_mac)

            # Reset idle timer immediately on a connect attempt.
            mgr._activity_detected()

            scale.mac = mgr.discovered_mac

            logging.info("Clearing old shot data (Preparing to Connect)")
            mgr.flow_rate_data.clear()

            # Only reset the shot timer if we are NOT mid-shot.
            if not mgr.relay_on():
                mgr.shot_timer_start = None

            # Check for ghost relay state (safety) under the relay lock.
            with mgr._relay_lock:
                if mgr.relay_on() and not mgr.paddle_switch.is_pressed:
                    logging.warning("Ghost Start detected during connection. Forcing Relay OFF.")
                    mgr.relay.off()

            scale.connect()

            mgr.discovered_mac = None
            return True

        return False

    except Exception as ex:
        logging.error("Failed to connect to found device:%s" % str(ex))
        mgr.discovered_mac = None
        return False