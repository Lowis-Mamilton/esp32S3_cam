# M7 QR Code（功能 6）＋幾何形狀（功能 11）＋動態偵測（功能 12）實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development（建議）或 superpowers:executing-plans 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> 🧑‍🔧 步驟由主對話跟使用者一起做（需要實物：手機或印出的 QR Code、A4 印的形狀、使用者的手）。直接在 `main` 上 commit；路徑 `C:\dev\esp32S3_cam`。
> 設計問題已由使用者授權「直接採用建議」；下方「設計決定」列出每個決定與理由，執行時依各 Task 的「記錄」步驟寫進 SPEC／protocol.md／ADR。ADR 編號用 **0020**（QR）、**0021**（形狀與動態）；執行時若已被占用，用下一個空號並同步改本計畫內的引用。
> **使用者的速度模式（2026-11-19）：** Task 一個接一個派出，**不做逐 Task 審查**，最後做一次整體審查；硬體延遲在整合（Task 5）時量；使用者最後驗收（Task 8）。所以 Task 依檔案切開、可以平行（見「平行分組」）。**永遠不要 `git add -A`**，只 add 本 Task 列出的檔案（其他 Task 的代理可能同時在改別的檔案）。

**目標：** MXCam 新增三個功能：QR Code（讀內容、可把內容學成 ID）、幾何形狀（圓、三角形、正方形、長方形）、動態偵測（畫面中動的區域）。韌體偵測、TFT 疊圖、USB 推送、可存檔；MXCam Studio（真板子與展示模式）能選功能、看結果與 QR 內容、學 QR、取名、調參數。

**架構：** 協定只新增：功能 ID 6／11／12、Shape config v1、Motion config v1（ADR-0016 的 func config）、QR 的 18 bytes extra（與 AprilTag 同格式）、兩個新指令 `GET_RESULT_DATA (0x21)`（讀最近一批結果第 i 筆的附帶資料＝QR 內容）與 `GET_LEARNED_DATA (0x35)`（讀學到的 QR 內容）。韌體新增三個元件：`mxcam_qr`（包 `espressif/quirc`，純 C 的後處理可主機測試）、`mxcam_shape`、`mxcam_motion`（純 C、主機測試）；store blob 版本不變（v3 的 func config 清單多兩筆、學習資料清單多一筆 QR 對照表）；處理流程新增 `main/scan.c`（同 `tracking.c` 的做法），不讓 `pipeline.c` 再長大。

**技術：** ESP-IDF v5.5.6、`espressif/quirc` 1.2.0~1（ISC）、純 C99（host test：CMake＋MinGW＋ctest）、React/TS（Vitest）、lucide-react 1.75.0。

**規格：** `docs/SPEC.md` §3（擴充功能表：6、11、12 在 M7）、§6.1、§10；`docs/research/vision-functions.md` §1、§4.2、§4.3、§7；`docs/protocol.md` §4、§7、§9、§10；ADR-0010、0016、0017、0018、0019；`docs/measurements/m6.md`、`m8a.md`（預算基準）。

## 設計決定（執行時寫進 SPEC／protocol.md／ADR；理由附在後面）

### QR Code（功能 6）

1. **函式庫：`espressif/quirc` `~1.2.0`（2026-11-19 查 ESP Component Registry：最新 1.2.0~1，約 2026-10 底上傳，ISC，支援所有 target）**。這個 port 已用 `QUIRC_FLOAT_TYPE=float`＋`QUIRC_USE_TGMATH` 編譯（S3 只有單精度 FPU）；README 說辨識＋解碼小的碼約要 10 KiB stack、版本越大越多。API（上游 `quirc.h`）：`quirc_new`、`quirc_resize(q, 320, 240)`、`quirc_begin`（回傳 8-bit 灰階緩衝區）、`quirc_end`（找定位圖案、擬合格子）、`quirc_count`、`quirc_extract(q, i, &code)`（`code.corners[4]`：QR 自己的左上、右上、右下、左下）、`quirc_decode(&code, &data)`（`data.payload`、`data.payload_len`）。Task 2 用 `idf.py update-dependencies` 鎖版本並對照下載的 `quirc.h` 確認以上名稱；IDF 版本限制以 `dependencies.lock` 為準。不用 esp-code-scanner（閉源、沒有位置）、zbar（LGPL）。
2. **輸入：全解析度 320×240 灰階，每張畫面都辨識**（ADR-0020）。理由：QR 每個模組至少要約 2 px，半解析度（160×120）會讓 25 模組的碼要 ≥ 100 px 才讀得到；quirc 有碼時的時間主要花在找定位圖案與格子擬合（研究 §4.3：解碼本身約 3 ms），「位置沒變就沿用上次內容」只省約 3 ms，不值得多一套快取與「換卡沒換位置」的錯誤；每張都辨識讓框與 RESULTS 的 `frame_id` 一致（同 ADR-0019 的理由）。灰階直接用 `mxg_luma_be` 寫進 `quirc_begin` 的緩衝區（不多一份 76.8 KB）。速度退路見決定 15。
3. **記憶體**：`mxq_init` 在開機時（`mxh_init` 之後、建立 task 之前）呼叫 `quirc_new`＋`quirc_resize(320, 240)`，用 `heap_caps_malloc_extmem_enable(0)` 包住（同 ADR-0011），之後一直留著（PSRAM 約 90 KB：灰階 76,800 B＋flood fill 堆疊＋`struct quirc`）；`struct quirc_code`（約 4 KB）、`struct quirc_data`（約 9 KB）用 `EXT_RAM_BSS_ATTR` 靜態放 PSRAM（不放 stack）。失敗時功能 6 不列在 GET_INFO。處理流程 stack 24 KB、目前未用約 21.5 KB（m8a.md）；Task 5 量 QR 時的 stack 高水位，**剩 < 4 KB 就把 `PIPE_STACK` 改成 32 KB**（內部 RAM 多 8 KB）並記錄。
4. **一張畫面最多 4 個碼**（`MXQ_MAX_CODES 4`）：quirc 找到的格子全部解碼（最多 8 個，每個約 3 ms），只回報**解碼成功**的，依四角形面積由大到小取前 4 個。解碼失敗的格子不回報（R4 拿到的每一筆都有內容）。不做 `quirc_flip` 重試（鏡頭給的是真實影像，ADR-0010）。
5. **ResultRecord**：`id` = 學到的 ID（內容完全相符時 1–8），沒學過 = `0`；`x, y, width, height` = 四個角的外接框（裁到畫面內，寬高 = max − min + 1，中心 = (min + max) / 2 取整）；`confidence = 255`（不適用：解碼成功就是確定的）；**extra 18 bytes 與 AprilTag 相同**：`rotation_cdeg i16`（`atan2(右上.y − 左上.y, 右上.x − 左上.x)`，QR 正放是 0、順時針為正）＋四個角 `x0,y0..x3,y3 i16`（QR 自己的左上、右上、右下、左下，不裁切）。理由：R4 與 Studio 可以共用 AprilTag 的解析；≤ 18 bytes。
6. **讀 QR 內容：新指令 `GET_RESULT_DATA (0x21)`**（ADR-0020）。請求 `index u8`；回應 `0xA1`：`frame_id u32, index u8, flags u8（bit0 TRUNCATED）, total_len u16, len u8, data[len]`（9 + len bytes）。指的是 **GET_RESULTS 同一份快取**（最近一張處理完的畫面）的第 `index` 筆；主機比對 `frame_id` 與剛讀到的 GET_RESULTS 是否相同，不同就重讀兩者。`data` 是原始 bytes（QR 的 byte 模式通常是 UTF-8，數字／英數模式是 ASCII），**最多 128 bytes**（`MXP_RESULT_DATA_MAX`，R4 記憶體有限），更長的截斷並設 `TRUNCATED`、`total_len` 是原長。沒有附帶資料的紀錄（其他功能）回 `len 0`。錯誤：長度不是 1 → `BAD_LENGTH`；還沒有處理完的畫面、或 `index` ≥ 這批的筆數 → `BAD_PARAM`。理由：內容放不進 18 bytes 的 extra；跟 R4 一問一答的用法一致；不改 RESULTS 格式也不加新推送（Studio 用同一個指令，見決定 17）。
7. **學 QR：LEARN 6 把「內容 → ID」記下來**（同 HUSKYLENS 2）。目前功能必須是 6；區域語意同顏色（左上角＋大小，R4 可以給整張畫面）。延後回應（ADR-0007），處理流程在**辨識完這張畫面之後**，從這張的碼裡選「中心在區域內」的、面積最大的那個：內容已學過 → 回原本的 ID（不改資料）；新的 → 最小的空 ID 1–8；學滿 8 個 → `FULL`；區域內沒有解碼成功的碼 → `BAD_PARAM`。比對規則：`total_len` 相同且前 `min(total_len, 128)` bytes 相同。新 ID 從下一張畫面起生效。11 bytes 的 LEARN（指定 ID）對功能 6 仍回 `BAD_PARAM`（YAGNI）。FORGET 6：`id` 刪對照與名字，`0xFFFF` 全刪，不存在也成功。SET_NAME 6：只接受已學的 ID。GET_LEARNED 6：已學的 ID（依 ID 排序、`r,g,b` 0、名字）。
8. **讀學到的內容：新指令 `GET_LEARNED_DATA (0x35)`**：請求 `function_id u8, id u16`；回應 `0xB5`：`function_id u8, id u16, flags u8, total_len u16, len u8, data[len]`（7 + len）。只支援功能 6；其他功能或沒學過的 ID → `BAD_PARAM`；長度不是 3 → `BAD_LENGTH`。理由：Studio 的學習分頁要顯示「ID 3 = LEFT」，否則老師分不出哪張卡是哪個 ID；GET_LEARNED 的版面不能改（`0x34` 保留給別的用途，不占用）。
9. **QR 對照表存進 NVS，blob 版本不變**：store blob v3 的學習資料清單加一筆 `function_id 6, layout_version 1`：`n u8, n × { id u8, total_len u16, len u8, bytes[len] }`（`n ≤ 8`、ID 1–8 不重複、`len ≤ 128`、`len ≤ total_len`、`total_len > 128` 時 `len = 128`，否則整包拒絕）。最大 1 + 8 × 132 = 1,057 B；`MXST_BLOB_MAX` 2,993 → **4,054 B**（24 KB NVS 寫新 blob 時新舊並存約 8 KB，放得下）。`mxs_data_t` 多約 1.1 KB（main、存檔工作、load 的暫存各一份，內部 RAM 約多 3.3 KB）。M6 的韌體讀到這筆會略過（不認得的功能），燒回舊韌體只是 QR 對照不見。理由：「設定好拔掉電腦」（SPEC §1）；ADR-0017 的紀錄清單就是為了不升版本。
10. **名字表維持全部功能合計 32 筆，不在 M7 加大**：QR 最多多 8 筆、形狀 4 筆；加到 64 筆要多約 10 KB 內部 RAM（`mxs_data_t` ×3、`MXS_RESPONSE_MAX` 的 scratch）與約 1.7 KB blob，NVS 會很緊。M9 有 `mxdata` 分割區（ADR-0017）時再一起加大，記到 SPEC §10。

### 幾何形狀（功能 11）

