# QR Code 用 quirc、全解析度每張辨識，內容用 GET_RESULT_DATA 讀

QR Code（功能 6，M7）要讀出碼的內容，並能把「內容 → ID」學起來給 R4 用（同 HUSKYLENS 2）。韌體元件 `mxcam_qr`：`mxcam_qr.c` 包 Espressif 的 quirc port（只能在板子上跑），`mxcam_qr_post.c` 是純 C 的後處理（角 → ResultRecord、排序、學習挑選、TFT 標籤），有電腦測試（`host-test/`）。

決定：

1. **函式庫：`espressif/quirc` `~1.2.0`，鎖定 `1.2.0~1`**（ISC，`dependencies.lock`；IDF 需求 `>=4.3.0`，所有 target）。這個 port 已用 `QUIRC_FLOAT_TYPE=float`＋`QUIRC_USE_TGMATH` 編譯（S3 只有單精度 FPU），我們不改上游程式。下載的 `quirc.h` 與計畫的 API 名稱一致：`quirc_new`、`quirc_resize`、`quirc_begin`、`quirc_end`、`quirc_count`、`quirc_extract`（`code.corners[4]` 是 QR 自己的左上起順時針：左上、右上、右下、左下）、`quirc_decode`（`data.payload`、`data.payload_len`）、`quirc_destroy`；`QUIRC_MAX_PAYLOAD` = 8,896。與計畫不同的兩點：
   - **`QUIRC_MAX_GRIDS` 是 64，不是 8**。所以 `mxq_detect` 每張最多只 extract＋decode **前 8 個格子**（`MXQ_MAX_GRIDS 8`，依 quirc 找到的順序），把時間上限固定在約 8 × 3 ms。
   - **`quirc_decode` 把約 8.9 KB 的 `struct datastream`（`data[QUIRC_MAX_PAYLOAD]`）放在呼叫者的 stack 上**（README 說的「約 10 KiB stack」就是這個）。`struct quirc_code`（約 3.9 KB）與 `struct quirc_data`（約 8.9 KB）我們已放 PSRAM，這 8.9 KB 沒辦法搬。見決定 3。
2. **輸入：全解析度 320×240 灰階，每張畫面都辨識**。QR 每個模組至少要約 2 px，半解析度（160×120）會讓 25 模組的碼要 ≥ 100 px 才讀得到。有碼時的時間主要花在找定位圖案與格子擬合，解碼本身約 3 ms，「位置沒變就沿用上次內容」只省約 3 ms，不值得多一套快取與「換卡沒換位置」的錯誤；每張都辨識讓框與 RESULTS 的 `frame_id` 一致（同 ADR-0019）。灰階用 `mxg_luma_be` 直接寫進 `quirc_begin` 回傳的緩衝區（不多一份 76.8 KB）。不做 `quirc_flip` 重試（鏡頭給的是真實影像，ADR-0010）。
3. **記憶體**：`mxq_init` 在開機時（`mxh_init` 之後、建立 task 之前）呼叫 `quirc_new`＋`quirc_resize(320, 240)`，用 `heap_caps_malloc_extmem_enable(0)` 包住（同 ADR-0011），之後一直留著：PSRAM 約 92 KB（灰階 76,800 B＋`struct quirc` 約 12 KB＋flood fill 工作區 160 × 16 B）。quirc 的區域上限是 254（`QUIRC_MAX_REGIONS`），所以像素緩衝區與灰階共用（`QUIRC_PIXEL_ALIAS_IMAGE`），不多一份。`struct quirc_code`、`struct quirc_data` 和解碼成功的碼（最多 8 個 `mxq_code_t`）用 `EXT_RAM_BSS_ATTR` 靜態放 PSRAM。開機 log：`qr ready (internal A -> B, PSRAM C -> D)`。失敗時功能 6 不列在 GET_INFO。**stack**：處理流程 stack 24 KB、目前未用約 21.5 KB（m8a.md），`quirc_decode` 要約 9 KB＋呼叫鏈；Task 5 量 QR 時的 stack 高水位，剩 < 4 KB 就把 `PIPE_STACK` 改成 32 KB（內部 RAM 多 8 KB）並記錄。
4. **一張畫面最多 4 個碼**（`MXQ_MAX_CODES 4`）：前 8 個格子全部解碼，只回報**解碼成功**的，依四角形面積（鞋帶公式）由大到小取前 4 個（同面積保持原順序）。解碼失敗的格子不回報，R4 拿到的每一筆都有內容。
5. **ResultRecord**：`id` = 學到的 ID（內容完全相符時 1–8），沒學過 = 0；`x, y, width, height` = 四個角的外接框，裁到畫面內（0–319、0–239），寬高 = max − min + 1，中心 = (min + max) / 2 無條件捨去；`confidence = 255`（不適用：解碼成功就是確定的）；**extra 18 bytes 與 AprilTag 相同**：`rotation_cdeg i16`（`atan2(右上.y − 左上.y, 右上.x − 左上.x)`，QR 正放是 0、畫面上順時針為正，範圍 ±18000）＋四個角 `x0,y0..x3,y3 i16`（QR 自己的左上、右上、右下、左下，**不裁切**）。R4 與 Studio 共用 AprilTag 的解析（`mxp_unpack_corners_extra`）。
6. **讀 QR 內容：新指令 `GET_RESULT_DATA (0x21)`**。請求 `index u8`；回應 `0xA1`：`frame_id u32, index u8, flags u8（bit0 TRUNCATED）, total_len u16, len u8, data[len]`。指的是 GET_RESULTS 同一份快取（最近一張處理完的畫面）的第 `index` 筆；主機比對 `frame_id`，不同就重讀兩者。`data` 是原始 bytes，**最多 128 bytes**（`MXP_RESULT_DATA_MAX`，R4 記憶體有限），更長的截斷並設 `TRUNCATED`、`total_len` 是原長（`mxq_set_payload`：`len = min(n, 128)`、`total_len = n`，超過 65,535 時飽和，quirc 最多 8,896 不會發生）。沒有附帶資料的紀錄（其他功能）回 `len 0`。錯誤：長度不是 1 → `BAD_LENGTH`；沒有處理完的畫面、或 `index` ≥ 筆數 → `BAD_PARAM`。細節見 `docs/protocol.md` 第 11 節。
7. **學 QR：LEARN 6 把「內容 → ID」記下來**。目前功能必須是 6；區域語意同顏色（左上角＋大小，可以超出畫面）。延後回應（ADR-0007），處理流程在辨識完這張畫面之後，用 `mxq_pick_for_learn` 從這張的碼裡選「紀錄中心（決定 5 的 x, y）在 `[x, x+w) × [y, y+h)` 內」、面積最大的那個：內容已學過 → 回原本的 ID；新的 → 最小的空 ID 1–8；學滿 8 個 → `FULL`；區域內沒有解碼成功的碼 → `BAD_PARAM`。比對規則：`total_len` 相同且前 `min(total_len, 128)` bytes 相同（前 128 bytes 相同但長度不同＝不同的碼）。新 ID 從下一張畫面起生效。指定 ID 的 LEARN 對功能 6 回 `BAD_PARAM`。FORGET 6：`id` 刪對照與名字，`0xFFFF` 全刪；SET_NAME 6：只接受已學的 ID；GET_LEARNED 6：已學的 ID 依 ID 排序。
8. **讀學到的內容：新指令 `GET_LEARNED_DATA (0x35)`**：請求 `function_id u8, id u16`；回應 `0xB5`：`function_id u8, id u16, flags u8, total_len u16, len u8, data[len]`。只支援功能 6；其他功能或沒學過的 ID → `BAD_PARAM`；長度不是 3 → `BAD_LENGTH`。Studio 的學習分頁要顯示「ID 3 = LEFT」，GET_LEARNED 的版面不能改，`0x34` 保留不占用。
9. **QR 對照表存進 NVS，store blob 版本不變**：v3 的學習資料清單加一筆 `function_id 6, layout_version 1`：`n u8, n × { id u8, total_len u16, len u8, bytes[len] }`（`n ≤ 8`、ID 1–8 不重複、`len ≤ 128`、`len ≤ total_len`、`total_len > 128` 時 `len = 128`，否則整包拒絕）。最大 1,057 B；`MXST_BLOB_MAX` 2,993 → 4,054 B。M6 的韌體讀到這筆會略過，燒回舊韌體只是 QR 對照不見。理由：「設定好拔掉電腦」（SPEC §1）；ADR-0017 的紀錄清單就是為了不升版本。
10. **名字表維持全部功能合計 32 筆，不在 M7 加大**：加到 64 筆要多約 10 KB 內部 RAM 與約 1.7 KB blob，NVS 會很緊；M9 有 `mxdata` 分割區（ADR-0017）時再一起加大。

