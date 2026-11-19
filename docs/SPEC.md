# MXCam 規格書 v0.1（第一版）

> 狀態：草稿，待確認。拷問結論整理於 2026-11-18。
> 背景決策見 `docs/adr/`，名詞定義見 `GLOSSARY.md`。

## 1. 產品定位

用現有的 GOOUUU ESP32-S3 擴展板 + 2.8 吋 TFT，做出**類似 HUSKYLENS 2** 的視覺感測器，搭配 **MATRIX Mini R4** 使用。

與 HUSKYLENS 2 的差別：
- ESP32-S3 上只跑**小型模型與傳統演算法**，螢幕只顯示即時畫面與辨識結果，**沒有選單**。
- HUSKYLENS 2 機身上的系統功能（切換功能、學習、調參數、設定）全部搬到**電腦端網頁軟體**，網頁介面模仿 HUSKYLENS 的螢幕介面。
- 電腦只在**設定時**連接；設定存進板子後拔掉電腦，MXCam 單獨與 R4 運作。

## 2. 系統組成

```
┌────────────── 電腦（Windows / macOS，Chrome 或 Edge）──────────────┐
│  MXCam Studio（Web Serial PWA）                                    │
│  即時畫面＋辨識框｜切換功能｜顏色學習｜調參數｜存到板子｜燒錄韌體    │
└──────────────────────────┬─────────────────────────────────────────┘
                           │ USB（OTG 孔，原生 USB）
┌──────────────────────────┴─────────────────────────────────────────┐
│  MXCam 韌體（ESP-IDF v5.x，ESP32-S3 N16R8）                         │
│  OV3660 取像 → 演算法 → ILI9341 顯示畫面＋框 → 回應主機             │
└──────────────┬──────────────────────────────┬──────────────────────┘
               │ UART（GPIO1/2）              │ I2C（GPIO41/42）
               └──── BSS138 電位轉換模組 ─────┘
                           │ 5V
┌──────────────────────────┴─────────────────────────────────────────┐
│  MATRIX Mini R4 ＋ MXCam Arduino 函式庫                              │
└────────────────────────────────────────────────────────────────────┘
```

## 3. 第一版功能

| 功能 | 做法 | 需要學習 | 每筆結果額外資料 |
|---|---|---|---|
| 顏色 | HSV 閾值 + 連通區域 | 是，最多 7 個顏色 ID | 無 |
| AprilTag | 只支援 tag36h11 | 否，ID 來自 tag 本身 | 旋轉角度、四個角座標 |
| 人臉偵測 | esp-dl 官方人臉偵測模型 | 否 | 無 |

- **同一時間只跑一個功能**，由網頁或 R4 切換。
- AprilTag **不估算距離與 3D 姿態**（需要鏡頭校正，留待之後的版本）。
- 第二版候選：幾何形狀、人臉辨識（需學習）、固定類別物件偵測。

### 顏色學習流程

1. 在網頁的即時畫面上框選或點選要學的顏色
2. 自動算出 HSV 範圍，並分配下一個 ID（ID1、ID2……）
3. 可用滑桿微調範圍、幫 ID 取名字、刪除 ID
4. 按「存到板子」寫入 NVS

### 效能目標（驗收標準，實測後可調整）

- 鏡頭解析度：QVGA 320×240
- 顏色、AprilTag：≥ 10 fps
- 人臉偵測：≥ 5 fps
- R4 讀一次結果：延遲 ≤ 50 ms

## 4. 硬體

依 V1.2 擴展板的公開資料整理，**V1.3 實板接線前要先確認**。

