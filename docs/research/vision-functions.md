# 研究：MXCam 還能加哪些類 HUSKYLENS 2 視覺功能

> 日期：2026-11-19 ・ 狀態：研究筆記（建議，尚未定案；定案時寫進 `docs/SPEC.md`／新 ADR）
> 範圍：在現有硬體（ESP32-S3 N16R8、OV3660 QVGA RGB565）與現有架構（ESP-IDF v5.5.6、esp-dl 3.3.15、一次只跑一個功能）下，哪些功能做得出來、該照什麼順序做。
> 延遲數字除非標「實測」，都是 Espressif 官方 README 在 ESP32-S3（240 MHz、64 KB data cache、octal PSRAM 80 MHz、int8、`-O2`）上量的**單獨推論**時間；放進 MXCam 時，core 0 的 JPEG 壓縮會搶 PSRAM 頻寬，M3 實測人臉偵測因此慢了約 10–20%（52→62 ms）。

## 1. 結論摘要

| 建議 ID | 功能 | 做法 | 學習 | 預估速度 | 工作量 | 優先度 |
|---|---|---|---|---|---|---|
| 4 | 線條追蹤 | 傳統：灰階＋門檻、分列掃描、最小平方法擬合 → 線的角度、偏移、端點 | 選用（學線的顏色；預設自動「深色線、淺色底」） | 估 3–6 ms，受限於鏡頭／TFT 約 10–11 fps | 小（3–5 天） | **最高**：R4 循跡車的核心用途 |
| 5 | 物體追蹤 | 傳統：學一個框 → 灰階 NCC 範本比對（搜尋窗＋多尺度）＋色彩直方圖找回 | 是（框選一次） | 估 10–20 ms，≥ 10 fps | 中（5–8 天） | 高：跟隨物體的機器人 |
| 6 | QR Code | `espressif/quirc` 1.2.0（ISC） | 選用（學一張 QR → 指定 ID） | 無 QR 約 22 ms、有 QR 約 230 ms（官方 demo，240×240 灰階）；估 4–10 fps | 中（4–6 天＋協定 2 天） | 高：路標卡、任務卡 |
| 7 | 人體偵測 | `espressif/pedestrian_detect` 0.3.3（MIT），224×224 | 否 | 125 ms → 約 7–8 fps | 小（2–3 天，**在模型載入基礎建設之後**） | 中：跟人走的車；驗證按需載入機制 |
| 8 | 手勢辨識 | `espressif/hand_detect` 0.2.1 ＋ `hand_gesture_recognition` 0.2.1（MIT） | 否（固定 9 種手勢） | 一隻手 129＋74 ≈ 203 ms → 約 4.9 fps | 中（3–4 天） | 中高：互動、遙控 |
| 9 | 人臉辨識 | `espressif/human_face_recognition` 0.3.3 的 MFN 特徵模型＋自寫比對與儲存 | 是（ID、名字、多張樣本） | 偵測 45–53 ms＋每張臉 256 ms → 約 3 fps（一張臉） | 大（8–12 天） | 中：教學上很受歡迎，但慢 |
| 10 | 自學習分類 | 第一版用手工特徵（HSV 直方圖＋梯度方向直方圖）＋ kNN | 是（Studio 擷取樣本） | 估 < 10 ms | 中大（6–10 天，品質待驗證） | 中低 |
| 11 | 幾何形狀 | 傳統：二值化 → 輪廓 → 多邊形近似 → 圓／三角／方 | 否 | 估 5–15 ms | 中（4–6 天） | 低（SPEC 列為第二版候選） |
| 12 | 動態偵測 | 傳統：縮小灰階畫面差分＋連通區域 | 否 | 估 < 5 ms | 小（2–3 天） | 低 |

**延後**：條碼（沒有授權乾淨、可直接用的函式庫）、貓狗偵測（可做但教學價值低）、ImageNet 1000 類分類（3.6 MB、約 2 fps）。
**放棄**：COCO 80 類物件偵測（YOLO11n 320×320 約 1 s／張）、姿態、實例分割、OCR、車牌、情緒、視線、臉部朝向、跌倒偵測。

**架構前提**：功能 7 以後的模型加起來約 2.9 MB，app 分割區只剩約 1.87 MB，必須（1）把模型移到獨立的 flash 分割區、（2）切換功能時才載入／卸載模型。這會改分割表、修改 ADR-0011 的開機載入限制，也影響 M5 的網頁燒錄（第 5、7 節）。

