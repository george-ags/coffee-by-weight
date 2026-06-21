// board_display.h — LVGL <-> hardware glue for the AMOLED + touch.
//
// *** THIS IS THE ONE FILE TO RECONCILE WITH WAVESHARE'S DEMO ***
// Everything else (scale protocols, grind logic, UI) is panel-independent.
#pragma once
void board_display_init();   // init Arduino_GFX panel, LVGL buffers, touch indev