11. **形狀 ID（固定類別，不需要學習，可以取名，同手勢）**：`1 圓形 CIRCLE`、`2 三角形 TRIANGLE`、`3 正方形 SQUARE`、`4 長方形 RECT`（ASCII 是 TFT 標籤）。五邊形、星形等**不回報**；六邊形以上在 320×240 上和圓分不開，會當成圓（寫進已知限制）。理由：教材最常見的四種；可靠度優先，ID 5 以後保留。SET_NAME 11：`id` 1–4；GET_LEARNED 11：有名字的形狀；FORGET 11：刪名字；LEARN 11 → `BAD_PARAM`。
12. **做法（ADR-0021）**：在 `mxg_half` 的 160×120 灰階上二值化：模式 `0` 深色形狀淺色底（預設）、`1` 淺色形狀深色底；門檻 `0` = Otsu（兩類平均差 < 40 → 沒有形狀，同線條），`1–255` 固定。8 連通的連通區域（顯式堆疊，不遞迴）→ 丟掉碰到畫面邊緣的、外接框長邊 < `min_size`（換成 320 px）的、外接框面積 > 畫面 60% 的（背景／紙張）→ 對每個區域追外輪廓（Moore 鄰點追蹤，最多 1,024 點）→ 特徵：鏈碼周長 `P`、面積 `A' = 鞋帶公式面積 + P/2`、圓度 `C = 4πA'/P²`、各輪廓點到形心距離的變異係數 `CV`；Douglas–Peucker（`ε = max(1, 3% × P)`）→ 頂點數 `n`。分類（依序）：`C ≥ 0.80` 且 `CV ≤ 0.06` → 圓；`n = 3` → 三角形；`n = 4`、凸、四個內角都在 90° ± 20° → 兩組對邊平均長度的比 ≤ 1.25 是正方形，否則長方形；其他 → 不回報。常數若測試需要可以微調，最後的值寫進 ADR-0021。理由：只有空心外框（用筆畫的形狀）也能用（面積用輪廓算，不數像素）；半解析度讓每張 ≤ 15 ms；碰到邊緣的形狀不完整，分類會錯。
13. **形狀的 ResultRecord**：每個形狀一筆，最多 `max_results` 筆，依面積由大到小；`id` = 形狀 ID；`x, y, width, height` = 外接框（320 座標）；`confidence` = round(100 × 小 ÷ 大)，比的是輪廓面積 `A'` 與近似形狀的面積（多邊形是 DP 多邊形、圓是平均半徑的圓）；`extra_len 0`。**Shape config v1**（function_id 11，layout 1，body 8）：`mode u8`（0／1，> 1 → 0，預設 0）、`threshold u8`（0 = 自動，預設 0）、`min_size u8`（px，16–240 夾到邊界，預設 32）、`max_results u8`（1–8 夾到邊界，預設 4）、`reserved u8 ×4`。

### 動態偵測（功能 12）

14. **做法（ADR-0021）**：`mxg_half` 再 2×2 平均成 **80×60** 灰階；背景是指數移動平均（`u16`，×16 定點，每張 `bg += (cur×16 − bg) >> 3`，約 0.8 秒）。**曝光補償**：先算全畫面平均差 `offset = mean(cur) − mean(bg/16)`，每格的差減掉它再比門檻；**全域變化**：超過 50% 的格子在動（燈開關、鏡頭被移動、車子在走）→ 背景重設成這張、這張與之後 2 張不回報。**暖機**：切到功能 12、`mxm_reset` 之後前 3 張只建背景不回報。變化格做一次 3×3 膨脹（把一隻手的碎片接起來）再做 8 連通區域；外接框長邊（×4 換成 320 px）≥ `min_size` 才回報。每個區域一筆：`id = 0`（同人臉：只有偵測、沒有類別）、框（320 座標、裁到畫面內）、`confidence` = round(100 × 框內真正變化的格數 ÷ 框內格數)（不含膨脹）、`extra_len 0`；依面積由大到小、最多 `max_results` 筆。靜止下來的物體約 1–2 秒後融入背景（不再回報）。**Motion config v1**（function_id 12，layout 1，body 8）：`threshold u8`（灰階差，5–100 夾到邊界，預設 25）、`min_size u8`（px，8–240，預設 24）、`max_results u8`（1–8，預設 4）、`reserved u8 ×5`。不需要學習、不能取名（LEARN／SET_NAME 12 → `BAD_PARAM`，FORGET 12 成功，GET_LEARNED 12 → `BAD_PARAM`，同人臉）。理由：前後兩張相減只抓得到移動物體的邊緣、慢的動作抓不到；移動平均＋曝光補償能處理 OV3660 自動曝光的跳動；動態偵測只適用於固定不動的鏡頭（寫進 SPEC）。

### 共通

15. **效能目標（SPEC §3 傳統功能 ≥ 10 fps）**：形狀每張 ≤ 15 ms、動態 ≤ 5 ms，兩者 TFT（不串流）≥ 10 fps；QR **沒有碼時 ≥ 8 fps**（每張約 30–40 ms），**畫面上有一張碼時 ≥ 3 fps**（每張 ≤ 250 ms，研究 §4.3 估 100–300 ms）。QR 有碼時低於 3 fps：先把 `MXQ_MAX_CODES` 降到 2；仍不夠就停下來回報主對話（quirc 已是 `-O2`、float，不改上游程式），兩種情況都寫進 m7.md。形狀超過 15 ms：輪廓上限改成 512 點、只處理面積前 8 大的區域。
16. **TFT（ADR-0010：文字只用 ASCII、鏡像繪製）**：功能名稱 `QR`、`SHAPE`、`MOTION`（`pipeline.c` 的 `fn_name`）。QR：四個角連成四邊形（`mxo_draw_line`，2 px），顏色 `id ≥ 1` 用 `k_id_colors[(id − 1) % 8]`、`id 0` 白色；標籤 `QR3 HELLO`（沒學過是 `QR HELLO`），內容最多 8 個可列印 ASCII 字元，其他 byte 印成 `?`。形狀：框＋`CIRCLE 92`／`TRIANGLE 88`／`SQUARE 90`／`RECT 85`，顏色 `k_id_colors[(id − 1) % 8]`。動態：白色框＋`MOVE 63`。
17. **Studio**：功能列：QR Code（`ScanQrCode`，放在 AprilTag 後面）、幾何形狀（`Shapes`）、動態偵測（`Activity`）放最後；分類色 `--cat-qr #e8743b`、`--cat-shape #b5c934`、`--cat-motion #e05297`。**QR 內容**：RealDevice 收到功能 6 的 RESULTS 時用 `GET_RESULT_DATA` 讀內容（`QrTextCache`：有沒對上的碼、或距上次讀超過 1 秒才讀；回應的 `frame_id` 不是那批就放棄、下一批再讀；之後的批次依中心距離 ≤ 32 px 把內容接到紀錄上）；學到的 QR 用 `GET_LEARNED_DATA` 讀內容。結果多 `text`、`truncated`；LiveStage 畫四邊形＋「名字或 QR／ID」＋內容前 16 字；學習分頁：QR 框選學習、列出 ID／內容／名字／忘記；形狀 4 列取名表（同手勢）；動態「不需要學習」。參數分頁：形狀（模式、門檻自動＋滑桿、最小尺寸、最多幾個）、動態（靈敏度門檻、最小尺寸、最多幾個）、QR「沒有參數」。程式分頁：`MXCAM_ALGO_QR`、`MXCAM_ALGO_SHAPE`、`MXCAM_ALGO_MOTION`。展示模式：兩張假 QR 卡（`MXCam`、`LEFT`）慢慢移動並轉動、四種形狀在白紙上漂移、一顆球週期性地動與停。設定檔維持 `version: 1`，新增可省略的 `params.shape`、`params.motion`、`learned[6]`、`learned[11]`；QR 對照跟顏色一樣要用鏡頭重新學（`colorsNeedRelearn` 也涵蓋 `learned[6]`；QR 名字只在板上同一 ID 的內容相同時才套用），形狀名字直接套用。
18. **Arduino API（記錄給 M4，不在本計畫實作）**：`MXCAM_ALGO_QR`（6）、`MXCAM_ALGO_SHAPE`（11）、`MXCAM_ALGO_MOTION`（12）；`MXCAM_SHAPE_CIRCLE`…`MXCAM_SHAPE_RECT`（1–4，值來自共用 C 的 `MXP_SHAPE_*`）；`bool MXCam::readText(uint8_t index, char *buf, size_t cap)`（GET_RESULT_DATA，`frame_id` 和上一次 `request()` 不同就回 false，學生再 `request()` 一次）；QR 的 `r.angle` 與 AprilTag 共用 `mxp_unpack_corners_extra`。Studio 程式分頁的範例照這組名稱寫。

## 全域限制

- 協定只新增、不改既有欄位；新的測試資料只**加進** `docs/protocol-vectors.json`（不改既有項目），C 與 TS 都要通過；`encoded_hex` 的 CRC 用程式算（`gen_vectors.py` 或 `encodeFrame`），不要手打
- `arduino-lib/src/protocol/` 維持純 C99（ADR-0006）；`MXP_RESULT_EXTRA_MAX` 維持 18；`MXP_MAX_RESULTS` 維持 8
- 座標一律 320×240 真實影像（ADR-0010）；ResultRecord 的 x/y 是中心、LEARN 區域是左上角；辨識路徑上不做任何翻轉
- 一次只跑一個功能；LEARN 的 `function_id` 必須等於目前功能，否則 `BAD_PARAM`；LEARN 延後回應沿用 ADR-0007
- 新的純 C 元件不在函式裡 malloc：工作緩衝區用 `EXT_RAM_BSS_ATTR` 靜態放 PSRAM（host build 時巨集為空，照 `mxcam_line.c`）；`mxq_init` 是唯一的開機配置（決定 3）
- 預算：app 映像 **≤ 3,774,873 B**（目前 3,548,528 B，可用 226,345 B；估計 quirc 約 25–35 KB＋三個元件與 `scan.c` 約 15–20 KB＋其他約 5 KB ≈ 45–60 KB → 約 3.59–3.61 MB，factory 4 MB 剩約 14%）；內部 RAM 最低剩餘 **≥ 60 KB**（目前約 102.8 KB，預估 M7 後約 95 KB）；開機後 PSRAM 剩餘 **≥ 2 MiB**（目前約 3.8 MB，M7 的靜態緩衝區合計約 0.4 MB）；任一條不成立就停下來回報主對話
- 處理流程（core 1）不能因 USB 阻塞；推送走 `mxcam_link_push`；統計 log 每秒一行
- **平行執行的規則**：Task 2、3、4、6 會同時進行——只有 Task 5 跑 `idf.py build`／燒錄（之前任何 Task 都**不要**跑 `idf.py build`，因為 `components/` 下所有元件都會被編譯、別的代理的半成品會讓建置失敗，而且同一個 `firmware/build` 不能同時建）；Task 2 只跑 `idf.py update-dependencies`（下載 quirc、更新 `dependencies.lock`，不編譯）。`.github/workflows/ci.yml` 只由 Task 5 改；`docs/SPEC.md` 只由 Task 1、Task 8 改；各 Task 的 host test 用自己的 `build/<name>-host-test` 目錄
- host test 的 CMake 參數照現有元件：`-std=c99 -Wall -Wextra -Werror`
- 本機驗證指令：
  - host test（Git Bash）：`cmake -S <dir>/host-test -B build/<name>-host-test -G "MinGW Makefiles" && cmake --build build/<name>-host-test && ctest --test-dir build/<name>-host-test --output-on-failure`
  - 韌體（PowerShell，只有 Task 2 的 update-dependencies 與 Task 5、8）：`$env:IDF_TOOLS_PATH = "C:\Espressif"; . C:\Espressif\frameworks\esp-idf-v5.5.6\export.ps1; cd C:\dev\esp32S3_cam\firmware; idf.py build`
  - 燒錄 `idf.py -p COM5 flash`（OTG），log `idf.py -p COM9 monitor`（TTL）；pyserial 開 COM5 前先設 `dtr=False, rts=False`；送指令前關掉所有 Studio 分頁
  - Studio（`pc-app/`）：`npm run lint`、`npm test`、`npm run build`