## 2. 現況與限制

| 項目 | 現況 | 對新功能的影響 |
|---|---|---|
| CPU | 雙核 240 MHz；core 1 跑辨識＋TFT，core 0 跑 JPEG 預覽 | esp-dl 的多核推論用不到（core 0 被 JPEG 佔用） |
| 記憶體 | 8 MB PSRAM；內部 RAM 開機以來最低剩約 105 KB（底線 60 KB），esp-dl 靜態佔約 24 KB（M3 實測） | 每載入一個 esp-dl 模型，若不把 malloc 導向 PSRAM，會多佔約 15 KB 內部 RAM（人臉模型實測 15.5 KB） |
| Flash | factory 4 MB，映像 2,232,848 B（esp-dl 程式 1.39 MB＋人臉模型 191 KB），剩約 1.87 MiB；`storage` 分割區 0xBF0000（約 11.9 MiB）保留給模型、目前沒用 | 見第 5 節 |
| 效能目標 | 傳統 ≥ 10 fps、NN ≥ 5 fps（重模型可放寬到約 3 fps 並寫進文件） | 每張 ≤ 100 ms／≤ 200 ms（≤ 330 ms） |
| 結果格式 | ResultRecord：`id u16, x, y i16, w, h u16, confidence u8, extra_len u8, extra`；一次最多 8 筆 | 新功能的資訊要擠進這些欄位 |
| Arduino 函式庫上限 | `MXP_RESULT_EXTRA_MAX = 18`：extra 超過 18 bytes 的紀錄會讓**整包 GET_RESULTS 解析失敗**；`INFO_MAX_FUNCS = 8`：GET_INFO 列超過 8 個功能也會解析失敗（`arduino-lib/src/protocol/mxcam_proto.c`） | 功能 ID 加到 12 前要先調大 `INFO_MAX_FUNCS`（建議 32）；extra 設計一律 ≤ 18 bytes |
| 設定 | Config v1 固定 18 bytes 的結構，協定說「只新增指令、不改既有欄位」 | 新功能的參數不要塞進 Config v1，見第 7 節 |
| 學習 | LEARN 只給一個區域，回傳新 ID；學習資料存在 NVS（24 KB 分割區）；名字表全部功能合計 32 筆 | 人臉辨識要「同一個 ID 多加樣本」、嵌入向量要更大的儲存空間 |
| 模型載入 | ADR-0011：`mxf_init` 只能在 `app_main` 建 task 之前呼叫，因為載入期間用 `heap_caps_malloc_extmem_enable(0)` 把**全域** malloc 導到 PSRAM | 按需載入不能沿用這個做法，見第 5.3 節 |

## 3. HUSKYLENS 2 的功能清單

來源：DFRobot wiki SEN0638 Function Guide、DFRobot_HuskylensV2 Arduino 函式庫的 `eAlgorithm_t`。HUSKYLENS 2 用的是 K230（約 6 TOPS NPU），ESP32-S3 沒有 NPU，所以很多功能只能放棄。

