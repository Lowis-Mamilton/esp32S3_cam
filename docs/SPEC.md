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
- AprilTag 請用黑色印在白紙上並保持良好對比；低對比的 tag 會被濾掉（`min_white_black_diff` 設為 20）
- AprilTag 的方向（旋轉角度、四個角的順序）以官方 AprilRobotics tag36h11 圖片為準；OpenCV／ArUco 產生的 tag36h11 圖案方向相差 180°。
- AprilTag **不估算距離與 3D 姿態**（需要鏡頭校正，留待之後的版本）。
- 人臉偵測（M3）：Espressif `human_face_detect`（MSR+MNP 兩階段）；信心度 = 分數 ×100，`face.threshold` 以下不回報。第一階段固定 0.5 的分數門檻，所以 `face.threshold` 設在 50 以下**不會多找到臉**。不需要學習、不能取名（LEARN／SET_NAME 回 `BAD_PARAM`，FORGET 回成功）。
- **擴充功能（2026-11-19 決定，依 `docs/research/vision-functions.md`）**：打樣前把下列功能做完整，依序實作：

| ID | 功能 | 做法 | 學習 | 里程碑 |
|---|---|---|---|---|
| 4 | 線條追蹤 | 自寫（灰階門檻＋分帶質心＋直線擬合） | 可選 | M6 |
| 5 | 物體追蹤 | 自寫（NCC 範本比對＋色彩直方圖找回） | 是（框選） | M6 |
| 6 | QR Code | `espressif/quirc`（ISC） | 可選（內容對應 ID） | M7 |
| 11 | 幾何形狀 | 自寫（輪廓＋多邊形近似） | 否 | M7 |
| 12 | 動態偵測 | 自寫（畫面差分＋連通區域） | 否 | M7 |
| 7 | 人體偵測 | `pedestrian_detect`（esp-dl） | 否 | M8 |
| 8 | 手勢辨識 | `hand_detect`＋`hand_gesture_recognition`（esp-dl） | 否 | M8a（提前，2026-11-19 使用者決定） |
| 9 | 人臉辨識 | 人臉偵測＋`human_face_recognition` 特徵模型 | 是 | M9 |
| 10 | 自學習分類 | 手工特徵＋kNN | 是 | M9 |

  - **線條追蹤（功能 4，ADR-0018）**：模式 `0` 自動深線（預設，黑膠帶白紙）、`1` 自動淺線、`2` 學到的顏色（彩色膠帶；LEARN 學線色，黑／白膠帶學不到，模式 2 沒學過時照模式 0 跑）；「自動」是 Otsu 門檻，也可以設固定門檻值。預設只看畫面**下方 60%**，切成 8 條橫帶由下往上找線，**至少 2 帶找到線才回報**。只回報**一條主線**（`id = 1`）：角度（0 = 垂直向上，順時針為正）、偏移（底端 x − 160，正 = 線在右邊）、兩端點，另有路口旗標 `CROSS`（橫線／十字／T 字路口）與岔路旗標 `BRANCH`。偏移與角度都是**真實影像座標**（ADR-0010），鏡頭朝前時「右」就是車子的右邊。參數見 `docs/protocol.md` §9（Line config v1）。
  - **物體追蹤（功能 5，ADR-0018）**：**一次追一個目標**（`id = 1`）。在即時畫面上框住物體按 LEARN 就學會（再 LEARN 會取代舊的、名字一起清掉）；物體**要有花紋或明暗變化**，純色、沒有紋理的東西或太小（< 16×16 px）學不到。之後每張在上次位置附近找（會跟著變大變小），結果是框的中心與大小、`confidence` = 相似度 ×100；**跟丟時沒有紀錄**（R4 讀到 0 筆就是跟丟）。跟丟後預設會**自動在全畫面找回**（`reacquire`）；同色但花紋不同的東西不會被當成目標。學到的物體跟顏色一樣，**SAVE 後斷電仍在**。參數見 `docs/protocol.md` §9（Track config v1）。
  - **手勢辨識（功能 8，ADR-0019，M8a）**：Espressif 手部偵測＋手勢分類，**9 種固定手勢**：1–5（比幾根手指）、讚、OK、打電話、倒讚（ID 1–9）。**不需要學習**，但可以取名（跟 AprilTag 一樣）。`id 0` = 有手但認不出（回報這隻手，R4 只看特定 ID 就會略過）。最多同時 2 隻手（`max_hands`，預設 1；一隻手約 5.5 fps、兩隻手約 4 fps）。信心度 = 手勢分數 ×100。參數（手的偵測門檻、手勢門檻、最多幾隻手）與完整規則見 `docs/protocol.md` §10（Gesture config v1）。
  - **QR Code（功能 6，ADR-0020，M7）**：`espressif/quirc`（ISC），在全解析度 320×240 上**每張都辨識**，一張畫面最多回報 **4 個解碼成功的碼**（大的在前）。結果是四個角的外接框，另帶與 AprilTag 相同的旋轉角度＋四個角（QR 自己的左上、右上、右下、左下）；`confidence` 固定 255。**內容用 `GET_RESULT_DATA (0x21)` 讀**（最近一批結果第 i 筆），**最多 128 bytes**，更長的截斷並標示 `TRUNCATED`。可以把內容學成 ID：框住一張碼按 LEARN（ID 1–8，同一個內容回原本的 ID，學滿 8 個回 `FULL`），之後同內容的碼回報那個 ID、沒學過的是 `id 0`；學到的內容用 `GET_LEARNED_DATA (0x35)` 讀，可以取名，SAVE 後斷電仍在。沒有參數。完整規則見 `docs/protocol.md` §11。
  - **幾何形狀（功能 11，ADR-0021，M7）**：自寫（半解析度二值化＋連通區域＋外輪廓＋多邊形近似）。**只認 4 種**：圓形、三角形、正方形、長方形（ID 1–4），五邊形、星形等不回報，**六邊形以上會算成圓**；碰到畫面邊緣的形狀與太大的區域（紙張、背景）不回報。只有外框（用筆畫的形狀）也可以。不需要學習，但可以取名（同手勢）。`confidence` = 與理想形狀的面積相符程度。參數（深色／淺色形狀、門檻自動或固定、最小尺寸、最多幾個）見 `docs/protocol.md` §11（Shape config v1）。
  - **動態偵測（功能 12，ADR-0021，M7）**：自寫（移動平均背景＋曝光補償＋連通區域），回報畫面中**動的區域**（`id 0`，`confidence` = 框內真正在動的比例）。**只適用於固定不動的鏡頭**：鏡頭被移動或整個畫面一起變亮變暗時會重建背景、幾張內不回報；切到這個功能後的前幾張也不回報；停下來的物體約 1–2 秒後不再回報。不需要學習、不能取名（同人臉）。參數（靈敏度門檻、最小尺寸、最多幾個）見 `docs/protocol.md` §11（Motion config v1）。
  - 延後：條碼（沒有適合授權、可用於 IDF 5 的函式庫）、貓狗偵測、ImageNet 分類。放棄：COCO 物件偵測（約 1 s／張）、姿態、OCR 等 ESP32-S3 沒有可用模型的功能。
  - 手勢（兩隻手）與人臉辨識約 3 fps，NN 功能的效能目標放寬為 ≥ 3 fps。
  - 手部關鍵點（21 點，類似 MediaPipe）：2026-11-19 原型評估後**不做**——MediaPipe hand_landmark_lite 在 ESP32-S3 上每隻手約 1 s、PSRAM 不夠和人臉／手勢模型共存（`docs/research/hand-landmarks-proto-b.md`）。手勢維持 ADR-0019 的分類器。
  - 架構前置（M6 開頭）：GET_INFO 功能上限調到 32、各功能自己的參數（Config v1 之外）、LEARN 可指定 ID；手勢（M8a）先放 app rodata、開機載入（ADR-0019）；加入第三個 NN 模型（人體或人臉辨識）時才改成模型分割區＋按需載入。
    - **M6 已完成協定部分**（ADR-0016、`docs/protocol.md` §9）：GET_INFO 功能清單上限 32；`GET_FUNC_CONFIG (0x44)`／`SET_FUNC_CONFIG (0x45)`，每個功能一個固定版面＋自己的 `layout_version`（功能 1–3 的參數留在 Config v1）；不新增能力位元，GET_INFO 的功能清單就是能力；LEARN 可多帶 `id (u16)`（11 bytes，M6 韌體一律回 `BAD_PARAM`）。模型分割區仍待 M8。

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
| 電位轉換 | 驗證階段用現成 BSS138 雙向模組＋杜邦線；打樣版改成板上電位轉換（見 `docs/hardware-requirements.md`） |
| 狀態燈 | WS2812 RGB LED（GPIO48）：藍＝待機、綠＝偵測到目標、紅＝錯誤 |
| TF 卡 | 第一版不使用；GPIO38/39/40 保留備用 |

