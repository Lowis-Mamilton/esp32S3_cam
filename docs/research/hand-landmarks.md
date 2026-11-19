# 研究：MXCam 能不能像 MediaPipe Hands 一樣標出手的 21 個關鍵點

> 日期：2026-11-19 ・ 狀態：研究筆記（建議，尚未定案；定案時寫進 `docs/SPEC.md`／新 ADR）
> 範圍：ESP32-S3 N16R8（240 MHz、8 MB octal PSRAM 80 MHz、**32 KB** D-cache）、ESP-IDF v5.5.6、esp-dl 3.3.15，沿用現有功能 8（手勢辨識，ADR-0019）的 `hand_detect` 偵測器。
> 數字標「實測」的是 MXCam 量的（`docs/measurements/m8a.md`）；標「自算」的是本研究直接解析官方 `.tflite` 檔算出的；其他是官方文件數字或推估（推估一律寫範圍）。

## 1. 結論

**建議：先做原型（prototype first），通過門檻才排進路線圖。**

| 項目 | 結論 |
|---|---|
| 做得出來嗎 | **可以**，但沒有現成的 ESP32 移植，要自己轉模型。esp-dl 模型庫只有 `hand_detect`（只給框）和 `hand_gesture_recognition`（分類器），**沒有關鍵點模型**；GitHub／論壇也找不到任何在 ESP32 上跑 21 點手部關鍵點的專案（找到的「ESP32 + MediaPipe」專案都是電腦跑 MediaPipe、ESP32 只當輸出）。 |
| 用哪個模型 | **MediaPipe `hand_landmark_lite`**（224×224、Apache-2.0）：自算 **1.01 M 參數、145.6 M MACs**，運算子全部是 esp-dl 支援的簡單算子（Conv／DepthwiseConv／Add／MaxPool／Mean／FC／Sigmoid）。 |
| 預估速度（一隻手） | 關鍵點推論本身約 **200–290 ms**（推估，含 MXCam 實測的變慢係數）。整條流程：每張都先偵測 → 約 **2.0–2.4 fps**；MediaPipe 式追蹤（手還在就不跑偵測器）→ 約 **3.0–4.0 fps**，和現在的手勢辨識（實測 3.4 fps）差不多。兩隻手約 1.4–1.9 fps。 |
| 和現在的手勢分類器比 | 關鍵點＋規則判斷手勢：每隻手 ~250 ms 換到 21 個點、可自訂／**可學習**手勢（像 HUSKYLENS 2）；分類器每隻手 89 ms 但只有固定 9 類。追蹤模式下兩者每張時間差不多。 |
| Flash／RAM | `.espdl` 約 1.0–1.1 MB，**放不進 app 分割區**（映像 3.53 MB，SPEC 上限 3,774,873 B，就算拿掉分類器的 787 KB 也會超過），必須先做 SPEC 已規劃的「模型分割區＋按需載入」。PSRAM 估多用 2.3–2.9 MB（權重複製＋中間層），開機後剩 ~4.1 MB，放得下但不寬裕。 |
| 工作量 | 原型 4–6 天；通過後完整功能再 8–12 天（不含模型分割區基礎建設）。 |
| 最大風險 | ① ESP32-S3 上 esp-dl 只有 **per-tensor、2 的次方 scale** 的 int8 量化，迴歸型關鍵點模型掉精度的風險高；② 我們的偵測框是**沒有旋轉**的整手框，MediaPipe 的關鍵點模型是用「轉正的手掌框放大 2.6 倍」的裁切訓練的；③ 速度只有 2–4 fps，追蹤在這個幀率下容易跟丟。 |
| 不建議 | MediaPipe full（331 M MACs，~1.3 fps）、YOLO11n-pose 手部版（AGPL-3.0＋資料集 CC BY-NC-SA，且單模型 ~1 s／張）、RTMPose（最小的也 0.36 GFLOPs 且是身體模型、含 GAU 注意力、手部只有 m 版）、TFLite Micro（比 esp-dl 慢 3–4 倍）。 |