| # | HUSKYLENS 2 功能 | 內容 | 可學習 | MXCam 可行性 |
|---|---|---|---|---|
| 1 | 人臉辨識 | 偵測人臉、學習並辨識 ID／名字 | 是 | 可（約 3 fps）→ 建議 ID 9 |
| 2 | 物體辨識 | 80 類（COCO）偵測 | 否 | 不可行：YOLO11n 320 約 1 s／張 |
| 3 | 物體追蹤 | 框選一個物體並追蹤 | 是 | 可（傳統）→ 建議 ID 5 |
| 4 | 顏色辨識 | 學顏色 ID | 是 | 已有（功能 1） |
| 5 | 物體分類 | ImageNet 1000 類 | 是（把類別學成 ID） | 勉強：MobileNetV2 約 440 ms、3.6 MB → 延後 |
| 6 | 自學習分類 | 多角度拍攝自訂物體並分類 | 是 | 可（手工特徵＋kNN）→ 建議 ID 10 |
| 7 | 實例分割 | 物體輪廓遮罩 | — | 不可行（YOLO11n-seg 級別） |
| 8 | 手部辨識 | 21 個關鍵點、手勢 | 是（學手勢） | 部分：有手部偵測＋固定 9 種手勢分類，**沒有**關鍵點模型 → 建議 ID 8 |
| 9 | 姿態辨識 | 17 個人體關鍵點、學姿勢 | 是 | 不可行：YOLO11n-pose 640 約 4.4 s／張；可退而求其次做人體偵測（ID 7） |
| 10 | 車牌辨識 | 偵測、辨識車牌 | 是 | 不可行（無 S3 模型） |
| 11 | OCR | 文字偵測與辨識 | 是 | 不可行（`pp_ocr_v6` 只支援 ESP32-P4） |
| 12 | 線條追蹤 | 依顏色對比找線、標出路徑；結果有起點、終點、角度、長度 | 否 | 可（傳統）→ 建議 ID 4 |
| 13 | 情緒辨識 | 7 種表情 | 否 | 不可行（無 S3 模型，自訓成本高） |
| 14 | 視線 | 視線角度方向 | 否 | 不可行 |
| 15 | 臉部朝向 | roll／yaw／pitch，學姿勢成 ID | 是 | 不可行（需臉部關鍵點模型） |
| 16 | 標籤辨識 | AprilTag | 是（取名） | 已有（功能 2） |
| 17 | QR Code | 偵測、學習、取名、追蹤 | 是 | 可（quirc）→ 建議 ID 6 |
| 18 | 條碼 | 偵測、學習、取名 | 是 | 延後（授權／無位置輸出，見 4.3） |
| 19 | 跌倒偵測 | 偵測倒地的人 | 否 | 不可行 |
| — | 深度相機、自動駕駛（交通、城市道路） | 函式庫列有 ID 20–22 | — | 需額外硬體或大模型，不考慮 |

HUSKYLENS 2 的線條追蹤結果在函式庫裡用 `xTarget/yTarget`、`angle`、`length` 等欄位表示，所以我們把「角度＋端點」放進 extra 與它的使用習慣一致。

## 4. 候選功能逐項評估

### 4.1 esp-dl 模型（Espressif 官方，全部 MIT，全部依賴 esp-dl `~3.3.14`，與我們的 3.3.15 相容）

模型檔大小取自 esp-dl GitHub `models/<名稱>/models/s3/*.espdl`；版本與授權取自 components.espressif.com。

| 元件（版本） | S3 模型 | 輸入 | 模型檔 | S3 延遲（前處理／模型／後處理） | 預估 fps | 結論 |
|---|---|---|---|---|---|---|
| `human_face_detect` 0.5.2（已用） | MSR＋MNP s8 | 動態 | 61＋130 KB | 實測 44–58 ms | 實測 10–11 TFT | 已有（功能 3） |
| `pedestrian_detect` 0.3.3 | pico_s8_v1 | 224×224 | 435,328 B | 10.6／113.3／1.1 = 125 ms | 7–8（串流約 6–7） | **做**（ID 7） |
| `hand_detect` 0.2.1 | espdet_pico_224_224_hand | 224×224 | 497,184 B | 7.6／120.1／0.9 = 129 ms；mAP50-95 0.408 | — | 給手勢用 |
| `hand_gesture_recognition` 0.2.1（依賴 hand_detect `~0.2.0`） | mobilenetv2_0_5_s8_v1 | 128×128（每隻手裁一次） | 787,296 B | 4.2／69.2／0.5 ≈ 74 ms／手 | 一隻手約 4.9、兩隻手約 3.6 | **做**（ID 8），每張最多 2 隻手 |
| `human_face_recognition` 0.3.3（依賴 human_face_detect `~0.5.0`） | MFN_S8_V1（1.2M 參數，IJB-C TAR@FAR=1e-4 90.03%） | 112×112 | 1,295,168 B | 3.9／252.1／0.1 ≈ 256 ms／臉 | 一張臉約 3.2 | **做**（ID 9），每張只辨識最大的 1–2 張臉 |
| 同上 | MBF_S8_V1（3.4M 參數，93.94%） | 112×112 | 3,522,784 B | 3.9／464.7／0.1 ≈ 469 ms／臉 | 約 1.9 | 不用（太慢太大） |
| `cat_detect` 0.3.1 | espdet_pico_224_224_cat（mAP 0.656） | 224×224 | 498,576 B | 8.8／120.3／0.8 ≈ 130 ms | 約 7 | 延後 |
| `dog_detect` 0.2.1 | espdet_pico_224_224_dog（mAP 0.541） | 224×224 | 500,064 B | 8.1／121.0／0.7 ≈ 130 ms | 約 7 | 延後 |
| `imagenet_cls` 0.4.2 | mobilenetv2_s8_v1（top-1 70.5%） | 224×224 | 3,612,288 B | 8.5／437.9／1.2 ≈ 448 ms（雙核 400 ms） | 約 2 | 延後 |
| `coco_detect` 0.4.2 | yolo11n_320_s8_v1 | 320×320 | 2,858,512 B | 15.1／961.5／15.2 ≈ 992 ms | 約 1 | **放棄** |
| 同上 | yolo11n_s8_v1 | 640×640 | 2,855,792 B | 51.5／3906.3／48.0 ≈ 4 s | 0.25 | 放棄 |
| `yolo26` 0.1.1 | yolo26n_512_s8_s3 | 512×512 | 3,073,040 B | 34.6／2348.2／23.5 ≈ 2.4 s；README 寫需 16 MB PSRAM | 0.4 | 放棄（我們只有 8 MB） |
| `coco_pose` 0.4.2 | yolo11n_pose_s8_v1 | 640×640 | 3,117,600 B | 51.3／4422.9／7.4 ≈ 4.5 s | 0.2 | 放棄 |
| `person_reid` 0.1.1 | osn_s8_v1 | 192×96 | 371,152 B | 3.7／102.5／0.1 | — | 不需要（行人重識別，教學用途少） |
| `motion_detect` 0.1.2 | 傳統（非 NN） | — | — | 640×360 stride 2 約 15 ms | — | 只回傳「動的點數」，沒有框；自己做（ID 12） |
| `color_detect` 0.1.1 | 傳統（OpenCV-mobile） | — | — | — | — | 不需要（已有自製顏色偵測，ADR-0009） |

