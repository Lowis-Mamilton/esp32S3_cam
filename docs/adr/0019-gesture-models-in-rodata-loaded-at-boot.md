# 手勢模型放在 flash rodata、開機時載入，每張畫面同步辨識

手勢辨識（功能 8，M8a）使用 Espressif `hand_detect` 0.2.1（espdet_pico 224×224，497,184 B）找手，再用 `hand_gesture_recognition` 0.2.1 的 `HandGestureCls`（MobileNetV2 0.5，128×128，787,296 B）對每隻手的裁切分類（esp-dl 3.3.15）。不用上游的 `HandGestureRecognizer`：它的分類器是第一張畫面才載入（在 malloc 導向時段之外）、會對最多 10 個框全部分類（時間無上限）、沒有手時每張都印 warning。

決定：

1. **模型位置**：兩個模型放 app 分割區的 flash rodata，不改分割表。實測映像 3,526,000 B（M3 後約 2.23 MB，多 1.29 MB），4 MB factory 分割區剩 668,304 B（16%），低於計畫上限 3,774,873 B（4 MiB 的 90%）。
2. **載入時機**：`mxh_init` 在 `app_main` 裡緊接在 `mxf_init` 之後、建立任何應用程式 task 之前呼叫，兩個 C++ 物件都用 `lazy_load = false`，並同樣用 `heap_caps_malloc_extmem_enable(0)` 把載入期間的一般 malloc 導到 PSRAM（ADR-0011 的做法）。實測（M8a Task 1）：載入 332 ms、PSRAM 用 1,827,532 B、內部 RAM 0 B；開機完成時 PSRAM 剩 4,115,260 B（≥ 2 MiB）、內部 RAM 最低 104,836 B（≥ 60 KB）。`MXH_MIN_PSRAM_FREE` = 3 MiB（實測用量＋1 MiB = 2.74 MiB，取整），`MXH_MIN_INTERNAL_FREE` = 64 KB。失敗時功能 8 不列在 GET_INFO，其他功能照常。
3. **每張畫面同步辨識**：功能 8 在 core 1 的處理流程裡每張畫面都跑 `mxh_detect`（1 次偵測＋最多 `max_hands`（1–2）次分類），TFT 跟著辨識速度更新；RESULTS 與 FRAME 的 `frame_id` 保持一致。速度不夠（一隻手平均 > 300 ms）時的退路是 `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`（影響全部功能，要重量），不改 64 KB 資料快取。每張時間在 M8a Task 7 與使用者實測後補進 `docs/measurements/m8a.md`。
4. **heap guard 跳過不是「0 隻手」**：`mxh_detect` 在未初始化或記憶體不足（內部 < 32 KB、PSRAM 最大區塊 < 256 KB）時回 `MXH_SKIPPED`，處理流程不發布那張畫面的結果。

## Considered Options

- **模型分割區（`hand_det`／`hand_gesture_cls`）**：要改分割表，M5 網頁燒錄要多燒兩個映像；目前 app 映像放得下，沒有必要。
- **切換功能時才載入**：要讓 SET_FUNCTION 延後回應，而且那時其他 task 已在跑，不能用全域的 malloc 導向（會多吃內部 RAM 或要先驗證 `operator new` 導向）；PSRAM 目前用不完。
- **獨立 NN task 非同步辨識**：框會落後畫面、`frame_id` 對不上，還要多一份畫面複製；串流時 core 0 忙 JPEG，沒有空閒核心。
- **64 KB 資料快取**：上游 README 的延遲是在 64 KB／64 B 快取線下量的，但要多 32 KB 內部 RAM，會讓最低剩餘逼近 60 KB 底線。

## Consequences

- `mxh_init` 和 `mxf_init` 一樣只能在 `app_main` 啟動 task 之前呼叫，之後調整啟動順序時不能往後挪。
- factory 分割區只剩約 0.65 MB：**加入第三個 NN 模型（功能 7 人體 435 KB、功能 9 人臉特徵 1.3 MB）時一定放不下**，屆時把手勢模型連同新模型一起搬到模型分割區（只改 Kconfig 位置選項與分割表）並改成按需載入；PSRAM 也會不夠（目前開機後剩約 3.9 MiB）。
- 實機基準用的測試影像（`-DMXCAM_GESTURE_BENCH_IMAGE`）每次最多 4 張 320×240 RGB565 畫面（每張 150 KB），否則映像超過 4 MB 分割區。
