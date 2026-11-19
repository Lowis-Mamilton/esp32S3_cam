# 原型 B：手部 21 點關鍵點模型上機實測（ESP32-S3）

> 日期：2026-11-19 ・ 狀態：原型結果（建議**不做**，見第 1 節；若定案寫進 `docs/SPEC.md`／ADR）
> 前兩份：`docs/research/hand-landmarks.md`（研究、4 個門檻）、`docs/research/hand-landmarks-proto-a.md`（Phase A：電腦端轉換與量化）。
> 這一份是 **Phase B**：Phase A 匯出的 `.espdl` 放進 `storage` 分割區，在實板（ESP32-S3 N16R8、OV3660、64 B 資料快取線、韌體 `83bc892`＋bench）上量速度、記憶體與數值正確性。
> 數字全部是實測，除非標「推估」。

## 1. 結論

| 項目 | 結果 |
|---|---|
| **門檻 1**（一隻手推論 ≤ 300 ms） | **不通過，差 3.5 倍**。精度合格的版本每次推論 **1,055–1,099 ms**（`int8+dw16+heads16` 1,057、`int8+dw16` 1,093、全 `int16` 1,072）；最少 int16 層的 `int8+auto8` 也要 **740 ms**；連精度不合格的全 int8 都要 **502 ms**。研究估的 200–290 ms 太樂觀。 |
| 追蹤模式整體 fps | 推估一隻手 **約 0.8 fps**（`int8+dw16+heads16`），兩隻手約 0.4 fps；第一次抓到手要 2.3–4.6 秒。目標 ≥ 3 fps，差很遠。 |
| **門檻 4**（人臉＋手勢模型都載入後 PSRAM 剩 ≥ 1 MB） | **不通過**。`int8+dw16+heads16` 要 **4.50 MB PSRAM**，其中變數區（中間層）要一塊**連續 3.06 MiB**；人臉＋手勢載入後只剩 4.11 MB、最大區塊 3.08 MB → 根本載不進去。改成開機第一個載入：關鍵點＋人臉剩 1.44 MB，但手勢模型就放不下了（需要 1.83 MB）。 |
| 數值正確性（`model->test()`） | `int8+dw16`：**PASS**（實機輸出與 ESP-PPQ 模擬逐位元相同）。有 int16 FC 頭／第一層的版本 **FAIL**（容差是 1 個 LSB）：`int8+dw16+heads16` 實機與模擬平均差 0.78 px、最大 2.3 px（224 為單位，＜ 1.1 % 裁切邊長）；`int8+auto8` 差 1 個 int8 步距（2 px）；全 int16 差 2.0 px。都比門檻 2（3 % ≈ 6.7 px）小很多，**精度不是問題**。 |
| 規則手勢（門檻 3，實機版） | 8 張測試畫面、10 隻有標籤的手（`hand_detect` 實機框 → 軸對齊 ×1.3 → presence < 0.9 試 ±90° → 轉正）：`int8+dw16+heads16`、`int8+dw16` **10/10**（取得那張與追蹤那張都對）；全 int16 追蹤 10/10；`int8+auto8` 追蹤 10/10（取得那張 9/10）；全 int8 **1–2/10**。和 Phase A 的電腦結果一致。 |
| 為什麼慢 | 逐層 profile：int16 的 depthwise 很貴（`int8+dw16+heads16` 的 17 層 int16 Conv 佔 522 ms／49 %）、int8↔int16 轉換（42 個 RequantizeLinear）**253 ms／24 %**、ReLU6（32 個獨立的 Clip，沒有併進 Conv）**124 ms**；連全 int8 的 Conv 也只有 145.6 M MACs ÷ 365 ms ≈ 0.40 G MAC/s，而且第一層（3→24 通道、224→112）一層就 104 ms。 |
| **建議** | **No-go：不做「手部關鍵點」功能，也不把手勢升級成關鍵點版**；功能 8 維持現在的分類器（ADR-0019，一隻手 5.5 次／秒）。模型分割區＋按需載入、`GET_KEYPOINTS` 都**不需要為這個功能提前做**。理由與重新評估的條件見第 7 節。 |

