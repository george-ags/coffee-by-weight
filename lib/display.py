import logging
import math
import os
import time
import traceback
import pandas as pd
from datetime import datetime
from enum import Enum
from multiprocessing import Process, Queue
from queue import Empty
from typing import Optional

from PIL import Image, ImageFont, ImageDraw

# --- FONT CONFIGURATION ---
try:
    label_font = ImageFont.truetype("lib/font/LiberationMono-Regular.ttf", 16)
    label_font_sml = ImageFont.truetype("lib/font/LiberationMono-Regular.ttf", 12)
    label_font_mid = ImageFont.truetype("lib/font/LiberationMono-Regular.ttf", 20)
    label_font_lg = ImageFont.truetype("lib/font/LiberationMono-Regular.ttf", 24)
    value_font = ImageFont.truetype("lib/font/Quicksand-Regular.ttf", 24)
    value_font_med = ImageFont.truetype("lib/font/Quicksand-Regular.ttf", 28) 
    value_font_lg = ImageFont.truetype("lib/font/Quicksand-Regular.ttf", 36)
    value_font_lg_bold = ImageFont.truetype("lib/font/Quicksand-Bold.ttf", 36)
except Exception as e:
    # If the font files are missing, fall back to PIL's built-in font so the
    # display degrades gracefully instead of crashing later with NameError on
    # the first draw.text() call.
    logging.error(f"Error loading fonts, falling back to default: {e}")
    _default = ImageFont.load_default()
    label_font = label_font_sml = label_font_mid = label_font_lg = _default
    value_font = value_font_med = value_font_lg = value_font_lg_bold = _default

# --- GRAPH CONFIGURATION ---
Graph_Max_Display_Value = 4
Graph_Density_Threshold = 6
Drip_Out_Window = float(os.environ.get('DRIP_OUT_WINDOW', '3.5'))

# --- SCREEN CONFIGURATION ---
display_brightness = int(os.environ.get('DISPLAY_BRIGHTNESS', '100'))

# --- MEMORY BANK DISPLAY NAMES ---
# Optional human-readable names per bank. If set, the header shows the name
# (e.g. "Espresso") instead of "TARGET A". Blank/unset falls back to "TARGET X".
MEMORY_NAMES = {
    'A': os.environ.get('MEMORY_A_NAME', '').strip(),
    'B': os.environ.get('MEMORY_B_NAME', '').strip(),
    'C': os.environ.get('MEMORY_C_NAME', '').strip(),
}

def memory_label(bank_name: str) -> str:
    """Human name for a bank if configured, else the default 'TARGET X'."""
    custom = MEMORY_NAMES.get(bank_name, '')
    return custom if custom else ("TARGET %s" % bank_name)

# --- LOGO CONFIGURATION ---
IMG_DIR="/opt/lm-bbw/lib/img/"

logo_img = None
coffee_cup_img = None
warning_img = None
lion_img = None

try:
    # Load Main Logo
    logo_path = IMG_DIR + "lamarzocco.png"
    if os.path.exists(logo_path):
        raw_img = Image.open(logo_path).convert("RGBA")
        resize_factor = 0.7
        new_w = int(raw_img.width * resize_factor)
        new_h = int(raw_img.height * resize_factor)
        logo_img = raw_img.resize((new_w, new_h))

    # Load Lion Logo (shown in the idle "Ready" area)
    lion_path = IMG_DIR + "LM-lion-logo.png"
    if os.path.exists(lion_path):
        raw_lion_img = Image.open(lion_path).convert("RGBA")
        # Scale to fit a target height while preserving aspect ratio.
        lion_target_h = 120
        lion_w = int(lion_target_h * (raw_lion_img.width / raw_lion_img.height))
        lion_img = raw_lion_img.resize((lion_w, lion_target_h))
        
    # Load Coffee Cup Icon
    cup_path = IMG_DIR + "coffee-cup.png"
    if os.path.exists(cup_path):
        raw_cup_img = Image.open(cup_path).convert("RGBA")
        # Resize cup to perfectly match the main value font height (24px)
        cup_h = 24
        cup_w = int(cup_h * (raw_cup_img.width / raw_cup_img.height))
        coffee_cup_img = raw_cup_img.resize((cup_w, cup_h))

    # Load Warning Icon (shown when shot is stopped by timeout)
    warn_path = IMG_DIR + "warning.png"
    if os.path.exists(warn_path):
        raw_warn_img = Image.open(warn_path).convert("RGBA")
        warn_target_h = 120
        warn_w = int(warn_target_h * (raw_warn_img.width / raw_warn_img.height))
        warning_img = raw_warn_img.resize((warn_w, warn_target_h))