| 項目 | 規格 / 接腳 |
|---|---|
| 模組 | ESP32-S3-WROOM-1 N16R8（16MB flash、8MB octal PSRAM） |
| 鏡頭 | OV3660（實板偵測結果；早期資料寫 OV2640，兩者驅動相容）；XCLK=15、SIOD=4、SIOC=5、D0–D7=11,9,8,10,12,18,17,16、VSYNC=6、HREF=7、PCLK=13 |
| 螢幕 | ILI9341 橫向 320×240；SCLK=3、MOSI=45、MISO 不使用、CS=14、DC=47、RST=21，背光常亮 |
| 觸控 | XPT2046，**不使用**（腳位讓給 I2C） |
| USB | OTG 孔＝原生 USB（網頁連線、燒錄）；TTL 孔＝UART0（開發者除錯訊息） |
| 接 R4：I2C | SDA=42、SCL=41（板上 OLED 排針） |
| 接 R4：UART | TX/RX=GPIO1/2（須拔掉跳線帽 P7、P8，電位器與 DHT11 停用） |
| 電位轉換 | 第一版用現成 BSS138 雙向模組＋杜邦線 |
| 狀態燈 | WS2812 RGB LED（GPIO48）：藍＝待機、綠＝偵測到目標、紅＝錯誤 |
| TF 卡 | 第一版不使用；GPIO38/39/40 保留備用 |

### TFT 畫面

- 即時鏡頭畫面＋辨識框、ID、標籤（類似 HUSKYLENS 主畫面，無選單）
- 螢幕**橫向** 320×240，鏡頭畫面剛好填滿，不縮放、不裁切
- 螢幕裝在鏡頭背面（同 HUSKYLENS），畫面**不鏡像**；確切旋轉方向在第一次實機燒錄時調整
- 左上角疊一行小字：目前功能名稱＋fps；右上角小圖示表示 MXCam Studio 是否連線中
- 框和文字直接畫在 RGB565 畫面上（內建點陣字型），**不使用 LVGL**

## 5. 通訊協定（MXCam Protocol v1）

全新的版本化協定，**不相容** HUSKYLENS 1/2、K230 規劃、M-Vision Camera（見 ADR-0003）。
USB、UART、I2C 三種傳輸用同一套封包。

### 封包格式

```
| 'M' 'X' | ver | cmd | seq | len (u16) | payload (len bytes) | crc16 (u16) |
```

- 數字一律 little-endian
- `ver`：協定版本，v1 為 `0x01`
- `seq`：主機自行遞增，回應時原樣帶回，用來配對請求和回應
- `crc16`：CRC-16/CCITT-FALSE（poly 0x1021、init 0xFFFF），範圍為 `ver` 到 `payload` 結尾
- 完整定義（指令代碼、錯誤碼、各指令的 payload）見 **`docs/protocol.md`**，測試資料見 `docs/protocol-vectors.json`
- I2C 上的傳輸方式（讀長度、分段讀取）在 M4 定稿

### 互動模式

- **R4（UART / I2C）**：主機發問、MXCam 回答，一問一答
  - I2C 從屬裝置要回傳可變長度資料，讀取方式（例如先讀長度再讀內容）在 `docs/protocol.md` 定稿
- **網頁（USB）**：同樣可以一問一答，另外可以**訂閱**，之後 MXCam 主動持續推送 JPEG 畫面與辨識結果
- **能力查詢**：主機連線後先送「查詢能力」，MXCam 回報協定版本、韌體版本、支援的功能清單

### 指令類別（初稿）

| 類別 | 指令 |
|---|---|
| 連線 | 查詢能力、心跳 |
| 功能 | 切換功能、查詢目前功能 |
| 結果 | 讀取結果 |
| 學習 | 學習（顏色）、刪除 ID、幫 ID 取名、讀取 / 寫入顏色參數 |
| 設定 | 讀取 / 寫入設定（UART baud、I2C 位址…）、存到 flash、恢復原廠設定 |
| 串流（僅 USB） | 訂閱 / 取消訂閱畫面＋結果 |

### 辨識結果

- 共同欄位：`id`、`x`、`y`（中心）、`width`、`height`、`confidence`；座標以 **320×240** 為準
- AprilTag 另帶：旋轉角度、四個角座標
- 一次最多 **8 筆**，依面積或信心度排序