PSRAM 用量 README 都沒寫。以人臉偵測實測推估：191 KB 模型載入後佔約 425 KB PSRAM（參數副本＋中間張量），所以 espdet_pico 類約 1 MB、MFN 約 2 MB 等級；**一次只載入一個功能的模型**時，8 MB PSRAM 綽綽有餘。正確數字要在實作時量。

**人臉辨識的細節**

- 官方的 `HumanFaceRecognizer` 用 `dl::recognition::DataBase`，它用 `fopen` 把特徵存在檔案系統路徑上（需要 SPIFFS／FATFS），格式也不在我們控制之下。建議**只用 `HumanFaceFeat` 取特徵**，比對（cosine similarity、門檻、top-1）自己用 C 寫在可主機測試的純邏輯檔，跟 `mxcam_face_post.c` 一樣。
- 特徵是 float 向量（長度由 `get_feat_len()` 取得，ArcFace 類模型常見 512，實作時確認）。以 fp16 或 int8 存：每個樣本 0.5–1 KB；例如 10 個 ID × 5 個樣本 ≈ 25–50 KB，現在的 24 KB NVS 放不下 → 需要新的 NVS 分割區（第 7 節）。
- 偵測模型與功能 3 共用（已在開機載入），只多一個 MFN 特徵模型。
- 速度：偵測約 50 ms＋每張臉 256 ms。每張畫面只對最大的 1–2 張臉取特徵，其他臉回 `id = 0`；也可以用 IoU 把前一張的身分沿用到下一張、輪流重新辨識，讓多張臉時的 fps 不掉太多。

### 4.2 傳統演算法（自己寫 C，MIT，可主機端單元測試）