- commit 訊息結尾加 `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`

## 審查重點

1. **QR 在畫面邊緣、斜放、很小、或同時很多張**：四個角可能在畫面外；外接框要裁切、extra 的角不裁；超過 4 張只回報最大的 4 張；不能讀到 quirc 緩衝區外。→ Task 2 `to_result_clips_bbox`、`sort_limit_keeps_largest`；Task 8 Step 2（斜放、靠邊、6 張卡）
2. **很長、二進位、不是 UTF-8 的 QR 內容**：> 128 bytes 要截斷並設 `TRUNCATED`；學習比對要看 `total_len`（前 128 bytes 相同但長度不同＝不同的碼）；TFT 標籤只印可列印 ASCII；Studio 用替代字元顯示，不能丟例外。→ Task 1 `result_data_truncated` 向量；Task 2 `label_non_ascii`、`set_payload_truncates`；Task 4 `learn_qr_long_payload`；Task 6 `qr text with invalid utf-8`
3. **光線與鏡頭移動（動態）**：自動曝光跳動、開關燈、手拿著板子晃 → 不能整個畫面變成一個大框；切到功能 12 的頭幾張不能報動態。→ Task 3 `motion_global_brightness_shift`、`motion_camera_pan_resets`、`motion_warmup`
4. **形狀的誤判**：紙張邊緣、碰到畫面邊的形狀、雜訊、空心外框、轉 45° 的正方形、五邊形。→ Task 3 `shape_border_touching_ignored`、`shape_outline`、`square_rotated_45`、`pentagon_not_reported`、`shape_noise_frame`
5. **舊資料與新舊版本混搭**：M6／M8a 韌體存的 v3 blob（沒有 QR 紀錄、沒有形狀／動態參數）要讀得進來；Studio 連舊韌體（沒有 6／11／12、`0x21` 回 `UNKNOWN_CMD`）時不送新指令；`GET_RESULT_DATA` 回應的 `frame_id` 對不上時不能把別張畫面的內容接到框上。→ Task 4 `deserialize_m6_v3_fixture`；Task 6 `old firmware: no m7 requests`、`stale frame id discards text`

## 平行分組

| 組 | Task | 何時開工 | 檔案範圍（彼此不重疊） |
|---|---|---|---|
| A | 1 協定 | 立刻 | `arduino-lib/src/protocol/`、`arduino-lib/extras/host-test/`、`docs/protocol-vectors.json`、`pc-app/src/protocol/`、`docs/protocol.md`、`docs/SPEC.md` |
| B | 2 `mxcam_qr` | Task 1 commit 後 | `firmware/components/mxcam_qr/`、`firmware/dependencies.lock`、`docs/adr/0020-*` |
| B | 3 `mxcam_shape`＋`mxcam_motion` | Task 1 commit 後 | `firmware/components/mxcam_shape/`、`firmware/components/mxcam_motion/`、`docs/adr/0021-*` |
| B | 4 store／service | Task 1 commit 後 | `firmware/components/mxcam_store/`、`firmware/components/mxcam_service/` |
| B | 6 Studio 裝置層 | Task 1 commit 後 | `pc-app/src/{ui/layout/types.ts,device/,testing/}` |
| C | 5 處理流程整合＋板上量測 | Task 2、3、4 都 commit 後 | `firmware/main/`、`.github/workflows/ci.yml`、`firmware/sdkconfig.defaults`（若需要） |
| C | 7 Studio 介面 | Task 6 commit 後（可與 Task 5 同時） | `pc-app/src/{ui/,i18n/,theme.css,App.tsx}`、`docs/studio-ui.md` |
| D | 8 🧑‍🔧 驗收 | 全部 commit、整體審查後 | `docs/measurements/m7.md`、`docs/SPEC.md`、`CLAUDE.md` |

B 組四個 Task 同時進行時，每個代理只 `git add` 自己的檔案；commit 前 `git status` 確認，別人的檔案不要碰。

## 檔案結構

| 檔案 | 職責 | Task |
|---|---|---|
| `arduino-lib/src/protocol/mxcam_proto.{h,c}`、host test、`docs/protocol-vectors.json` | 功能 ID、形狀 ID、Shape／Motion config、corners extra、GET_RESULT_DATA／GET_LEARNED_DATA | 1 |
| `pc-app/src/protocol/{messages.ts,messages.test.ts,vectors.ts}` | TS 編解碼 | 1 |
| `firmware/components/mxcam_qr/`（新）`mxcam_qr_post.{h,c}`、`host-test/` | 角→紀錄、排序、學習挑選、ASCII 標籤（純 C） | 2 |
| `firmware/components/mxcam_qr/mxcam_qr.{h,c}`、`idf_component.yml` | 包 quirc：開機配置、每張辨識＋解碼 | 2 |
| `firmware/components/mxcam_shape/`（新）＋`host-test/` | 形狀偵測（純 C） | 3 |
| `firmware/components/mxcam_motion/`（新）＋`host-test/` | 動態偵測（純 C） | 3 |
| `firmware/components/mxcam_store/` | 形狀／動態參數、QR 對照表（v3 的紀錄清單） | 4 |
| `firmware/components/mxcam_service/` | 0x21／0x35、功能 6／11／12 的 LEARN／FORGET／SET_NAME／GET_LEARNED、func config 11／12、結果附帶資料快取 | 4 |
| `firmware/main/scan.{c,h}`（新）、`pipeline.c`、`main.c`、`CMakeLists.txt` | 功能 6／11／12 的每張處理、QR 學習、TFT | 5 |
| `pc-app/src/device/*`、`device/qrText.ts`、`device/demo/{qr,shapes,motion}.ts`（新）、`testing/fakeFirmware.ts` | 裝置層、展示模式、假韌體 | 6 |
| `pc-app/src/ui/{layout,live,panel}/*`、`i18n/*`、`theme.css`（只加 token）、`App.tsx` | 介面 | 7 |
| `docs/adr/0020-qr-code-quirc-and-result-data.md`、`0021-own-shape-and-motion-detection-in-c.md`、`docs/measurements/m7.md` | 決定與量測 | 2、3、8 |

---

### Task 1：協定：功能 6／11／12、Shape／Motion config、QR extra、GET_RESULT_DATA、GET_LEARNED_DATA（C＋TS＋測試資料）

**檔案：**
- 修改：`arduino-lib/src/protocol/mxcam_proto.{h,c}`、`arduino-lib/extras/host-test/{test_proto.c,gen_vectors.py}`、`docs/protocol-vectors.json`（只新增）
- 修改：`pc-app/src/protocol/{messages.ts,messages.test.ts,vectors.ts}`
- 修改：`docs/protocol.md`、`docs/SPEC.md`（§3、§5、§6.1、§8、§10）

**介面：**
- 產出（C，`mxcam_proto.h`，只新增）：
```c
/* 功能 enum 加：*/ MXP_FUNC_QR = 6, MXP_FUNC_SHAPE = 11, MXP_FUNC_MOTION = 12
/* 指令 enum 加：*/ MXP_CMD_GET_RESULT_DATA = 0x21, MXP_CMD_GET_LEARNED_DATA = 0x35
enum { MXP_SHAPE_CIRCLE = 1, MXP_SHAPE_TRIANGLE = 2, MXP_SHAPE_SQUARE = 3, MXP_SHAPE_RECT = 4 };
#define MXP_SHAPE_MAX 4
#define MXP_SHAPE_CONFIG_VERSION 1
#define MXP_MOTION_CONFIG_VERSION 1
typedef struct { uint8_t mode, threshold, min_size, max_results; } mxp_shape_config_t;
typedef struct { uint8_t threshold, min_size, max_results; } mxp_motion_config_t;
void   mxp_shape_config_defaults(mxp_shape_config_t *c);   /* 0, 0, 32, 4 */
void   mxp_motion_config_defaults(mxp_motion_config_t *c); /* 25, 24, 4 */
size_t mxp_pack_shape_config(const mxp_shape_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);   /* reserved 0 */
bool   mxp_unpack_shape_config(const uint8_t *p, uint16_t len, mxp_shape_config_t *out);       /* len 10、fn 11、ver 1；夾限 */
size_t mxp_pack_motion_config(const mxp_motion_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
bool   mxp_unpack_motion_config(const uint8_t *p, uint16_t len, mxp_motion_config_t *out);     /* len 10、fn 12、ver 1；夾限 */
/* AprilTag 與 QR 共用的 18 bytes extra */
#define MXP_CORNERS_EXTRA_SIZE 18
typedef struct { int16_t rotation_cdeg; int16_t x[4], y[4]; } mxp_corners_extra_t;
void mxp_pack_corners_extra(const mxp_corners_extra_t *e, mxp_result_t *r);      /* extra_len = 18 */
bool mxp_unpack_corners_extra(const mxp_result_t *r, mxp_corners_extra_t *out);  /* extra_len != 18 → false */
/* GET_RESULT_DATA (0x21) / GET_LEARNED_DATA (0x35) */
#define MXP_RESULT_DATA_MAX 128
#define MXP_DATA_FLAG_TRUNCATED 0x01
typedef struct { uint32_t frame_id; uint8_t index, flags; uint16_t total_len; uint8_t len; uint8_t data[MXP_RESULT_DATA_MAX]; } mxp_result_data_t;
typedef struct { uint8_t function_id; uint16_t id; uint8_t flags; uint16_t total_len; uint8_t len; uint8_t data[MXP_RESULT_DATA_MAX]; } mxp_learned_data_t;
bool   mxp_unpack_get_result_data(const uint8_t *p, uint16_t len, uint8_t *index);                    /* len 1 */
size_t mxp_pack_result_data(const mxp_result_data_t *d, uint8_t *out, size_t cap);                     /* 9 + len；len > 128 → 0 */
bool   mxp_unpack_result_data(const uint8_t *p, uint16_t len, mxp_result_data_t *out);                 /* 長度一致、len ≤ 128 */
bool   mxp_unpack_get_learned_data(const uint8_t *p, uint16_t len, uint8_t *function_id, uint16_t *id); /* len 3 */
size_t mxp_pack_learned_data(const mxp_learned_data_t *d, uint8_t *out, size_t cap);                   /* 7 + len */
bool   mxp_unpack_learned_data(const uint8_t *p, uint16_t len, mxp_learned_data_t *out);
```
- 夾限：shape：mode > 1 → 0；min_size 夾 16–240；max_results 夾 1–8；threshold 不夾。motion：threshold 夾 5–100；min_size 夾 8–240；max_results 夾 1–8。reserved 一律寫 0。
- 產出（TS，`messages.ts`）：`Cmd.GetResultData = 0x21`、`Cmd.GetLearnedData = 0x35`；`FunctionId.Qr = 6`、`Shape = 11`、`Motion = 12`；`export const Shape = { Circle: 1, Triangle: 2, Square: 3, Rect: 4 } as const`；`interface ShapeConfig { mode: 0 | 1; threshold: number; minSize: number; maxResults: number }`、`interface MotionConfig { threshold: number; minSize: number; maxResults: number }`；`FuncConfig` 聯集加 `{ functionId: 11; shape: ShapeConfig }`、`{ functionId: 12; motion: MotionConfig }`；`encodeGetResultData(index: number): Uint8Array`、`parseResultData(p): { frameId: number; index: number; truncated: boolean; totalLength: number; data: Uint8Array }`、`encodeGetLearnedData(fn: number, id: number)`、`parseLearnedData(p): { functionId: number; id: number; truncated: boolean; totalLength: number; data: Uint8Array }`（長度不符就丟錯）；`parseResults`：功能 6 且 extra 18 bytes → `extra`（同 AprilTag 的 `{ rotationDeg, corners }`）

