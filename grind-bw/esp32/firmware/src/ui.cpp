// ui.cpp — GRIND-BW screen.
//
// Layout (portrait 280x456):
//   • live weight (big) + "g"                         <- what the scale reads now
//   • [ - ]  ( round red target = START/STOP )  [ + ] <- tap the circle to grind
//   • [ BT ]      [ battery % ]      [ gear ]          <- status + scale picker
//   • message line
//
// The gear opens a modal listing every scale discovered in the last BLE scan;
// tapping one selects it (the scale library reconnects to the chosen device).
#include "ui.h"
#include "config.h"
#include <scale.h>
#include "grinder.h"
#include <lvgl.h>

// ---- palette ----
static lv_color_t COL_BG, COL_FG, COL_DIM, COL_ACCENT, COL_RED, COL_RED_DK,
                  COL_GREEN, COL_YELLOW, COL_BLUE, COL_TEETH;

// ---- main-screen widgets ----
static lv_obj_t *lbl_weight, *lbl_weight_u;
static lv_obj_t *btn_minus, *btn_plus;
static lv_obj_t *btn_start, *lbl_start;          // round target / start-stop
static lv_obj_t *lbl_conn, *lbl_batt, *btn_gear;
static lv_obj_t *lbl_msg;

// Gear teeth around the start button — small gray squares that spin while the
// motor is turning (grinding / pulsing).
#define GEAR_TEETH       12
#define GEAR_RADIUS      58      // centre-to-tooth; keeps the whole tooth inside the rim
#define GEAR_TOOTH_PX    12
#define GEAR_DEG_PER_MS  0.24f   // ~one revolution every 1.5 s
static lv_obj_t *teeth[GEAR_TEETH];
static float     gear_angle   = 0;
static uint32_t  gear_last_ms = 0;

// ---- settings modal ----
static lv_obj_t *modal = nullptr;
static lv_obj_t *modal_list = nullptr;
static lv_obj_t *btn_mode_w = nullptr, *btn_mode_t = nullptr;
static uint32_t  modal_gen = 0;

static lv_color_t batt_color(int pct) {
  if (pct >= 50) return COL_GREEN;
  if (pct >= 20) return COL_YELLOW;
  return COL_RED;
}
static const char* batt_glyph(int p) {
  if (p >= 80) return LV_SYMBOL_BATTERY_FULL;
  if (p >= 55) return LV_SYMBOL_BATTERY_3;
  if (p >= 30) return LV_SYMBOL_BATTERY_2;
  if (p >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

// ---------- callbacks ----------
static void minus_cb(lv_event_t*) { g_grinder.adjustTarget(-1); }
static void plus_cb (lv_event_t*) { g_grinder.adjustTarget(+1); }
static void start_cb(lv_event_t*) {
  GrindState s = g_grinder.state();
  if (s != GrindState::IDLE && s != GrindState::DONE) g_grinder.stop();
  else                                                g_grinder.start();
}

static void modal_open();
static void modal_close();
static void modal_populate();

static void gear_cb  (lv_event_t*) { modal_open(); }
static void close_cb (lv_event_t*) { modal_close(); }
static void rescan_cb(lv_event_t*) { g_scale.rescan(); }
static void pick_cb  (lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  String name, mac; Vendor v;
  if (g_scale.discoveredAt(idx, name, mac, v)) g_scale.selectScale(mac, v);
  modal_close();
}

// highlight the active grind-mode button
static void refresh_mode_btns() {
  if (!btn_mode_w || !btn_mode_t) return;
  bool timeMode = (g_grinder.mode() == GrindMode::TIME);
  lv_obj_set_style_bg_color(btn_mode_w, timeMode ? COL_ACCENT : COL_BLUE, 0);
  lv_obj_set_style_bg_color(btn_mode_t, timeMode ? COL_BLUE  : COL_ACCENT, 0);
}
static void mode_w_cb(lv_event_t*) { g_grinder.setMode(GrindMode::WEIGHT); refresh_mode_btns(); }
static void mode_t_cb(lv_event_t*) { g_grinder.setMode(GrindMode::TIME);   refresh_mode_btns(); }

// ---------- helper ----------
static lv_obj_t* mk_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb,
                        lv_coord_t w, lv_coord_t h, lv_color_t bg,
                        const lv_font_t* font, lv_obj_t** out_lbl, void* ud = nullptr) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_center(l);
  if (out_lbl) *out_lbl = l;
  return b;
}