## 2. 現有移植與參考實作

| 來源 | 內容 | 對我們的意義 |
|---|---|---|
| esp-dl `models/`（master） | 18 個模型：`hand_detect`、`hand_gesture_recognition`、`coco_pose`（YOLO11n-pose，**人體 17 點**）……沒有手部關鍵點 | 要自己轉 |
| esp-dl issues | 搜「keypoint／landmark」只有人臉相關（#261 臉部 22 點 pipeline 等），沒有手部 | 官方沒有在做的跡象 |
| ST `x-cube-n6-ai-hand-landmarks`、HF `STMicroelectronics/hand_landmarks` | MediaPipe 手掌偵測＋關鍵點兩段式；`handlandmarks_full_224_int8.tflite`（TFLite per-channel int8，來源 PINTO_model_zoo 033），STM32N6 **NPU** 上 20.75 ms，權重 3,283 KiB，Apache-2.0 | 證明 MediaPipe 關鍵點模型 **可以 int8 量化**後在 MCU 級晶片跑；但 N6 有 NPU，速度不可比 |
| OpenCV Zoo `handpose_estimation_mediapipe` | MediaPipe 關鍵點轉成 ONNX（tf2onnx＋onnx-simplifier），Apache-2.0；README 警告「int8 量化版可能因精度大幅下降而產生無效結果」 | ONNX 可直接拿來試；**int8 掉精度是真實風險** |
| Luxonis model zoo `mediapipe-hand-landmarker` | 224×224，輸出 `[1,63]`、`[1,1]`、`[1,1]`、`[1,63]`；RVC4 INT8 720 次／秒 | 再次確認輸入輸出格式 |
| `mrivero-sec/posture_computervision` | ESP32-S3 上用 esp-dl 跑 YOLO11n-pose 640，**約 4.4 s／張** | 「單模型偵測＋關鍵點」在 S3 上太慢 |
| `DenisZheng/esp32s3-gesture-recognition` | ESPDet-Pico＋HaGRID 自訓 10 類手勢偵測（ESP-PPQ W8A8） | 只有框＋類別，沒有關鍵點 |

## 3. 候選模型

| 模型 | 輸入 | 參數 | MACs | 授權 | int8 可用性 | 評估 |
|---|---|---|---|---|---|---|
| **MediaPipe hand_landmark_lite**（2021-10） | 224×224×3 | **1,011,714**（自算） | **145.6 M**（Conv 123.9、DW 21.6、FC 0.09；自算） | Apache-2.0（model card） | 官方是 fp16 權重 `.tflite`（2.07 MB）；要自己量化 | **首選** |
| MediaPipe hand_landmark_full | 224×224×3 | 2,715,682（自算） | 330.97 M（自算） | Apache-2.0 | 同上；ST 有 per-channel int8 版 | 太慢（~2.3 倍 lite） |
| （論文 2020 版本）Light／Full／Heavy | 256×256 | 1.0／1.98／4.02 M | — | — | — | 論文 Table 3：MSE 11.83／10.05／9.817，Pixel 3 上 6.6／16.1／36.9 ms；lite→full 精度只好約 15% |
| MediaPipe palm_detection_lite（對照） | 192×192 | 960,122（自算） | 283.2 M（自算），含 PReLU、Resize | Apache-2.0 | — | 比現有 `hand_detect` 重，**不換**偵測器 |
| YOLO11n-pose 訓練在 Ultralytics hand-keypoints | 自訂（224 時約 0.47 G MACs，推估） | ~2.9 M | 7.6 GFLOPs @640 | 程式 AGPL-3.0；資料集 **CC BY-NC-SA 4.0**（非商用）；官方**沒有**預訓練手部權重 | 要自己訓練 | 授權不適合產品、要訓練、仍需 ~0.7 s 以上 |
| RTMPose（MMPose） | 256×256（hand5 只有 m 版） | — | 身體版 t／s／m：0.36／0.68／1.93 GFLOPs @256×192 | Apache-2.0 | 要自己轉 | m 版 ~2 GFLOPs 太重；含 GAU（自注意力），轉 esp-dl 風險高 |

