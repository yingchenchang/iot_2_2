#pragma once

// Set these before uploading. Use the same LAN as your RTSP viewer.
static char WIFI_SSID[] = "LAPTOP";
static char WIFI_PASSWORD[] = "12345678";

// Set to 1 only AFTER converting/installing the trained two-class YOLOv7-tiny .nb.
#define USE_CUSTOM_BOTTLE_CUP_MODEL 1

#if USE_CUSTOM_BOTTLE_CUP_MODEL
// DisposableBottle/ReusableContainer model: best validation macro-F1 at 30%.
static const int MIN_SCORE_PERCENT = 30;
#else
// Built-in COCO YOLOv7-tiny: Bottle index 39, Cup index 41.
static const int MIN_SCORE_PERCENT = 45;
#endif
static const unsigned long SERIAL_REPORT_INTERVAL_MS = 500;