| 功能 | 演算法 | 預估耗時（320×240，未實測） | 學習 | ResultRecord 對應 |
|---|---|---|---|---|
| 線條追蹤（4） | RGB565 → 灰階（可在下半部 ROI、隔列取樣）；門檻用 Otsu 或學到的線色；把 ROI 切成 N 條橫帶，每帶取線像素的質心；最小平方法擬合直線；連續性檢查（質心跳太遠的帶丟掉）；可選：一帶出現兩段以上視為岔路 | 3–6 ms | 選用：LEARN 區域學線的顏色（沿用顏色功能的 HSV 學習） | `id` = 1（主線；有岔路時岔線 2、3…）；`x, y` = 線段中點；`w, h` = 線段外接框；`confidence` = 有找到線的橫帶比例 ×100；**extra 12 B**：`angle_cdeg i16`（0 = 垂直向上，畫面上順時針為正）、`offset_x i16`（底端 x − 160，給 PID 用）、`x0, y0`（底端）、`x1, y1`（頂端） |
| 物體追蹤（5） | LEARN 框選 → 存灰階範本（縮小到 ≤ 32×32）＋HSV 直方圖；每張在上次位置附近的搜尋窗做 NCC 比對（半解析度、0.9／1.0／1.1 三個尺度）；分數夠高時慢慢更新範本；跟丟時用直方圖反投影在全畫面找回 | 10–20 ms | 是（一次一個目標） | `id` = 1；框＝追到的位置；`confidence` = NCC ×100；跟丟時沒有紀錄；extra 0 |
| 幾何形狀（11） | 二值化（自適應門檻或學到的顏色）→ 連通區域 → 輪廓 → 多邊形近似（Douglas–Peucker）→ 依頂點數、圓度分類 | 5–15 ms | 否 | `id` = 形狀種類（1 圓、2 三角、3 四邊形、4 五邊形以上…）；`confidence` = 擬合程度；extra 0 |
| 動態偵測（12） | 縮到 80×60 灰階，與背景（指數平均）差分，門檻後做連通區域，框放大回 320×240 | < 5 ms | 否 | `id` = 0；框 = 移動區域；`confidence` = 區域內變化像素比例；依面積排序 |
| 自學習分類（10） | 在固定的中央框（或 LEARN 給的框）算特徵：HSV 直方圖（例如 8×4×4）＋梯度方向直方圖（4×4 格 × 8 方向），L2 正規化；Studio 對同一類拍多張樣本；kNN（cosine）分類 | < 10 ms | 是 | 一筆：`id` = 類別 ID（0 = 不確定）；框 = 分類用的區域；`confidence` = 相似度 ×100 |

自學習分類的品質風險最高：手工特徵在背景單純、物體顏色或形狀差異大時好用，換背景就容易錯。Espressif 沒有給 S3 用的通用特徵模型（ImageNet MobileNetV2 只輸出 1000 類分數，要取倒數第二層得自己用 ESP-PPQ 重新匯出，而且 3.6 MB、約 440 ms）。所以第一版用手工特徵，教學上當作「讓電腦學會分辨幾樣東西」的入門，效果不夠再評估 NN 特徵。

### 4.3 QR Code 與條碼

| 方案 | 授權 | 狀態 | 結論 |
|---|---|---|---|
| `espressif/quirc` 1.2.0~1（QR） | **ISC**（Daniel Beer），與 MIT 相容 | IDF Component Registry 上維護中；esp-who 目前的 `who_qrcode` 元件也是用它 | **採用** |
| `esp-code-scanner`（QR＋Code39＋Code128） | ESPRESSIF MIT（限 Espressif 晶片） | 只在舊的 esp-who `release/v1.1.0` 有，**只有預編譯的 `libesp-code-scanner.a`**（S3 版約 1.1 MB）＋`libnewlib_iconv.a`，對應 IDF 4.x 時代；API 只回傳類型和內容字串，**沒有位置**；Component Registry 上查無此元件 | 不採用：閉源、與 IDF 5.5 相容性未知、無法畫框 |
| zbar（條碼＋QR） | **LGPL-2.1** | 成熟 | 不採用：靜態連結到 MIT 韌體並發佈二進位檔，要讓使用者能重新連結（提供目的檔等），負擔大 |
| 自寫 1D 條碼解碼（EAN-13／Code128 掃描線） | MIT | 要自己寫 | 延後；真的需要條碼時再做 |

quirc 的速度：Espressif `qrcode-demo`（ESP32-S3、240×240 灰階）記錄**沒有 QR 時約 22 ms、有 QR 時約 229 ms**，其中解碼本身約 3 ms，其餘是辨識定位圖案與取樣。320×240 預估沒有 QR 時約 30 ms、有 QR 時 100–300 ms（4–10 fps），需實測；有 QR 時偏慢可以接受（QR 卡片通常是靜止的）。quirc 需要約 77 KB 灰階緩衝區＋工作記憶體，放 PSRAM 即可。

QR 的 ResultRecord：`id` = 學過的 ID（LEARN 一張 QR，就把「內容→ID」存起來，跟 HUSKYLENS 一樣），沒學過填 0；框由四個角算出；`confidence` = 255（不適用）；**extra 18 B 沿用 AprilTag 的格式**（`rotation_cdeg` ＋四個角），R4 端可以共用解析程式。內容字串放不進 18 bytes，需要新指令讀取（第 7 節）。

## 5. 多模型共存：flash 與 RAM

### 5.1 Flash 預算