## 2. 方法

### 2.1 bench（不進預設建置）

- `firmware/main/handlm_bench.cpp`（＋`.h`）：只有 `idf.py -DMXCAM_HANDLM_BENCH=ON build` 才編進去（`main/CMakeLists.txt` 最後一段；`main.c`／`pipeline.c` 只有 `#ifdef` 掛勾）。預設建置與 CI 完全不受影響。
- 模型**不放 app 映像**：`tools/handlm/pack_storage.py` 把各版 `.espdl`、一張固定的輸入裁切、8 張測試畫面打包成一個映像，用 esptool 寫到 `storage` 分割區（0x410000，`partitions.csv` 標「reserved for model files」，韌體沒有用到）。每個項目 64 KiB 對齊、最後 64 KiB 是目錄。`.espdl` 不進 repo。
- 第一個模型放在 offset 0，用 esp-dl 正式的 `MODEL_LOCATION_IN_FLASH_PARTITION`（`dl::Model("storage", ...)`）載入；其他版本用 `esp_partition_mmap` 映射自己那一段，再當「rodata」載入。兩種載入結果相同（`param_copy` 都把參數複製到 PSRAM）。
- 每張處理流程的畫面只做一步（載入／`test()`／一次推論／profile／一隻手的完整流程），所以處理流程（TFT、core 0 的 JPEG）一直在跑，量到的是「在韌體裡」的速度。功能停在 NONE（開機時 NVS 的功能），沒有切換。
- 每個模型：載入（時間、PSRAM／內部 RAM 用量、`get_memory_info`）→ 有測試資料的跑 `model->test()` → 暖機 3 次＋**計時 30 次**（固定輸入：Phase A 的 eval crop 0＝grid_one 轉正裁切，就是 `.espdl` 內嵌的測試輸入）→ 記錄輸出 → 第一輪做逐層 profile → 完整流程（下一段）→ 卸載。
- 完整流程（`track_hand`）：`hand_detect` 實機框 → 以框中心、長邊 ×1.3 的軸對齊正方形裁切（自己寫的雙線性仿射，RGB565BE → 量化輸入）→ 推論 → presence < 0.9 再試 +90°／−90°，取 presence 最高的 → MediaPipe 轉正 ROI → 推論（「取得」那張，Phase A 的 `adaptive+r`）→ 用它的 21 點再算 ROI → 推論（「追蹤」那張＝之後每張的成本）。21 點記在 log，電腦用 `tools/handlm/gesture_rules.py` 判手勢（`tools/handlm/device_report.py`）。
- 測試畫面：M8a 用的 batch1＋batch2（esp-dl 範例圖做的 320×240，one／one＋two／three＋four／five＋like／ok＋call／dislike／桌上的手／邊緣＋很小的手），框是 `hand_detect`（hand_threshold 40）在實機上的結果。

### 2.2 量的版本（Phase A 的輸出）

| storage | 版本 | `.espdl` | 說明 |
|---|---|---|---|
| #0 | `int8+dw16+heads16`（notest） | 1,348,288 B | Phase A 的主版本 |
| #1 | 同上（含測試資料） | 1,649,920 B | `test()` 用 |
| #2／#3 | `int8+dw16`（notest／含測試資料） | 1,299,728／1,450,800 B | 備案 |
| #4 | `int8+auto8`（含測試資料） | 1,493,104 B | 備案（只有 8 層 int16） |
| #5 | `int8`（notest） | 1,149,056 B | 速度下限參考（精度不合格） |
| #6 | `int16`（notest） | 2,161,520 B | 精度上限 |

### 2.3 跑了哪幾次