except Exception as e:
    logging.error(f"Error loading images: {e}")

# --- COLORS ---
bg_color = "BLACK"
light_bg_color = "DIMGREY"
fg_color = "WHITE"

# --- HELPER: Draw Battery Icon ---
def draw_battery(draw, xy, level, scale=1.0):
    x, y = xy
    w = int(24 * scale)
    h = int(12 * scale)
    terminal_w = int(3 * scale)
    padding = 2
    
    if level < 20: fill_color = "RED"
    elif level < 50: fill_color = "YELLOW"
    else: fill_color = "GREEN"

    draw.rectangle((x, y, x + w, y + h), outline=fill_color, fill=bg_color, width=2)
    
    term_y_start = y + int(h * 0.25)
    term_y_end = y + int(h * 0.75)
    draw.rectangle((x + w, term_y_start, x + w + terminal_w, term_y_end), fill=fill_color)
    
    max_fill_w = w - (padding * 2)
    current_fill_w = int(max_fill_w * (level / 100.0))
    if level > 0 and current_fill_w < 1: current_fill_w = 1
        
    if current_fill_w > 0:
        draw.rectangle((x + padding, y + padding, x + padding + current_fill_w, y + h - padding), fill=fill_color)

# --- HELPER: Draw Paddle/Toggle Switch ---
def draw_paddle_switch(draw, xy, is_on, color, scale=1.0):
    x, y = xy
    y -= 1
    w = int(32 * scale)
    h = int(14 * scale)
    padding = 2
    knob_dia = h - (padding * 2)
    
    # Draw Track
    draw.rectangle((x, y, x + w, y + h), outline=fg_color, fill=bg_color, width=2)

    if is_on:
        knob_color = color
        knob_x = x + padding
    else:
        knob_color = color
        knob_x = x + w - padding - knob_dia

    # Draw Knob
    knob_y = y + padding
    draw.ellipse((knob_x, knob_y, knob_x + knob_dia, knob_y + knob_dia), fill=knob_color, outline=fg_color, width=1)

# --- FIX: HELPER Calculate Smart Average ---
def calculate_smart_average(data) -> float:
    """
    Calculates average flow from the first drop (>0.2g/s) until the end.
    Falls back to simple average if calculation fails to ensure summary always displays.
    """
    if data.shot_time_elapsed <= 0:
        return 0.0
        
    start_index = 0
    threshold = 0.2
    
    raw_flow = list(data.flow_data)
    
    if not raw_flow:
        return data.weight / data.shot_time_elapsed
        
    # Find start of flow
    for i, val in enumerate(raw_flow):
        if val > threshold:
            start_index = i
            break
    
    total_samples = len(raw_flow)
    active_samples = total_samples - start_index
    
    if active_samples > 0:
        effective_duration = data.shot_time_elapsed * (active_samples / total_samples)
        if effective_duration > 0.5:
            return data.weight / effective_duration
            
    # Fallback to simple average
    return data.weight / data.shot_time_elapsed