MediaPipe lite 的運算子（自算，去掉 fp16 權重用的 102 個 DEQUANTIZE）：`CONV_2D` ×31、`DEPTHWISE_CONV_2D` ×16、`ADD` ×10、`MAX_POOL_2D` ×1、`MEAN` ×1、`FULLY_CONNECTED` ×4、`LOGISTIC` ×2。最大的中間張量 802,816 個元素（112×112×64，int8 = 784 KB）。輸出：21 點螢幕座標（x、y 以 224 像素為單位、z 相對深度）、手存在分數、左右手、21 點世界座標（可裁掉）。

## 4. 轉換路徑

### 4.1 esp-dl（建議）

1. **TFLite → ONNX**：`tf2onnx`（`--inputs-as-nchw`，fp16 權重會被摺疊），再 onnx-simplifier；或直接用 OpenCV Zoo 的 ONNX 當對照。可把 `[1,63]` 世界座標頭裁掉。
2. **檢查運算子**：esp-dl `operator_support_state.md` 中 Conv（groups 只能 1 或 = 輸入通道，depthwise 符合）、Add、MaxPool、GlobalAveragePool／ReduceMean、Gemm、Sigmoid、Relu、Clip、PRelu、Reshape 在 int8／int16 都 ✔。**沒有看到不支援的算子。**
3. **ESP-PPQ 量化**（`espdl_quantize_onnx`）：ESP32-S3 是**對稱、2 的次方 scale、per-tensor**（per-channel 只有 P4／S31 的 Conv／Gemm 有）。可混精度：w8a8／w8a16（Conv、Gemm、MatMul，esp-dl ≥ 3.3.11）／w16a16，用 `QuantizationSetting.dispatching_table` 指定層。建議第一層 Conv 與最後的 FC 頭用 w8a16，其餘 int8；必要時試 TQT／等化。QAT 不實際（MediaPipe 沒公開訓練程式與資料）。
4. **校正資料**：300–1000 張手部裁切，優先用 MXCam OV3660 自己拍的畫面（照我們要用的裁切方式切），可補 HaGRID 等公開資料（只做校正、不散布）。
5. **驗證**：PC 上用 float `.tflite` 的輸出當基準，比較量化後每點誤差（以裁切邊長正規化）；再上機用 esp-dl 的 test 流程確認與 PC 模擬一致。
6. **前後處理**：裁切＋縮放（之後需要旋轉時用 esp-dl 的仿射變換，人臉辨識對齊就是這樣做），輸出座標反算回 QVGA。

主要風險在第 3 步。輸出座標以 224 為單位，int8 per-tensor 2 的次方 scale 若取 2，步距就是 2 px（約 1% 裁切邊長），本身可接受；真正怕的是 31 層 Conv 一路累積的 per-tensor 誤差（TFLite 的 int8 是 per-channel affine，ST 用的就是這種；OpenCV Zoo 的 int8 版已有精度大降的警告）。

### 4.2 TFLite Micro＋esp-nn（不建議，當精度對照可以）

- 好處：可以直接吃 TFLite 的 per-channel int8（例如 ST 的 full int8，或自己用 TFLite converter 量化 lite），精度風險較低。
- 壞處：**慢**。esp-nn README（S3，PSRAM tensor arena、64 KB cache）：MobileNetV3-Small 224 729 ms（≈ 0.08 G MAC/s）、YOLO11n 192 1253 ms（≈ 0.23 G MAC/s）；同類模型 esp-dl 約 0.7–0.85 G MAC/s，**快 3–4 倍**。lite 用 TFLM 估 600 ms 以上。還要多一套 runtime 的程式碼（app 分割區已經很滿）。

## 5. 速度估計