### TFT 畫面

- 即時鏡頭畫面＋辨識框、ID、標籤（類似 HUSKYLENS 主畫面，無選單）
- 螢幕**橫向** 320×240，鏡頭畫面剛好填滿，不縮放、不裁切
- 使用者面對鏡頭和螢幕拿著板子，TFT 顯示**鏡像**畫面（像照鏡子，M1 實機確認）；辨識和協定座標一律用鏡頭的**真實影像**（不鏡像），見 ADR-0010
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

- 共同欄位：`id`、`x`、`y`（中心）、`width`、`height`、`confidence`（255 = 不適用）；座標以 **320×240** 為準
- AprilTag 另帶：旋轉角度、四個角座標
- 線條追蹤（M6）另帶：角度、相對畫面中央的偏移、線段兩端點、路口／岔路旗標（14 bytes，`docs/protocol.md` §9）
- QR Code（M7）另帶：與 AprilTag 相同的旋轉角度＋四個角（18 bytes）；內容是**附帶資料**，用 `GET_RESULT_DATA (0x21)` 另外讀（最多 128 bytes，`docs/protocol.md` §11）
- 一次最多 **8 筆**，依面積或信心度排序

### 預設值

- UART：**115200** baud 8N1，可在網頁改成 9600
- I2C：從屬位址 **0x5A**，可在網頁改