### 預設值

- UART：**115200** baud 8N1，可在網頁改成 9600
- I2C：從屬位址 **0x5A**，可在網頁改

## 6. 軟體元件

### 6.1 韌體 `firmware/`

- ESP-IDF v5.x（見 ADR-0002）
- 鏡頭用 `esp32-camera` 元件，人臉偵測用 `esp-dl` / `esp-who`，螢幕用 `esp_lcd`（ILI9341）
- 設定與學習資料存在 NVS
- USB 連線先用 USB-Serial-JTAG（網頁燒錄最省事），在 M1 實測頻寬；不夠再改用 TinyUSB CDC
- 鏡頭輸出 **RGB565**（辨識和螢幕直接用）；傳給網頁時才用軟體壓 JPEG，預覽上限每秒 10 張，M1 實測約 6 張（目標 ≥ 5，見 `docs/measurements/m1.md`），把 CPU 留給辨識
- 效能設定：CPU 240 MHz、PSRAM 80 MHz、flash QIO 80 MHz（M1 起固定，作為量測基準）

### 6.2 電腦軟體 `pc-app/`：MXCam Studio

- **Web Serial PWA**，TypeScript + Vite + React（見 ADR-0004）
- 介面細節（版面、功能頁、學習、參數、展示模式、外觀）見 **`docs/studio-ui.md`**；Studio UI 已依此實作（含展示模式），GitHub Pages 已上線：https://lowis-mamilton.github.io/esp32S3_cam/
- 支援 Windows、macOS 上的 Chrome / Edge；Safari 不支援 Web Serial
- 發佈到 GitHub Pages（已完成）；可安裝到桌面、離線可用（**尚未完成**，M5 做 PWA）
- 介面模仿 HUSKYLENS 的螢幕介面：即時畫面＋辨識框、功能切換、顏色學習、參數調整、存到板子
- 內建網頁燒錄韌體（esptool-js），使用者不需安裝 ESP-IDF
- 語言：繁體中文、English（從一開始就做多語系）
- 辨識框由網頁依據 RESULTS 自己畫在畫面上，不畫進 JPEG
- 自動重新連線：網頁打開或板子重新插上時，自動連回上次授權過的序列埠；只有第一次要手動選
- 串流時每秒送一次 PING；MXCam 3 秒沒收到請求就自動停止串流

### 6.3 Arduino 函式庫 `arduino-lib/`

- 給 MATRIX Mini R4（Arduino UNO R4 WiFi 板型）使用，UART 與 I2C 共用同一套 API
- 寫法模仿 HUSKYLENS 函式庫，方便老師和學生轉換：

```cpp
MXCam cam;
cam.begin(Serial1);               // 或 cam.begin(Wire, 0x5A)
cam.setAlgorithm(MXCAM_ALGO_COLOR);
if (cam.request()) {
  while (cam.available()) {
    MXCamResult r = cam.read();
    // r.id, r.x, r.y, r.width, r.height
  }
}
```

- MATRIXblock 積木**不在第一版**，等函式庫 API 穩定後再做

### 6.4 協定程式碼共用

- 協定編解碼用 C 寫一份，韌體和 Arduino 函式庫共用：正本在 `arduino-lib/src/protocol/`（純 C），韌體元件 `firmware/components/mxcam_protocol/` 直接引用（見 ADR-0006）
- 網頁另外用 TypeScript 寫一份
- 兩邊用同一組測試資料（`docs/protocol-vectors.json`）驗證結果一致

## 7. Repo 結構與授權

```
firmware/      ESP-IDF 韌體
pc-app/        MXCam Studio（Web Serial PWA）
arduino-lib/   給 MATRIX Mini R4 的 Arduino 函式庫
docs/          規格、協定、ADR
```