### 5.1 esp-dl 在 S3 的實際吞吐（官方 README，model 時間，64 KB cache）

| 模型 | MACs | model 時間 | 吞吐 |
|---|---|---|---|
| MobileNetV2 1.0，224 | 300 M | 437.9 ms | 0.69 G MAC/s |
| YOLO11n，320 | ~0.81 G | 961.5 ms | 0.85 G MAC/s |
| YOLO11n，640 | ~3.25 G | 3906.3 ms | 0.83 G MAC/s |
| YOLO11n-pose，640 | ~3.8 G | 4371.2 ms | 0.87 G MAC/s |
| MobileNetV2 0.5，128（我們的手勢分類器） | ~31.7 M | 69.2 ms | 0.46 G MAC/s（小模型，固定開銷比例高） |

MXCam 實測比 README 慢：`hand_detect` 128→170 ms（×1.33）、分類器 74→89 ms（×1.2）。原因之一是我們的 D-cache 是 32 KB（`CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000`），README 是 64 KB；另外 core 0 的 JPEG 會搶 PSRAM 頻寬（約 4%）。

### 5.2 hand_landmark_lite 推估

- 只算模型（README 條件）：145.6 M ÷ 0.69–0.85 ≈ **171–213 ms**（悲觀用 0.46 → 317 ms）。
- 乘 MXCam 係數 1.2–1.35，加上裁切／後處理約 5 ms：**約 205–290 ms，取 ~250 ms**。
- 輸入改 192×192（107 M MACs，不必重訓也能跑，因為最後是 global mean＋FC，但精度未知）：約 150–210 ms。
- full 版：331 M → 約 470–650 ms。

### 5.3 整條流程（一張畫面另加 ~35 ms 等畫面／畫框／送 TFT，實測）

| 流程 | 一隻手 | 兩隻手 |
|---|---|---|
| 現在：偵測 170＋分類 89／手（實測） | 260 ms → **3.4 fps** | 349 ms → 2.6 fps |
| A：每張偵測＋lite 關鍵點 | 170＋250＋35 ≈ 455 ms → **2.0–2.4 fps** | ≈ 705 ms → ~1.4 fps |
| B：MediaPipe 式追蹤（用上一張的 21 點算下一張裁切框；存在分數低才重跑偵測） | 250＋35 ≈ 285 ms → **3.0–4.0 fps**（跟丟那張 +170 ms） | ≈ 535 ms → ~1.9 fps |
| A＋分類器都跑 | ~545 ms → ~1.8 fps | — | 
| full 版＋A | ~1.2–1.5 fps | — |

SPEC 的 NN 目標是 ≥ 3 fps：**只有 B（追蹤）能達到**，A 不行。B 的問題是 3–4 fps 之下手一動就可能跑出裁切框（MediaPipe 是設計給 30 fps 的），需要把裁切框放大一點並靠存在分數快速回到偵測。

可再試的加速：D-cache 改 64 KB（會吃 32 KB 內部 RAM，內部 RAM 最低剩約 104 KB、底線 60 KB，要量）；192 輸入；不開預覽只快約 6%（M8a 實測），幫助不大；esp-dl 雙核模式在 README 只快 11%，而且 core 0 被 JPEG 佔用。

## 6. Flash 與記憶體

| 項目 | 數字 | 結論 |
|---|---|---|
| lite `.espdl` 大小 | 估 1.0–1.1 MB（1.01 M int8 權重＋偏置；混精度層會再大一些） | — |
| app 分割區 | 映像 3,531,136 B／4 MB（剩 668 KB）；SPEC 上限 3,774,873 B | **放不進 rodata**。拿掉分類器（787,296 B）再加 lite 仍約 3.79 MB，超過上限 |
| `storage` 分割區 | 0xBF0000（約 11.9 MiB），目前沒用 | 切一個 `hand_lmk`（1.5 MB）綽綽有餘；`hand_det`／`hand_gesture_cls` 也可一起搬 |
| PSRAM | 開機完成剩約 4.1 MB（實測 4,115,260 B，含兩個手勢模型 1.74 MiB） | lite：權重複製到 PSRAM ~1.05 MB（`param_copy` 預設 true）＋中間層估 1.2–1.8 MB → 多用 **2.3–2.9 MB**。三個手部模型同時常駐剩約 1.2–1.8 MB；按需載入或拿掉分類器就寬裕。`param_copy=false` 可省 ~1 MB 但會變慢（未量） |
| 內部 RAM | esp-dl 載入時把 malloc 導到 PSRAM 就不吃內部 RAM（M8a 實測 0） | 照 ADR-0019 的做法 |