## 6. 軟體元件

### 6.1 韌體 `firmware/`

- ESP-IDF v5.x（見 ADR-0002）
- 鏡頭用 `esp32-camera` 元件，人臉偵測用 `espressif/human_face_detect`（esp-dl），螢幕用 `esp_lcd`（ILI9341）
- 設定與學習資料存在 NVS（單一 blob）；blob v2（M8a）：多一份各功能參數的紀錄清單（func config：線條、物體追蹤、手勢），可讀 v1；不認得的紀錄略過。blob v3（M6）：再多一份學習資料的紀錄清單（學到的線色、物體範本），最大約 3 KB（2,993 B），可讀 v1、v2；M7：形狀／動態參數加進 func config 紀錄清單，QR 對照表（最多 8 個 ID、內容 128 bytes）是 v3 學習資料清單的一筆，blob 最大 4,054 B（版本不變）；分割表不改，之後的新分割區只從 `storage` 切（ADR-0017）
- USB 連線先用 USB-Serial-JTAG（網頁燒錄最省事），在 M1 實測頻寬；不夠再改用 TinyUSB CDC
- 鏡頭輸出 **RGB565**（辨識和螢幕直接用）；傳給網頁時才用軟體壓 JPEG，預覽上限每秒 10 張，M1 實測約 6 張（目標 ≥ 5，見 `docs/measurements/m1.md`），把 CPU 留給辨識
- 效能設定：CPU 240 MHz、PSRAM 80 MHz、flash QIO 80 MHz（M1 起固定，作為量測基準）；資料快取 32 KB、**64 B 快取線**（M8a 加速起，所有功能都變快、內部 RAM 不變，ADR-0019 補充）
- 人臉模型放在 app 的 flash rodata（不改分割表），映像檔約 2.1 MiB（4 MB factory 分割區剩 47%）；開機時在其他 task 啟動前載入（約 80 ms、PSRAM 約 425 KB），失敗時功能 3 不列在 GET_INFO（見 ADR-0011）
- 人臉偵測無串流 44–58 ms（0–2 張臉），串流時最多約 62 ms（M3 實測），**每張畫面都偵測**（不降頻）：TFT 不串流時 10–11 fps、Studio 串流時 7 fps，都高於 ≥ 5 fps 的目標（降頻的門檻：不串流時 TFT < 7 fps 才改成每 2 張偵測一次，`pipeline.c` 的 `FACE_EVERY`）
- 手勢模型（`hand_detect` espdet_pico 224×224＋`hand_gesture_recognition` MobileNetV2 0.5，約 1.28 MB）也放 app 的 flash rodata，映像檔 3,526,000 B（factory 4 MB 剩 16%，上限 3,774,873 B）；開機時緊接在人臉之後、其他 task 啟動前載入（實測 332 ms、PSRAM 1.74 MiB、內部 RAM 0），開機完成時 PSRAM 剩約 3.9 MiB（見 ADR-0019、`docs/measurements/m8a.md`）
- 手勢辨識每張畫面都出結果（不另開 task，ADR-0019），TFT／預覽的速度＝辨識速度；**偵測之間重用手的框**（ADR-0019 補充）：找到手之後，接下來 2 張沿用上次的框（放大 25%）只跑分類，每 3 張重新偵測一次，重用的裁切被分成 `no_hand`、手的數量不足 `max_hands`、參數改變或切換功能時立刻重新偵測；重用的畫面回報的框是上次偵測的框（手在動時最多落後 2 張）。實測（64 B 快取線，餵測試畫面，`docs/measurements/m8a.md`「加速 1」「加速 2」）：沒有手 137 ms／張、5.8 次／秒；一隻手平均 120 ms、**5.5 次／秒**（之前 3.2）；兩隻手（`max_hands` 2）176 ms、**4.2 次／秒**（串流 4.0）；`max_hands` 2 但只有一隻手時每張都偵測（3.9 次／秒）

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
- 設定檔匯入真板子時只套用參數、系統設定和名字（顏色名字只在板上同一 ID 的顏色相符時才套用）；學到的顏色是鏡頭取樣得來的，無法從檔案重建，Studio 提示「顏色需要重新學習」（2026-11-19 決定；見 `docs/studio-ui.md` §4）

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
    // r.id, r.x, r.y, r.width, r.height, r.confidence（255 = 此功能不適用）
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
| M6 | 架構前置、線條追蹤、物體追蹤 | 兩個功能 TFT ≥ 10 fps（不串流）、實機驗收通過（**已完成**，2026-11-19 實機驗收通過，`docs/measurements/m6.md`） |
| M7 | QR、幾何形狀、動態偵測（功能 6、11、12） | 形狀／動態 TFT ≥ 10 fps、QR 沒有碼 ≥ 8 fps、一張碼 ≥ 3 fps、實機驗收通過 |
| M8a | 手勢辨識（功能 8） | 一隻手 ≥ 3 fps、實機 9 種手勢驗收通過 |

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
- [x] M2：預覽回到約 10 fps（esp_new_jpeg，見 `docs/measurements/m2.md`）
- [ ] TFT 從 11 fps 提升（SPI 時脈、XCLK）；AprilTag／人臉模式串流時 7 fps
- [ ] V1.3 擴展板：鏡頭和螢幕腳位已在實板驗證；I2C（41/42）和 UART（1/2）在 M4 驗證
- [x] 協定 C 程式碼的位置（ADR-0006）
- [ ] M4：Arduino 函式庫的發佈方式（把 `arduino-lib/` 打包成 release ZIP）
- [ ] M4 開工前（M2 總審查建議）：service/link 改成與傳輸介面無關（`mxs_handle` 帶傳輸別、各傳輸自己的解碼器／outbox／回應路徑、state mutex 移出 USB 元件；UART/I2C 上 STREAM_START 回 NOT_SUPPORTED；R4 輪詢不能讓 USB 串流維持或點亮連線圖示）；Arduino 函式庫需要的 C 端 GET_LEARNED／FORGET 解析；outbox 以 seq 確認；AprilTag 信心度改用 decision_margin 縮放；顯示器故障時辨識仍可運作（R4 無螢幕使用）
- [x] M3：人臉不支援取名（無身分辨識）；模型放 flash rodata（映像 2.13 MiB）、PSRAM 約 425 KB；無串流 44–58 ms（0–2 張臉），串流時最多約 62 ms，每張都偵測，TFT 10–11 fps（`docs/measurements/m3.md`）
- [ ] M4（M3 總審查建議）：
  - 辨識不再需要螢幕時，`mxf_init` 的條件改成只看鏡頭，並仍須放在建立 task 之前
  - 內部 RAM 預算：最低剩餘約 105 KB（底線 60 KB）；esp-dl 即使不用人臉也靜態佔約 24 KB，留給 UART／I2C 的餘裕約 45 KB；重新檢查 `mxf_detect` 的 heap guard（32 KB／256 KB）餘裕
  - Arduino `MXCamResult` 要加 `confidence`、`width`、`height` 欄位，與 Studio 的程式範例一致
  - heap guard 跳過偵測時要回報為「跳過」，不可快取或推送成「0 張臉」（也不計入 face_ms）
  - Studio 的「需要更新韌體」分不出舊韌體與人臉初始化失敗（改用韌體版本判斷）
  - [x] store 的 `name_key_ok` 接受人臉的 key：要拒絕，或讓 FORGET 3 清掉人臉名字——M6 Task 4 改成拒絕（線條 4 也不能取名、物體 5 只有 id 1）
  - `FACE_EVERY` > 1 的路徑未測試（不會用到就刪掉，要用就補測試）