| 次 | 建置 | 內容 |
|---|---|---|
| 1 | bench，開機在人臉＋手勢之後載入 #0；之後的載入**沒有** malloc 導向 | #0／#1 載不進去；量到「沒有導向」時的內部 RAM 問題（第 4 節）就換掉 |
| 2 | 同上，全部載入都用 malloc→PSRAM 導向；bench 開始時先算好 8 張畫面的框，再 `mxh_deinit()` 釋放手勢模型 | #2、#3、#5、#6 各兩輪：第 0 輪不串流、第 1 輪 **0x03 串流**（`STREAM_START` flags 0x03 ＋每秒 PING，330 秒）；#0、#1、#4 因為 PSRAM 沒有夠大的連續區塊仍然載不進去 |
| 3 | `-DMXCAM_HANDLM_BENCH_EARLY=ON`：開機時**在人臉、手勢之前**先載入 #0 | #0 完整量測；之後再輪一次 #2、#3、#5、#6（數字和第 2 次相同） |
| 4、5 | `_EARLY`＋`-DMXCAM_HANDLM_BENCH_ONLY=1`／`=4` | #1 的 `test()`、#4 的完整量測 |

## 3. 結果

### 3.1 推論時間（固定輸入，30 次；不串流）

| 版本 | 平均 | 最小／最大 | 串流（0x03）時 | int16 層的 MACs 佔比（Phase A） |
|---|---|---|---|---|
| int8（精度不合格） | **502 ms** | 501.6／502.7 | 508 ms | 0 % |
| `int8+auto8` | **740 ms** | 738.9／740.9 | — | 15 % |
| `int8+dw16+heads16` | **1,057 ms** | 1,055.6／1,058.2 | — | 20 % |
| `int8+dw16` | **1,093 ms** | 1,091／1,097 | 1,097–1,099 ms | 15 % |
| int16 | **1,072 ms** | 1,071／1,074 | 1,075 ms | 100 % |

- 30 次之間幾乎沒有變化（±3 ms），暖機只慢 2–7 ms。含測試資料的版本速度相同（#3 1,093 ms、#1 1,055 ms）。
- 串流只慢 0.4–1.6 %（處理流程每秒才 1 張，JPEG 編碼量很少）。
- 推論期間內部 RAM 最低 142.4 KB（推論本身只用約 0.6 KB 內部 RAM）；處理流程 stack 高水位剩 20 KB。

### 3.2 時間花在哪（esp-dl `get_module_info`，每層計時加總）

| 版本 | int16 Conv | int8 Conv | RequantizeLinear | Clip（ReLU6） | 其他 | 合計 |
|---|---|---|---|---|---|---|
| int8 | — | 365 ms（47 層） | 1 ms（2） | 124 ms（32） | 12 ms | 503 ms |
| `int8+auto8` | 229 ms（8） | 195 ms（39） | 182 ms（17） | 124 ms | 12 ms | 741 ms |
| `int8+dw16+heads16` | 522 ms（17） | 145 ms（30） | **253 ms（42）** | 124 ms | 13 ms | 1,057 ms |
| `int8+dw16` | 476 ms（16） | 249 ms（31） | 232 ms（36） | 124 ms | 12 ms | 1,093 ms |
| int16 | 852 ms（47） | — | 3 ms（3） | 196 ms | 23 ms | 1,074 ms |

- **int16 depthwise 是主因**：最慢的三層都是 112→56 附近的 depthwise（各 56–63 ms）。整個模型 int16 的 Conv（852 ms）約是 int8（365 ms）的 2.3 倍。
- **int8↔int16 轉換很貴**：混精度版要 17–42 個 RequantizeLinear，加起來 182–253 ms，大到讓 `int8+dw16`（1,093）比全 int16（1,072）還慢。
- **ReLU6 沒有併進 Conv**：32 個 Clip 各自掃一遍 112×112／56×56 的張量，int8 也要 124 ms（佔全 int8 的 25 %）。
- **第一層很慢**：輸入只有 3 通道的 3×3 Conv（224→112，24 通道）int8 一層 104 ms；int16 版（heads16）反而只要 47 ms。
- 全 int8 的 Conv 吞吐約 0.40 G MAC/s，研究用的 esp-dl README 數字（0.69–0.85）是較大、通道數較整齊的模型。