- [ ] **Step 1：新增測試資料**（`gen_vectors.py` 產生，CRC 用程式算）。frames：`get_info_response_m7`（功能 [1,2,3,4,5,6,8,11,12]）、`get_func_config_shape_request`（0x44，`0b`）、`func_config_shape_default`（0xC4，`0b 01 00 00 20 04 00 00 00 00`）、`func_config_motion_default`（0xC4，`0c 01 19 18 04 00 00 00 00 00`）、`set_func_config_motion_request`（0x45，`0c 01 0f 30 02 00 00 00 00 00`）、`get_result_data_request`（0x21，`00`）、`result_data_hello`（0xA1）、`result_data_truncated`（0xA1）、`get_learned_data_request`（0x35，`06 02 00`）、`learned_data_qr`（0xB5）、`results_qr_two`（0x61）。payloads：`shape_config_default`、`motion_config_set`（15, 48, 2）、`result_data_hello` = `{frame_id: 300, index: 0, flags: 0, total_len: 5, data: "HELLO"}`、`result_data_truncated` = `{frame_id: 301, index: 1, flags: 1, total_len: 300, data: bytes 0..127}`、`learned_data_qr` = `{function_id: 6, id: 2, flags: 0, total_len: 4, data: "LEFT"}`、`results_qr_two` = `{frame_id: 300, function_id: 6, results: [{id: 2, x: 130, y: 90, width: 61, height: 61, confidence: 255, corners_extra: {rotation_cdeg: 0, corners: [[100,60],[160,60],[160,120],[100,120]]}}, {id: 0, x: 240, y: 180, width: 41, height: 41, confidence: 255, corners_extra: {rotation_cdeg: -9000, corners: [[220,200],[220,160],[260,160],[260,200]]}}]}`。C 常數 `VEC_P_SHAPE_CONFIG_DEFAULT` 等；`vectors.ts` 的 `PayloadVectors` 加型別。
- [ ] **Step 2：寫失敗的 C 測試**（`test_proto.c`）：`test_info_m7`（9 個 ID、最後是 12）、`test_shape_motion_config_vectors`（default 往返 bytes 相同；motion set 解出 15/48/2）、`test_shape_motion_clamp`（shape `{11,1, 5,77,3,20, 9,9,9,9}` → 0、77、16、8，再 pack reserved 全 0；motion `{12,1, 0,255,0, 9,…}` → 5、240、1）、`test_shape_motion_reject`（len 9、fn 8、ver 2 → false）、`test_corners_extra`（`results_qr_two` 往返、解出第二筆 −9000 與四個角；`extra_len` 14 → unpack false）、`test_result_data`（hello、truncated 往返；`len` 129 pack 回 0；payload 長度比 9 + len 多或少 → unpack false）、`test_learned_data`（往返；請求 3 bytes 解出 fn 6 id 2；2 bytes → false）、`test_get_result_data_req`（1 byte OK、0／2 bytes false）。
- [ ] **Step 3：跑 C 測試確認失敗**：`cmake -S arduino-lib/extras/host-test -B build/host-test -G "MinGW Makefiles" && cmake --build build/host-test` → 編譯失敗。
- [ ] **Step 4：實作 C**（照「介面」）。
- [ ] **Step 5：`ctest --test-dir build/host-test --output-on-failure`** → 全部 PASS。
- [ ] **Step 6：寫失敗的 TS 測試**（`messages.test.ts`）：`parseInfo reads m7 function ids`、`shape and motion func config vectors round trip`、`parseFuncConfig rejects wrong shape/motion version and length`、`parseResults decodes qr corners`（第一筆 `extra.corners[1]` 是 [160,60]、第二筆 `rotationDeg` −90）、`result data vectors`（hello 的 `data` 解成 "HELLO"、truncated 的 `truncated` true、`totalLength` 300、`data.length` 128）、`learned data vector`、`encodeGetResultData and encodeGetLearnedData equal the vectors`、`Shape ids match the protocol table`。
- [ ] **Step 7：`cd pc-app && npx vitest run src/protocol`** → 失敗；實作 TS；再跑 → PASS；`npm run lint`。
- [ ] **Step 8：記錄**：
  - `docs/protocol.md`：§4 加 `6 QR Code M7`、`11 幾何形狀 M7`、`12 動態偵測 M7`；§5 加 `0x21 GET_RESULT_DATA M7`、`0x35 GET_LEARNED_DATA M7`；§7 的 LEARN／FORGET／SET_NAME／GET_LEARNED 條目各加「QR（6）、形狀（11）、動態（12）見第 11 節」，ResultRecord 段加「QR 的 extra 與 AprilTag 相同（18 bytes）」；§9 func config 的 function_id 說明加 11、12；檔尾新增「## 11. M7 指令細節（QR、幾何形狀、動態偵測）」：GET_RESULT_DATA（決定 6）、GET_LEARNED_DATA（決定 8）、QR 的紀錄與學習／名字規則（決定 4、5、7）、形狀 ID 表與 Shape config v1（決定 11、13）、形狀的紀錄、動態的紀錄與 Motion config v1（決定 14）、速度（以 m7.md 為準）
  - `docs/SPEC.md`：§3 擴充功能表下加三個功能的說明（各一段，同線條／手勢的寫法；動態偵測只適用固定不動的鏡頭；QR 內容用 GET_RESULT_DATA 讀、最多 128 bytes；形狀只認 4 種、六邊形以上算圓）；§5「辨識結果」加 QR extra 與附帶資料一句；§6.1 store 那句補「QR 對照表（最多 8 個 ID、內容 128 bytes）是 v3 學習資料清單的一筆，最大 4,054 B」；§8 加 `M7｜QR、幾何形狀、動態偵測｜形狀／動態 TFT ≥ 10 fps、QR 沒有碼 ≥ 8 fps、一張碼 ≥ 3 fps、實機驗收通過`；§10：名字表那項補「M7 決定不加大（決定 10），M9 再一起改」，新增 M4 的 Arduino API（決定 18）
- [ ] **Step 9：Commit**
```bash
git add arduino-lib/src/protocol/mxcam_proto.h arduino-lib/src/protocol/mxcam_proto.c arduino-lib/extras/host-test/test_proto.c arduino-lib/extras/host-test/gen_vectors.py docs/protocol-vectors.json pc-app/src/protocol/messages.ts pc-app/src/protocol/messages.test.ts pc-app/src/protocol/vectors.ts docs/protocol.md docs/SPEC.md
git commit -m "feat(protocol): M7 QR, shapes and motion: function ids, configs, result data" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 2：QR 元件 `mxcam_qr`（quirc 包裝＋純 C 後處理）

（B 組，與 Task 3、4、6 同時進行；不要跑 `idf.py build`。）

**檔案：**
- 新增：`firmware/components/mxcam_qr/{CMakeLists.txt,idf_component.yml,mxcam_qr.h,mxcam_qr.c,mxcam_qr_post.h,mxcam_qr_post.c}`、`firmware/components/mxcam_qr/host-test/{CMakeLists.txt,test_qr_post.c}`
- 修改：`firmware/dependencies.lock`（`idf.py update-dependencies` 產生）
- 新增：`docs/adr/0020-qr-code-quirc-and-result-data.md`

**介面：**
- 消耗：Task 1 的 `mxp_result_t`、`mxp_pack_corners_extra`、`MXP_RESULT_DATA_MAX`；既有的 `mxg_luma_be`
- 產出（`mxcam_qr_post.h`，純 C）：
```c
#define MXQ_MAX_CODES 4
#define MXQ_LABEL_CHARS 8
typedef struct {
  int16_t x[4], y[4];   /* quirc corners: the code's own TL, TR, BR, BL (320x240, not clipped) */
  uint16_t total_len;   /* payload length as decoded */
  uint8_t len;          /* stored bytes = min(total_len, MXP_RESULT_DATA_MAX) */
  uint8_t bytes[MXP_RESULT_DATA_MAX];
} mxq_code_t;
void    mxq_set_payload(mxq_code_t *c, const uint8_t *p, size_t n);   /* len、total_len（n > 65535 → 65535） */
int32_t mxq_area2(const mxq_code_t *c);                               /* 四角形面積 ×2（鞋帶公式，絕對值） */
uint8_t mxq_sort_limit(mxq_code_t *codes, uint8_t n, uint8_t max);    /* 面積由大到小（同面積保持原順序），回傳 min(n, max) */
void    mxq_to_result(const mxq_code_t *c, uint16_t id, mxp_result_t *out); /* 決定 5 */
int     mxq_pick_for_learn(const mxq_code_t *codes, uint8_t n, uint16_t x, uint16_t y, uint16_t w, uint16_t h); /* 中心在區域內、面積最大的 index；沒有 → -1 */
int     mxq_label(uint16_t id, const mxq_code_t *c, char *buf, size_t cap); /* "QR3 HELLO"／"QR HELLO"，snprintf 語意 */
```
- 產出（`mxcam_qr.h`，ESP）：
```c
#define MXQ_SKIPPED (-1)
typedef struct { uint32_t total_us, gray_us, identify_us, decode_us; uint8_t grids, decoded, failed; } mxq_stats_t;
bool mxq_init(void);   /* 冪等；開機、建 task 之前呼叫；自己用 heap_caps_malloc_extmem_enable(0) 包住 quirc_new/quirc_resize */
/* 320x240 RGB565 big-endian 真實影像 → 解碼成功的碼（已 mxq_sort_limit 到 MXQ_MAX_CODES）的個數，或 MXQ_SKIPPED（未初始化）。
 * 不可重入；只在處理流程 task 呼叫。stats 可為 NULL。 */
