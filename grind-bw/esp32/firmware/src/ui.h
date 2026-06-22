// ui.h — LVGL screen for GRIND-BW.
//
// One screen, state-driven: a status bar (scale + battery), a large live
// weight readout, a target row with -/+ buttons, and a START/STOP button that
// changes with the grind state. Pure LVGL widgets, so this compiles against
// the display/touch bring-up regardless of which exact panel driver you use.
#pragma once

void ui_create();   // build widgets (call once, after lv_init + display init)
void ui_update();   // refresh from g_scale / g_grinder (call every loop)
