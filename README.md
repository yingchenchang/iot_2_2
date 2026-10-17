# BottleCupStandalone

在 **AMB82-MINI** 上執行的水杯／瓶子辨識程式。板子使用攝影機與內建 **YOLOv7-tiny** 模型進行推論，並自行提供網頁、辨識結果 API 與 MJPEG 影像串流。電腦或手機只需與板子位於可互通的區域網路，即可用瀏覽器查看結果。

本文件對應目前還原的獨立版 `BottleCupStandalone.ino`，還原來源與版本限制見 [RESTORE.md](RESTORE.md)。

## 功能與設定

| 項目 | 目前內容 |
| --- | --- |
| 模型 | SDK 內建 `DEFAULT_YOLOV7TINY` |
| 辨識類別 | COCO Bottle（索引 39）、Cup（索引 41） |
| 信心門檻 | 35% |
| JPEG 預覽 | 通道 0，640 × 360，設定 5 FPS，品質等級 5 |
| AI 輸入 | 通道 3，576 × 320 RGB，設定 10 FPS |
| 網頁及 API | HTTP 80 埠 |
| 攝影機串流 | HTTP 81 埠，MJPEG |
| 網頁更新 | 每 500 ms 輪詢辨識結果 |
| 序列埠 | 115200 baud |

FPS 為程式設定值，實際速度取決於推論、攝影機與網路狀況。網頁會顯示 Cup／Bottle 的最高信心分數，以及最多 8 個物件的框線；結果超過 2 秒未更新時，API 會將分數歸零並清空物件。

Bottle 是模型的瓶子類別，不能據此判定材質、是否為 PET 或能否回收。網頁框線使用最新辨識結果，並非與每張串流影像嚴格同步。

## 環境需求

- AMB82-MINI、相容攝影機與可傳輸資料的 USB 線。
- Arduino IDE，已安裝 AmebaPro2 板卡套件。
- 板子可連線的 Wi-Fi，以及可與板子互通的電腦或手機。

本專案已使用 **AmebaPro2 4.0.9-build20250805** 編譯成功。`WiFi`、`StreamIO`、`VideoStream`、`NeuralNetwork` 等程式庫由板卡 SDK 提供。

若尚未安裝板卡套件，可在 Arduino IDE 的額外開發板管理員網址加入：

```text
https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json
```

接著於開發板管理員安裝 AmebaPro2，選擇 **AMB82-MINI**。

## 設定與燒錄

1. 開啟 `BottleCupStandalone.ino`。
2. 修改主程式內的 Wi-Fi 設定：

   ```cpp
   static char WIFI_SSID[] = "你的 Wi-Fi 名稱";
   static char WIFI_PASSWORD[] = "你的 Wi-Fi 密碼";
   ```

3. 選擇 AMB82-MINI、實際連接的序列埠與相符的攝影機型號，將模型載入來源設為 **Flash**。
4. 編譯並上傳；若自動燒錄模式無法使用，依板卡工具提示讓板子進入下載模式。
5. 上傳完成後按一下板子的 **RESET**。
6. 開啟 115200 baud 的序列埠監控視窗，查看啟動訊息：

   ```text
   Web page: http://<板子IP>
   Web camera stream: http://<板子IP>:81
   ```

Wi-Fi 設定直接位於 `.ino`，此版本不讀取資料夾內的 `config.h`。程式在約 30 秒內未連上 Wi-Fi 時會顯示 `WiFi connection failed.` 並停止初始化；修正設定後需重新燒錄或重啟。

## 開啟網頁

在同一個可互通的區域網路中開啟：

```text
http://<板子IP>/
```

網頁會自動從相同主機的 **81 埠**載入攝影機影像，並從 80 埠取得辨識結果。可直接開啟串流位址檢查影像：

```text
http://<板子IP>:81/
```

最近一次實機確認的網頁位址為 `http://192.168.137.38/`。IP 可能在重啟、切換 Wi-Fi 或熱點重新分配位址後改變，請以序列埠輸出為準。