| 組合 | 模型檔合計 | 放進 app（剩約 1.87 MiB）？ |
|---|---|---|
| 人體＋手部＋手勢＋人臉特徵（ID 7–9） | 435,328＋497,184＋787,296＋1,295,168 = 3,014,976 B ≈ 2.88 MiB | **放不下** |
| 再加貓、狗 | 約 3.83 MiB | 放不下 |
| 只加人體偵測 | 0.42 MiB | 放得下，但下一個就不行 |

esp-dl 每個模型元件都支援三種位置：flash rodata、flash 分割區（`CONFIG_*_MODEL_IN_FLASH_PARTITION`）、SD 卡。分割區模式下每個元件要求**固定名稱**的分割區：`pedestrian_det`、`hand_det`、`hand_gesture_cls`、`human_face_feat`（貓狗為 `cat_det`、`dog_det`）。`storage` 有約 11.9 MiB 空間，切出這些分割區綽綽有餘。

建議分割表（從現在的 `storage` 切出來，nvs／phy／factory 的位置不變）：

```
# Name,           Type, SubType, Offset,  Size
nvs,              data, nvs,     ,        24K
phy_init,         data, phy,     ,        4K
factory,          app,  factory, ,        4M
pedestrian_det,   data, spiffs,  ,        512K
hand_det,         data, spiffs,  ,        512K
hand_gesture_cls, data, spiffs,  ,        1M
human_face_feat,  data, spiffs,  ,        1536K
mxdata,           data, nvs,     ,        256K     # 人臉嵌入、QR 內容、自學習特徵
storage,          data, spiffs,  ,        （剩下的空間，保留）
```

（subtype 照 esp-dl 範例 `examples/hand_gesture_recognition/partitions2.csv` 用 `spiffs`，內容其實是原始模型檔，不是 SPIFFS 檔案系統；esp-dl 的人臉辨識範例另外用一個 FAT `storage` 分割區放特徵資料庫，我們改用 `mxdata` NVS。）人臉偵測模型繼續放 rodata，不動 ADR-0011 已驗證的部分。

後果：
- 模型和韌體分開燒錄。`idf.py flash` 會由各元件的 CMake 一起寫入模型分割區；**M5 的網頁燒錄要把這些分割區映像也一起燒**，而且版本要和韌體對得上（建議 GET_INFO 或 log 帶模型版本，或開機時檢查模型分割區內容；載入失敗就不在 GET_INFO 列出該功能，與 ADR-0011 的做法一致）。
- 改分割表等於要整片重燒；目前還沒有發佈，現在改成本最低。

### 5.2 RAM：全部開機載入 vs 切換功能時載入

| 做法 | PSRAM | 內部 RAM | 開機時間 | 風險 |
|---|---|---|---|---|
| A. 開機全部載入（沿用 ADR-0011 的 malloc 導向） | 所有模型加總，估 4–6 MB，加上畫面緩衝區會很緊 | 不增加（載入期間導到 PSRAM） | 每個模型數十到數百 ms，合計可能超過 1 s | PSRAM 不夠時連人臉都載不了；浪費 |
| B. 切換功能時載入、離開時卸載，**不**導 malloc | 只有目前功能的模型 | 每個載入中的模型約多 15 KB（人臉實測 15.5 KB），卸載時歸還 | 不變 | 內部 RAM 從約 105 KB 再降約 15 KB，M4 加 UART／I2C 後要重算；反覆載入卸載可能造成內部 RAM 碎片 |
| C. 切換功能時載入，**只把 C++ 的 `operator new` 導到 PSRAM** | 同 B | 幾乎不增加 | 不變 | 要確認專案裡沒有其他 C++ 程式碼（IDF 元件大多是 C）依賴 `new` 回傳內部 RAM；esp-dl 若有用一般 `malloc` 的小物件仍會進內部 RAM，要實測 |

ADR-0011 的限制是「全域切換 `heap_caps_malloc_extmem_enable` 只能在其他 task 啟動前」，所以 **B／C 都不能在執行期間用那個全域切換**。建議：

1. 人臉偵測照舊開機載入（功能 3 和 9 共用）。
2. 其他模型走 C（不行就退回 B），在 core 1 的處理流程 task 裡載入：SET_FUNCTION 時先卸載舊模型再載入新模型，SET_FUNCTION 的回應延到載入完成（沿用 ADR-0007 的延後回應），主機逾時 ≥ 1 秒。
3. 載入前檢查 PSRAM 最大區塊與內部可用量（沿用 `mxf_init` 的防護），載入失敗回 `INTERNAL`、功能維持原本的，並在 TFT 顯示原因。
4. 寫新的 ADR（例如 ADR-0012「模型放獨立分割區、切換功能時載入」），補充修改 ADR-0011。

