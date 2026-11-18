# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案概觀

ESP32-S3 相機板（GOOUUU ESP32-S3-CAM + 2.8" HSD028903 TFT）搭配電腦端 Python 做影像辨識。
架構是「板子負責取像與顯示，電腦負責運算」：ESP32 透過 USB 序列埠把 JPEG 影格丟給 PC，
PC 用 OpenCV / AprilTag 辨識，必要時再把結果以文字回傳給板子畫在 TFT 上。

沒有 build system、測試或 lint。

## 執行方式

- **韌體**：每個 `*/xxx.ino` 資料夾都是獨立的 Arduino sketch，用 Arduino IDE（或 arduino-cli）以 ESP32-S3 板型編譯上傳。
  - 必須開啟 PSRAM（`fb_location = CAMERA_FB_IN_PSRAM`）。
  - 依賴 `TFT_eSPI`，其腳位 / 驅動設定在函式庫自己的 `User_Setup.h`，**不在本 repo**。
- **電腦端**：`python <script>.py`，依賴 `opencv-python numpy pyserial pupil-apriltags`（無 requirements.txt）。
  - 每支腳本都寫死 `PORT = "COM11"`、`BAUD = 921600`，換機要改。
  - 序列埠是獨佔的：跑 Python 前要關掉 Arduino Serial Monitor。
  - 視窗中按 `q` 離開。

## 韌體與腳本的配對

| Sketch | 搭配的 Python | 說明 |
|---|---|---|
| `cam_timely/` | 無 | 純 TFT 預覽，115200 baud，不傳圖 |
| `01_color_detect/` | `detect_HSV.py`（拉桿調 HSV 範圍）→ `detect_color.py`（用調好的值做多色辨識） | 單向：板 → PC，240×240 影格 |
| `04_apriltag/` | `pure_april.py` | **雙向**：板傳圖，PC 回傳 `#TAG` 行，板子在 TFT 上畫框 |
| （任何會傳圖的 sketch） | `detect_apriltag.py` | 只在 PC 視窗顯示，不回傳 |

`detect_color.py` 的 `COLORS` 格式是 `(Hmin, Hmax, Smin, Smax, Vmin, Vmax, min_area)`，數值來自 `detect_HSV.py` 調出來的結果。

## 序列協定（改任一端都要同步改另一端）

**板 → PC（影格）**：`0xAA 0x55` + `uint32` little-endian 長度 + JPEG bytes。
- 板端用 `frame2jpg()` 把 RGB565 影格轉 JPEG，依 `SEND_INTERVAL_MS` 節流送出。
- PC 端 `read_frame()` 逐 byte 掃 `AA 55` 重新同步，所以同一條序列線上夾雜 `Serial.println` 文字是可容忍的。
- PC 端會丟掉長度 `<100` 或 `>200000`（`pure_april.py` 為 `>300000`）的影格。
- `read_exact` / `read_frame` / `drain_old_frames` 在每支 Python 腳本裡各自複製一份，沒有共用模組 — 改協定要四支都改。

**PC → 板（AprilTag 結果，僅 `pure_april.py` ↔ `04_apriltag`）**：一行 ASCII
```
#TAG,<id>,<cx>,<cy>,<x0>,<y0>,<x1>,<y1>,<x2>,<y2>,<x3>,<y3>,<rot_deg>\n
#TAG,NONE\n
```
座標是 PC 端處理後（已翻轉）的 320×240 影格座標，已 clamp 到 0–319 / 0–239。板端 `parseTagLine` 解析固定 11 個整數 + 1 個 float，行長超過 160 字元會被丟棄。

## 方向 / 座標系的陷阱

影像方向是這個專案最常出錯的地方，兩端的翻轉是成對設計的：
- 板端 sensor 設定 `set_vflip(0)`、`set_hmirror(0)`；PC 端用 `cv2.flip(frame, 1)` 左右翻轉讓 PC 畫面與 TFT 一致。
- `04_apriltag` 畫框時用 `319 - x` 把 x 鏡像回來、y 加 40（TFT 上方 40px 是狀態列）。所以如果改了 `pure_april.py` 的 `USE_FLIP` / `USE_ROTATE_180`，板端的 `mx()` 也要跟著改。
- TFT 是 `setRotation(0)` 直向 240×320，影像貼在 y=40。`01_color_detect` 用 `FRAMESIZE_240X240` 剛好貼滿寬度；`cam_timely` 與 `04_apriltag` 用 QVGA（320×240）推到 240 寬的面板上，右側會被裁掉。
- `detect_apriltag.py` 目前會 flip 兩次（其中一次在 `None` 檢查之前），並把畫面裁成 240 寬再拉伸回 320×240 — 改這支時注意。

## 硬體腳位

相機腳位（GOOUUU 版）在三個 sketch 裡各自 `#define` 一份且內容相同（XCLK=15、SIOD=4、SIOC=5、D0–D7=11,9,8,10,12,18,17,16、VSYNC=6、HREF=7、PCLK=13，無 PWDN/RESET）。改腳位要三份一起改。