int  mxq_detect(const uint16_t *frame, mxq_code_t out[MXQ_MAX_CODES], mxq_stats_t *stats);
```
- `mxq_detect` 流程：`quirc_begin` → 每個像素 `mxg_luma_be` 寫進緩衝區 → `quirc_end` → 對每個 `i < quirc_count` 做 `quirc_extract`＋`quirc_decode`（`struct quirc_code`／`struct quirc_data` 是 `EXT_RAM_BSS_ATTR` 靜態）→ 成功的轉成 `mxq_code_t`（`mxq_set_payload(data.payload, data.payload_len)`）→ `mxq_sort_limit(…, MXQ_MAX_CODES)`；填 stats。

- [ ] **Step 1：寫失敗的 host test**（`test_qr_post.c`，`CHECK` 巨集照 `test_gesture_post.c`）：
  - `to_result_axis_aligned`：角 (100,60)(160,60)(160,120)(100,120)、id 3 → x 130、y 90、w 61、h 61、confidence 255、extra_len 18、`mxp_unpack_corners_extra` 得 rotation 0 與相同的角
  - `to_result_rotated`：以 (160,120) 為中心、邊長 80 轉 30° 的四角 → rotation_cdeg 3000 ± 50；轉 −90° → −9000 ± 50
  - `to_result_clips_bbox`：角 (−20,−10)(40,−10)(40,50)(−20,50) → x0 = 0、w 41、h 51；extra 的第一個角仍是 (−20,−10)（審查重點 1）
  - `sort_limit_keeps_largest`：6 個碼（面積各不同）、max 4 → 回 4、依面積遞減；n 2、max 4 → 回 2 且順序不變
  - `pick_for_learn`：區域只含 B 的中心 → B；兩個中心都在 → 面積大的；都不在 → −1；區域 (300,200,100,100)（超出畫面）仍可選到中心在 (310,220) 的碼
  - `set_payload_truncates`：n 5 → len 5、total 5；n 300 → len 128、total 300、bytes 是前 128 個（審查重點 2）
  - `label`：(3, "HELLO") → `"QR3 HELLO"`；(0, "HELLO") → `"QR HELLO"`；內容 `"ABCDEFGHIJK"` → `"QR1 ABCDEFGH"`；`label_non_ascii`：內容 `{0x41, 0x00, 0xE4, 0x7F}` → `"QR A???"`；cap 5 → `buf[4] == 0`、回傳完整長度、後面的 canary 沒被寫
- [ ] **Step 2：跑測試確認失敗**：`cmake -S firmware/components/mxcam_qr/host-test -B build/qr-host-test -G "MinGW Makefiles" && cmake --build build/qr-host-test` → 編譯失敗。host-test 只編 `mxcam_qr_post.c`＋`mxcam_proto.c`（照 `mxcam_gesture/host-test`）。
- [ ] **Step 3：實作 `mxcam_qr_post.c`**。
- [ ] **Step 4：`ctest --test-dir build/qr-host-test --output-on-failure`** → PASS。
- [ ] **Step 5：quirc 依賴**：`idf_component.yml`：`espressif/quirc: "~1.2.0"`（註解：ISC、float port、約 10 KiB stack）；PowerShell 載入 IDF 後在 `firmware/` 跑 `idf.py update-dependencies`（**不要** build）→ `dependencies.lock` 多 quirc；打開 `managed_components/espressif__quirc` 的 `quirc.h` 確認決定 1 的 API 名稱、`struct quirc_code`／`quirc_data` 欄位、`QUIRC_MAX_PAYLOAD`，有不同就照實際的寫並記在 ADR。`CMakeLists.txt`：`SRCS mxcam_qr.c mxcam_qr_post.c`、`REQUIRES mxcam_protocol`、`PRIV_REQUIRES mxcam_gray espressif__quirc esp_timer heap log`（實際元件名以 managed_components 目錄名為準）。
- [ ] **Step 6：實作 `mxcam_qr.c`**（照「介面」；`mxq_init` 的記憶體 log：`qr ready (internal A -> B, PSRAM C -> D)`，寫法照 `mxcam_gesture.cpp` 的 `mxh_init`）。這個檔案要到 Task 5 才第一次編譯；Task 5 會修編譯錯誤。
- [ ] **Step 7：記錄**：新增 `docs/adr/0020-qr-code-quirc-and-result-data.md`（決定 1–10：quirc 版本與授權、全解析度每張辨識、開機配置與 PSRAM、最多 4 個、extra 與 AprilTag 相同、GET_RESULT_DATA／GET_LEARNED_DATA、內容→ID 學習與存檔、名字表不加大；Considered Options：esp-code-scanner、zbar、半解析度、位置快取省解碼、把內容塞進 RESULTS 或新增推送、獨立 QR task 非同步辨識；Consequences：有碼時約 3–5 fps、內容上限 128 bytes、stack 需求待 Task 5 量）。
- [ ] **Step 8：Commit**
```bash
git add firmware/components/mxcam_qr firmware/dependencies.lock docs/adr/0020-qr-code-quirc-and-result-data.md
git commit -m "feat(firmware): QR code module (quirc) with host tests" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 3：形狀 `mxcam_shape` 與動態 `mxcam_motion`

（B 組，與 Task 2、4、6 同時進行；不要跑 `idf.py build`；不改 `mxcam_gray`、`mxcam_line`。）

**檔案：**
- 新增：`firmware/components/mxcam_shape/{CMakeLists.txt,mxcam_shape.h,mxcam_shape.c}`、`firmware/components/mxcam_shape/host-test/{CMakeLists.txt,test_shape.c}`
- 新增：`firmware/components/mxcam_motion/{CMakeLists.txt,mxcam_motion.h,mxcam_motion.c}`、`firmware/components/mxcam_motion/host-test/{CMakeLists.txt,test_motion.c}`
- 新增：`docs/adr/0021-own-shape-and-motion-detection-in-c.md`

**介面：**
- 消耗：`mxg_half`（既有）；Task 1 的 `mxp_shape_config_t`、`mxp_motion_config_t`、`MXP_SHAPE_*`、`mxp_result_t`
- 產出：
```c
/* mxcam_shape.h */
#define MXSH_MIN_CONTRAST 40
#define MXSH_MAX_CONTOUR 1024
#define MXSH_CIRC_MIN 0.80f
#define MXSH_CV_MAX 0.06f
#define MXSH_DP_EPS_PCT 3
#define MXSH_QUAD_ANGLE_TOL 20
#define MXSH_SQUARE_RATIO 1.25f
#define MXSH_MAX_BG_PCT 60
/* 輪廓點（任意座標單位）→ 形狀 ID（0 = 不回報）與 confidence。給測試與 mxsh_detect 用。 */
uint16_t    mxsh_classify(const int16_t (*pts)[2], int n, uint8_t *confidence);
/* 0..cap 筆（≤ cfg->max_results），依面積遞減；不可重入（static PSRAM 緩衝區）。 */
uint8_t     mxsh_detect(const uint16_t *frame, const mxp_shape_config_t *cfg, mxp_result_t *out, uint8_t cap);
const char *mxsh_ascii_name(uint16_t id);                  /* "CIRCLE" "TRIANGLE" "SQUARE" "RECT"；其他 "?" */
int         mxsh_label(uint16_t id, uint8_t conf, char *buf, size_t cap); /* "SQUARE 90"，snprintf 語意 */
/* mxcam_motion.h */
#define MXM_W 80
#define MXM_H 60
#define MXM_BG_SHIFT 3
#define MXM_WARMUP 3
#define MXM_GLOBAL_PCT 50
typedef struct { uint8_t global_reset; uint16_t changed_cells; } mxm_stats_t;
void    mxm_reset(void);   /* 下一張重建背景＋暖機；換功能時呼叫 */
/* 0..cap 筆（≤ cfg->max_results），依面積遞減，id 0；不可重入（背景與緩衝區是 static PSRAM）。stats 可為 NULL。 */
uint8_t mxm_detect(const uint16_t *frame, const mxp_motion_config_t *cfg, mxp_result_t *out, uint8_t cap, mxm_stats_t *stats);
```
- 演算法照設計決定 12（形狀）與 14（動態）。形狀的輪廓座標在 160×120，框換回 320（`x0*2`、寬 ×2）；連通區域用顯式堆疊（static PSRAM），輪廓超過 `MXSH_MAX_CONTOUR` 的區域跳過。動態：3 張暖機期間仍更新背景；全域變化那張回 0 筆並設 `stats->global_reset`。

- [ ] **Step 1：寫失敗的形狀測試**（`test_shape.c`；合成 320×240 RGB565BE，白底 `0xFFFF`、黑形狀；helper `fill_polygon`、`fill_circle`、`stroke_*`；`MXSH_TEST_HOOKS` 開緩衝區 canary，照 `mxcam_track`）：
  - `classify_ideal`：密集取樣的理想輪廓（圓 r 40、正三角形、正方形、2:1 長方形）直接給 `mxsh_classify` → 1、2、3、4，confidence ≥ 90
  - `circle_filled`：r 40 在 (160,120) → 1 筆 id 1、中心誤差 ≤ 3、w 81 ± 4、confidence ≥ 85、extra_len 0
  - `triangle_filled`：邊長 100 的正三角形，轉 0° 與 20° → id 2
  - `square_axis` 80×80 → id 3；`square_rotated_45` → id 3（審查重點 4）
  - `rectangle`：140×70，轉 0° 與 30° → id 4
  - `shape_outline`：4 px 粗的空心圓與空心正方形 → id 1、3（審查重點 4）
  - `pentagon_not_reported`：外接圓半徑 50 的正五邊形 → 0 筆
  - `shape_border_touching_ignored`：正方形一半在畫面左邊外 → 0 筆；整張深色只留中間白框（紙張邊緣）→ 不把背景當形狀
  - `small_ignored`：24 px 正方形，min_size 32 → 0 筆；min_size 16 → 1 筆
  - `light_mode`：黑底白形狀，mode 1 → 找到；mode 0 → 0 筆
  - `fixed_threshold`：灰色（gray ≈ 100）形狀，threshold 120 → 找到
  - `shape_noise_frame`：灰階 120–140 隨機（固定種子）→ 0 筆
  - `max_results_sort`：6 個大小不同的形狀，max_results 4 → 4 筆、面積遞減；全程 canary 完好
  - `label`：`(3, 90)` → `"SQUARE 90"`；`(9, 50)` → `"? 50"`
- [ ] **Step 2：寫失敗的動態測試**（`test_motion.c`；背景是固定種子的灰階紋理）：
  - `motion_warmup`：`mxm_reset` 後前 3 張（就算有移動）→ 0 筆（審查重點 3）
  - `static_scene_no_motion`：同一張 20 次 → 0 筆
  - `moving_block_detected`：40×40 的深色方塊每張右移 10 px；暖機後 → 1 筆、id 0、框包含方塊目前的位置、confidence > 0、extra_len 0
  - `motion_global_brightness_shift`：暖機後整張每個通道加亮（灰階約 +30）→ 0 筆（審查重點 3）
  - `motion_camera_pan_resets`：整張紋理平移 20 px → 0 筆、`global_reset` 1；之後同一張再 3 張 → 0 筆
  - `small_motion_ignored`：12×12 的點移動，min_size 24 → 0 筆；min_size 8 → 1 筆
  - `threshold_param`：灰階差 15 的方塊移動，threshold 25 → 0 筆；threshold 10 → 1 筆
  - `max_results`：三個分開的移動方塊（大小不同），max_results 2 → 2 筆、面積遞減
  - `still_object_absorbed`：方塊移動 5 張後停住 → 30 張內變成 0 筆