程式不需要電腦鏡頭、Node.js、外部網頁伺服器或 SD 卡，也沒有提供 RTSP 串流。MJPEG 任務一次服務一個串流連線，測試時建議只保留一個觀看中的分頁。

## HTTP 介面

| 埠 | 路徑 | 用途 |
| --- | --- | --- |
| 80 | `/`、`/index.html` | 內嵌網頁，含 CSS 與 JavaScript |
| 80 | `/api/status` | JSON 辨識結果 |
| 81 | `/` | `multipart/x-mixed-replace` MJPEG 串流 |

`/api/status` 回應範例（僅供格式說明）：

```json
{
  "ready": true,
  "age_ms": 120,
  "cup": 82,
  "bottle": 0,
  "objects": [
    {"class": "cup", "score": 82, "x1": 80, "y1": 60, "x2": 210, "y2": 280}
  ]
}
```

- `ready`：是否已收到至少一次推論回呼；不代表一定偵測到物件。
- `age_ms`：最近一次推論回呼距今的毫秒數。
- `cup`、`bottle`：各類別的最高信心分數，未偵測或結果過期時為 0。
- `objects`：辨識框清單，座標以 640 × 360 畫面為基準。

## 程式結構

`setup()` 連接 Wi-Fi、建立辨識結果互斥鎖、啟動網頁服務、初始化攝影機與模型，再啟動影像串流任務。`loop()` 處理 80 埠的網頁與 API 請求。

`onDetection()` 篩選 Bottle／Cup，儲存分數與座標，並透過互斥鎖更新共享結果。`cameraTask()` 在 FreeRTOS 任務中從攝影機取得 JPEG，傳送至 81 埠的觀看端。網頁使用 Canvas 在影像上疊加框線。

| 檔案 | 用途 |
| --- | --- |
| `BottleCupStandalone.ino` | 目前實際使用的主程式、Wi-Fi 設定與內嵌網頁 |
| `RESTORE.md` | 還原來源、備份位置與版本限制 |
| `config.h`、`WebAssets.h`、`WebBridge.h` | 保留的其他版本檔案，目前主程式未引用 |
| `yolov7_tiny.nb` | 保留的自訂模型檔，目前程式未選用此自訂模型 |

修改目前網頁請編輯 `.ino` 內的 `WEB_PAGE`；修改門檻請調整 `MIN_SCORE`。不需執行外部網頁資產產生腳本。

## 已知問題與排查

### 網頁有結果，但攝影機畫面全黑

此版本是還原後供測試的舊獨立版，**黑屏問題尚未確認修復**。網頁顯示「已連線」只表示辨識 API 有回應，不能證明 81 埠的影像串流正常。

1. 直接開啟 `http://<板子IP>:81/`，確認是否有影像。
2. 關閉其他觀看串流的分頁，再重新載入主頁。
3. 確認燒錄時選擇的攝影機型號符合硬體，並檢查攝影機連接。
4. 檢查序列埠是否出現 `Camera web task failed.` 或其他攝影機錯誤。

### 網頁無法開啟

確認 RESET 已按下、Wi-Fi／電腦行動熱點已開啟，並以序列埠顯示的最新 IP 連線。若使用訪客網路，裝置隔離可能阻止電腦與板子互連。

### 編譯出現語系錯誤

若 Windows Arduino CLI 出現 `locale::facet::_S_create_c_locale`，可在編譯用的 PowerShell 工作階段先設定：

```powershell
$env:LC_ALL = 'C'
$env:LANG = 'C'
```

再於相同工作階段執行 Arduino CLI 編譯。

## 驗證紀錄

- 2026-10-17：還原版編譯成功，程式使用 10,113,024 bytes，約占 16 MB Flash 的 60%。
- 已透過 COM3 燒錄成功，工具回報 `upload success`。
- RESET 後主頁回應 `HTTP/1.1 200 OK`，HTML 長度為 3,739 bytes。
- 上述紀錄僅確認編譯、燒錄與主頁回應，尚未驗證此還原版的實際串流畫面與辨識正確率。
