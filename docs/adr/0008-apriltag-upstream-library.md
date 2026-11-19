# AprilTag 使用 AprilRobotics 原版函式庫（只保留 tag36h11）

Espressif 沒有官方 AprilTag 元件。我們把 AprilRobotics/apriltag（BSD-2）的必要原始碼放進 `firmware/components/apriltag/`（保留授權檔），只編譯 tag36h11 家族，在 320×240 灰階上以 `quad_decimate = 2` 執行。

## Considered Options

- **社群元件 `liux-pro/apriltag`**：三年沒更新，註冊頁沒寫授權。
- **OpenMV 的 apriltag.c（MIT）**：綁定 OpenMV 自己的記憶體配置器，移植成本高。
- **`raspiduino/apriltag-esp32`**：Arduino 移植，為了省記憶體把 tag36h11 砍到 35 個 tag，不符合「支援 0–586」的需求。

## Consequences

- 實際 fps 沒有 ESP32-S3 的公開數據，M2 要實測；達不到 10 fps 時優先調 `quad_decimate` 和降解析度，再考慮其他做法。
- M2 實測（2026-11-19，Task 6）：原版設定每張 230–410 ms（TFT 2–3 fps），主因是 S3 沒有 double FPU（軟體模擬 double）和低對比雜訊產生大量候選四邊形。不改上游檔案，改用強制 include 的 shim：`double` 編成 `float`、配置全部走 PSRAM，`min_white_black_diff` 從 5 調到 20。`quad_decimate = 2` 保留（偵測距離優先），沒有 tag 時每張約 56 ms（TFT 8–10 fps，串流預覽時約 7.4 fps）；`quad_decimate = 3` 可到 11 fps 但可偵測的最小 tag 變大約 1.5 倍。細節見 `firmware/components/apriltag/README.md`。