載入時間估計：模型參數從 flash 映射後複製到 PSRAM（`param_copy = true`，預設；設 false 會直接從 flash 跑、省 PSRAM 但變慢），0.5–1.3 MB 估 50–200 ms，需實測。

## 6. 建議順序與功能 ID

功能 ID 照實作順序遞增（ID 只是編號，不代表重要性；跳過的編號保留）。

| 階段 | ID | 功能 | 為什麼放這裡 | 前置工作 |
|---|---|---|---|---|
| A：傳統，不動架構 | 4 | 線條追蹤 | R4 循跡車最常見的教材；工作量小；完全不碰 esp-dl | Config 的參數擴充方式（第 7 節） |
| | 5 | 物體追蹤 | HUSKYLENS 招牌功能；直接沿用現有 LEARN（區域）語意 | — |
| | 6 | QR Code | 路標卡、任務卡；quirc 授權乾淨 | 新增讀取內容字串的指令 |
| B：NN 基礎建設 | — | 模型分割區＋按需載入（ADR-0012） | 功能 7 以後都需要 | 改分割表、loader、M5 網頁燒錄要燒模型分割區 |
| | 7 | 人體偵測 | 單一模型最簡單，用來驗證 loader | B 的基礎建設 |
| | 8 | 手勢辨識 | 兩個模型串接，互動性高 | 同上 |
| | 9 | 人臉辨識 | 最受歡迎但最複雜（嵌入儲存、多樣本學習、約 3 fps） | 同上＋`mxdata` NVS 分割區＋LEARN 擴充 |
| C：補完 | 10 | 自學習分類 | 品質風險高，放在後面 | Studio 的多樣本擷取介面 |
| | 11 | 幾何形狀 | SPEC 列為第二版候選；教學展示用 | — |
| | 12 | 動態偵測 | 簡單，但對機器人用途有限 | — |

**延後**（有需要再評估）

- 條碼：沒有可直接用、授權乾淨、會給位置的函式庫；自寫 EAN-13／Code128 是可行的備案。
- 貓狗偵測：速度可以（約 7 fps）、每個約 0.5 MB，但對 R4 教學用途小；loader 做好之後加入的成本很低（2 天），可當作「寵物偵測」一次載入兩個模型（約 260 ms，約 3.8 fps）或二選一。
- ImageNet 分類：約 2 fps、3.6 MB；1000 類大部分對教室環境沒意義。

**放棄**：COCO 80 類偵測（約 1 s／張，低於放寬後的 3 fps）、YOLO26（README 要 16 MB PSRAM）、姿態（4.5 s）、實例分割、OCR（只支援 P4）、車牌、情緒、視線、臉部朝向、跌倒偵測——ESP32-S3 沒有可用模型，或速度差一個數量級。

## 7. 需要的架構與協定變更

1. **Arduino 函式庫／共用 C 協定的上限**（M4 發佈前就要改，之後改會破壞相容）：
   - `INFO_MAX_FUNCS` 從 8 調到 32（GET_INFO 列出 9 個以上功能時，舊函式庫會整包解析失敗）。
   - `MXP_RESULT_EXTRA_MAX = 18` 保留；所有新功能的 extra 設計都 ≤ 18 bytes（本文的設計都符合）。
2. **功能專屬參數**：Config v1 是固定 18 bytes 的結構。建議新增一對指令 `GET_FUNC_CONFIG／SET_FUNC_CONFIG (function_id, 版本, 固定版面的參數)`，每個功能自己的參數各自版本化，避免每加一個功能就改全域 Config。例如線條追蹤：線色模式（自動深線／自動淺線／學到的顏色）、ROI 高度、最小線寬；物體追蹤：門檻；QR／人臉辨識：門檻；手勢：最多幾隻手。
3. **讀取結果附帶資料**：QR 內容（最多數百 bytes）放不進 extra。新增 `GET_RESULT_DATA (index)`，回傳最近一批結果中第 `index` 筆的附帶資料（UTF-8，建議上限 128 bytes，R4 記憶體有限）。
4. **LEARN 擴充**：
   - 物體追蹤、QR、自學習分類、人臉辨識都用現有「區域」語意。
   - 人臉辨識、自學習分類需要「把樣本加到已有的 ID」：在 LEARN payload 後面加選用的 `id (u16)`（payload 長度 9 = 新 ID、11 = 加到指定 ID），向後相容。
   - 名字表目前全部功能合計 32 筆，功能變多後可能要加大。