# --- CLASS: Flow Graph Renderer ---
class FlowGraph:
    def __init__(self, flow_data: list, series_color="BLUE", label_color="#c7c7c7", line_color="#5a5a5a", max_value=Graph_Max_Display_Value,
                 width_pixels=240, height_pixels=160, avg_flow=None, grid_step=1, final_weight=None):
        self.flow_data = flow_data
        self.max_value = int(os.environ.get('GRAPH_MAX_VALUE', max_value))
        self.value_density_threshold = int(os.environ.get('GRAPH_MAX_DENSITY_THRESHOLD', Graph_Density_Threshold))
        self.series_color = series_color
        self.label_color = label_color
        self.line_color = line_color
        self.y_pix = height_pixels
        self.x_pix = width_pixels
        self.avg_flow = avg_flow
        self.final_weight = final_weight
        self.grid_step = grid_step
        self.y_pix_interval = height_pixels / self.max_value
        if len(flow_data) > 0:
            self.x_pix_interval = width_pixels / len(flow_data)
        else:
            self.x_pix_interval = width_pixels / 1


    def generate_graph(self) -> Image:
        points = list()
        i = 0
        for y in self.flow_data:
            x_coord = i * self.x_pix_interval if i * self.x_pix_interval < self.x_pix else self.x_pix
            y_coord = y * self.y_pix_interval + 2 if y * self.y_pix_interval < self.y_pix else self.y_pix
            y_coord = abs(y_coord - self.y_pix)
            points.append((x_coord, y_coord))
            i += 1

        # Collapse points that land on the same x pixel (keep mean y) so the line
        # isn't over-tessellated into visible facets.
        if len(points) > self.x_pix:
            collapsed = {}
            for (px, py) in points:
                key = int(round(px))
                if key in collapsed:
                    collapsed[key] = (collapsed[key] + py) / 2.0
                else:
                    collapsed[key] = py
            points = [(x, collapsed[x]) for x in sorted(collapsed)]

        img = Image.new("RGBA", (self.x_pix, self.y_pix), "BLACK")
        draw = ImageDraw.Draw(img)

        # Draw a line for every 'grid_step' from 0 to max_value
        for v in range(0, self.max_value + 1, self.grid_step):
            # Calculate pixel Y position (inverted)
            y_pos = self.y_pix - (v * self.y_pix_interval)
            
            # Adjust edges to keep lines visible
            if v == 0: y_pos -= 1 
            
            # Determine Color (Top/Bottom = Bright, Middle = Dim)
            color = self.line_color
            if v == 0 or v == self.max_value:
                color = self.label_color
                
            # Draw horizontal line
            self.__draw_y_line(draw, y_pos, color)

            # Draw Labels (Only Even numbers if max_value > MAX_GRAPH_DENSITY_THRESHOLD to avoid clutter)
            if v > 0 and v < self.max_value and (self.max_value < self.value_density_threshold or v % 2 == 0):
                draw.text((2, y_pos - 8), str(v), self.label_color, label_font)

        draw.line(points, fill=self.series_color, width=2)

        # Logic: If we have a frozen/sticky average AND weight, show them side-by-side
        if self.avg_flow is not None and self.final_weight is not None:
            display_val = self.avg_flow
            lbl_top = "avg"
            lbl_bot = "g/s"
            
            fmt_flow = "{:0.1f}".format(display_val)
            fmt_weight = "{:0.1f}g".format(self.final_weight)
            
            w_weight = draw.textlength(fmt_weight, value_font)
            w_separator = draw.textlength(" | ", label_font)
            w_flow = draw.textlength(fmt_flow, value_font)
            
            # Measure widths to perfectly center the stacked text
            w_lbl_top = draw.textlength(lbl_top, label_font_sml)
            w_lbl_bot = draw.textlength(lbl_bot, label_font_sml)
            w_label_block = max(w_lbl_top, w_lbl_bot)
            
            padding_labels = 4 # Small gap between the number and stacked text
            
            # Space needed for cup image + a small padding gap
            w_cup = coffee_cup_img.width + 6 if coffee_cup_img else 0
            
            total_w = w_cup + w_weight + w_separator + w_flow + padding_labels + w_label_block
            start_x = self.x_pix - 4 - total_w
            
            y_base = (self.y_pix * .25) - value_font.size - 4
            y_label_base = (self.y_pix * .25) - label_font.size - 4
            
            curr_x = start_x
            
            # Draw Cup Image
            if coffee_cup_img:
                cup_y = int(y_base + (value_font.size - coffee_cup_img.height) / 2) + 2
                img.paste(coffee_cup_img, (int(curr_x), cup_y), coffee_cup_img)
                curr_x += w_cup

            # Draw weight
            draw.text((curr_x, y_base), fmt_weight, fg_color, value_font)
            curr_x += w_weight
            
            # Draw separator
            draw.text((curr_x, y_label_base), " | ", fg_color, label_font)
            curr_x += w_separator
            
            # Draw flow
            draw.text((curr_x, y_base), fmt_flow, fg_color, value_font)
            curr_x += w_flow + padding_labels
            
            # Draw stacked labels (size 12 + size 12 = size 24 value height)
            x_top = curr_x + (w_label_block - w_lbl_top) / 2
            x_bot = curr_x + (w_label_block - w_lbl_bot) / 2
            
            draw.text((x_top, y_base), lbl_top, fg_color, label_font_sml)
            draw.text((x_bot, y_base + 12), lbl_bot, fg_color, label_font_sml)

        else:
            # Fallback to real-time last value
            display_val = self.flow_data[-1] if len(self.flow_data) > 0 else 0
            label = "g/s"

            fmt_flow = "{:0.1f}".format(display_val)
            w = draw.textlength(fmt_flow, value_font)
            wl = draw.textlength(label, label_font)
            draw.text(((self.x_pix - 4 - w - wl), (self.y_pix * .25) - value_font.size - 4), fmt_flow, fg_color, value_font)
            draw.text(((self.x_pix - wl), (self.y_pix * .25) - label_font.size - 4), label, fg_color, label_font)
            
        return img

    def __draw_y_line(self, draw: ImageDraw, y, color):
        draw.line((0, y, self.x_pix, y), fill=color, width=1)


