# AprilTag 使用 AprilRobotics 原版函式庫（只保留 tag36h11）

Espressif 沒有官方 AprilTag 元件。我們把 AprilRobotics/apriltag（BSD-2）的必要原始碼放進 `firmware/components/apriltag/`（保留授權檔），只編譯 tag36h11 家族，在 320×240 灰階上以 `quad_decimate = 2` 執行。

## Considered Options

- **社群元件 `liux-pro/apriltag`**：三年沒更新，註冊頁沒寫授權。
- **OpenMV 的 apriltag.c（MIT）**：綁定 OpenMV 自己的記憶體配置器，移植成本高。
- **`raspiduino/apriltag-esp32`**：Arduino 移植，為了省記憶體把 tag36h11 砍到 35 個 tag，不符合「支援 0–586」的需求。

## Consequences

- 實際 fps 沒有 ESP32-S3 的公開數據，M2 要實測；達不到 10 fps 時優先調 `quad_decimate` 和降解析度，再考慮其他做法。
