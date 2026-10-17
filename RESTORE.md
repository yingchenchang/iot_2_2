# Standalone 還原紀錄

此主程式還原自 Arduino 編譯快取中的 `BottleCupStandalone.ino.cpp`：

`C:/Users/yung7/AppData/Local/arduino/sketches/5CBBF12FF26D7A26646AAF9C00372B6D/sketch/BottleCupStandalone.ino.cpp`

快取修改時間為 2026-10-15 16:11:21，無法據此證明它是 10/15 之前的版本。
還原時移除 Arduino 自動加入的 `#include <Arduino.h>` 與 `#line` 標記，保留快取中的函式宣告。

此版本使用內建 YOLOv7-tiny、35% 門檻；網頁使用 HTTP 80 埠，攝影機串流使用 81 埠。
Wi-Fi 設定與網頁直接寫在 `.ino` 中。資料夾內現有 `config.h`、`WebAssets.h`、`WebBridge.h` 與自訂模型檔未被此版本引用。

還原前的完整資料夾備份位於 `../.build/standalone-before-restore-20261017-173816/BottleCupStandalone/`。
本次還原未修復黑屏問題。
