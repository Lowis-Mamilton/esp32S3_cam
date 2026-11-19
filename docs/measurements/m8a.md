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