# --- CLASS: Data Container ---
class DisplayData:
    def __init__(self, weight: float, sample_rate: float, memory, flow_data: list, battery: int,
                 paddle_on: bool, shot_time_elapsed: float, save_image: bool = False,
                 flow_smooth_factor: int = 10, timeout_stop: bool = False, force_ready: bool = False):
        self.weight = weight
        self.sample_rate = sample_rate
        self.memory = memory
        self.flow_data = flow_data
        self.battery = battery
        self.paddle_on = paddle_on
        self.shot_time_elapsed = shot_time_elapsed
        self.save_image = save_image
        self.flow_smooth_factor = flow_smooth_factor
        self.timeout_stop = timeout_stop
        # When True, the display reverts to the Ready/logo screen (clears the
        # post-shot summary and graph). Set after READY_SCREEN_TIMEOUT of idle.
        self.force_ready = force_ready

    def flow_rate_moving_avg(self) -> list:
        if not self.flow_data:
            return []
        s = pd.Series(self.flow_data)
        # Centered rolling mean: averages flow_smooth_factor//2 samples on each
        # side. A mean (unlike a median) preserves the peak/plateau amplitude of
        # the flow envelope, and center=True avoids lag/left-shift. min_periods=1
        # keeps the curve full-length (no leading NaNs to drop).
        return s.rolling(self.flow_smooth_factor, min_periods=1, center=True).mean().to_list()

class DisplaySize(Enum):
    SIZE_2_4 = 1
    SIZE_2_0 = 2

class DisplayOrientation(str, Enum):
    PORTRAIT = "PORTRAIT"
    LANDSCAPE = "LANDSCAPE"

    @classmethod
    def _missing_(cls, value):
        if isinstance(value, str):
            value_upper = value.upper()
            for member in cls:
                if member.value == value_upper:
                    return member
        return None