- [ ] **Step 3：跑兩個 host test 確認失敗**（`build/shape-host-test`、`build/motion-host-test`；host-test 編 `mxcam_<name>.c`＋`mxcam_gray.c`＋`mxcam_proto.c`）。
- [ ] **Step 4：實作**兩個元件（元件 `CMakeLists.txt` 照 `mxcam_line`：`REQUIRES mxcam_protocol mxcam_gray`；緩衝區 `EXT_RAM_BSS_ATTR`）。
- [ ] **Step 5：兩個 host test 的 `ctest --output-on-failure`** → PASS（Task 5 才把它們加進 CI）。
- [ ] **Step 6：記錄**：新增 `docs/adr/0021-own-shape-and-motion-detection-in-c.md`（決定 11–14，含最後定案的常數；Considered Options：形狀用 Canny＋Hough（慢、要調很多參數）、在全解析度做（20+ ms）、只數頂點不看圓度（圓會變五、六邊形）；動態用前後兩張相減（只抓邊緣）、esp-dl `motion_detect`（只回傳動的點數、沒有框）、不做曝光補償（自動曝光一跳整張都在動）；Consequences：六邊形以上算圓、黃色等很淺的顏色在白紙上可能分不出來、動態只適用固定鏡頭、靜止物體 1–2 秒後消失）。
- [ ] **Step 7：Commit**
```bash
git add firmware/components/mxcam_shape firmware/components/mxcam_motion docs/adr/0021-own-shape-and-motion-detection-in-c.md
git commit -m "feat(firmware): shape and motion detection modules with host tests" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 4：store 與 service（QR 對照表、形狀／動態參數、附帶資料）

（B 組，與 Task 2、3、6 同時進行；不要跑 `idf.py build`。）

**檔案：**
- 修改：`firmware/components/mxcam_store/{mxcam_store.h,mxcam_store.c,store_nvs.c}`、`firmware/components/mxcam_store/host-test/test_store.c`
- 修改：`firmware/components/mxcam_service/{mxcam_service.h,mxcam_service.c}`、`firmware/components/mxcam_service/host-test/test_service.c`

**介面：**
- 消耗：Task 1 的 shape／motion config codec、`MXP_FUNC_QR/SHAPE/MOTION`、`MXP_SHAPE_MAX`、`mxp_result_data_t`、`mxp_learned_data_t` 的 pack、`mxp_unpack_get_result_data`、`mxp_unpack_get_learned_data`、`MXP_RESULT_DATA_MAX`
- 產出（`mxcam_store.h`）：
```c
#define MXST_FN_QR 6
#define MXST_FN_SHAPE 11
#define MXST_FN_MOTION 12
#define MXST_MAX_QR 8
#define MXST_QR_REC_MAX (1 + MXST_MAX_QR * (4 + MXP_RESULT_DATA_MAX))   /* 1057 */
typedef struct { uint8_t id, len; uint16_t total_len; uint8_t bytes[MXP_RESULT_DATA_MAX]; } mxst_qr_t;
/* mxs_data_t 最後新增：mxp_shape_config_t shape_cfg; mxp_motion_config_t motion_cfg; mxst_qr_t qr[MXST_MAX_QR]; uint8_t n_qr; （qr 依 id 排序） */
/* serialize 多寫 shape／motion 兩筆 func config 與（有學過時）一筆 fn 6 的學習紀錄（決定 9）；MXST_BLOB_MAX += 4 + MXST_QR_REC_MAX（= 4054） */
int      mxst_learn_qr(mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len); /* 已學 → 原 id；新 → 最小空 id 1-8；滿 → MXST_LEARN_FULL；len 0 → MXST_LEARN_NO_COLOR */
uint16_t mxst_qr_lookup(const mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len); /* 0 = 沒學過 */
void     mxst_forget_qr(mxs_data_t *d, uint16_t id);   /* 連名字一起；0xFFFF = 全部 */
/* name_key_ok：6 → 已學的 id；11 → 1..4；12 → 永遠 false */
```
- 產出（`mxcam_service.h`）：
```c
typedef struct { uint8_t flags; uint16_t total_len; uint8_t len; uint8_t data[MXP_RESULT_DATA_MAX]; } mxs_rdata_t;
/* 在 mxs_set_results 之後、同一個鎖內呼叫：items[i] 屬於這批的第 i 筆（n ≤ 這批筆數，其餘 len 0）。
 * mxs_set_results 與功能切換都會把附帶資料清成「沒有」。 */
void mxs_set_result_data(mxs_state_t *s, const mxs_rdata_t *items, uint8_t n);
/* LEARN 6 完成：bytes NULL → BAD_PARAM；mxst_learn_qr 的結果 → 0xB0 id／FULL；只有真的新增時 mark dirty。 */
bool mxs_finish_learn_qr(mxs_state_t *s, uint8_t seq, const uint8_t *bytes, uint8_t len, uint16_t total_len);
/* mxs_state_t 新增 mxs_rdata_t rdata[MXP_MAX_RESULTS]; uint8_t rdata_n; */
```
- service 規則：GET_RESULT_DATA／GET_LEARNED_DATA 照決定 6、8（`frame_id` 取自快取的 results 前 4 bytes）；LEARN 6 照決定 7（延後回應，同功能 1）；LEARN 11、12 → `BAD_PARAM`；FORGET／SET_NAME／GET_LEARNED 照決定 7、11、14；GET／SET_FUNC_CONFIG 加 `case 11／12`（規則照 protocol.md §9：要看能力清單）；FACTORY_RESET 經由 `mxst_defaults` 清掉 QR 與參數；`MXS_RESPONSE_MAX` 不變（新回應最大 9 + 128 B）。

- [ ] **Step 1：先抓 v3 樣本**（改 store 前）：在 `test_store.c` 用**目前的** `mxst_serialize` 對一組已知資料（1 個顏色、名字 (5,1)「小車」、線色 {200,30,30}、tw 16×th 16 的物體、手勢參數 50/70/2）產生 bytes，印成 C 陣列貼進測試當 `k_v3_m6_fixture`（之後永遠不改）。
- [ ] **Step 2：寫失敗的 store 測試**：`deserialize_m6_v3_fixture`（原內容都在；`shape_cfg`、`motion_cfg` 是預設；`n_qr` 0，審查重點 5）、`deserialize_v1_fixture` 仍通過、`roundtrip_m7`（3 個 QR、名字 (6,2)、(11,3)、形狀與動態參數非預設）、`serialize_max_fits`（8 個 128 bytes 的 QR＋32 個 48 bytes 名字＋7 色＋物體 32×32 ≤ `MXST_BLOB_MAX`）、`reject_bad_qr_record`（id 重複、id 9、len 129、len > total_len、total 300 但 len 5 → 整包拒絕）、`qr_record_unknown_layout_skipped`（fn 6 layout 2 → 讀得進來、`n_qr` 0）、`learn_qr_ids`（新 → 1、同內容 → 1、另一個 → 2、忘記 1 後新的 → 1、學滿 8 個後第 9 個 → FULL、len 0 → NO_COLOR）、`learn_qr_long_payload`（total 300 與 total 301、前 128 bytes 相同 → 不同 ID，審查重點 2）、`qr_lookup`、`forget_qr_drops_name`、`name_keys_m7`（(6,1) 只在學過時可以、(11,1)、(11,4) 可以、(11,5) 不行、(12,0) 不行）。
- [ ] **Step 3：寫失敗的 service 測試**：`get_info_lists_m7`（capabilities [1,2,3,4,5,6,8,11,12]）、`func_config_shape_motion`（GET 11 預設等於 Task 1 向量；SET 12 夾限並 dirty；同值再送不 dirty；capabilities 沒有 11 → GET 11 `BAD_PARAM`）、`learn_qr_flow`（SET_FUNCTION 6 → LEARN 6 → DEFERRED → `mxs_finish_learn_qr("LEFT")` → outbox `0xB0 01`、dirty；同內容再學 → `0xB0 01`、`data_gen` 不變；NULL → BAD_PARAM；學滿 → FULL）、`learn_shape_motion_rejected`、`result_data`（`mxs_set_results` 2 筆＋`mxs_set_result_data` 1 筆 → GET_RESULT_DATA 0 回內容與快取的 frame_id、1 回 len 0、2 → BAD_PARAM、長度 0 或 2 → BAD_LENGTH；開機還沒有結果 → BAD_PARAM；之後一批不帶資料 → 0 回 len 0；SET_FUNCTION 改功能後 → BAD_PARAM）、`learned_data`（GET_LEARNED_DATA 6 id 1 → "LEFT"；id 2（沒學）→ BAD_PARAM；fn 1 → BAD_PARAM；長度 2 → BAD_LENGTH）、`forget_qr`（id 與 0xFFFF；名字一起刪）、`shape_names`（SET_NAME 11 id 3 OK、id 5 BAD_PARAM、GET_LEARNED 11 依 ID 排序、FORGET 11 0xFFFF 只刪形狀名字）、`motion_rules`（SET_NAME 12 → BAD_PARAM、FORGET 12 成功、GET_LEARNED 12 → BAD_PARAM）、`factory_reset_m7`、`save_snapshot_includes_qr`、`learn_11_bytes_still_rejected`（fn 6 → BAD_PARAM）。
- [ ] **Step 4：跑測試確認失敗**：store、service 兩個 host test（`build/store-host-test`、`build/service-host-test`）編譯或測試失敗。
- [ ] **Step 5：實作** store（`data_valid` 也檢查 shape／motion 參數範圍與 QR 表；`store_nvs.c` 的暫存與 blob 緩衝區跟著 `MXST_BLOB_MAX` 變大，仍為 static；更新 `mxcam_store.h` 的 blob 版面註解、Worst case 算式）與 service（`mxs_handle` 的註解補 6／11／12 與兩個新指令）。
- [ ] **Step 6：跑測試確認通過**：store、service、`arduino-lib/extras/host-test` 全部 PASS。
- [ ] **Step 7：Commit**
```bash
git add firmware/components/mxcam_store/mxcam_store.h firmware/components/mxcam_store/mxcam_store.c firmware/components/mxcam_store/store_nvs.c firmware/components/mxcam_store/host-test/test_store.c firmware/components/mxcam_service/mxcam_service.h firmware/components/mxcam_service/mxcam_service.c firmware/components/mxcam_service/host-test/test_service.c
git commit -m "feat(firmware): store and service support for QR, shapes and motion" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 5：處理流程整合（韌體）＋板上量測

（C 組：Task 2、3、4 都 commit 之後才開工；整個 M7 第一次跑 `idf.py build` 的地方。）

**檔案：**
- 新增：`firmware/main/scan.{c,h}`
- 修改：`firmware/main/{pipeline.c,main.c,CMakeLists.txt}`、`.github/workflows/ci.yml`；必要時 `firmware/components/mxcam_qr/mxcam_qr.c`（修 Task 2 沒編譯過的錯誤）

**介面：**
- 消耗：`mxq_init`、`mxq_detect`、`mxq_to_result`、`mxq_pick_for_learn`、`mxq_label`、`mxq_code_t`（Task 2）；`mxsh_detect`、`mxsh_label`、`mxm_detect`、`mxm_reset`（Task 3）；`mxst_qr_lookup`、`mxs_set_result_data`、`mxs_finish_learn_qr`、`mxs_data_t` 新欄位（Task 4）；`mxo_draw_line`（既有）
- 產出（`scan.h`）：
```c
/* QR (6), shapes (11) and motion (12) glue for the pipeline task. Call only from the pipeline task. */
void    scan_sync(const mxs_state_t *s);   /* 持鎖：data_gen 變了才複製 shape_cfg / motion_cfg / qr 表（static，EXT_RAM_BSS_ATTR） */
void    scan_on_function(uint8_t fn);      /* 功能改變：mxm_reset、清掉上一張的 QR 碼 */
/* 不持鎖：6 → mxq_detect（MXQ_SKIPPED 回 -1）＋mxst 對照查 id＋mxq_to_result；11 → mxsh_detect；12 → mxm_detect */
int     scan_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out, uint8_t cap);
void    scan_publish_data(mxs_state_t *s, uint8_t fn); /* 持鎖、在 mxs_set_results 之後：功能 6 時把這張的碼內容交給 mxs_set_result_data */
/* LEARN 6（已 claim），在 scan_run 之後用這張的碼：mxq_pick_for_learn → 取鎖 mxs_finish_learn_qr（沒有碼傳 NULL）→ 重新 scan_sync。回傳是否排入回應。 */
bool    scan_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req);
void    scan_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res); /* 決定 16 */
```