- [x] M6 Task 4 交接（M8a Task 3 已改了 store／service，M6 暫停中）——1–7 已完成（1–6 在 M6 Task 4：blob v3、`case 4／5`、11 bytes 的 LEARN 回 `BAD_PARAM`；7 在 M6 Task 7）：
  1. 先 rebase 到 M8a 的 `mxcam_store`／`mxcam_service`（blob v2、`gesture_cfg`、GET/SET_FUNC_CONFIG 的 `fcfg_get`／`fcfg_set`）
  2. 線條／物體追蹤的參數加進 v2 的 func config 紀錄清單（`read_fcfg_list` 加 `case 4／5`、serialize 多寫兩筆，**不升 blob 版本**）；學到的線色與物體範本另外升 **v3**（v3 要能讀 v1、v2），`test_store.c` 的 `k_v1_fixture` 沿用、不可改
  3. service 的 func config 在 `fcfg_get`／`fcfg_set` 加 `case 4／5`，規則照 `docs/protocol.md` §9：**要看能力清單**（GET_INFO 沒列出 → `BAD_PARAM`）；M6 計畫 Task 4 寫的「只接受 4、5（不看能力清單）」作廢
  4. GET_INFO 的暫存區已改成 `23 + MXP_INFO_MAX_FUNCS`（9 個以上功能不再回 `INTERNAL`），不用再修
  5. M6 計畫 Task 4 Step 1「先抓 v1 樣本」已完成（`k_v1_fixture`）
  6. 目前韌體把 11 bytes 的 LEARN 當成一般 LEARN（沒有回 `BAD_PARAM`）；M8a 沒改，留給 M6 Task 4
  7. [x] Studio 的 LiveStage（`kind`、`stage*`）和 App（`showNames`）用串接三元運算子分辨功能，新增功能 ID 會**無聲地落到人臉分支**；M6 Task 7 已改成 `Record<FunctionId, …>`（commit aeab1a5；M8a 複審 M12）