// Position tooth i around the gear centre, offset by rot_deg.
static void place_tooth(int i, float rot_deg) {
  float a = (360.0f / GEAR_TEETH) * (float)i + rot_deg;
  float r = a * 0.01745329f;                 // degrees -> radians
  lv_coord_t x = (lv_coord_t)lroundf(GEAR_RADIUS * cosf(r));
  lv_coord_t y = (lv_coord_t)lroundf(GEAR_RADIUS * sinf(r));
  lv_obj_align(teeth[i], LV_ALIGN_CENTER, x, y);
}

void ui_create() {
  COL_BG     = lv_color_hex(0x0B0B0E);
  COL_FG     = lv_color_hex(0xF2F2F2);
  COL_DIM    = lv_color_hex(0x8A8A93);
  COL_ACCENT = lv_color_hex(0x2A2A33);
  COL_RED    = lv_color_hex(0xE03B3B);
  COL_RED_DK = lv_color_hex(0x8E2020);
  COL_GREEN  = lv_color_hex(0x1FB55F);
  COL_YELLOW = lv_color_hex(0xE0B020);
  COL_BLUE   = lv_color_hex(0x2E9BFF);
  COL_TEETH  = lv_color_hex(0x9AA0AA);

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(scr, 10, 0);

  // --- live weight ---
  lv_obj_t* wbox = lv_obj_create(scr);
  lv_obj_remove_style_all(wbox);
  lv_obj_set_size(wbox, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(wbox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(wbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lbl_weight = lv_label_create(wbox);
  lv_obj_set_style_text_color(lbl_weight, COL_FG, 0);
  lv_obj_set_style_text_font(lbl_weight, &lv_font_montserrat_48, 0);
  lv_label_set_text(lbl_weight, "0.0");
  lbl_weight_u = lv_label_create(wbox);
  lv_obj_set_style_text_color(lbl_weight_u, COL_DIM, 0);
  lv_obj_set_style_text_font(lbl_weight_u, &lv_font_montserrat_20, 0);
  lv_label_set_text(lbl_weight_u, "g");

  // --- target row: [-] (start circle) [+] ---
  lv_obj_t* trow = lv_obj_create(scr);
  lv_obj_remove_style_all(trow);
  lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  btn_minus = mk_btn(trow, LV_SYMBOL_MINUS, minus_cb, 60, 96, COL_ACCENT, &lv_font_montserrat_28, nullptr);
  lv_obj_set_style_radius(btn_minus, LV_RADIUS_CIRCLE, 0);

  // Red start/stop circle. The gray gear teeth are children of the button, so
  // they sit on top of the red fill, just inside the rim, and spin while the
  // motor is running.
  btn_start = mk_btn(trow, "0", start_cb, 132, 132, COL_RED, &lv_font_montserrat_48, &lbl_start);
  lv_obj_set_style_radius(btn_start, LV_RADIUS_CIRCLE, 0);
  for (int i = 0; i < GEAR_TEETH; i++) {
    lv_obj_t* t = lv_obj_create(btn_start);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, GEAR_TOOTH_PX, GEAR_TOOTH_PX);
    lv_obj_set_style_bg_color(t, COL_TEETH, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 2, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    teeth[i] = t;
  }
  for (int i = 0; i < GEAR_TEETH; i++) place_tooth(i, 0);   // static start position

  btn_plus = mk_btn(trow, LV_SYMBOL_PLUS, plus_cb, 60, 96, COL_ACCENT, &lv_font_montserrat_28, nullptr);
  lv_obj_set_style_radius(btn_plus, LV_RADIUS_CIRCLE, 0);

  // --- bottom bar: conn | battery | gear ---
  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_conn = lv_label_create(bar);
  lv_obj_set_style_text_font(lbl_conn, &lv_font_montserrat_20, 0);
  lv_label_set_text(lbl_conn, LV_SYMBOL_BLUETOOTH);

  lbl_batt = lv_label_create(bar);
  lv_obj_set_style_text_font(lbl_batt, &lv_font_montserrat_20, 0);
  lv_label_set_text(lbl_batt, LV_SYMBOL_BATTERY_EMPTY " --");

  btn_gear = mk_btn(bar, LV_SYMBOL_SETTINGS, gear_cb, 72, 60, COL_ACCENT, &lv_font_montserrat_28, nullptr);

  // --- message ---
  lbl_msg = lv_label_create(scr);
  lv_obj_set_style_text_color(lbl_msg, COL_DIM, 0);
  lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_20, 0);
  lv_label_set_text(lbl_msg, "");
}

// ---------- settings modal ----------
static void modal_populate() {
  if (!modal_list) return;
  lv_obj_clean(modal_list);
  int n = g_scale.discoveredCount();
  if (n == 0) {
    lv_obj_t* l = lv_label_create(modal_list);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_label_set_text(l, "no scales found —\npower one on, then Rescan");
    return;
  }
  String cur = g_scale.macAddress();
  for (int i = 0; i < n; i++) {
    String name, mac; Vendor v;
    if (!g_scale.discoveredAt(i, name, mac, v)) continue;
    const char* vn = (v == Vendor::ACAIA) ? "Acaia" : (v == Vendor::BOOKOO ? "BooKoo" : "?");
    char txt[72];
    snprintf(txt, sizeof(txt), "%s  (%s)", name.c_str(), vn);
    lv_obj_t* b = lv_btn_create(modal_list);
    lv_obj_set_size(b, LV_PCT(100), 46);
    lv_obj_set_style_bg_color(b, mac.equalsIgnoreCase(cur) ? COL_GREEN : COL_ACCENT, 0);
    lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
  }
}

static void modal_open() {
  if (modal) return;
  modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
  lv_obj_set_style_border_width(modal, 0, 0);
  lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);   // absorb taps to the screen below

  lv_obj_t* panel = lv_obj_create(modal);
  lv_obj_set_size(panel, 250, 410);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, COL_BG, 0);
  lv_obj_set_style_border_color(panel, COL_ACCENT, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(panel, 10, 0);
  lv_obj_set_style_pad_row(panel, 8, 0);

  lv_obj_t* title = lv_label_create(panel);
  lv_obj_set_style_text_color(title, COL_FG, 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_label_set_text(title, "Settings");

  // grind-mode selector
  lv_obj_t* mlbl = lv_label_create(panel);
  lv_obj_set_style_text_color(mlbl, COL_DIM, 0);
  lv_obj_set_style_text_font(mlbl, &lv_font_montserrat_14, 0);
  lv_label_set_text(mlbl, "Grind mode");

  lv_obj_t* mrow = lv_obj_create(panel);
  lv_obj_remove_style_all(mrow);
  lv_obj_set_size(mrow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  btn_mode_w = mk_btn(mrow, "By weight", mode_w_cb, 108, 52, COL_ACCENT, &lv_font_montserrat_14, nullptr);
  btn_mode_t = mk_btn(mrow, "By time",   mode_t_cb, 108, 52, COL_ACCENT, &lv_font_montserrat_14, nullptr);
  refresh_mode_btns();

  // scale picker
  lv_obj_t* slbl = lv_label_create(panel);
  lv_obj_set_style_text_color(slbl, COL_DIM, 0);
  lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
  lv_label_set_text(slbl, "Scale");

  modal_list = lv_obj_create(panel);
  lv_obj_set_size(modal_list, LV_PCT(100), 170);
  lv_obj_set_flex_flow(modal_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(modal_list, 6, 0);
  lv_obj_set_style_bg_opa(modal_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(modal_list, 0, 0);

  lv_obj_t* row = lv_obj_create(panel);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  mk_btn(row, LV_SYMBOL_REFRESH " Rescan", rescan_cb, 108, 52, COL_ACCENT, &lv_font_montserrat_14, nullptr);
  mk_btn(row, LV_SYMBOL_CLOSE  " Close",  close_cb,  108, 52, COL_ACCENT, &lv_font_montserrat_14, nullptr);

  modal_gen = g_scale.scanGeneration();
  modal_populate();
}

static void modal_close() {
  if (!modal) return;
  lv_obj_del(modal);
  modal = nullptr;
  modal_list = nullptr;
}

// ---------- per-loop refresh ----------
void ui_update() {
  GrindState st   = g_grinder.state();
  GrindMode  mode = g_grinder.mode();
  bool       conn = g_scale.connected;

  // top readout + unit + middle target depend on the mode
  if (mode == GrindMode::TIME) {
    // top is a countdown from the target time to zero once grinding starts
    float rem = (st == GrindState::GRINDING) ? (g_grinder.targetTime() - g_grinder.elapsed())
              : (st == GrindState::DONE     ? 0.0f
                                            : g_grinder.targetTime());
    if (rem < 0) rem = 0;
    char w[12]; snprintf(w, sizeof(w), "%.1f", rem);
    lv_label_set_text(lbl_weight, w);
    lv_label_set_text(lbl_weight_u, "seconds");
    char t[12]; snprintf(t, sizeof(t), "%.1f", g_grinder.targetTime());  // middle = target time
    lv_label_set_text(lbl_start, t);
  } else {
    char w[16]; snprintf(w, sizeof(w), "%.1f", g_scale.weight);
    lv_label_set_text(lbl_weight, w);
    lv_label_set_text(lbl_weight_u, "gram");
    char t[12]; snprintf(t, sizeof(t), "%.1f", g_grinder.target());      // middle = target dose
    lv_label_set_text(lbl_start, t);
  }

  // round start/stop button — active during tare, coarse, settle and pulse
  bool grinding = (st == GrindState::GRINDING || st == GrindState::TARING ||
                   st == GrindState::SETTLING || st == GrindState::PULSING);
  if (grinding) {
    lv_obj_set_style_bg_color(btn_start, COL_RED_DK, 0);
    lv_obj_clear_state(btn_start, LV_STATE_DISABLED);
    lv_obj_add_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_add_state(btn_plus,  LV_STATE_DISABLED);
  } else {
    lv_obj_set_style_bg_color(btn_start, COL_RED, 0);
    lv_obj_clear_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_clear_state(btn_plus,  LV_STATE_DISABLED);
    if (mode == GrindMode::TIME || conn) lv_obj_clear_state(btn_start, LV_STATE_DISABLED);
    else                                 lv_obj_add_state(btn_start, LV_STATE_DISABLED);
  }

  // spin the gear teeth while the motor is actually turning
  uint32_t now = millis();
  if (gear_last_ms == 0) gear_last_ms = now;
  uint32_t dt = now - gear_last_ms;
  gear_last_ms = now;
  if (st == GrindState::GRINDING || st == GrindState::PULSING) {
    gear_angle += dt * GEAR_DEG_PER_MS;
    while (gear_angle >= 360.0f) gear_angle -= 360.0f;
    for (int i = 0; i < GEAR_TEETH; i++) place_tooth(i, gear_angle);
  }

  // "done" celebration: flash the readout + teeth green for the DONE hold,
  // then everything restores to normal once the grind returns to idle.
  bool flashGreen = (st == GrindState::DONE) && !g_grinder.timedOut() &&
                    (((now / 350) % 2) == 0);
  lv_obj_set_style_text_color(lbl_weight, flashGreen ? COL_GREEN : COL_FG, 0);
  lv_color_t tcol = flashGreen ? COL_GREEN : COL_TEETH;
  for (int i = 0; i < GEAR_TEETH; i++) lv_obj_set_style_bg_color(teeth[i], tcol, 0);

  // connectivity icon
  lv_obj_set_style_text_color(lbl_conn, conn ? COL_BLUE : COL_DIM, 0);

  // battery (color-coded)
  if (conn) {
    int b = g_scale.battery;
    char bs[16]; snprintf(bs, sizeof(bs), "%s %d%%", batt_glyph(b), b);
    lv_label_set_text(lbl_batt, bs);
    lv_obj_set_style_text_color(lbl_batt, batt_color(b), 0);
  } else {
    lv_label_set_text(lbl_batt, LV_SYMBOL_BATTERY_EMPTY " --");
    lv_obj_set_style_text_color(lbl_batt, COL_DIM, 0);
  }

  // message line (default dim; a successful finish turns it green)
  lv_obj_set_style_text_color(lbl_msg, COL_DIM, 0);
  if (st == GrindState::DONE && !g_grinder.timedOut()) {
    lv_obj_set_style_text_color(lbl_msg, COL_GREEN, 0);
    lv_label_set_text(lbl_msg, "HERE YOU GO");
  } else if (mode == GrindMode::TIME) {
    if (st == GrindState::GRINDING)  lv_label_set_text(lbl_msg, "grinding...");
    else if (st == GrindState::DONE) lv_label_set_text(lbl_msg, "stopped (max time)");
    else                             lv_label_set_text(lbl_msg, "ready - tap to grind");
  } else if (st == GrindState::TARING) {
    lv_label_set_text(lbl_msg, "taring...");
  } else if (st == GrindState::GRINDING) {
    char m[40]; snprintf(m, sizeof(m), "grinding  %.1fs", g_grinder.elapsed());
    lv_label_set_text(lbl_msg, m);
  } else if (st == GrindState::SETTLING || st == GrindState::PULSING) {
    lv_label_set_text(lbl_msg, "approaching target...");
  } else if (st == GrindState::DONE) {   // timed out (failure)
    char m[48]; snprintf(m, sizeof(m), "timed out at %.1f g", g_grinder.finalWeight());
    lv_label_set_text(lbl_msg, m);
  } else {
    lv_label_set_text(lbl_msg, conn ? "ready - tap to grind" : "scanning for scale...");
  }

  // live-refresh the picker if a new scan completed while it's open
  if (modal && g_scale.scanGeneration() != modal_gen) {
    modal_gen = g_scale.scanGeneration();
    modal_populate();
  }
}