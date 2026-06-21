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
                  COL_GREEN, COL_YELLOW;

// ---- main-screen widgets ----
static lv_obj_t *lbl_weight, *lbl_weight_u;
static lv_obj_t *btn_minus, *btn_plus;
static lv_obj_t *btn_start, *lbl_start;          // round target / start-stop
static lv_obj_t *lbl_conn, *lbl_batt, *btn_gear;
static lv_obj_t *lbl_msg;

// ---- settings modal ----
static lv_obj_t *modal = nullptr;
static lv_obj_t *modal_list = nullptr;
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
static void minus_cb(lv_event_t*) { g_grinder.adjustTarget(-TARGET_STEP_G); }
static void plus_cb (lv_event_t*) { g_grinder.adjustTarget(+TARGET_STEP_G); }
static void start_cb(lv_event_t*) {
  if (g_grinder.state() == GrindState::GRINDING) g_grinder.stop();
  else                                           g_grinder.start();
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

void ui_create() {
  COL_BG     = lv_color_hex(0x0B0B0E);
  COL_FG     = lv_color_hex(0xF2F2F2);
  COL_DIM    = lv_color_hex(0x8A8A93);
  COL_ACCENT = lv_color_hex(0x2A2A33);
  COL_RED    = lv_color_hex(0xE03B3B);
  COL_RED_DK = lv_color_hex(0x8E2020);
  COL_GREEN  = lv_color_hex(0x1FB55F);
  COL_YELLOW = lv_color_hex(0xE0B020);

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

  btn_minus = mk_btn(trow, LV_SYMBOL_MINUS, minus_cb, 54, 54, COL_ACCENT, &lv_font_montserrat_20, nullptr);
  lv_obj_set_style_radius(btn_minus, LV_RADIUS_CIRCLE, 0);

  btn_start = mk_btn(trow, "0", start_cb, 132, 132, COL_RED, &lv_font_montserrat_48, &lbl_start);
  lv_obj_set_style_radius(btn_start, LV_RADIUS_CIRCLE, 0);

  btn_plus = mk_btn(trow, LV_SYMBOL_PLUS, plus_cb, 54, 54, COL_ACCENT, &lv_font_montserrat_20, nullptr);
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

  btn_gear = mk_btn(bar, LV_SYMBOL_SETTINGS, gear_cb, 50, 40, COL_ACCENT, &lv_font_montserrat_20, nullptr);

  // --- message ---
  lbl_msg = lv_label_create(scr);
  lv_obj_set_style_text_color(lbl_msg, COL_DIM, 0);
  lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_14, 0);
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
    lv_obj_set_width(b, LV_PCT(100));
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
  lv_obj_set_size(panel, 250, 390);
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
  lv_label_set_text(title, "Select scale");

  modal_list = lv_obj_create(panel);
  lv_obj_set_size(modal_list, LV_PCT(100), 270);
  lv_obj_set_flex_flow(modal_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(modal_list, 6, 0);
  lv_obj_set_style_bg_opa(modal_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(modal_list, 0, 0);

  lv_obj_t* row = lv_obj_create(panel);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  mk_btn(row, LV_SYMBOL_REFRESH " Rescan", rescan_cb, 108, 40, COL_ACCENT, &lv_font_montserrat_14, nullptr);
  mk_btn(row, LV_SYMBOL_CLOSE  " Close",  close_cb,  108, 40, COL_ACCENT, &lv_font_montserrat_14, nullptr);

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
  char w[16]; snprintf(w, sizeof(w), "%.1f", g_scale.weight);
  lv_label_set_text(lbl_weight, w);

  char t[12]; snprintf(t, sizeof(t), "%.1f", g_grinder.target());  // no unit
  lv_label_set_text(lbl_start, t);

  GrindState st = g_grinder.state();
  bool conn = g_scale.connected;

  // round start/stop button
  if (st == GrindState::GRINDING) {
    lv_obj_set_style_bg_color(btn_start, COL_RED_DK, 0);
    lv_obj_clear_state(btn_start, LV_STATE_DISABLED);
    lv_obj_add_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_add_state(btn_plus,  LV_STATE_DISABLED);
  } else {
    lv_obj_set_style_bg_color(btn_start, COL_RED, 0);
    lv_obj_clear_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_clear_state(btn_plus,  LV_STATE_DISABLED);
    if (conn) lv_obj_clear_state(btn_start, LV_STATE_DISABLED);
    else      lv_obj_add_state(btn_start, LV_STATE_DISABLED);
  }

  // connectivity icon
  lv_obj_set_style_text_color(lbl_conn, conn ? COL_GREEN : COL_DIM, 0);

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

  // message line
  if (st == GrindState::GRINDING) {
    char m[40]; snprintf(m, sizeof(m), "grinding  %.1fs", g_grinder.elapsed());
    lv_label_set_text(lbl_msg, m);
  } else if (st == GrindState::DONE) {
    char m[48];
    if (g_grinder.timedOut())
      snprintf(m, sizeof(m), "timed out at %.1f g", g_grinder.finalWeight());
    else
      snprintf(m, sizeof(m), "done: %.1f g",
               g_grinder.finalWeight() > 0 ? g_grinder.finalWeight() : g_scale.weight);
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