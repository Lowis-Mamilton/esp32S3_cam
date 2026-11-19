# M8a 實機量測（手勢辨識，功能 8）

## Task 1 基準

日期：2026-11-19 ・ 韌體 `91055c4` ＋ M8a Task 1（dirty 建置，即本 commit 的內容）・ ESP-IDF v5.5.6 ・ OV3660 ・ CPU 240 MHz、32 KB 資料快取／32 B 快取線
模型：`espressif/hand_detect` 0.2.1（`espdet_pico_224_224_hand.espdl`，S3 版 497,184 B）、`espressif/hand_gesture_recognition` 0.2.1（`mobilenetv2_0_5_128_128_gesture.espdl`，787,296 B）・ esp-dl 3.3.15 ・ 兩者放 app 的 flash rodata

| 項目 | 結果 | 規則 |
|---|---|---|
| 韌體映像（預設建置，`idf.py build`） | **3,526,000 B**（0x35cd70） | ≤ 3,774,873 B ✔ |
| factory 4 MB 分割區剩餘 | 668,304 B（16%） | |
| 靜態內部記憶體（`idf.py size`） | DIRAM 143,403 B 已用（.bss 34,696） | |
| `mxh_init`（開機載入兩個模型） | **332 ms** | ≤ 1 s ✔ |
| 載入用的內部 RAM | 0 B（228,487 → 228,487；malloc 導向 PSRAM） | |
| 載入用的 PSRAM | **1,827,532 B**（6,367,780 → 4,540,248） | |
| 開機完成時 PSRAM 剩餘 | **4,115,260 B**（約 3.9 MiB） | ≥ 2 MiB ✔ |
| 開機完成時內部 RAM | 剩 136,867 B；之後 1 Hz log `heap=145,931 min=104,836` | 最低 ≥ 60 KB ✔ |
| 處理流程 stack 高水位 | 22,256 B 未用（功能 NONE，手勢還沒接進流程） | |
| `mxf_init`（同一次開機） | 74 ms | 與 M3 相同 |

方法：預設建置燒錄（COM5），從 TTL 埠（這次 CH340 是 COM7）讀開機 log：`gesture ready in 332 ms (internal 228487 -> 228487, PSRAM 6367780 -> 4540248)`、`boot done: internal heap free 136867, PSRAM free 4115260`；之後即時畫面 11 fps 跑了約 10 秒，沒有當機或重開機。

**這次沒有量每張辨識時間、手的偵測分數**（主對話指示 Task 1 只做最小檢查，延遲與門檻改在 Task 7 跟使用者實測）。`gesture_bench`（`idf.py -DMXCAM_GESTURE_BENCH=ON build`，可加 `-DMXCAM_GESTURE_BENCH_IMAGE=<檔>`，每次最多 4 張 320×240 RGB565BE 畫面）已寫好、可以編譯，但還沒在板子上跑過。

**`hand_threshold` 預設值：40（暫定，沿用計畫的初始值）。** 計畫的規則是「真手的偵測分數中位數 < 0.55 → 30」，但 Task 1 沒有量到真手分數；**Task 2 先用 40，Task 7 實測後若中位數 < 0.55 要回頭改預設**（注意：protocol-vectors.json 的既有項目不能改，若要改預設只影響韌體／Studio 的預設值，不影響編碼測試資料；Task 2 寫測試資料時請避免把「預設值」本身寫成測試向量）。

## Task 4：`mxh_detect` 第一次在板子上跑（gesture_bench）

日期：2026-11-19 ・ 韌體 `76fa1d7`＋Task 4 前的工作目錄（`idf.py -DMXCAM_GESTURE_BENCH=ON -DMXCAM_GESTURE_BENCH_IMAGE=batch1.rgb565be build`，量完改回 OFF）・ 其他條件同 Task 1。
bench 每秒對即時畫面＋4 張嵌入的測試畫面各跑一次 `mxh_detect(frame, 10, 0, 2, …)`（功能 NONE，跑了約 25 秒，每張 15–20 次）。測試畫面是 Task 1 用 esp-dl 範例圖做的 320×240 RGB565BE（`gesture.jpg` 的人比 1；`gestures.png` 的手勢兩兩並排在灰底上）。

| 畫面 | 手 | 偵測結果（id、手勢分數、手的分數 hand_conf） | 預期 | total（平均／最大） | detect | classify |
|---|---|---|---|---|---|---|
| live（鏡頭前沒有手） | 0 | 0 筆（有一次門檻 10 下抓到 12 分的假手，預設門檻 40 不會出現） | 0 | **174 ms**／179 | 174 | 0 |
| img0 `gesture.jpg`（人比 1） | 1 | ONE 100、hand 82 | ONE | **260 ms**／264 | 170 | 89–94 |
| img1 one＋two | 2 | TWO 100（hand 88）、ONE 100（hand 82） | ONE、TWO | **349 ms**／349 | 170 | 178 |
| img2 three＋four | 2 | FOUR 100（hand 92）、THREE 100（hand 82） | THREE、FOUR | **349 ms**／349 | 170 | 178 |
| img3 five＋like | 2 | FIVE 100（hand 88）、LIKE 100（hand 82） | FIVE、LIKE | **349 ms**／349 | 170 | 178 |