- [ ] M6 審查延後項目：切到功能 5 後「目標遺失」可能閃一張；匯入模式 'learned' 但板上沒有線色時，選項顯示為停用；TFT 與 Studio 的線條／追蹤顏色不同（#2cc8d8／#4d8ef5 對 `--cat-line`／`--cat-track`）；降級到 M6 以前的韌體時，下一次 SAVE 會丟掉 v3 blob（M5 燒錄器要提醒）；FakeFirmware 對 LEARN 長度、灰色、dirty 比韌體寬鬆；`tracking.c` 沒有主機測試
- [ ] 名字表維持全部功能合計 32 筆（M6：線條不能取名、物體只有 id 1）；M7（QR 內容 → ID）或 M9 有 `mxdata` 分割區（ADR-0017）時再加大，`MXS_RESPONSE_MAX` 綁在名字表大小上，要一起改。**M7 決定不加大**（QR 最多多 8 筆、形狀 4 筆；加到 64 筆要多約 10 KB 內部 RAM 與約 1.7 KB blob，NVS 會很緊），M9 有 `mxdata` 分割區時再一起改
- [ ] M4：Arduino API 加 M7 功能：`MXCAM_ALGO_QR`（6）、`MXCAM_ALGO_SHAPE`（11）、`MXCAM_ALGO_MOTION`（12）；`MXCAM_SHAPE_CIRCLE`…`MXCAM_SHAPE_RECT`（1–4，值來自共用 C 的 `MXP_SHAPE_*`）；`bool MXCam::readText(uint8_t index, char *buf, size_t cap)`（`GET_RESULT_DATA`；`frame_id` 和上一次 `request()` 不同就回 false，學生再 `request()` 一次）；QR 的 `r.angle` 與 AprilTag 共用 `mxp_unpack_corners_extra`。Studio 程式分頁的範例照這組名稱寫（M7 計畫設計決定 18）
- [ ] M4：service 改寫（傳輸別、各傳輸自己的 outbox）要以 M6 之後的 `mxcam_service` 為基礎（`0x44`／`0x45` func config、功能 4／5 的 LEARN／FORGET／SET_NAME／GET_LEARNED、`mxs_finish_learn_object`）
- [ ] M4：Arduino API 加線條與物體追蹤：`MXCAM_ALGO_LINE`（4）、`MXCAM_ALGO_TRACK`（5）；`MXCamResult` 在線條功能多 `angle`（度，int）、`offset`（px）、`flags`（`MXCAM_LINE_BRANCH`、`MXCAM_LINE_CROSS`），由共用 C 的 `mxp_unpack_line_extra` 解出（M6 計畫設計決定 17）
- [x] M2 開工前（M1 總審查建議）——已在 M2 處理：
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
