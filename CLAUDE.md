# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 架構

MXCam：ESP32-S3 視覺感測器，搭配 MATRIX Mini R4 使用，設定由電腦端 Web Serial PWA 完成。三個子專案：

- `firmware/`：ESP-IDF v5.5.6 韌體（ESP32-S3 N16R8），專案名 `mxcam`
- `pc-app/`：MXCam Studio（Vite + React + TS）
- `arduino-lib/`：給 R4 用的 Arduino 函式庫 `MXCam`

規格與決策不在這裡重複，請看：`docs/SPEC.md`（規格）、`docs/adr/`（架構決策）、`GLOSSARY.md`（名詞）。
目前**還沒有協定程式碼**：MXCam Protocol v1 只在 `docs/SPEC.md` §5 有規格，M1 期間會定稿為 `docs/protocol.md`。

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
目前沒有韌體單元測試。本機沒裝 ESP-IDF 時，韌體只能靠 CI 或本機安裝 v5.5.6 驗證，不要宣稱「已建置成功」而沒跑過。

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

CI（`.github/workflows/ci.yml`）三個 job 分別建置以上三者。

## 注意事項

- 開發路徑是 `C:\dev\esp32S3_cam`。ESP-IDF 不支援路徑有空白，不要在舊的 `...\K230 Smart Cam\esp32S3_cam` 副本建置。
- 除錯訊息走 TTL 孔（UART0），協定走 OTG 孔（原生 USB），不要混用。
- 硬體腳位以 `docs/SPEC.md` 第 4 節為準；V1.3 實板尚未核對。

## 工作方式

- 與使用者的對話回覆用繁體中文。
- 使用者的全域規則：要建功能、改行為、做設計、選架構之前，先問清楚細節（grilling），不要直接動工；結論記錄到 `docs/SPEC.md` 或 `docs/adr/`。