# --- CLASS: Hardware Controller ---
class Display:
    def __init__(self, data_queue: Queue, display_size: DisplaySize = DisplaySize.SIZE_2_0, image_save_dir: str = None):
        self.data_queue: Queue[DisplayData] = data_queue
        self.display_size = display_size
        self.image_save_dir = image_save_dir
        self.display_orientation = DisplayOrientation(os.environ.get('DISPLAY_ORIENTATION', DisplayOrientation.LANDSCAPE))
        
        self.lcd = None
        self.process = None
        
        self.last_paddle_state = False
        self.frozen_avg = None
        self.frozen_weight = None
        self.shot_stop_time = 0.0
        self.drip_out_locked = True
        
        # --- End-to-End Tracking ---
        self.first_drop_time = None
        self.shot_duration = 0.0

        # --- Timeout-Stop Warning ---
        self.show_warning = False
        self.warn_flash_state = False
        self.warn_flash_time = 0.0

        # --- Render gating ---
        # Signature of the last frame actually drawn. When idle, identical
        # frames are skipped so we don't redraw 10x/sec and peg the CPU.
        self.last_render_sig = None

    def start(self):
        self.process = Process(target=self.__update_display)
        self.process.start()

    def stop(self):
        if self.process is not None:
            self.process.kill()
        self.display_off()

    def display_off(self):
        pass

    def display_on(self):
        pass

    def put_data(self, data: DisplayData):
        self.data_queue.put_nowait(data)

    def save_image(self, img: Image):
        if self.image_save_dir is None:
            logging.info("no directory set to save image")
            return
        if not os.path.exists(self.image_save_dir):
            return
        date = datetime.now().strftime("%Y-%m-%d_%I:%M:%S_%p")
        absolute_path = "{basedir}/{date}.png".format(basedir=self.image_save_dir, date=date)
        try:
            img.save(absolute_path)
        except Exception as ex:
            logging.error("Failed to save image: %s", str(ex))
            
    def _compute_avg(self, weight: float) -> float:
        """ Calculates accurate average completely independent of Graph History limit """
        if self.shot_duration <= 0:
            return 0.0
            
        if self.first_drop_time is not None and self.first_drop_time < self.shot_duration:
            effective_duration = self.shot_duration - self.first_drop_time
            if effective_duration > 0.5:
                return weight / effective_duration
                
        return weight / self.shot_duration

    def __update_display(self):
        # Hardware init
        try:
            from lib import LCD_2inch4, LCD_2inch
            if self.display_size == DisplaySize.SIZE_2_4:
                self.lcd = LCD_2inch4.LCD_2inch4()
            elif self.display_size == DisplaySize.SIZE_2_0:
                self.lcd = LCD_2inch.LCD_2inch()
            else:
                raise Exception("unknown display size configured: %s" % self.display_size.name)
            
            self.lcd.Init()
            self.lcd.clear()
            
            # --- CLEAN STARTUP: Force Black Frame ---
            w, h = (self.lcd.width, self.lcd.height) if self.display_orientation == DisplayOrientation.PORTRAIT else (self.lcd.height, self.lcd.width)
            img = Image.new("RGBA", (w, h), "BLACK")
            self.lcd.ShowImage(img, 0, 0)
            
            # --- Force Backlight OFF immediately on start ---
            try:
                self.lcd.bl_DutyCycle(0)
                self.lcd._pwm.stop()
            except Exception as e:
                logging.warning(f"Failed to kill backlight on init: {e}")
            
            logging.info("Display Hardware Initialized (Clean Start)")
        except Exception as e:
            logging.error(f"Display Hardware Init Failed: {e}")
            return

        screen_is_on = False

        while True:
            try:
                data = self.data_queue.get(timeout=2.0)

                # --- DRAIN STALE FRAMES ---
                # The producer pushes at the main-loop rate (~10 Hz). If a render
                # (PIL draw + SPI push) takes longer than that interval, frames
                # pile up and every update — including button presses — shows up
                # seconds late. Discard everything queued behind us and keep only
                # the newest frame so the screen always reflects current state.
                # The "save this shot image" flag rides on a single frame, so we
                # carry it forward onto the frame we actually render rather than
                # dropping it with the stale frames.
                pending_save = data.save_image
                while True:
                    try:
                        data = self.data_queue.get_nowait()
                        pending_save = pending_save or data.save_image
                    except Empty:
                        break
                data.save_image = pending_save
                # --------------------------

                # Wake Logic
                just_woke = False
                if not screen_is_on:
                    try:
                        self.lcd._pwm.start(display_brightness) 
                    except:
                        pass
                    self.lcd.bl_DutyCycle(display_brightness)
                    screen_is_on = True
                    just_woke = True

                if data.battery is None:
                    data.battery = 0
                if data.weight is None:
                    data.weight = 0.0

                # 1. Reset if new shot starts (OFF -> ON)
                if data.paddle_on and not self.last_paddle_state:
                    self.frozen_avg = None
                    self.frozen_weight = None
                    self.shot_stop_time = 0.0
                    self.drip_out_locked = True
                    self.first_drop_time = None
                    self.shot_duration = 0.0
                    self.show_warning = False
                    self.warn_flash_state = False
                    self.warn_flash_time = 0.0
                
                # 1.5 Track end-to-end timing metrics while paddle is ON
                if data.paddle_on:
                    self.shot_duration = data.shot_time_elapsed
                    # Log the exact time the first solid drop hits the cup
                    if self.first_drop_time is None and len(data.flow_data) > 0 and data.flow_data[-1] > 0.2:
                        self.first_drop_time = data.shot_time_elapsed
                
                # 2. Latch Average and start drip-out timer if shot stops (ON -> OFF)
                if not data.paddle_on and self.last_paddle_state:
                    self.shot_stop_time = time.time()
                    
                    # --- FIX: Average is computed right now and NEVER updated again ---
                    self.frozen_avg = self._compute_avg(data.weight)
                    
                    self.frozen_weight = data.weight
                    self.drip_out_locked = False
                
                # 3. Catch Drip-Out (Safeguarded by lock)
                if not data.paddle_on and not self.drip_out_locked:
                    if (time.time() - self.shot_stop_time) <= Drip_Out_Window:
                        # --- FIX: Only the weight is updated here ---
                        self.frozen_weight = data.weight
                    else:
                        self.drip_out_locked = True

                # 4. Activate timeout-stop warning
                if data.timeout_stop and not self.show_warning:
                    self.show_warning = True
                    self.warn_flash_state = True
                    self.warn_flash_time = time.time()

                # 5. Update flash state (toggle every 0.5s)
                if self.show_warning:
                    now_t = time.time()
                    if (now_t - self.warn_flash_time) >= 0.5:
                        self.warn_flash_state = not self.warn_flash_state
                        self.warn_flash_time = now_t
                
                self.last_paddle_state = data.paddle_on
                # ---------------------------------

                w, h = (self.lcd.width, self.lcd.height) if self.display_orientation == DisplayOrientation.PORTRAIT else (self.lcd.height, self.lcd.width)
                
                # Revert to the Ready/logo screen after READY_SCREEN_TIMEOUT of
                # idle: drop the finished-shot summary and clear the local frozen
                # state, and below we skip the graph by blanking flow_data.
                if data.force_ready:
                    self.frozen_avg = None
                    self.frozen_weight = None
                    self.drip_out_locked = True
                    data.flow_data = []   # empty graph -> draw_frame shows the logo

                # Hide summary line during drip-out (Only pass to renderer if lock has fully engaged)
                # Also hide if this is a timeout shot - data is not meaningful.
                # EXCEPTION: on the save frame, always include the summary so the
                # shot-history snapshot shows the cup/weight/avg. The save frame
                # arrives ~3s after stop (inside the drip-out window), when the
                # lock is still open, so without this the saved image omits it.
                show_summary = (self.drip_out_locked or data.save_image) and not data.timeout_stop and not data.force_ready
                display_avg = self.frozen_avg if show_summary else None
                display_weight = self.frozen_weight if show_summary else None

                # --- RENDER GATING ---
                # The state machine above runs every cycle, but the expensive part
                # (draw_frame + SPI push) only needs to happen when the picture
                # actually changes. Two cases force a render:
                #   1. "animating" — paddle on (live weight/timer/graph), the
                #      drip-out window (graph + weight still settling), or a
                #      flashing warning. Here something changes every frame.
                #   2. The change signature differs from the last drawn frame —
                #      catches discrete idle changes (memory/target button,
                #      battery, paddle edge, a cup placed on the scale).
                # Also always render on wake (screen was just cleared) and on a
                # save frame (the finished-shot snapshot must be drawn to be saved).
                animating = data.paddle_on or (not self.drip_out_locked) or self.show_warning
                render_sig = (
                    round(data.weight, 1),
                    round(data.memory.target, 1),
                    data.memory.name,
                    data.memory.color,
                    round(data.shot_time_elapsed, 1),
                    data.battery,
                    data.paddle_on,
                    self.warn_flash_state,
                    # Include summary visibility so the frame where the summary
                    # line first appears (drip-out lock closing) isn't skipped by
                    # the gate for having an otherwise-unchanged signature.
                    show_summary,
                    # Include force_ready so the transition TO the logo screen
                    # renders even though nothing else in the signature changed.
                    data.force_ready,
                )

                if animating or just_woke or data.save_image or render_sig != self.last_render_sig:
                    img = draw_frame(w, h, data, self.display_orientation, display_avg, display_weight,
                                     self.show_warning and self.warn_flash_state)

                    if data.save_image and img is not None:
                        self.save_image(img)
                    self.lcd.ShowImage(img, 0, 0)
                    self.last_render_sig = render_sig
                # ---------------------
                
            except Empty:
                # Sleep Logic
                if screen_is_on:
                    try:
                        self.lcd.bl_DutyCycle(0)
                        self.lcd._pwm.stop() 
                    except:
                        pass
                    w, h = (self.lcd.width, self.lcd.height) if self.display_orientation == DisplayOrientation.PORTRAIT else (self.lcd.height, self.lcd.width)
                    black_img = Image.new("RGBA", (w, h), "BLACK")
                    self.lcd.ShowImage(black_img, 0, 0)
                    
                    logging.info("Display Entering Deep Sleep (PWM Stopped)")
                    screen_is_on = False
                    # Cleared to black; force a fresh draw on next wake.
                    self.last_render_sig = None

                    # Clear transient shot state so a reconnect/wake starts fresh
                    # (logo screen) rather than resuming a stale flashing timeout
                    # warning or a leftover summary line from the previous shot.
                    self.show_warning = False
                    self.warn_flash_state = False
                    self.frozen_avg = None
                    self.frozen_weight = None
                    self.drip_out_locked = True

            except Exception as e:
                logging.error(f"CRASH IN DISPLAY LOOP: {e}")
                traceback.print_exc()
                time.sleep(1)


