// ui.cpp
#include "ui.h"
#include "config.h"
#include "scale.h"
#include "grinder.h"
#include <lvgl.h>

// ---- widgets ---------------------------------------------------------------
static lv_obj_t* lbl_status;     // top bar: scale + battery + link
static lv_obj_t* lbl_weight;     // big live weight
static lv_obj_t* lbl_weight_u;   // "g" under the weight
static lv_obj_t* lbl_target;     // target value
static lv_obj_t* btn_minus;
static lv_obj_t* btn_plus;
static lv_obj_t* btn_action;     // START / STOP
static lv_obj_t* lbl_action;
static lv_obj_t* lbl_msg;        // bottom status line

// ---- colours ---------------------------------------------------------------
static lv_color_t COL_BG, COL_FG, COL_DIM, COL_GO, COL_STOP, COL_ACCENT;

// ---- callbacks -------------------------------------------------------------
static void minus_cb(lv_event_t*)  { g_grinder.adjustTarget(-TARGET_STEP_G); }
static void plus_cb(lv_event_t*)   { g_grinder.adjustTarget(+TARGET_STEP_G); }
static void action_cb(lv_event_t*) {
  if (g_grinder.state() == GrindState::GRINDING) g_grinder.stop();
  else                                           g_grinder.start();
}

// ---- helpers ---------------------------------------------------------------
static lv_obj_t* make_button(lv_obj_t* parent, const char* txt, lv_event_cb_t cb,
                             lv_coord_t w, lv_coord_t h, lv_obj_t** out_lbl = nullptr) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  if (out_lbl) *out_lbl = l;
  return b;
}

void ui_create() {
  COL_BG     = lv_color_hex(0x0B0B0E);
  COL_FG     = lv_color_hex(0xF2F2F2);
  COL_DIM    = lv_color_hex(0x8A8A93);
  COL_GO     = lv_color_hex(0x1FB55F);
  COL_STOP   = lv_color_hex(0xE03B3B);
  COL_ACCENT = lv_color_hex(0x2A2A33);

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(scr, 10, 0);
  lv_obj_set_style_pad_row(scr, 8, 0);

  // --- status bar ---
  lbl_status = lv_label_create(scr);
  lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
  lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
  lv_label_set_text(lbl_status, "scanning…");

  // --- big weight ---
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
  lv_label_set_text(lbl_weight_u, "grams");

  // --- target row:  [-]   18.0 g   [+] ---
  lv_obj_t* trow = lv_obj_create(scr);
  lv_obj_remove_style_all(trow);
  lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  btn_minus = make_button(trow, LV_SYMBOL_MINUS, minus_cb, 64, 64);
  lv_obj_set_style_bg_color(btn_minus, COL_ACCENT, 0);

  lbl_target = lv_label_create(trow);
  lv_obj_set_style_text_color(lbl_target, COL_FG, 0);
  lv_obj_set_style_text_font(lbl_target, &lv_font_montserrat_28, 0);
  lv_label_set_text(lbl_target, "18.0 g");

  btn_plus = make_button(trow, LV_SYMBOL_PLUS, plus_cb, 64, 64);
  lv_obj_set_style_bg_color(btn_plus, COL_ACCENT, 0);

  // --- action button ---
  btn_action = make_button(scr, "START", action_cb, LV_PCT(100), 72, &lbl_action);
  lv_obj_set_style_bg_color(btn_action, COL_GO, 0);
  lv_obj_set_style_text_font(lbl_action, &lv_font_montserrat_28, 0);

  // --- bottom message ---
  lbl_msg = lv_label_create(scr);
  lv_obj_set_style_text_color(lbl_msg, COL_DIM, 0);
  lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_14, 0);
  lv_label_set_text(lbl_msg, "");
}

void ui_update() {
  // --- status bar ---
  char sb[64];
  if (g_scale.connected)
    snprintf(sb, sizeof(sb), LV_SYMBOL_BLUETOOTH " %s   " LV_SYMBOL_BATTERY_FULL " %d%%",
             g_scale.vendorName(), g_scale.battery);
  else
    snprintf(sb, sizeof(sb), LV_SYMBOL_BLUETOOTH " scanning for scale…");
  lv_label_set_text(lbl_status, sb);

  // --- live weight ---
  char w[16];
  snprintf(w, sizeof(w), "%.1f", g_scale.weight);
  lv_label_set_text(lbl_weight, w);

  // --- target ---
  char t[16];
  snprintf(t, sizeof(t), "%.1f g", g_grinder.target());
  lv_label_set_text(lbl_target, t);

  // --- action button + message reflect the grind state ---
  GrindState st = g_grinder.state();
  bool ready = g_scale.connected;

  if (st == GrindState::GRINDING) {
    lv_label_set_text(lbl_action, "STOP");
    lv_obj_set_style_bg_color(btn_action, COL_STOP, 0);
    lv_obj_clear_state(btn_action, LV_STATE_DISABLED);
    lv_obj_add_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_add_state(btn_plus,  LV_STATE_DISABLED);
    char m[48];
    snprintf(m, sizeof(m), "grinding…  %.1fs", g_grinder.elapsed());
    lv_label_set_text(lbl_msg, m);

  } else if (st == GrindState::DONE) {
    lv_label_set_text(lbl_action, "START");
    lv_obj_set_style_bg_color(btn_action, COL_GO, 0);
    lv_obj_clear_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_clear_state(btn_plus,  LV_STATE_DISABLED);
    char m[64];
    if (g_grinder.timedOut())
      snprintf(m, sizeof(m), "timed out at %.1f g", g_grinder.finalWeight());
    else
      snprintf(m, sizeof(m), "done: %.1f g (target %.1f)",
               g_grinder.finalWeight() > 0 ? g_grinder.finalWeight() : g_scale.weight,
               g_grinder.target());
    lv_label_set_text(lbl_msg, m);

  } else {  // IDLE
    lv_label_set_text(lbl_action, "START");
    lv_obj_clear_state(btn_minus, LV_STATE_DISABLED);
    lv_obj_clear_state(btn_plus,  LV_STATE_DISABLED);
    if (ready) {
      lv_obj_set_style_bg_color(btn_action, COL_GO, 0);
      lv_obj_clear_state(btn_action, LV_STATE_DISABLED);
      lv_label_set_text(lbl_msg, "ready");
    } else {
      lv_obj_set_style_bg_color(btn_action, COL_ACCENT, 0);
      lv_obj_add_state(btn_action, LV_STATE_DISABLED);
      lv_label_set_text(lbl_msg, "connect a scale to grind");
    }
  }
}