TFT 標籤（`mxq_label`）：`QR3 HELLO`（沒學過是 `QR HELLO`），內容最多 8 個字元，可列印 ASCII（0x20–0x7E）以外的 byte 印成 `?`；內容是空的時只有 `QR3`／`QR`（沒有結尾空白）。

## Considered Options

- **esp-code-scanner**（Espressif）：閉源、不回報碼的位置（沒有框與角），R4 不能用位置追碼。
- **zbar**：LGPL，靜態連結進韌體有授權義務；而且比 quirc 大。
- **半解析度（160×120）辨識**：小一點的碼會讀不到（見決定 2），省下的時間不值得。
- **位置沒變就沿用上次內容（省解碼）**：只省約 3 ms／碼，卻多一套快取與「換卡沒換位置」的錯誤。
- **把內容塞進 RESULTS、或新增 QR 內容推送**：內容可能遠超過 18 bytes 的 extra，RESULTS 格式也不能改；R4 本來就是一問一答，Studio 也用同一個指令（有沒對上的碼或距上次超過 1 秒才讀）。
- **獨立 QR task 非同步辨識**：框會落後畫面、`frame_id` 對不上，還要多一份畫面複製；串流時 core 0 忙 JPEG，沒有空閒核心（同 ADR-0019）。

## Consequences

- 有碼時預估約 3–5 fps（研究 §4.3 估每張 100–300 ms），沒有碼時目標 ≥ 8 fps；實測在 Task 5／`docs/measurements/m7.md`。有碼時 < 3 fps 的退路：`MXQ_MAX_CODES` 降到 2，仍不夠就停下來回報（quirc 已是 `-O2`、float）。
- QR 內容上限 128 bytes（R4 與協定），更長的碼只能讀到前 128 bytes（`TRUNCATED`）；學習比對仍看完整長度。
- `quirc_decode` 在處理流程 task 的 stack 上要約 9 KB，stack 高水位待 Task 5 量（決定 3）。
- quirc 每張只處理前 8 個格子：畫面上有很多假的定位圖案（例如格子布、棋盤）時，可能輪不到真的碼；實機驗收（Task 8）用 6 張卡測。
- `mxq_init` 和 `mxf_init`／`mxh_init` 一樣只能在 `app_main` 啟動 task 之前呼叫。