- [ ] **Step 1：整合 `pipeline.c`**（只加這三個功能需要的部分）：持鎖複製 ctx 時呼叫 `scan_sync`；功能改變時 `scan_on_function`；LEARN 的 claim 若 `lreq.function_id == MXP_FUNC_QR`，延到辨識之後呼叫 `scan_learn`（成功排入回應就 `mxcam_link_kick_deferred()`）；`ctx.fn` 是 6／11／12 時 `n = scan_run(...)`，`n < 0` 時照手勢的 `MXH_SKIPPED` 處理（不發布）；`mxs_set_results` 之後在同一個鎖內呼叫 `scan_publish_data`；`draw_results` 之後對三個功能呼叫 `scan_draw`；`fn_name` 加 `QR`、`SHAPE`、`MOTION`；1 Hz log 在本秒有跑 QR 時另印一行 `qr_ms=avg/max/runs gray=avg id=avg dec=avg grids=n ok=n fail=n`。
- [ ] **Step 2：`main.c`**：`mxh_init` 之後、建立 task 之前，同樣條件（`display_ok && camera_ok`）呼叫 `mxq_init`；能力清單：`mxq_init` 成功加 6，`display_ok && camera_ok` 時加 11、12；開機 log 印出學過幾個 QR。`main/CMakeLists.txt` 的 `REQUIRES` 加 `mxcam_qr mxcam_shape mxcam_motion`。
- [ ] **Step 3：CI**：`ci.yml` 的 host-tests 加 `build/qr-host-test`、`build/shape-host-test`、`build/motion-host-test` 各三行（cmake／build／ctest），只加這九行。
- [ ] **Step 4：建置與預算**：PowerShell `idf.py build` 通過（修好 Task 2 的 quirc 包裝）；`idf.py size` 記下映像大小 **≤ 3,774,873 B**、quirc 與三個元件各佔多少（`idf.py size-components`），超過就停下來回報主對話。
- [ ] **Step 5：實作者自己的冒煙測試**（`idf.py -p COM5 flash`；log 看 COM9；先關掉所有 Studio 分頁；pyserial `dtr=False, rts=False` 再 `open()`）：開機 log 的 `qr ready` 記憶體數字；GET_INFO → [1,2,3,4,5,6,8,11,12]；GET_FUNC_CONFIG 11／12 → 預設；SET_FUNCTION 6／11／12 各 30 秒（鏡頭對著一般桌面）：`proc_ms`、fps、`qr_ms`、內部 heap `min` ≥ 60 KB、PSRAM、stack 高水位（決定 3：QR 時剩 < 4 KB 就把 `PIPE_STACK` 改 32 KB 並重量）；功能 6→11→12→8→6 切換 30 次後 heap 不下降。
- [ ] **Step 6：🧑‍🔧 板上延遲（主對話＋使用者）**：使用者拿手機顯示一張 QR（例如內容 `HELLO`）、再一張長網址、再兩張同時；印出的圓／三角形／正方形／長方形；在鏡頭前揮手。記錄：QR 一張／兩張時的 `qr_ms`（avg/max、identify、decode）與 TFT fps、GET_RESULTS＋GET_RESULT_DATA 讀到的內容；LEARN 6 回 `0xB0 01`；形狀每張時間與四種的 id；動態每張時間與揮手的框。不達決定 15 的目標時照決定 15 的退路調整並重量。數字寫在報告裡（Task 8 整理進 m7.md）。
- [ ] **Step 7：Commit**
```bash
git add firmware/main/scan.c firmware/main/scan.h firmware/main/pipeline.c firmware/main/main.c firmware/main/CMakeLists.txt .github/workflows/ci.yml
git commit -m "feat(firmware): QR, shapes and motion end-to-end" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```
（Step 4 修了 `mxcam_qr.c` 或 Step 5 改了 `PIPE_STACK`／`MXQ_MAX_CODES` 時，把該檔案也加進 `git add`。）

### Task 6：Studio 裝置層（真板子、展示模式、假韌體）

（B 組：Task 1 commit 之後開工，與 Task 2、3、4 同時；只改 `pc-app/src/{ui/layout/types.ts,device/,testing/}`。開工前跑一次 `npm test` 確認基準是綠的。）

**檔案：**
- 修改：`pc-app/src/ui/layout/types.ts`（`FunctionId`）、`pc-app/src/device/{types.ts,RealDevice.ts,DemoDevice.ts,config.ts}`、`pc-app/src/testing/fakeFirmware.ts`
- 新增：`pc-app/src/device/qrText.ts`、`pc-app/src/device/demo/{qr.ts,shapes.ts,motion.ts}`
- 測試：`pc-app/src/device/{device.test.ts,realdevice.test.ts,config.test.ts,qrText.test.ts}`、`pc-app/src/device/demo/demo.test.ts`

**介面：**
- 消耗：Task 1 的 `FunctionId.Qr/Shape/Motion`、`Shape`、`ShapeConfig`、`MotionConfig`、`encodeGetResultData`、`parseResultData`、`encodeGetLearnedData`、`parseLearnedData`、`ResultRecord.extra`
- 產出：
```ts
export type FunctionId = 1 | 2 | 3 | 4 | 5 | 6 | 8 | 11 | 12
export type LearnableFn = 1 | 4 | 5 | 6
// Result.kind 加 'qr' | 'shape' | 'motion'；Result 加 text?: string（QR 內容，UTF-8 解碼，非法 byte 用 U+FFFD）、truncated?: boolean
// LearnedId 加 text?: string（QR）；LearnErrorCode 加 'no-code'（功能 6 的 BAD_PARAM；FULL 仍是 'full'）
export interface FunctionParams {
  …既有不變
  shape: { mode: 'dark' | 'light'; threshold: number /* 0 = 自動 */; minSize: number /* 16-240 */; maxResults: number /* 1-8 */ }
  motion: { threshold: number /* 5-100 */; minSize: number /* 8-240 */; maxResults: number /* 1-8 */ }
}
// defaultParams()：shape {dark, 0, 32, 4}；motion {25, 24, 4}；emptyLearned() 加 6、11、12
// device/qrText.ts（純邏輯，fetch 由呼叫端注入）
export class QrTextCache {
  constructor(fetch: (index: number) => Promise<{ frameId: number; text: string; truncated: boolean }>)
  /** 依中心距離 ≤ 32 px 把已知內容接到紀錄上；必要時（有紀錄對不上、或距上次讀 ≥ 1000 ms，且沒有讀取中）開始讀這批。 */
  annotate(results: Result[], frameId: number, nowMs: number): Result[]
  reset(): void
}
// demo/qr.ts
export interface DemoQr { cx: number; cy: number; size: number; angleDeg: number; text: string }
export function qrSceneAt(timeMs: number): DemoQr[]                // 兩張卡：'MXCam'、'LEFT'；可重現
export function detectQr(cards: DemoQr[], learned: LearnedId[]): Result[] // 角、外接框、id = 內容相符的學習 id
export function drawQrScene(ctx: CanvasRenderingContext2D, cards: DemoQr[]): void // 唯一碰 canvas 的函式（假 QR 圖樣）
// demo/shapes.ts
export function shapeSceneAt(timeMs: number): { id: number; cx: number; cy: number; size: number; angleDeg: number }[]
export function detectShapes(shapes: ReturnType<typeof shapeSceneAt>, p: FunctionParams['shape']): Result[]
export function drawShapeScene(ctx: CanvasRenderingContext2D, shapes: ReturnType<typeof shapeSceneAt>): void
// demo/motion.ts
export function motionSceneAt(timeMs: number): { x: number; y: number; r: number; moving: boolean }
export function detectMotion(s: ReturnType<typeof motionSceneAt>, p: FunctionParams['motion']): Result[] // 動的時候 1 筆 id 0
export function drawMotionScene(ctx: CanvasRenderingContext2D, s: ReturnType<typeof motionSceneAt>): void
```

- [ ] **Step 1：寫失敗的測試**：
  - `qrText.test.ts`：`attaches text by nearest centre`、`unmatched record triggers a fetch`、`fetch is throttled to once per second when all match`、`stale frame id discards text`（fetch 回的 `frameId` 不是那批 → 不更新、下一批再讀；審查重點 5）、`qr text with invalid utf-8`（bytes `41 ff 42` → `'A\uFFFDB'`，不丟例外；審查重點 2）、`only one fetch in flight`
  - `realdevice.test.ts`：`connect loads func config for 11 and 12`；`setParams shape and motion send SET_FUNC_CONFIG`；`qr results get text through GET_RESULT_DATA`（FakeFirmware 有兩張碼 → `Result.kind 'qr'`、`text`、`extra.corners`）；`learn(6) maps BAD_PARAM to no-code and FULL to full`；`learn(6) refreshes learned[6] with text`（GET_LEARNED 6＋GET_LEARNED_DATA）；`shape and motion results kinds`；`old firmware: no m7 requests`（`functionIds = [1,2,3,4,5,8]` → 不送 `0x21`／`0x35`／`0x44 0b`／`0x44 0c`，`supported` 不含 6／11／12；審查重點 5）；`func config unsupported drops shape`（列出 11 但 `0x44` 在 `unknown` 裡 → 連線成功、`supported` 不含 11）
  - `config.test.ts`：舊檔（沒有 shape／motion）可匯入並得到預設；`params.shape.minSize = 8` → `ConfigFileError`；`learned[11]` id 只能 1–4；檔案有 `learned[6]` → `colorsNeedRelearn` true；QR 名字只在板上同 id 的 `text` 相同時套用
  - `demo.test.ts`：`qrSceneAt is deterministic`、`detectQr returns corners and learned ids`、`demo learn(6) on a card learns its text`、`demo learn(6) on empty floor is no-code`、`detectShapes finds the four shapes`（ids 1–4 都出現、`maxResults` 2 → 2 筆）、`detectMotion reports only while moving`（停住時 0 筆）、`demo names shape ids`
  - `device.test.ts`：`emptyLearned` 含 6、11、12；`toFunctionIds([1,6,7,11,12,13])` → [1,6,11,12]
