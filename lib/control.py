import math
import logging
import pickle
import time
import copy
import threading
import os
import sys
from collections import deque
from timeit import default_timer as timer
from typing import Optional, Callable

from gpiozero import Button, DigitalOutputDevice

import lib.pyacaia as pyacaia
from lib.pyacaia import AcaiaScale

default_target = 36.0
default_overshoot = 1.0
valid_overshoot_threshold = 5
memory_save_file = "memory.save"

class TargetMemory:
    def __init__(self, name: str, color="#ff1303"):
        self.name: str = name
        self.target: float = default_target
        self.overshoot: float = default_overshoot
        self.color: str = color

    def target_minus_overshoot(self) -> float:
        return self.target - self.overshoot

    def update_overshoot(self, weight: float):
        new_overshoot = self.overshoot + (weight - self.target)
        if abs(new_overshoot) > valid_overshoot_threshold:
            logging.error("New overshoot %.2f out of safe range, ignoring" % new_overshoot)
        else:
            self.overshoot = new_overshoot
            logging.debug("Set new overshoot to %.2f" % self.overshoot)

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
        
        # --- AUTO-SLEEP CONFIG ---
        self.idle_timeout = int(os.environ.get('IDLE_TIMEOUT', 300))
        self.sleep_pause = int(os.environ.get('SLEEP_PAUSE', 360))
        
        self.last_activity = timer()
        self.is_sleeping = False
        self.sleep_end_time = 0.0
        self.last_weight_check = 0.0
        
        # --- WATCHDOG LATCH (The Fix) ---
        # If True, we ignore the paddle until it is toggled OFF and back ON
        self.paddle_release_required = False
        # --------------------------------

        # ASYNC SCANNER VARIABLES
        self.discovered_mac: Optional[str] = None
        self.scale_is_connected_flag = False 
        
        self.load_memory()

        self.relay = DigitalOutputDevice(ControlManager.RELAY_GPIO)

        # TARGET BUTTONS
        self.tgt_inc_button = Button(ControlManager.TGT_INC_GPIO, hold_time=0.5, hold_repeat=True, pull_up=True, bounce_time=0.02)
        self.tgt_inc_button.when_released = lambda: (self._activity_detected(), self.__change_target(0.1))
        self.tgt_inc_button.when_held = lambda: (self._activity_detected(), self.__change_target_held(1))

        self.tgt_dec_button = Button(ControlManager.TGT_DEC_GPIO, hold_time=0.5, hold_repeat=True, pull_up=True, bounce_time=0.02)
        self.tgt_dec_button.when_released = lambda: (self._activity_detected(), self.__change_target(-0.1))
        self.tgt_dec_button.when_held = lambda: (self._activity_detected(), self.__change_target_held(-1))

        # PADDLE SWITCH
        self.paddle_switch = Button(ControlManager.PADDLE_GPIO, pull_up=True, bounce_time=0.05)
        self.paddle_switch.when_pressed = lambda: (self._activity_detected(), self.__start_shot())
        
        # --- TARE BUTTON (Updated for 5s Long Press) ---
        self.tare_button = Button(ControlManager.TARE_GPIO, pull_up=True, hold_time=5.0) 
        self.tare_button.when_held = lambda: self.__restart_service()
        # -----------------------------------------------

        self.memory_button = Button(ControlManager.MEM_GPIO, pull_up=True)
        self.memory_button.when_pressed = lambda: (self._activity_detected(), self.__rotate_memory())

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

    def __restart_service(self):
        logging.warning("Tare button held for 5 seconds! Force restarting service...")
        # Since the service runs as root, we can directly restart it via systemctl
        # The '&' ensures the command runs in the background so it doesn't block python while killing it
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
            if abs(scale.weight - self.last_weight_check) > 0.3: # 0.3g threshold for activity
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
                self._activity_detected() # Resets flags and timers
    # ------------------------

    def _watchdog_loop(self):
        logging.info("Paddle Watchdog Started")

        OPEN_CONFIRM_READS = 4      # consecutive open reads needed to stop
        OPEN_READ_INTERVAL = 0.04   # ~160 ms total confirmation window

        while self.running:

            # 1. HANDLE MANUAL STOP (Paddle moved to OFF)
            if self.relay_on() and not self.paddle_switch.is_pressed:
                confirmed_open = True
                for _ in range(OPEN_CONFIRM_READS - 1):
                    time.sleep(OPEN_READ_INTERVAL)
                    if self.paddle_switch.is_pressed:
                        confirmed_open = False   # glitch — paddle came back
                        break
                # re-check relay too, in case target-stop already ran
                if confirmed_open and self.relay_on() and not self.paddle_switch.is_pressed:
                    logging.info("Watchdog detected paddle OPEN - Stopping shot")
                    self.disable_relay()

            # 2. HANDLE LATCH RESET (Paddle is OFF)
            # If paddle is OFF, we are allowed to start a new shot next time
            if not self.paddle_switch.is_pressed:
                self.paddle_release_required = False

            # 3. HANDLE START (Paddle CLOSED)
            if not self.relay_on() and self.paddle_switch.is_pressed:
                
                # --- FIX: Check Safety Latch ---
                if self.paddle_release_required:
                    # Paddle is ON, but we haven't seen it go OFF yet.
                    # Ignore it (Prevent restart loop).
                    time.sleep(0.05)
                    continue
                # -------------------------------

                time.sleep(0.05) # Debounce
                if self.paddle_switch.is_pressed:
                    if not self.relay_on():
                        logging.info("Watchdog detected paddle CLOSED - Force Starting shot")
                        self._activity_detected()
                        self.__start_shot()
            
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
                    devices = pyacaia.find_acaia_devices(timeout=1)
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
                        os._exit(1) # Kill Process. Systemd will restart it.
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
        # Read colors from env, falling back to the current defaults
        color_a = os.environ.get('MEMORY_A_COLOR', '#ff0000') # Red
        color_b = os.environ.get('MEMORY_B_COLOR', '#00ff00') # Green
        color_c = os.environ.get('MEMORY_C_COLOR', '#0000ff') # Blue

        try:
            with open(memory_save_file, 'rb') as savefile:
                self.memories = pickle.load(savefile)
            
            # --- OVERRIDE SAVED COLORS WITH ENV CONFIG ---
            # This allows color updates without deleting memory.save
            for mem in self.memories:
                if mem.name == "A":
                    mem.color = color_a
                elif mem.name == "B":
                    mem.color = color_b
                elif mem.name == "C":
                    mem.color = color_c
            # ---------------------------------------------
            
        except Exception as e:
            logging.warn("Not able to load memory from save, resetting memory to defaults. Error was: %s" % e)
            self.memories = deque([TargetMemory("A", color_a), TargetMemory("B", color_b), TargetMemory("C", color_c)])

    def add_tare_handler(self, callback: Callable):
        # Modified to trigger activity
        self.tare_button.when_pressed = lambda: (self._activity_detected(), callback())

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
        if self.relay_on():
            logging.info("disable relay")
            self.relay_off_time = timer()
            
            # --- FIX: Engage Safety Latch ---
            # If the paddle is currently ON when we stop (Auto-Stop), 
            # we require it to be released before next shot.
            if self.paddle_switch.is_pressed:
                self.paddle_release_required = True
            # --------------------------------
            
            self.relay.off()
            
            if self.scale_is_connected_flag:
                memories_snapshot = copy.deepcopy(self.memories)
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

    def __change_target(self, amount):
        if not self.tgt_button_was_held:
            self.memories[0].target += amount
        else:
            self.tgt_button_was_held = False

    def __change_target_held(self, amount):
        self.tgt_button_was_held = True
        if amount > 0:
            self.memories[0].target = math.floor(self.memories[0].target) + math.floor(amount)
        if amount < 0:
            self.memories[0].target = math.ceil(self.memories[0].target) + math.ceil(amount)

    def __rotate_memory(self):
        self.memories.rotate(-1)

    def __start_shot(self):
        # Additional safety check against rapid restarts
        if self.relay_on() or self.paddle_release_required:
            return
            
        logging.info("Start shot")
        self.flow_rate_data = deque([])
        
        # --- LOGIC: Priority to Relay (Coffee First) ---
        self.shot_timer_start = timer()
        self.relay.on()
        
        if self.scale_is_connected_flag:
            try:
                if self.tare_button.when_pressed:
                    logging.info("Scale Connected -> Auto-Taring...")
                    self.tare_button.when_pressed()
            except Exception as e:
                logging.error(f"Auto-Tare failed (Shot continuing): {e}")
        else:
            logging.info("Scale Not Connected -> Skipping Tare")
        # -----------------------------------------------

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
            
            # --- FIX 1: Reset Idle Timer immediately ---
            mgr._activity_detected() 
            # -------------------------------------------

            scale.mac = mgr.discovered_mac
            
            logging.info("Clearing old shot data (Preparing to Connect)")
            mgr.flow_rate_data.clear()
            
            # --- FIX 2: Only reset timer if we are NOT mid-shot ---
            if not mgr.relay_on():
                mgr.shot_timer_start = None
            # ------------------------------------------------------

            # Check for ghost relay state (Safety)
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