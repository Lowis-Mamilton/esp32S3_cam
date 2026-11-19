# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 架構

MXCam：ESP32-S3 視覺感測器，搭配 MATRIX Mini R4 使用，設定由電腦端 Web Serial PWA 完成。三個子專案：

- `firmware/`：ESP-IDF v5.5.6 韌體（ESP32-S3 N16R8），專案名 `mxcam`
- `pc-app/`：MXCam Studio（Vite + React + TS）
- `arduino-lib/`：給 R4 用的 Arduino 函式庫 `MXCam`

規格與決策不在這裡重複，請看：`docs/SPEC.md`（規格）、`docs/adr/`（架構決策）、`GLOSSARY.md`（名詞）。
協定規格在 `docs/protocol.md`，測試資料 `docs/protocol-vectors.json`（C 和 TS 兩份實作都要通過）：
- C（正本，韌體和 Arduino 函式庫共用，ADR-0006）：`arduino-lib/src/protocol/`；韌體元件 `firmware/components/mxcam_protocol/` 直接引用
- TypeScript：`pc-app/src/protocol/`

韌體裡純 C、可在電腦上測的元件（各有 `host-test/`，CI 的 host-tests job 全部跑）：`mxcam_overlay`（畫框寫字，含鏡像文字）、`mxcam_service`（指令處理、延後回應佇列）、`mxcam_color`（顏色辨識）、`mxcam_tag`（包裝 vendored `apriltag`，單精度、PSRAM）、`mxcam_store`（NVS 存檔格式）、`mxcam_link/pushq`（推送佇列策略）、`mxcam_gray`（灰階轉換）、`mxcam_line`（線條追蹤）、`mxcam_track`（物體追蹤，NCC）、`mxcam_gesture` 的 post（手勢後處理；模型本體只能在板子上跑）、`mxcam_face` 的 `mxcam_face_post.c`（人臉框換算、排序；偵測本體 `mxcam_face.cpp` 包裝 Espressif `human_face_detect`／esp-dl，只能在板子上跑，基準量測用 `idf.py -DMXCAM_FACE_BENCH=ON build`）。碰硬體的：`mxcam_camera`、`mxcam_display`、`mxcam_link`（USB-Serial-JTAG RX/TX task）。功能 ID：1 顏色、2 AprilTag、3 人臉、4 線條、5 物體追蹤、8 手勢。流程在 `firmware/main/pipeline.c`（core 1 辨識＋TFT；追蹤狀態在 `tracking.c`，無主機測試）、`encoder.c`（core 0 esp_new_jpeg）。

**鏡像規則（ADR-0010）**：鏡頭輸出真實影像，所有辨識與協定座標都是真實影像座標；只有 TFT（面板翻轉＋鏡像文字）和 Studio（「鏡像顯示」開關）顯示鏡像。不要在辨識路徑上做任何翻轉。

**人臉模型載入**：`mxf_init` 載入時會全域把 malloc 導到 PSRAM（約 80 ms），所以必須在 `app_main` 建立其他 task 之前呼叫；esp-dl 配置失敗會 abort（C++ 例外關閉），`mxf_detect` 前有記憶體檢查。

pc-app（MXCam Studio）程式依用途分：
- `src/i18n`：多語系（繁中 / English）
- `src/device`：Device 抽象（`RealDevice` 接 Web Serial、`DemoDevice` 展示模式）；`config.ts` 驗證設定；`src/device/demo`（scene / recognize）是展示模式的模擬畫面與辨識
- `src/ui`：`layout`（外框與導覽）、`live`（`LiveStage` 即時畫面、geometry、labels）、`panel`（右側面板的分頁內容）、`pages`（設定頁與韌體頁）。CSS class 前綴：`ls-`（live stage）、`rp-`（right panel）、`pg-`（設定／韌體頁）、`tb-`（top bar）、`app-`（外框）
- 介面規格見 `docs/studio-ui.md`

正式 repo：github.com/Lowis-Mamilton/esp32S3_cam

## 指令