- [ ] **Step 2：`npx vitest run src/device`** → 失敗。
- [ ] **Step 3：實作**：FakeFirmware：`functionIds` 可含 6／11／12；`qrCodes: { record, bytes }[]`（測試設定）供 RESULTS 與 `0x21`；`0x35`；LEARN 6（用 `qrCodes` 決定 id／BAD_PARAM／FULL）；func config 11／12（夾限照 protocol.md §11）；FORGET／SET_NAME／GET_LEARNED 的 6／11／12；可設定 `0x21` 回 `UNKNOWN_CMD`。RealDevice：`FUNC_CONFIG_FNS` 加 11、12；`KIND` 加 6／11／12；功能 6 的 RESULTS 經 `QrTextCache.annotate`（fetch = `call(Cmd.GetResultData, …)`＋`parseResultData`＋`TextDecoder('utf-8', { fatal: false })`）；切換功能或斷線時 `reset()`；`refreshAfter([6])` 對每個學到的 ID 讀 GET_LEARNED_DATA 填 `text`；`learn(6, region)` 沿用 `learn` 的流程。DemoDevice：功能 6／11／12 畫各自的場景、結果用各自的 `detect*`；QR 學習與名字照既有 tag／color 的寫法。`config.ts` 匯出／匯入新欄位（optional）。
- [ ] **Step 4：`npm test`、`npm run lint`** → 全部通過。
- [ ] **Step 5：Commit**（先 `git status pc-app` 確認只有本 Task 的檔案）
```bash
git add pc-app/src/ui/layout/types.ts pc-app/src/device/types.ts pc-app/src/device/RealDevice.ts pc-app/src/device/DemoDevice.ts pc-app/src/device/config.ts pc-app/src/device/qrText.ts pc-app/src/device/demo/qr.ts pc-app/src/device/demo/shapes.ts pc-app/src/device/demo/motion.ts pc-app/src/testing/fakeFirmware.ts pc-app/src/device/device.test.ts pc-app/src/device/realdevice.test.ts pc-app/src/device/config.test.ts pc-app/src/device/qrText.test.ts pc-app/src/device/demo/demo.test.ts
git commit -m "feat(pc-app): device layer for QR, shapes and motion" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 7：Studio 介面（功能列、即時畫面、面板、多語系）

（C 組：Task 6 commit 之後開工，可與 Task 5 同時。樣式只用 MATRIXblock 改版的 class 與 token（ADR-0015），不改既有 CSS 規則。所有 `Record<FunctionId, …>` 表都要補 6／11／12，TypeScript 會擋漏掉的。）

**檔案：**
- 修改：`pc-app/src/ui/layout/{categories.ts,Sidebar.tsx}`、`pc-app/src/theme.css`（只新增 `--cat-qr`、`--cat-shape`、`--cat-motion`）
- 修改：`pc-app/src/ui/live/{LiveStage.tsx,labels.ts}`、`pc-app/src/ui/panel/{ResultsTab.tsx,LearnTab.tsx,ParamsTab.tsx,codeSnippets.ts}`、`pc-app/src/App.tsx`（`FN_KEY`、`SHOW_NAMES`、`CAN_LEARN`）
- 修改：`pc-app/src/i18n/{zh-TW.ts,en.ts}`；測試：`ui/live/{labels.test.ts,livestage.test.tsx}`、`ui/panel/panel.test.tsx`、`ui/layout/layout.test.tsx`
- 修改：`docs/studio-ui.md`

**介面：**
- 消耗：Task 6 的 `FunctionId`、`Result.kind/text/truncated`、`LearnedId.text`、`FunctionParams.shape/motion`、`Device.learn(6, …)`、`LearnError('no-code')`
- 產出：`Category` 加 `'qr' | 'shape' | 'motion'`；`FN_CATEGORY` 6 `'qr'`、11 `'shape'`、12 `'motion'`；`FN_ICON` 6 `ScanQrCode`、11 `Shapes`、12 `Activity`；Sidebar `FUNCTIONS`：`{ id: 6, key: 'fnQr' }` 放在 AprilTag 後面，`fnShape`、`fnMotion` 放最後；`CAN_LEARN[6] = true`、11／12 false；`SHOW_NAMES` 6、11 true，12 false
- i18n 新 key（兩種語言一致）：`fnQr`、`fnShape`、`fnMotion`、`catQr`、`catShape`、`catMotion`、`stageQr`、`stageShape`、`stageMotion`、`dragHintQr`、`learnedQr`、`shape1`…`shape4`（圓形／三角形／正方形／長方形、Circle／Triangle／Square／Rectangle）、`shapeNoLearn`、`shapeNamesHeading`、`motionNoLearn`、`motionLabel`（動態／Motion）、`qrUnlearned`（QR）、`qrNoParams`、`qrTruncated`、`learnErrorNoCode`、`paramShapeMode`、`paramShapeDark`、`paramShapeLight`、`paramMinSize`、`paramMotionThreshold`、`colContent`、`colShape`
- 介面內容：
  - labels：QR = `(名字 || (id ? 'ID ' + id : t('qrUnlearned')))`；形狀 = `(名字 || t('shape' + id)) + ' ' + confidence + '%'`；動態 = `t('motionLabel') + ' ' + confidence + '%'`；顏色：QR／形狀 `id ≥ 1` → `var(--id-${((id − 1) % 8) + 1})`、QR `id 0` → `var(--unlearned)`、動態 → `var(--cat-motion)`
  - LiveStage：QR 用 `extra.corners` 畫四邊形（鏡像時照 `mirror.ts` 換算每個角）＋標籤＋內容前 16 字（`truncated` 時加 `…`）；形狀畫框＋標籤；動態畫虛線框＋標籤；功能 6 可以框選學習（`dragHintQr`）
  - ResultsTab：功能 6 列 ID、名字、內容（完整，`truncated` 時加 `qrTruncated`）、框；功能 11 列形狀名稱、信心度、框；功能 12 列框與信心度
  - LearnTab：功能 6「在畫面上框住一張 QR Code 就會學起來（最多 8 個 ID）」＋已學清單（ID、內容、名字輸入、忘記）＋`learnErrorNoCode`；功能 11 `shapeNoLearn`＋4 列取名表（照手勢的取名表）；功能 12 `motionNoLearn`
  - ParamsTab：功能 11：模式（深色／淺色）、門檻（「自動」勾選＋0–255 滑桿）、最小尺寸（16–240）、最多幾個（1–8）；功能 12：靈敏度門檻（5–100，數字越小越靈敏）、最小尺寸（8–240）、最多幾個；功能 6 顯示 `qrNoParams`
  - codeSnippets：`ALGO` 6 `MXCAM_ALGO_QR`、11 `MXCAM_ALGO_SHAPE`、12 `MXCAM_ALGO_MOTION`；QR 範例用 `cam.readText(i, text, sizeof(text))` 印內容、`r.id` 是學到的 ID；形狀範例 `if (r.id == MXCAM_SHAPE_TRIANGLE)`；動態範例「`cam.available()` 有東西就是有東西在動，`r.x` 是位置」（設計決定 18）

- [ ] **Step 1：寫失敗的測試**：`layout.test.tsx`：功能列有 9 項、QR 在 AprilTag 後面、三個新項目的圖示與分類色；`supported` [1,2,3,4,5,8] 時三個新功能停用並有「需要更新韌體」。`labels.test.ts`：QR 沒學過 → `QR`、學過 → `ID 2`、有名字 → 名字；形狀 `三角形 88%`、有名字用名字；動態 `動態 63%`；顏色規則。`livestage.test.tsx`：功能 6 的結果畫出一個 `polygon`、4 個點，鏡像時每個點 x 換成 320 − x；內容文字出現；功能 6 可框選並呼叫 `onLearn`；功能 11、12 不能框選。`panel.test.tsx`：ParamsTab 功能 11 改最小尺寸 → `setParams({ shape: { …, minSize: 48 } })`；功能 12 改門檻 → `setParams({ motion: … })`；功能 6 顯示沒有參數；LearnTab 功能 6 列出學到的 ID 與內容、忘記呼叫 `forget(6, 2)`；`no-code` 錯誤訊息；功能 11 輸入名字送 `setName(11, 2, '屋頂')`；ResultsTab 功能 6 有內容欄；程式分頁三個 `MXCAM_ALGO_*`。既有 i18n 測試檢查兩種語言 key 一致。
- [ ] **Step 2：`npx vitest run src/ui`** → 失敗。
- [ ] **Step 3：實作**（照「介面內容」）。
- [ ] **Step 4：`npm run lint`、`npm test`、`npm run build`** → 全部通過；`npm run dev` 打開 http://localhost:5173/esp32S3_cam/ 用展示模式手動確認：QR 卡移動時四邊形跟著轉、內容顯示、框選學習後標籤變 `ID 1`、取名生效；形狀四種標籤正確；動態只有球在動時有框；鏡像開關正常。
- [ ] **Step 5：記錄**：`docs/studio-ui.md` §3（三個功能的標籤與顏色）、§4（形狀、動態參數；QR 沒有參數）、§5（學 QR、形狀取名、動態不需要學習）、§8（展示模式的三個場景）。
- [ ] **Step 6：Commit**（逐檔 add，先 `git status` 確認沒有別人的變更；`theme.css` 若有別人未提交的修改，用 `git add -p` 只加三個新 token）
```bash
git add pc-app/src/ui/layout/categories.ts pc-app/src/ui/layout/Sidebar.tsx pc-app/src/theme.css pc-app/src/ui/live/LiveStage.tsx pc-app/src/ui/live/labels.ts pc-app/src/ui/panel/ResultsTab.tsx pc-app/src/ui/panel/LearnTab.tsx pc-app/src/ui/panel/ParamsTab.tsx pc-app/src/ui/panel/codeSnippets.ts pc-app/src/App.tsx pc-app/src/i18n/zh-TW.ts pc-app/src/i18n/en.ts pc-app/src/ui/live/labels.test.ts pc-app/src/ui/live/livestage.test.tsx pc-app/src/ui/panel/panel.test.tsx pc-app/src/ui/layout/layout.test.tsx docs/studio-ui.md
git commit -m "feat(pc-app): QR, shapes and motion in Studio" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
```

### Task 8：🧑‍🔧 整體審查後的實機驗收與量測

（D 組：Task 1–7 都 commit、整體審查的修正也 commit 之後。）

**檔案：** 新增 `docs/measurements/m7.md`；修改 `docs/SPEC.md`（§3、§8、§10）、`CLAUDE.md`

- [ ] **Step 1：準備**：`idf.py -p COM5 flash`；Studio（`npm run dev`）連線。使用者準備：手機上 3 個 QR（`HELLO`、`LEFT`、一個 > 128 bytes 的網址），以及印在 A4 的 2 張 QR（約 3 cm 與 8 cm）；A4 印的實心圓、三角形、正方形、長方形、五邊形，加一張用粗筆畫的空心形狀。
- [ ] **Step 2：QR**：TFT 有四邊形與 `QR HELLO`、Studio 顯示內容；長網址顯示截斷標記；斜放 30°、靠畫面邊緣、拿遠到最小還讀得到的距離（記下 px）；同時 2 張都讀到；6 張時只回報 4 張（審查重點 1）；框選學習 `LEFT` → `ID 1`，取名「左轉」→ 標籤變名字；學 `HELLO` → `ID 2`；SAVE、拔電重插 → 對照與名字還在；FORGET 1 → `LEFT` 回到 `QR`；FACTORY_RESET 全清。
- [ ] **Step 3：形狀**：四種形狀各自正確（TFT 與 Studio）；轉 45° 的正方形仍是正方形；空心形狀正確；五邊形不回報；形狀一半拿出畫面外不回報；改最小尺寸、最多幾個生效；淺色模式（黑紙白形狀，若有）。
- [ ] **Step 4：動態**：板子放著不動、畫面靜止 → 沒有框；揮手 → 框跟著手；手停住 1–2 秒後框消失；開關燈 → 不出現整個畫面的大框；拿起板子晃 → 沒有大框（審查重點 3）；調靈敏度門檻有差。
- [ ] **Step 5：量測**（TTL 埠 1 Hz log）：三個功能的 `proc_ms`（avg/max）、QR 沒有碼／一張／兩張的 `qr_ms` 與 TFT fps（不串流／串流）、網頁預覽 fps、內部 heap 最低、PSRAM 剩餘、pipeline stack 高水位、映像大小（`idf.py size`）；功能 1–5、8 的 fps 與 m6／m8a 比沒有退步。
- [ ] **Step 6：寫 `docs/measurements/m7.md`**（格式比照 m6.md：日期、韌體 git、Studio git、IDF 版本；效能表；驗收清單逐條結果；已知限制，例如「QR 有碼時約 N fps」「六邊形以上算圓」「很淺的顏色在白紙上分不出來」「動態只適用固定鏡頭」）。
- [ ] **Step 7：更新文件**：SPEC §3 擴充功能表把 6、11、12 標記完成並附實測數字；§8 M7 標記完成；§10 新增實測發現的待辦；`CLAUDE.md` 的純 C 元件清單加 `mxcam_qr_post.c`、`mxcam_shape`、`mxcam_motion`，處理流程段落加 `scan.c`，模型載入那段補「`mxq_init` 也必須在建立 task 之前」。
- [ ] **Step 8：Commit 並 push**（push 前確認 CI 的 host-tests job 已含 qr、shape、motion）
```bash
git add docs/measurements/m7.md docs/SPEC.md CLAUDE.md
git commit -m "docs: M7 measurements and acceptance" -m "Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
git push
```
確認 CI 全綠、Pages 部署成功。