### 3.3 記憶體

開機（人臉＋手勢都載入、預設建置）完成時：PSRAM 剩 **4,110,312 B**，最大連續區塊 **3,080,192 B**；內部 RAM 剩 134 KB（最低 102 KB）。

| 版本 | 參數（複製到 PSRAM） | 變數區（中間層，一塊連續） | 其他 | **PSRAM 合計** | 載入時間 |
|---|---|---|---|---|---|
| int8 | 1,005,696 | 1,053,696 | 85,108 | **2,144,500** | 214 ms |
| `int8+dw16` | 1,134,448 | 2,659,328 | 106,120 | **3,899,896** | 286 ms |
| `int8+auto8` | 1,038,768 | 3,211,264 | 93,368 | **4,343,400** | 276 ms |
| `int8+dw16+heads16` | 1,179,312 | **3,211,264** | 109,780 | **4,500,356** | 321 ms（從分割區，開機） |
| int16 | 2,017,472 | 2,107,392 | 85,764 | **4,210,628** | 311 ms |

- 變數區是 esp-dl greedy 記憶體管理器的**一整塊**：混精度版有 112×112 的 int16 張量和它的 int8 副本同時存在，所以比全 int16 還大。
- **在人臉＋手勢之後載入**（第 1、2 次）：`int8+dw16+heads16`／`auto8` 都失敗（`Failed to alloc 3136.00KB PSRAM, largest available PSRAM block size 3008.00KB`，esp-dl 有錯誤處理、沒有 abort）。就算 `mxh_deinit()` 釋放手勢模型（PSRAM 5.94 MB 空），最大區塊仍只有 3.08–3.21 MB（被人臉模型、鏡頭與編碼器的配置切開），還是載不進去。`int8+dw16` 載得進去，但載入後 PSRAM 只剩 **0.21 MB**（4.11 − 3.90）。
- **開機第一個載入**（第 3 次）：`int8+dw16+heads16` 載入 321 ms、PSRAM 6.79 → 2.29 MB；接著人臉載入成功，**手勢模型失敗**（`mxh_init` 需要 3 MiB），開機完成 PSRAM 剩 **1,442,068 B**。
- 按需載入的情境（推估）：只留 `hand_detect`（約 0.5–1.0 MB）＋關鍵點 4.5 MB，從開機前的 6.79 MB 扣掉編碼器等約 0.43 MB → 剩約 0.9–1.4 MB，**勉強**；而且 3.06 MiB 的連續區塊要在開機時先保留，否則碎片化後就配不到。

### 3.4 數值正確性

| 版本 | `model->test()` | 實機 vs ESP-PPQ 模擬（eval crop 0，21 點平均／最大，px／224） | 實機 vs float | presence（實機／模擬／float） |
|---|---|---|---|---|
| `int8+dw16+heads16` | FAIL（第一個不一樣的值：landmarks[0] 14,223 vs 14,060，int16 exp −7 → 1.27 px） | 0.78／2.32 | 2.49／4.96 | 0.984／0.984／0.996 |
| `int8+dw16` | **PASS**（1,136 ms） | 0.000／0.000 | 3.16／7.92 | 0.969／0.969／0.996 |
| `int8+auto8` | FAIL（landmarks[4]：81 vs 80，差 1 個 int8 步距＝2 px） | 0.57／2.00 | 3.69／8.79 | 0.992／0.992／0.996 |
| int8 | （notest） | 0.000／0.000 | 14.5／28.5 | 0.977／0.977／0.996 |
| int16 | （notest） | 2.01／4.57 | 2.01／4.57 | 0.993／0.996／0.996 |