這等於把 SPEC 寫好的「加入第三個 NN 模型時改成模型分割區＋按需載入」提前觸發；那部分工作量見 `docs/research/vision-functions.md` 第 5 節，不算在本功能內。

## 7. 用關鍵點＋規則取代手勢分類器

MediaPipe 的做法也是先出 21 點，再判斷手勢。對我們現有的 9 種手勢：

| 手勢 | 規則（以手掌大小＝手腕到中指 MCP 距離做正規化） |
|---|---|
| 1–5 | 每根手指「伸直」＝指尖到手腕距離 > PIP 到手腕距離（或 PIP 關節角 > ~160°）；拇指另用指尖到食指 MCP 的距離判斷；依伸直的手指組合對應 1–5 |
| 讚／倒讚 | 只有拇指伸直，看拇指方向（指尖在 MCP 上方或下方） |
| OK | 拇指尖與食指尖距離 < ~0.3 手掌大小，中、無名、小指伸直 |
| 打電話 | 拇指＋小指伸直，其他彎曲 |

| 比較 | 分類器（現在） | 關鍵點＋規則 |
|---|---|---|
| 每隻手時間 | 89 ms（實測） | ~250 ms（推估）＋規則 < 1 ms |
| 資訊 | 1 個類別＋分數 | 21 點、左右手、手指伸直狀態、手勢 |
| 擴充 | 固定 9 類，要重訓 | 改規則即可；**可學習手勢**：把 21 點正規化（平移到手腕、縮放、轉正）成 42 維向量，kNN 比對，每個樣本 42 bytes 存 NVS——這就是 HUSKYLENS 2「手部辨識：學手勢」的做法 |
| 準確度 | 官方訓練，對側面／遮擋較穩 | 正面直立的手很準；側面、手指互相遮擋時規則容易錯，要實測 |
| Flash／PSRAM | 787 KB／~1 MB | 1.0–1.1 MB／2.3–2.9 MB |

**省不了空間**（lite 比分類器大），**也不一定省時間**（每隻手多 ~160 ms；但追蹤模式省掉偵測器後，總時間和現在差不多）。所以建議：關鍵點版通過原型後，把功能 8 升級成「手部辨識」（關鍵點＋規則手勢＋可學習手勢），**分類器先保留**當備案，等規則版用真手驗收到不輸分類器再拿掉。

## 8. 協定與其他影響

- **ResultRecord 的 extra 上限 18 bytes**（`MXP_RESULT_EXTRA_MAX`），21 點至少要 42 bytes（u8 相對座標）——塞不進。建議：既有紀錄照舊（框、手勢 ID、信心度），extra 只放手指伸直位元遮罩、左右手、食指尖座標等少數欄位；完整 21 點另開指令（例如 `GET_KEYPOINTS`，依 ADR-0016 的協定擴充方式），Arduino 函式庫加 `getKeypoint(hand, idx)`。要拷問後寫 ADR。
- 左右手：辨識用真實影像、顯示才鏡像（ADR-0010），左右手的定義要寫清楚。
- Studio 與 TFT 都要畫骨架（21 點＋連線）。

## 9. 原型計畫與門檻