firmware（在 `firmware/`，需 ESP-IDF v5.5.6 環境；本機裝在 `C:\Espressif`）：
```
# PowerShell 先啟用環境（Git Bash 不行）
$env:IDF_TOOLS_PATH = "C:\Espressif"; . C:\Espressif\frameworks\esp-idf-v5.5.6\export.ps1
idf.py build
idf.py -p COMx flash        # TTL 或 OTG 的 COM 皆可
idf.py -p COMy monitor      # COMy 必須是 TTL 埠
```
注意：log 只從 UART0（TTL 埠）輸出；在 OTG 埠的 COM 上 monitor 過了 ROM banner 就沒有任何輸出（console 僅 UART0）。
埠號（此開發機的範例，各機器不同，重插也可能變）：TTL = CH340（VID 1A86，2026-11-19 是 COM7，之前曾是 COM9），OTG = COM5（VID 303A / PID 1001）；不確定時用 `python -m serial.tools.list_ports -v` 看。
用 pyserial 開 OTG 埠前先設 `dtr=False, rts=False` 再 `open()`，否則開關埠會讓板子重開機。

電腦上的 C 測試（Git Bash，MinGW 本機加 `-G "MinGW Makefiles"`）：
```
cmake -S arduino-lib/extras/host-test -B build/host-test && cmake --build build/host-test && ctest --test-dir build/host-test --output-on-failure
cmake -S firmware/components/mxcam_overlay/host-test -B build/overlay-host-test   # 同樣 build + ctest
cmake -S firmware/components/mxcam_service/host-test -B build/service-host-test   # 同樣 build + ctest
```

pc-app（在 `pc-app/`）：
```
npm ci
npm run dev | build | lint | test
npx vitest run src/app.test.tsx          # 單一測試檔
npx vitest run -t "測試名稱"              # 單一測試
```
Vite `base` 為 `/esp32S3_cam/`，dev server 網址為 http://localhost:5173/esp32S3_cam/。
部署：push 到 `main` 且 `pc-app/**` 有變更時，`.github/workflows/pages.yml` 會測試、建置並發佈到 GitHub Pages。

arduino-lib（在 repo 根目錄）：
```
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi --library arduino-lib arduino-lib/examples/HelloMXCam
```
編譯之外，協定的 C 測試在 `arduino-lib/extras/host-test`（見上方「電腦上的 C 測試」）；函式庫其餘部分以編譯通過為準。

CI（`.github/workflows/ci.yml`）：firmware、pc-app、arduino-lib、host-tests 四個 job。

## 注意事項

- 開發路徑是 `C:\dev\esp32S3_cam`。ESP-IDF 不支援路徑有空白，不要在舊的 `...\K230 Smart Cam\esp32S3_cam` 副本建置。
- 實機除錯：每秒一行的統計 log 在 TTL 埠（fps、proc_ms、enc_ms、heap、stack_hw…）。要從電腦送協定指令前，先關掉所有連著板子的 Studio 分頁（自動重連會佔住 OTG 埠）。驗證 AprilTag／影像方向可以把 FRAME 存成 JPEG 用 OpenCV（`cv2.aruco.DICT_APRILTAG_36h11`）離線比對。
- 除錯訊息走 TTL 孔（UART0），協定走 OTG 孔（原生 USB），不要混用。
- 硬體腳位以 `docs/SPEC.md` 第 4 節為準；鏡頭（實際是 OV3660）和螢幕腳位已在實板驗證，I2C／UART 腳位到 M4 才驗證。
- 實機量測紀錄在 `docs/measurements/`。

## 工作方式

- 與使用者的對話回覆用繁體中文。
- 使用者的全域規則：要建功能、改行為、做設計、選架構之前，先問清楚細節（grilling），不要直接動工；結論記錄到 `docs/SPEC.md` 或 `docs/adr/`。
- 本專案例外（使用者 2026-11-19 指示）：之後的設計問題**直接採用建議答案**，不用停下來問；但仍要把每個決定和理由寫進 `docs/SPEC.md` 或 ADR，並在回報時列出。