- 全 int8 的部分（`int8+dw16`、`int8`）實機和 ESP-PPQ 模擬**逐位元一樣**。
- 有 int16 Gemm／第一層 Conv 的版本和模擬差一點（esp-dl 的 int16 捨入和 PPQ 模擬不同；全 int16 的模擬和 float 幾乎一樣，實機卻差 2 px）。`test()` 的容差是 1 個 LSB，所以判 FAIL；但差距都 ≤ 4.6 px（2 % 裁切邊長），對手勢判斷沒有影響（第 3.5 節）。

### 3.5 完整流程（8 張測試畫面、13 個框，其中 10 隻有標籤）

| 版本 | 取得那張：推論次數平均 | 取得耗時（2 次／4 次推論） | 追蹤那張（推論＋裁切） | 規則手勢 取得／追蹤 |
|---|---|---|---|---|
| int8 | 3.08 | 1,186／2,373 ms | 593 ms（501＋91） | 2/10／1/10 |
| `int8+auto8` | 2.77 | 1,659／3,321 ms | 829 ms（737＋91） | 9/10／**10/10** |
| `int8+dw16+heads16` | 2.92 | 2,292／4,586 ms | 1,145 ms（1,054＋90） | **10/10**／**10/10** |
| `int8+dw16` | 2.92 | 2,365／4,732 ms | 1,182 ms（1,090＋91） | **10/10**／**10/10** |
| int16 | 2.62 | 2,315／4,633 ms | 1,157 ms（1,066＋90） | 9/10／**10/10** |

- 側著的手（讚、打電話、倒讚）軸對齊裁切的 presence 掉到 0.008–0.74，±90° 重試後 0.98–0.99，和 Phase A 一樣。13 個框裡 4–7 個要 4 次推論（主版本 6 個）。
- 沒有標籤的：桌上的手（框 30×20、17×19，很小）其中一個 presence 0.008–0.04（正確地判成「沒有手」），另一個 0.3–0.93、手勢不穩；邊緣被切掉的手（只露出 90 px）判成 five。
- **裁切（仿射＋量化）91 ms** 是 bench 自己寫的逐像素浮點版本，沒有最佳化；改成定點／esp-dl 的影像前處理應該可以到 10–20 ms（推估），但對結論沒有影響。
- `hand_detect` 實機 132–133 ms（8 張）。

## 4. 其他觀察（將來做模型分割區時要記得）

1. **載入一定要在 malloc→PSRAM 導向裡**（ADR-0011 的做法）：第 1 次執行沒有導向時，每次載入永久多吃 **50–64 KB 內部 RAM**（esp-dl 的物件），載入過程中內部 RAM 一度只剩 **16–71 B**（處理流程統計的 `min=16`）；有導向時只用 80–256 B。但開機後才導向是全域的，其他 task 那 0.2–0.3 秒內的配置也會跑到 PSRAM——「按需載入」要先解決這件事（ADR-0019 的 Considered Options 也提過）。
2. **esp-dl 的 `MODEL_LOCATION_IN_FLASH_PARTITION` 會 mmap 整個分割區**（`storage` 是 11.9 MB，約 191 個 64 KB MMU 頁），模型多大都一樣。正式做的話要切一個剛好大小的分割區（例如 1.5 MB 的 `hand_lmk`），或自己 `esp_partition_mmap` 需要的那段再當 rodata 載入（bench 的 #1–#6 就是這樣，結果相同）。
3. **大模型的變數區要連續**：greedy 記憶體管理器一次要一整塊（這裡 1.0–3.06 MiB）；開機後 PSRAM 會被鏡頭、人臉、編碼器切成幾塊，3 MB 以上的區塊要開機時先載入或先保留。
4. esp-dl 在 `Input cap=0x400 can not callocate with MALLOC_CAP_SIMD` 後會改用其他配置方式，不影響結果。
5. 一次處理流程步驟裡做 4 次推論（約 4.6 秒）會讓 core 1 的 IDLE 任務超過 5 秒沒跑，出現 task watchdog **警告**（沒有設 panic，不會重開機）。正式做要拆步驟或另開 task。
6. bench 的逐層 profile（`get_module_info`，map＋很長的層名字串）一次用掉約 108 KB 內部 RAM（全域最低剩 34.6 KB），只有 bench 才有，不代表推論本身。