| 步驟 | 內容 | 工作量 |
|---|---|---|
| P1（PC） | float `.tflite` 跑 MXCam 拍的手（用 `hand_detect` 的框做**不旋轉**的正方形裁切，邊長 ×1.3–1.5），看點準不準；同時試追蹤用的旋轉裁切 | 1–2 天 |
| P2（PC） | tf2onnx → ESP-PPQ，int8 與混精度各一版，比較量化誤差 | 2 天 |
| P3（實機） | 模型放 `storage` 分割區、量推論時間與 PSRAM | 1–2 天 |

**通過門檻（全部要過才做完整功能）：**

1. 實機 lite 推論 ≤ 300 ms（一隻手）；追蹤模式整體 ≥ 3 fps。
2. 量化後與 float 的平均點誤差 ≤ 3% 裁切邊長（224 上約 7 px）。
3. 不旋轉裁切下，直立的手 9 種手勢的規則判斷正確率不低於現在的分類器（用 M8a 的測試畫面＋真手）。
4. 載入後 PSRAM 剩 ≥ 1 MB。

**通過後的完整功能**：模型分割區按需載入（另計）、追蹤狀態機、規則手勢、可學習手勢（kNN＋NVS）、協定擴充、Studio／TFT 骨架、Arduino API 與範例，約 **8–12 天**。

**沒過時的退路**：量化不行 → 試 w16a16 全層（速度約慢 1.5–2 倍，可能掉到 ~1.5 fps）或改 TFLM 跑 per-channel int8（~0.5 s／手）只當「慢速關鍵點模式」；速度不行 → 192 輸入或只在 R4 要求時才算關鍵點；都不行 → 維持現在的分類器，關鍵點放棄。

## 10. 來源

- esp-dl 模型目錄：https://github.com/espressif/esp-dl/tree/master/models
- esp-dl README 延遲：`models/coco_pose/README.md`、`models/coco_detect/README.md`、`models/imagenet_cls/README.md`、`models/hand_detect/README.md`（github.com/espressif/esp-dl）
- esp-dl 量化說明（per-tensor／2 的次方、w8a16、dispatching_table）：https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_quantize_model.html
- esp-dl 運算子支援表：https://github.com/espressif/esp-dl/blob/master/operator_support_state.md
- esp-dl issues（landmark／keypoint 搜尋）：https://github.com/espressif/esp-dl/issues
- MediaPipe Hands 論文（Table 3）：https://arxiv.org/abs/2006.10214
- MediaPipe Hand Tracking (Lite/Full) model card（輸入 224×224、Apache-2.0）：https://storage.googleapis.com/mediapipe-assets/Model%20Card%20Hand%20Tracking%20(Lite_Full)%20with%20Fairness%20Oct%202021.pdf
- 模型檔（本研究自算參數與 MACs 用）：`https://storage.googleapis.com/mediapipe-assets/hand_landmark_lite.tflite`、`hand_landmark_full.tflite`、`palm_detection_lite.tflite`
- ST hand_landmarks：https://huggingface.co/STMicroelectronics/hand_landmarks ；x-cube-n6-ai-hand-landmarks
- OpenCV Zoo：https://github.com/opencv/opencv_zoo/tree/main/models/handpose_estimation_mediapipe
- Luxonis：https://models.luxonis.com/luxonis/mediapipe-hand-landmarker
- Qualcomm AI Hub（HandLandmarkDetector 2.01 M 參數，256 版）：https://huggingface.co/qualcomm/MediaPipe-Hand-Detection
- Ultralytics hand-keypoints 資料集（CC BY-NC-SA 4.0）：https://docs.ultralytics.com/datasets/pose/hand-keypoints/
- RTMPose 論文：https://arxiv.org/abs/2303.07399
- esp-tflite-micro／esp-nn 效能：https://github.com/espressif/esp-tflite-micro 、https://github.com/espressif/esp-nn
- ESP32-S3 YOLO11n-pose 實例：https://github.com/mrivero-sec/posture_computervision
- MXCam 實測：`docs/measurements/m8a.md`、`docs/SPEC.md`、`firmware/sdkconfig`、`firmware/partitions.csv`