- **裁切／`run_crop`／RGB565BE 路徑正確**：7 隻手的手勢全對，框的中心落在對的那一格（例：img1 的 TWO 在 x=235、ONE 在 x=81；圖上 one 在左、two 在右）。
- 時間：偵測一次約 **170–179 ms**（README 64 KB 快取下 128 ms），每隻手分類約 **89 ms**（README 74 ms）。一隻手平均 260 ms **< 300 ms，不用退路**（不改快取線）。預期純辨識速度：沒有手約 5.7 fps、一隻手約 3.8 fps、兩隻手約 2.9 fps（比計畫估的 6–7／4–5／3–3.5 慢一點）。
- 記憶體：呼叫中內部 RAM 最低 142,859 B（呼叫前 144,555，最多用約 1.7 KB）；全域最低 `min` 103,972 B；PSRAM 剩 3,481,064 B（含 4 張測試畫面 614 KB），最大區塊 3,407,872 B；處理流程 stack 高水位 21,536 B 未用。heap guard（32 KB／256 KB）維持不變，沒有被觸發。
- **手的偵測分數（暫定 `hand_threshold` 用）**：7 隻手 82、88、82、92、82、88、82，**中位數 82（0.82 ≥ 0.55）→ 依規則維持 40**。但這是乾淨背景、光線均勻的範例圖，真手的分數可能較低；**Task 7 跟使用者用真手再量一次才定案**。

## Task 4：功能 8 接進處理流程後的冒煙測試

韌體：Task 4 的預設建置（bench OFF）・ 映像 **3,530,048 B**（0x35dd40，factory 剩 16%）≤ 3,774,873 ✔ ・ 指令從 OTG（COM5）用 pyserial 送，log 從 TTL（這台電腦是 COM7）。鏡頭前**沒有手**（只量到 0 隻手的即時速度；有手的速度用上面的 bench 推算）。

| 項目 | 結果 |
|---|---|
| 開機 | `gesture ready in 339 ms`、`gesture config: hand_threshold 40, gesture_threshold 60, max_hands 1`、`boot done: internal heap free 136523, PSRAM free 4115260` |
| GET_INFO 功能清單 | `[1, 2, 3, 8]` ✔ |
| GET_FUNC_CONFIG 8 | `08 01 28 3C 01 00…`（40, 60, 1）✔ |
| SET_FUNC_CONFIG 8 (5, 200, 9) | 回應夾限成 (10, 100, 2) ✔；(50, 70, 2) 照收 ✔；長度 9 → `BAD_LENGTH`、版本 2 → `BAD_PARAM`、GET_FUNC_CONFIG 3 → `BAD_PARAM` ✔ |
| SET_FUNCTION 8 → GET_RESULTS | function_id 8、0 筆 ✔；1 Hz log `fn=GESTURE` |
| SAVE → 重開機（esptool hard reset）→ GET_FUNC_CONFIG 8 | 仍是 (50, 70, 2) ✔；開機 log 也印 50/70/2 |
| FACTORY_RESET → GET_FUNC_CONFIG 8 | 回到 (40, 60, 1) ✔ |
| 功能 3→8→2→8→1→8 切換 30 次（每次停 1 秒） | 每次 SET_FUNCTION 回應正確、GET_RESULTS 的 function_id 都是新功能；`heap` 145,467（第一次切換後到最後都不變）、`min` 104,444（不變，≥ 60 KB ✔）、`psram` 第一次用人臉／AprilTag 時各少 5.4 KB／1 KB（第一次配置），之後 4,103,240 不變 → 沒有漏記憶體 ✔；`stale` 0；人臉 46–47 ms、各功能都有 fps |
| heap guard 跳過（`skip`） | 0（整段測試都沒觸發） |

### 只收結果（暫停預覽，STREAM_START flags 0x02）會不會比較快？

功能 8、沒有手、各 20 秒、交替兩輪（PING 每秒一次），`max_fps` 10：

| 模式 | 每張辨識（`gest_ms` 平均／最大） | RESULTS 推送 | FRAME 推送 | JPEG |
|---|---|---|---|---|
| 不串流（只有 TFT） | 173–174／174 ms | — | — | — |
| flags 0x03（畫面＋結果） | **181**／182 ms | **4.49／s**（兩輪 4.49、4.50） | 4.49 fps（每張都送） | 38–39 ms（core 0）、約 5.4 KB |
| flags 0x02（只有結果） | **174**／174 ms | **4.78／s**（兩輪 4.75、4.80） | 0 | 0 |

**結論：只快約 6%（4.5 → 4.8 次／秒）。** JPEG 在另一顆核心（core 0）壓，不佔辨識的 CPU；每張多花約 14 ms：辨識本身慢 7 ms（約 4%，推測是兩顆核心同時讀寫 PSRAM 的頻寬競爭），其餘是 core 1 每張先把 150 KB 畫面複製給編碼器（`encoder_try_submit`）。所以暫停預覽的主要好處是省 USB 頻寬與電腦端的解碼，不是讓辨識變快。有手時辨識每張 260–349 ms，比例應該差不多（差幾 ms），Task 7 用真手確認。

速度的組成（0 隻手）：辨識 174 ms＋每張其他工作約 35 ms（等下一張畫面、畫框、送 TFT）→ 約 209 ms／張 ＝ 4.8 fps。依 bench 推算：一隻手約 1000 ÷ (260 + 35) ≈ **3.4 fps**、兩隻手約 1000 ÷ (349 + 35) ≈ **2.6 fps**（串流時再慢約 4%），比計畫預估（4–5／3–3.5）慢；一隻手平均 < 300 ms，依計畫**不啟用退路**（64 B 快取線）。是否要為了速度改用退路，留給 Task 7 跟使用者看實際手感再決定。