## 5. 推估：追蹤模式的整體速度

假設：每張「處理流程其他工作」約 45 ms（M8a：送 TFT 29 ms＋等鏡頭／畫框）；追蹤時每 3 張至少偵測一次（`hand_detect` 133 ms → 每張攤 45 ms）；裁切用 bench 的 91 ms（括號裡是最佳化到約 15 ms 的推估）。

| 版本 | 一隻手每張 | **一隻手 fps** | 兩隻手 fps | 第一次抓到手（偵測＋2–4 次） |
|---|---|---|---|---|
| int8（精度不合格） | 683 ms（607） | 1.5（1.6） | 0.8 | 1.3–2.5 s |
| `int8+auto8` | 921 ms（845） | 1.1（1.2） | 0.6 | 1.8–3.5 s |
| `int8+dw16+heads16` | 1,238 ms（1,162） | **0.8（0.9）** | 0.4 | 2.4–4.7 s |
| int16 | 1,253 ms（1,177） | 0.8（0.85） | 0.4 | 2.4–4.8 s |

研究的目標是追蹤模式 ≥ 3 fps；現在的手勢分類器一隻手 5.5 次／秒。另外 MediaPipe 的追蹤是設計給 30 fps 的，在 1 fps 下手稍微一動就會跑出上一張算出的 ROI，實際上會常常回到「取得」（2–4 次推論），速度還會更低。

可以想到的加速（都還沒驗證）：

| 做法 | 可能省下 | 備註 |
|---|---|---|
| ReLU6 併進 Conv | ~124 ms | esp-dl／ESP-PPQ 目前沒有把 Clip 併進 Conv；要改上游或自己改圖 |
| 全 int16（不要 RequantizeLinear） | 0（實測 1,072 ≈ 1,057） | int16 Conv 本身慢 2.3 倍，抵掉省下的轉換 |
| 輸入 192×192 | ~27 %（MACs 107 M／145.6 M） | 精度沒驗證 |
| 雙核 | ≤ 11 %（esp-dl README） | core 0 要做 JPEG |
| 第一層改 int16 | ~57 ms（int8 版） | heads16 已經是這樣 |

全部疊上去樂觀估計 `int8+dw16+heads16` 也還要 **550–650 ms**，仍是門檻的 2 倍；全 int8 的速度可以接近 300 ms，但精度不合格（Phase A：per-tensor 量化下 depthwise 失真）。

## 6. 門檻判定

| 門檻 | 判定 | 依據 |
|---|---|---|
| 1. 實機 ≤ 300 ms／手、追蹤 ≥ 3 fps | **不通過**（最快的合格版 740 ms；主版本 1,057 ms；追蹤約 0.8 fps） | 3.1、5 |
| 2. 量化誤差 ≤ 3 % 裁切邊長 | **通過**（實機 vs float：主版本 2.49 px＝1.1 %；Phase A 的整體評估不變，實機和模擬只差 ≤ 1 %） | 3.4 |
| 3. 規則手勢不輸分類器 | **測試畫面上通過**（主版本 10/10；分類器在 batch1 的 7 隻手也全對，ok／call／dislike 沒有分類器的實機數字）；真手沒測 | 3.5 |
| 4. 載入後 PSRAM 剩 ≥ 1 MB（人臉＋手勢都在） | **不通過**（主版本載不進去；`int8+dw16` 剩 0.21 MB；先載關鍵點則手勢載不進去） | 3.3 |

## 7. 建議：No-go

**不做功能「手部關鍵點」，功能 8 也不升級成關鍵點＋規則。**

