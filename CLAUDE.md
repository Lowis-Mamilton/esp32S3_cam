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

韌體裡純 C、可在電腦上測的元件：`mxcam_overlay`（畫框寫字）、`mxcam_service`（指令處理）；碰硬體的：`mxcam_camera`、`mxcam_display`、`mxcam_link`（USB-Serial-JTAG），流程在 `firmware/main/pipeline.c`、`encoder.c`。

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
埠號（此開發機的範例，各機器不同）：TTL = COM9（CH340），OTG = COM5（VID 303A / PID 1001）。
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
Vite `base` 為 `/esp32S3_cam/`（GitHub Pages）。

arduino-lib（在 repo 根目錄）：
```
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi --library arduino-lib arduino-lib/examples/HelloMXCam
```
目前沒有單元測試，以編譯通過為準。

CI（`.github/workflows/ci.yml`）：firmware、pc-app、arduino-lib、host-tests 四個 job。

## 注意事項

- 開發路徑是 `C:\dev\esp32S3_cam`。ESP-IDF 不支援路徑有空白，不要在舊的 `...\K230 Smart Cam\esp32S3_cam` 副本建置。
- 除錯訊息走 TTL 孔（UART0），協定走 OTG 孔（原生 USB），不要混用。
- 硬體腳位以 `docs/SPEC.md` 第 4 節為準；鏡頭（實際是 OV3660）和螢幕腳位已在實板驗證，I2C／UART 腳位到 M4 才驗證。
- 實機量測紀錄在 `docs/measurements/`。

## 工作方式

- 與使用者的對話回覆用繁體中文。
- 使用者的全域規則：要建功能、改行為、做設計、選架構之前，先問清楚細節（grilling），不要直接動工；結論記錄到 `docs/SPEC.md` 或 `docs/adr/`。