- 公開 repo，**MIT 授權**
- 開發時 repo 放在沒有空白的路徑 `C:\dev\esp32S3_cam`（ESP-IDF 不支援路徑空白），ESP-IDF v5.5.6 也裝在沒有空白的路徑
- 直接在 `main` 上開發，CI 把關
- 舊的 `.ino` 與 Python 腳本直接刪除，靠 git 歷史保存
- `CLAUDE.md` 在 M0 改寫成新架構

## 8. 里程碑（建議順序）

| 里程碑 | 內容 | 完成條件 |
|---|---|---|
| M0 | 清掉舊程式、建立三個子專案骨架、CI（IDF 編譯、網頁 build/test、函式庫編譯） | CI 全綠 |
| M1 | 韌體：鏡頭＋ILI9341 顯示＋USB 協定＋能力查詢；網頁：連線並顯示即時畫面 | 網頁上看到即時畫面 |
| M2 | 顏色（含網頁學習、存 NVS）、AprilTag | 兩個功能達到 fps 目標，畫面上框得到 |
| M3 | 人臉偵測（esp-dl） | ≥ 5 fps |
| M4 | UART、I2C、Arduino 函式庫、電位轉換接線 | R4 用範例程式讀到三種功能的結果，延遲 ≤ 50 ms |
| M5 | 網頁燒錄、PWA 離線、GitHub Pages、多語系 | 新電腦只開網頁就能燒錄並使用 |

## 9. 測試策略

- 協定：C 與 TypeScript 兩份實作都跑同一組測試資料
- 網頁：Vitest 單元測試
- 韌體：協定與演算法的純邏輯部分做主機端單元測試；硬體相關以實機驗收為主
- Arduino 函式庫：CI 用 arduino-cli 編譯範例

## 10. 待確認事項

- [x] 螢幕確定是 ILI9341（實板驗證）
- [x] USB-Serial-JTAG 頻寬足夠（M1 實測只用 < 30 KB/s）
- [x] 320×240 影像的擺法：螢幕改橫向（第 4 節）
- [x] 協定封包、指令代碼、錯誤碼定稿（`docs/protocol.md`）
- [ ] M4：I2C 傳輸方式定稿
- [x] M1：`firmware/dependencies.lock` 已 commit
- [x] M1：效能實測（`docs/measurements/m1.md`）
- [ ] M2：預覽回到 10 fps（換更快的 JPEG 編碼器）、TFT 從 11 fps 提升（SPI 時脈、XCLK）
- [ ] V1.3 擴展板：鏡頭和螢幕腳位已在實板驗證；I2C（41/42）和 UART（1/2）在 M4 驗證
- [x] 協定 C 程式碼的位置（ADR-0006）
- [ ] M4：Arduino 函式庫的發佈方式（把 `arduino-lib/` 打包成 release ZIP）
- [ ] M2 開工前（M1 總審查建議）：
  - `mxcam_link` 改成非阻塞的推送佇列（`mxcam_link_push`），推送序號集中在 link；RESULTS 不可以讓 TFT 卡住
  - 需要 pipeline 資料才能回答的指令（例如 LEARN）：先決定用 BUSY＋輪詢，還是延後回應
  - 量測 TFT 每張約 91 ms 花在哪裡（等鏡頭 vs SPI 傳輸），再加入辨識演算法
  - 換 JPEG 編碼器時，輸入格式要選 **big-endian** RGB565
  - 初始化失敗時在 TFT 顯示錯誤字串、USB 仍可回應 GET_INFO
  - git hash 每次建置都更新、加上 `-dirty`
  - 測試補強：`mxp_unpack_info` n=2、解碼器 len==cap 邊界、逐 byte 餵入、`unhex` 長度檢查
  - 1 Hz log 加上 task stack 高水位和最小 heap
  - FRAME 和 RESULTS 的 timestamp 統一用同一個來源
  - 網頁小問題：多個已授權埠時自動連線會忽略使用者的「中斷」；watchdog 改用時間戳而不是計數
  - macOS Chrome 驗證：關分頁不重開機、拔插自動重連