- 速度差 3.5 倍（門檻 1），而且不是靠調整就能補上：時間是 int16 depthwise、int8↔int16 轉換、獨立 ReLU6 這三種 esp-dl 在 ESP32-S3 上的固定成本；能換成 int8 的地方（全 int8）精度就壞掉（ESP32-S3 只有 per-tensor、2 的次方 scale）。
- 記憶體（門檻 4）也不過：主版本要 4.5 MB PSRAM，加上一塊 3 MiB 的連續區塊，和人臉／手勢模型不能同時常駐。
- 精度與規則手勢其實沒問題（10/10），所以失敗的原因是**這顆晶片＋這個 runtime 跑這個模型太慢太大**，不是方法錯。

因此：

1. 功能 8 維持 ADR-0019 的分類器（一隻手 5.5 次／秒、兩隻手 4.0–4.2 次／秒）。
2. **基礎建設不提前做**：模型分割區＋按需載入照 SPEC 的原計畫，等第三個 NN 模型（人體、人臉辨識）時再做；`GET_KEYPOINTS` 不需要。第 4 節的觀察（malloc 導向、整個分割區 mmap、連續變數區）留給那時參考。
3. 這次的 bench（`MXCAM_HANDLM_BENCH`、`tools/handlm/pack_storage.py`、`device_report.py`）留在 repo，預設不編譯，之後換模型／換 esp-dl 版本可以直接重量。

**什麼情況值得重新評估**（任何一項成立再開原型 C）：

- esp-dl 更新後 ReLU6 能併進 Conv、int16 depthwise 明顯變快，或支援 per-channel 量化（目前只有 P4／S31 的 Conv／Gemm 有）——重量主版本，目標 ≤ 300 ms。
- 有比 MediaPipe lite 小很多、且 int8 友善的手部關鍵點模型（例如用 ReLU、QAT 自己訓練的 128–160 輸入、寬度 0.5 的版本；要資料與訓練，工作量另估）。
- 硬體換成有 NPU 或 per-channel 量化的晶片（ESP32-P4、K230）。

## 8. 重現步驟

```bash
# 1. 打包（Phase A 的 venv；W = Phase A 工作目錄，Q = $W/quant）
$W/.venv/Scripts/python.exe -I tools/handlm/pack_storage.py storage.bin $W/data/eval.npz <gesture-frames 目錄> \
  "dw16h16=$Q/hand_landmark_lite_int8+dw16+heads16_notest.espdl" "dw16h16:t=$Q/hand_landmark_lite_int8+dw16+heads16.espdl" \
  "dw16=$Q/hand_landmark_lite_int8+dw16_notest.espdl" "dw16:t=$Q/hand_landmark_lite_int8+dw16.espdl" \
  "auto8:t=$Q/hand_landmark_lite_int8+auto8.espdl" "int8=$Q/hand_landmark_lite_int8_notest.espdl" "int16=$Q/hand_landmark_lite_int16_notest.espdl"
#    名字結尾 ":t" = 含測試資料（跑 model->test()）；有同名 notest 版時 profile／完整流程只做 notest 那個。
# 2. 寫到 storage（12.4 MB，USB 約 70 秒）
python -m esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x410000 storage.bin
# 3. bench 建置（PowerShell、ESP-IDF 環境；用另一個 build 目錄）
idf.py -B <build> -DMXCAM_HANDLM_BENCH=ON [-DMXCAM_HANDLM_BENCH_EARLY=ON] [-DMXCAM_HANDLM_BENCH_ONLY=<n>] build
idf.py -B <build> -p COM5 app-flash      # app-flash：不要動 storage
# 4. 從 TTL 埠（COM7）收 log；串流時另外對 OTG 埠送 STREAM_START 0x03＋每秒 PING
# 5. 整理
$W/.venv/Scripts/python.exe -I -X utf8 tools/handlm/device_report.py <log> $W/models/hand_landmark_lite.onnx $W/data/eval.npz $Q
# 6. 結束：燒回預設建置，清掉 storage
python -m esptool --chip esp32s3 -p COM5 erase_region 0x410000 0xBF0000
```

量測結束後板子已燒回 `main` HEAD 的預設建置，`storage` 已清除（erase_region）。