5. **學習資料儲存**：新增 `mxdata` NVS 分割區（256 KB）放人臉嵌入（fp16）、QR 內容→ID 對照、自學習特徵，沿用「LEARN 改記憶體、SAVE 才寫 flash」的語意；`FACTORY_RESET` 也要清它。
6. **模型分割區＋按需載入**：第 5 節；新 ADR；M5 網頁燒錄要包含模型分割區映像。
7. **SET_FUNCTION 延後回應**：載入模型時回應會晚 50–200 ms（估計），文件要寫主機逾時 ≥ 1 秒；載入失敗回 `INTERNAL`，功能不變。
8. **灰階共用**：線條、QR、物體追蹤、形狀、動態都需要灰階影像，把 RGB565 → 灰階做成共用函式（必要時只轉需要的區域或降採樣）。

## 8. 需要實測的項目與風險

- 所有傳統演算法的耗時都是估計；先在功能 4 的實作裡量灰階轉換＋掃描的實際時間。
- esp-dl 延遲是官方單獨量測；在 MXCam 裡（JPEG 在 core 0 搶 PSRAM）預期再慢 10–20%。手勢（兩隻手約 3.6 fps）和人臉辨識（約 3 fps）會落在放寬後的 3 fps 邊緣，要寫進 SPEC。
- 每個模型的 PSRAM 用量、按需載入的時間、做法 C（`operator new` 導向）實際省下多少內部 RAM，都要在 loader 實作時量。
- 內部 RAM：M4 加入 UART／I2C 之後，再加按需載入的模型，要重新確認最低剩餘 ≥ 60 KB。
- quirc 在 320×240 有 QR 時的耗時，以及小 QR（< 60 px）的辨識率。
- 手工特徵自學習分類的準確度（換背景、換光線）——做之前先做小規模原型比較。

## 參考資料

- esp-dl 模型清單與各模型 README（延遲、輸入、模型位置設定）：https://github.com/espressif/esp-dl/tree/master/models
  - `coco_detect`、`yolo26`、`pedestrian_detect`、`hand_detect`、`hand_gesture_recognition`、`human_face_recognition`、`cat_detect`、`dog_detect`、`imagenet_cls`、`coco_pose`、`person_reid`、`motion_detect` 的 `README.md`
  - 手勢類別：`esp-dl/vision/classification/hand_gesture_category_name.hpp`（one、two、three、four、five、like、ok、no_gesture、call、dislike、no_hand）
  - 特徵資料庫：`esp-dl/vision/recognition/dl_recognition_database.{hpp,cpp}`（`fopen` 檔案儲存）；`models/feat_database`
  - `dl::Model` 的 `param_copy` 說明：`esp-dl/dl/model/include/dl_model_base.hpp`
- ESP Component Registry（版本、授權）：https://components.espressif.com （`espressif/human_face_recognition`、`pedestrian_detect`、`hand_gesture_recognition`、`motion_detect`、`color_detect`、`quirc`、`esp-dl`）
- quirc 授權（ISC）：https://components-file.espressif.com/components/espressif/quirc/1.2.0~1/license.txt
- Espressif QR demo（ESP32-S3、240×240、22 ms／229 ms）：https://github.com/espressif/qrcode-demo
- esp-who：`components/who_qrcode`（用 quirc）、`examples/object_tracking`（ESP32-P4、ByteTrack）：https://github.com/espressif/esp-who ；舊版 `esp-code-scanner`（預編譯 .a）：`release/v1.1.0` 分支 `components/esp-code-scanner`
- HUSKYLENS 2 功能說明：https://wiki.dfrobot.com/sen0638/ （Function Guide；物體追蹤 docs/22614、線條追蹤 docs/22623）
- HUSKYLENS 2 Arduino 函式庫（`eAlgorithm_t`、結果欄位）：https://github.com/DFRobot/DFRobot_HuskylensV2
- 專案內：`docs/SPEC.md`、`docs/protocol.md` §7、`docs/adr/0007`、`docs/adr/0011`、`docs/measurements/m2.md`、`docs/measurements/m3.md`、`arduino-lib/src/protocol/mxcam_proto.{h,c}`