def draw_frame(width: int, height: int, data: DisplayData, orientation: DisplayOrientation, frozen_avg: float = None, frozen_weight: float = None, show_warning: bool = False) -> Image:
    # --- 1. CONFIGURATION ---
    is_landscape = (orientation == DisplayOrientation.LANDSCAPE)
    
    if is_landscape:
        header_h = 60 
        col_w = 106
        graph_y = 60
        ready_y = 110 
        has_header_batt = False 
        footer_line_y = height - 35
        header_val_font = value_font_med 
    else:
        header_h = 96
        col_w = 120
        graph_y = 98
        ready_y = 164
        has_header_batt = False
        footer_line_y = 285 
        header_val_font = value_font_lg
        
    background = bg_color
    if data.paddle_on:
        background = light_bg_color
        
    img = Image.new("RGBA", (width, height), background)
    draw = ImageDraw.Draw(img)

    # --- 2. GRID LINES ---
    draw.line([(0, header_h), (width, header_h)], fill=fg_color, width=2)
    draw.line([(col_w, 0), (col_w, header_h)], fill=fg_color, width=2)
    if is_landscape:
        draw.line([(col_w * 2, 0), (col_w * 2, header_h)], fill=fg_color, width=2)
        # Footer Line for Landscape
        draw.line([(0, footer_line_y), (width, footer_line_y)], fill=fg_color, width=2)
    else:
        # Footer Line for Portrait
        draw.line([(0, 285), (240, 285)], fill=fg_color, width=2)

    # --- 3. HEADER LABELS & VALUES ---
    # Common Offsets
    lbl_x_1 = 24 if is_landscape else 30
    lbl_x_2 = 120 if is_landscape else 140
    lbl_y = 8 if is_landscape else 16
    
    # Column 1: Weight
    draw.text((lbl_x_1, lbl_y), "WEIGHT", fg_color, label_font)
    fmt_weight = "{:0.1f}".format(data.weight)
    w = draw.textlength(fmt_weight, header_val_font)
    h = header_val_font.size
    draw.text(((col_w - w) / 2, (header_h + 12 - h) / 2), fmt_weight, fg_color, header_val_font)
    
    # Column 2: Target
    target_label = memory_label(data.memory.name)
    # Drop to the smaller font if the name is too wide for the column so it
    # can't overflow into the next column.
    label_max_w = col_w - 8
    lbl_font = label_font
    if draw.textlength(target_label, lbl_font) > label_max_w:
        lbl_font = label_font_sml
    # Center the label within column 2 (spans col_w .. col_w*2).
    w_label = draw.textlength(target_label, lbl_font)
    draw.text(((col_w - w_label) / 2 + col_w, lbl_y), target_label, data.memory.color, lbl_font)
    fmt_target = "{:0.1f}".format(data.memory.target)
    # Use same font size for target
    w = draw.textlength(fmt_target, header_val_font)
    draw.text(((col_w - w) / 2 + col_w, (header_h + 12 - h) / 2), fmt_target, data.memory.color, header_val_font)

    # Column 3: Logic varies
    if is_landscape:
        # LANDSCAPE HEADER COL 3: BREW TIMER
        timer_label = "TIMER"
        w_label = draw.textlength(timer_label, label_font)
        # Centered label: Use same offset logic as value
        draw.text(((col_w - w_label)/2 + (col_w * 2) + 2, 8), timer_label, fg_color, label_font)
        
        fmt_timer = "{:0.1f}".format(data.shot_time_elapsed)
        w = draw.textlength(fmt_timer, header_val_font)
        # Center in 3rd col (start ~212)
        draw.text(((col_w - w)/2 + (col_w * 2) + 2, (header_h + 12 - h) / 2), fmt_timer, fg_color, header_val_font)
    
    # --- 4. FOOTER (Battery, Paddle, & Logo) ---
    p_text = ""
    if data.paddle_on:
        p_color = "BLUE" 
    else:
        p_color = "RED"
        
    fmt_batt = "%d%%" % data.battery
    w_batt_text = draw.textlength(fmt_batt, label_font)
    
    if is_landscape:
        footer_icon_y = footer_line_y + 11 
        footer_text_y = footer_line_y + 9
        
        # --- Right: Battery ---
        padding_right = 8
        icon_width = 27 
        batt_icon_x = width - padding_right - icon_width
        batt_text_x = batt_icon_x - 4 - w_batt_text
        
        draw_battery(draw, (batt_icon_x, footer_icon_y), data.battery, scale=1.0)
        draw.text((batt_text_x, footer_text_y), fmt_batt, fg_color, label_font)
        
        # --- Left: Paddle ---
        paddle_icon_x = 8
        draw_paddle_switch(draw, (paddle_icon_x, footer_icon_y), data.paddle_on, color=p_color, scale=1.0)
        # draw.text((paddle_icon_x + 42, footer_text_y), p_text, p_color, label_font)

        # --- Center: Logo ---
        if logo_img is not None:
            # Center relative to entire screen width
            footer_height = 35 
            logo_x = int((width - logo_img.width) // 2)
            logo_y = int(footer_line_y + (footer_height - logo_img.height) // 2)
            
            img.paste(logo_img, (logo_x, logo_y), logo_img)
            
    else:
        # PORTRAIT FOOTER
        # Right Side: Battery
        padding_right = 8
        icon_width = 27 
        batt_icon_x = width - padding_right - icon_width
        batt_text_x = batt_icon_x - 4 - w_batt_text
        
        draw_battery(draw, (batt_icon_x, 296), data.battery, scale=1.0)
        
        # Left Side: Paddle
        draw_paddle_switch(draw, (8, 294), data.paddle_on, color=p_color, scale=1.0)
        
        # Center: Logo (Optional support for portrait)
        if logo_img is not None:
             # Basic centering for portrait footer (starts at 285, height 35)
             logo_x = int((width - logo_img.width) // 2)
             logo_y = int(285 + (35 - logo_img.height) // 2)
             img.paste(logo_img, (logo_x, logo_y), logo_img)

    # --- 5. READY STATE (Lion Logo, or "Ready" text fallback) ---
    if lion_img is not None:
        # Center the lion in the idle band between the header line and the
        # footer line, so it stays centered regardless of lion_target_h.
        band_top = header_h
        band_bottom = footer_line_y
        lion_x = (width - lion_img.width) // 2
        lion_y = int(band_top + (band_bottom - band_top - lion_img.height) // 2)
        if lion_y < band_top:
            lion_y = band_top  # don't overrun the header if the image is tall
        img.paste(lion_img, (lion_x, lion_y), lion_img)
    else:
        fmt_ready = "Ready"
        # Use main font for Ready text
        w = draw.textlength(fmt_ready, value_font_lg)
        h = value_font_lg.size + value_font_lg.size // 2
        center_x = width // 2

        draw.rectangle((center_x - w / 2 - 4, ready_y, center_x + w / 2 + 4, ready_y + h), bg_color, data.memory.color, 4)
        draw.text((center_x - w / 2, ready_y), fmt_ready, fg_color, value_font_lg)

    # --- 6. GRAPH ---
    if data.flow_data is not None and len(data.flow_data) > 0:
        flow_rate_data = data.flow_rate_moving_avg()
        
        # Determine average value: Prefer sticky frozen val, else calculate on fly (rare)
        final_avg_val = frozen_avg
        final_weight_val = frozen_weight

        if is_landscape:
            g_w, g_h = 320, 145 
            flow_image = FlowGraph(flow_rate_data, data.memory.color, width_pixels=g_w, height_pixels=g_h, avg_flow=final_avg_val, final_weight=final_weight_val).generate_graph()
            img.paste(flow_image, (0, header_h))
            
            # Draw Time Axis Labels (bottom of graph area)
            last_sample_time = data.sample_rate * float(len(data.flow_data))
            axis_y = footer_line_y - 20 
            draw.text((4, axis_y), "-%ds" % math.ceil(last_sample_time), fg_color, label_font_sml)
            draw.text((width / 2 - 22, axis_y), "-%ds" % math.ceil(last_sample_time / 2), fg_color, label_font_sml)
            draw.text((width - 22, axis_y), "0s", fg_color, label_font_sml)
            
        else:
            g_w, g_h = 240, 160
            timer_y = 262
            timer_font = label_font
            
            flow_image = FlowGraph(flow_rate_data, data.memory.color, width_pixels=g_w, height_pixels=g_h, avg_flow=final_avg_val, final_weight=final_weight_val).generate_graph()
            img.paste(flow_image, (0, graph_y))
            
            last_sample_time = data.sample_rate * float(len(data.flow_data))
            axis_y = timer_y 
            draw.text((4, axis_y), "-%ds" % math.ceil(last_sample_time), fg_color, label_font_sml)
            draw.text((width - 22, axis_y), "0s", fg_color, label_font_sml)

            fmt_shot_time = "timer:{:0.1f}s".format(data.shot_time_elapsed)
            w = draw.textlength(fmt_shot_time, timer_font)
            draw.text(((width - w) / 2, timer_y), fmt_shot_time, fg_color, timer_font)

    # --- 7. TIMEOUT-STOP WARNING OVERLAY ---
    if show_warning and warning_img is not None:
        # Centre the warning icon inside the graph band.
        if is_landscape:
            graph_center_y = graph_y + (145 // 2)
        else:
            graph_center_y = graph_y + (160 // 2)
        warn_x = (width - warning_img.width) // 2
        warn_y = graph_center_y - warning_img.height // 2
        if warn_y < graph_y:
            warn_y = graph_y  # keep it below the header if the icon is tall
        img.paste(warning_img, (warn_x, warn_y), warning_img)

    return img