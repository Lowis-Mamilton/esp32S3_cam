# M6 擴充功能前置＋線條追蹤＋物體追蹤 實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development（建議）或 superpowers:executing-plans 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> 🧑‍🔧 步驟由主對話跟使用者一起做（需要實物：白紙＋黑色電工膠帶、一個有花紋的玩具）。直接在 `main` 上 commit；路徑 `C:\dev\esp32S3_cam`。
> 設計問題已由使用者授權「直接採用建議」；下方「設計決定」列出每個決定與理由，執行時依各 Task 的「記錄」步驟寫進 SPEC／protocol.md／ADR。
> ADR 編號：0012、0013 已由 M4 計畫預留，0014 由 M5 計畫、0015 由 Studio 改版預留；本計畫用 **0016、0017、0018**（執行時若已被占用，用 0016 之後的下一個空號，並同步改本計畫內的引用）。
> **同時進行的工作：** M5（網頁燒錄／PWA）會改 `pc-app` 的 FirmwarePage、PWA、`serial/connection.ts` 與 `.github/workflows/pages.yml`；Studio 改版會改 `theme.css`、`ui/layout/*`、`LiveStage.tsx` 等（工作目錄有未提交的檔案）。本計畫的 Studio 變更只在 `device/`、`protocol/`、`testing/`、`ui/panel/`、`ui/live/`、`ui/layout` 的功能清單、`i18n/` 與 theme 的新 token，並排在 Task 6、7（韌體做完之後），開工前先 rebase。**永遠不要 `git add -A`**，只 add 本 Task 列出的檔案。

**目標：** 為功能 4–12 打好協定與架構基礎，並完成功能 4 線條追蹤、功能 5 物體追蹤：韌體偵測、TFT 疊圖、USB 推送、可存檔，MXCam Studio（真板子與展示模式）能學習、調參數、顯示結果。

**架構：** 協定只新增指令與欄位（GET_INFO 功能清單上限 32、`GET_FUNC_CONFIG`／`SET_FUNC_CONFIG` 各功能自己的參數、LEARN 可選 id、線條的 14 bytes extra），C 正本在 `arduino-lib/src/protocol/`、TS 在 `pc-app/src/protocol/`，兩邊跑同一組 `docs/protocol-vectors.json`。韌體新增三個純 C、可主機測試的元件：`mxcam_gray`（RGB565→灰階共用函式）、`mxcam_line`（灰階門檻＋分帶質心＋最小平方法）、`mxcam_track`（NCC 範本比對＋色彩直方圖找回）；`mxcam_store` 的 blob 升到 v2（可讀 v1）；處理流程新增 `main/tracking.c` 把兩個功能接進 `pipeline.c`，不讓 `pipeline.c` 再長大。

**技術：** ESP-IDF v5.5.6、純 C99（host test：CMake＋MinGW＋ctest）、React/TS（Vitest）。

**規格：** `docs/SPEC.md` §3（擴充功能表）、§5、§6；`docs/research/vision-functions.md` §4.2、§5、§7；`docs/protocol.md`；ADR-0006、0007、0009、0010、0011、0014（M5 計畫）。

## 設計決定（執行時寫進 SPEC／protocol.md／ADR；理由附在後面）

1. **GET_INFO 功能清單上限 32（Task 1，ADR-0016）**：公開常數 `MXP_INFO_MAX_FUNCS 32`，`mxp_info_t.function_ids[32]`，pack／unpack 對 > 32 失敗。理由：功能 ID 會到 12，舊的上限 8 會讓 GET_INFO 整包解析失敗；Arduino 函式庫還沒發佈（`MXCam.h` 是空殼），現在改沒有相容問題；R4 只多 24 bytes RAM。
2. **各功能自己的參數：新增 `GET_FUNC_CONFIG (0x44)`／`SET_FUNC_CONFIG (0x45)`，每個功能一個固定版面（Task 1，ADR-0016）**：payload `function_id u8, layout_version u8, body`；SET 整包取代、回應 `0xC5` 是夾限後的整包；GET 回 `0xC4`。不支援 func config 的功能（0–3、未知）→ `BAD_PARAM`；`layout_version` 不符 → `BAD_PARAM`；長度不符 → `BAD_LENGTH`。功能 1–3 的參數**留在 Config v1**，不搬。改到參數會設 `dirty`，SAVE 才寫 flash，FACTORY_RESET 回預設。理由：Config v1 的規則是「不改既有欄位」，改成 Config v2 會讓每加一個功能就改一次全域版面、主機要整包重寫；TLV 對 R4 的 C 程式碼多一層解析和測試面；固定版面＋各自版本號和 Config v1 一樣好夾限、好測。
3. **不新增能力位元（Task 1，ADR-0016）**：GET_INFO 的功能清單就是能力；有列出 4／5 的韌體就支援 func config；之後版面變動靠 `layout_version`。理由：少一個欄位，主機邏輯不變（Studio 已經靠功能清單決定哪些功能可選）。
4. **LEARN 可選 id（Task 1 定義、Task 4 實作）**：payload 9 bytes＝現在的語意；11 bytes＝後面多 `id (u16)`「學到指定 ID」。C 的 `mxp_learn_req_t` 加 `has_id, id`，TS `encodeLearn(fn, rect, id?)`。**M6 的韌體對 11 bytes 的 LEARN 一律回 `BAD_PARAM`**（M9 人臉辨識、自學習分類才用）。理由：格式現在定很便宜、向後相容；功能面 YAGNI。
5. **名字表維持全部功能合計 32 筆（Task 4）**：線條（4）不能取名；物體追蹤（5）只有 ID 1 可以取名。理由：M6 最多多 1 筆；`MXS_RESPONSE_MAX` 綁在名字表大小上，改了要連動回應緩衝區。M7（QR 內容→ID）或 M9 有 `mxdata` 時再加大，記到 SPEC §10。
6. **線條的 ResultRecord（Task 1，protocol.md）**：只回報一條主線，`id = 1`；沒有線或找到的帶 < 2 條 → 0 筆。`x, y` = 線段中點；`w, h` = 兩端點含端點的外接框（`|x1−x0|+1`、`|y0−y1|+1`，至少 2）；`confidence` = 找到線的帶數 ×100 ÷ 帶數；**extra 14 bytes**：`angle_cdeg i16`（0 = 垂直向上，真實影像上順時針為正，−9000～9000）、`offset_x i16`（底端 x − 160，正 = 線在右邊）、`x0, y0`（底端）、`x1, y1`（頂端）`i16 ×4`、`flags u8`（bit0 `BRANCH`：某一帶有 2 段以上的線；bit1 `CROSS`：某一帶有比 `max_width` 寬的深色段，即十字路口／T 字路口／橫線）、`reserved u8 = 0`。理由：R4 循跡只需要一條線＋角度＋偏移（PID），路口旗標讓「遇到路口停下／轉彎」不必自己判斷；岔路回報成多筆會讓學生程式變複雜；≤ 18 bytes，舊的 Arduino 解析不會失敗。偏移與角度都在真實影像座標（ADR-0010），機器人上鏡頭朝前時「右」就是車子的右邊。
7. **線的門檻（Task 2，ADR-0018）**：模式 `0` 自動深線（預設，黑膠帶白紙）、`1` 自動淺線、`2` 學到的顏色。自動＝Otsu，門檻值參數 `0` 表示自動、`1–255` 固定；Otsu 兩類平均差 < 40 視為「沒有線」（避免白紙雜訊被切成線）。學到的顏色沿用顏色功能的 HSV 規則（ADR-0009）：LEARN 功能 4 取區域的平均色（`mxc_sample`），存下來並把模式設成 2；FORGET 4 清掉並把模式改回 0；模式 2 但沒學過時照模式 0 跑。黑／白膠帶沒有飽和度，學不到（`BAD_PARAM`），Studio 提示改用自動模式。理由：黑線白紙是循跡教材的標準情境，不需要學；彩色膠帶（多條路線）才需要學。
8. **線條參數 Line config v1（body 8 bytes）**：`mode u8`（0–2，預設 0；>2 → 0）、`threshold u8`（0 = 自動，預設 0）、`roi_pct u8`（只看畫面下方幾 %，20–100，預設 60）、`bands u8`（4–12，預設 8）、`min_width u8`（線寬下限 px，1–100，預設 4）、`max_width u8`（線寬上限 px，預設 80；< min_width 時改成 min_width）、`color_tolerance u8`（0–100，預設 30，只在模式 2 用）、`reserved u8 = 0`。
9. **物體追蹤（Task 3，ADR-0018）**：一次一個目標，`id = 1`。LEARN 框選（現有區域語意，目前功能必須是 5）→ 灰階範本（半解析度，縮到 ≤ 32×32、≥ 8×8）＋32 格色彩直方圖＋學習時的框大小與平均色。區域裁到畫面內後小於 16×16，或灰階標準差 < 6（沒有紋理）→ `BAD_PARAM`。再 LEARN 就取代（名字清掉）。每張畫面在上次位置附近搜尋（半徑＝框的長邊），三個尺度 0.9／1.0／1.1（`scale = 0` 時只用 1.0），粗搜步長 2 再 ±1 細修；`confidence = round(max(0, NCC) × 100)`；低於門檻＝跟丟、**沒有紀錄**；跟丟時（`reacquire = 1`）用直方圖反投影在全畫面找候選位置、再用 NCC 確認。分數 ≥ max(門檻, 70) 時以 `adapt`% 慢慢更新「目前範本」，學到的範本保持不變（跟丟時回到學到的範本，避免漂移）。extra 0。理由：照研究的建議；只追一個目標符合 HUSKYLENS 的用法，R4 端最簡單。
10. **物體追蹤參數 Track config v1（body 8 bytes）**：`threshold u8`（10–95，預設 50）、`adapt u8`（0–50，預設 10）、`reacquire u8`（0／1，預設 1）、`scale u8`（0／1，預設 1）、`reserved u8 ×4 = 0`。
11. **學到的物體存進 NVS（Task 4）**：跟顏色一樣「LEARN 改記憶體、SAVE 才寫 flash」，放進同一個 store blob（v2）。大小：範本 ≤ 1024 B＋直方圖 64 B＋其他約 20 B，blob 最大約 2.8 KB（24 KB NVS 放得下，寫新 blob 時新舊並存也只要約 6 KB）。理由：SPEC §1 的用法是「設定好拔掉電腦、R4 單獨運作」，拔電腦通常等於斷電，只放 RAM 會讓學好的物體不見；同一個 blob 讓 SAVE 仍是一次原子寫入，FACTORY_RESET 自動清掉。blob v2 必須能讀 v1（使用者燒新韌體後學到的顏色、名字、設定都要保留）。
12. **分割表：M6 不改，訂下演進規則（Task 4，ADR-0017）**：`nvs`（0x9000，24K）、`phy_init`、`factory`（0x10000，4M）**永遠不動**；之後的新分割區只從 `storage`（0x410000 起）切。預定版面（M8 才真的改）：`mxdata`（NVS，256K，放在 0x410000，固定在最前面）→ esp-dl 指定名稱的模型分割區（`pedestrian_det`、`hand_det`、`hand_gesture_cls`、`human_face_feat`，大小 M8 實測後定）→ 剩下的仍叫 `storage`。理由：M6 需要的空間 24K NVS 就夠；模型大小要 M8 實測才知道，現在切等於改兩次；M5 的網頁燒錄預設寫 bootloader／分割表／app 並保留 0x9000 的 NVS，只要 NVS 位置不動，改分割表也不會清掉學到的資料，所以「之後若改分割表就強制完整清除」（M5 計畫的 ADR-0014）只在 nvs／phy／factory 移動時才需要。`mxdata` 放最前面，之後模型分割區變大變小都不會移動它。M8 起韌體要能處理「分割表是舊的、找不到分割區」（例如只燒了 app）：相關功能不列在 GET_INFO。
13. **灰階共用（Task 2）**：新元件 `mxcam_gray`，公式與 AprilTag 相同（`Y = (77R + 150G + 29B) >> 8`，通道先展開到 8 bit），提供逐點與 160×120（2×2 平均）兩種；線條與物體追蹤都只吃 RGB565 畫面、自己呼叫它（模組邊界簡單；只跑一個功能，各自一份 19.2 KB PSRAM 緩衝區沒問題）。`mxcam_tag` 不動。
14. **TFT 疊圖（Task 5）**：線條：ROI 上緣一條 1 px 白線、每帶的質心 4×4 小方塊、擬合線段 3 px，顏色用第 4 個 ID 色（cyan）；標籤 `A-8 O+20`（角度°、偏移 px，**協定的真實影像數值**），有路口加 ` X`、有岔路加 ` Y`。物體：框用第 5 個 ID 色，標籤 `OBJ 87`；跟丟時左上角功能名稱改成 `TRACK LOST`，沒學過時 `TRACK -`。所有文字照 ADR-0010 鏡像繪製，只用 ASCII。`mxcam_overlay` 新增 `mxo_draw_line`。
15. **效能目標**：線條每張 ≤ 10 ms、物體追蹤每張 ≤ 30 ms（平均），讓 TFT（不串流）維持 ≥ 10 fps（SPEC §3 傳統功能 ≥ 10 fps）；串流時的 fps 照實記錄。物體追蹤超過 30 ms 時依序：(1) 尺度搜尋改成每 2 張一次、(2) 粗搜步長 3、(3) 範本上限 24×24，把用了哪一項寫進 m6.md。
16. **Studio**：`Device.learnColor(region)` 改成 `learn(fn, region)`（fn = 1／4／5）；`LearnError` 加 `'no-target'`；設定檔（匯出／匯入）維持 `version: 1`，新增**可省略**的 `params.line`、`params.track`、`learned[4]`、`learned[5]`（舊檔照樣能匯入）；學到的線色與物體跟顏色一樣無法從檔案重建（`colorsNeedRelearn` 擴充成也涵蓋 4／5），物體的名字只在板上已學過物體時才套用。展示模式：功能 4 換成「地板＋黑線」場景（線會左右擺動、週期性出現一條橫線當路口），結果用與韌體相同的分帶擬合算；功能 5 沿用現有場景，框選到哪個物體就追那個物體，物體離開畫面＝跟丟、回來＝找回。功能列圖示：線條 lucide `Route`、物體 `Crosshair`。
17. **Arduino API（記錄給 M4，不在 M6 實作）**：`MXCAM_ALGO_LINE`（4）、`MXCAM_ALGO_TRACK`（5）；`MXCamResult` 在線條功能多 `angle`（度，int）、`offset`（px）、`flags`（`MXCAM_LINE_BRANCH`、`MXCAM_LINE_CROSS`），由共用 C 的 `mxp_unpack_line_extra` 解出。Studio 程式分頁的範例照這組名稱寫。

## 全域限制

- 協定只新增、不改既有欄位；新的測試資料只**加進** `docs/protocol-vectors.json`（不改既有項目），C 與 TS 都要通過；`encoded_hex` 的 CRC 用程式算（python 或 `encodeFrame`），不要手打
- `arduino-lib/src/protocol/` 維持純 C99（ADR-0006），不能引用 Arduino 或 ESP-IDF 標頭；`MXP_RESULT_EXTRA_MAX` 維持 18
- 座標一律 320×240 真實影像（ADR-0010）；ResultRecord 的 x/y 是中心、LEARN 區域是左上角；辨識路徑上不做任何翻轉
- 一次只跑一個功能；LEARN 的 `function_id` 必須等於目前功能，否則 `BAD_PARAM`；LEARN 延後回應沿用 ADR-0007
- 新的純 C 元件不在函式裡 malloc：工作緩衝區用 `EXT_RAM_BSS_ATTR` 靜態放 PSRAM（host build 時巨集為空，照 `mxcam_color.c` 的寫法）；`mxs_data_t` 變大後 `mxst_load` 的暫存改成 static（不放 stack）
- 處理流程（core 1）不能因 USB 阻塞；推送走 `mxcam_link_push`；每秒一行的統計 log 照舊（proc_ms 已含新功能）
- 內部 RAM 最低剩餘仍需 ≥ 60 KB（M3 量到約 105 KB）；Task 5 量測並寫報告
- TFT 文字只用 ASCII、鏡像繪製（`mxo_draw_text_mirrored`）
- host test 的 CMake 參數照現有元件：`-std=c99 -Wall -Wextra -Werror`；新的 host test 都要加進 `.github/workflows/ci.yml` 的 host-tests job
- 本機驗證指令：
  - host test（Git Bash）：`cmake -S <dir>/host-test -B build/<name>-host-test -G "MinGW Makefiles" && cmake --build build/<name>-host-test && ctest --test-dir build/<name>-host-test --output-on-failure`
  - 韌體（PowerShell）：`$env:IDF_TOOLS_PATH = "C:\Espressif"; . C:\Espressif\frameworks\esp-idf-v5.5.6\export.ps1; cd C:\dev\esp32S3_cam\firmware; idf.py build`
  - Studio（`pc-app/`）：`npm run lint`、`npm test`、`npm run build`
- 只 `git add` 本 Task 列出的檔案（其他代理在 `pc-app` 有未提交的工作）
- commit 訊息結尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`

## 審查重點

1. **陰影與光線不均（線條）**：畫面一半被陰影蓋住時，Otsu 可能把陰影切成「深色」。陰影區比 `max_width` 寬，不能被當成線回報（最多設 `CROSS`），線本身仍要找到。→ Task 2 `shadow_half_frame` 測試
2. **模式 2 但沒有線色／FORGET 發生在偵測中**：要退回模式 0，不能當機或卡在「永遠 0 筆」。→ Task 2 `learned_mode_without_color_falls_back`、Task 4 `forget_line_resets_mode`
3. **物體在畫面邊緣、框在畫面外、物體離開畫面**：學習區域要裁切；搜尋窗靠邊時不能讀到畫面外；離開畫面時要報跟丟、回來時找回。→ Task 3 `learn_region_clipped`、`object_leaves_and_returns`（含 canary 檢查緩衝區沒寫爆）
4. **舊的 NVS 資料（M2／M3 韌體存的 v1 blob）**：燒新韌體後顏色、名字、Config 必須還在，新功能用預設值。→ Task 4 `deserialize_v1_fixture`
5. **新舊版本混搭**：Studio 連舊韌體（沒有 4／5、`0x44` 回 UNKNOWN_CMD）時線條／物體顯示「需要更新韌體」且不送 `GET_FUNC_CONFIG`；extra_len 不是 0／18 的紀錄不能讓 `parseResults` 丟例外（新韌體配上之後的功能也不會整包失敗）。→ Task 1 `parseResults accepts unknown extra lengths`、Task 6 `old firmware: no func config requests`

## 檔案結構

| 檔案 | 職責 | Task |
|---|---|---|
| `arduino-lib/src/protocol/mxcam_proto.{h,c}` | INFO 32、func config codec、LEARN id、line extra codec、功能 ID 4／5、指令 0x44／0x45 | 1 |
| `arduino-lib/extras/host-test/{test_proto.c,gen_vectors.py}`、`docs/protocol-vectors.json` | C 測試與測試資料 | 1 |
| `pc-app/src/protocol/{messages.ts,messages.test.ts,vectors.ts}` | TS 編解碼 | 1 |
| `firmware/components/mxcam_gray/`（新） | RGB565BE → 灰階（逐點、160×120） | 2 |
| `firmware/components/mxcam_line/`（新） | 線條偵測（純 C）＋host-test | 2 |
| `firmware/components/mxcam_overlay/` | 新增 `mxo_draw_line` | 2 |
| `firmware/components/mxcam_color/mxcam_color.{h,c}` | 新增逐點比對 `mxc_match_px` | 2 |
| `firmware/components/mxcam_track/`（新） | 物體追蹤（純 C）＋host-test | 3 |
| `firmware/components/mxcam_store/` | blob v2、線色、物體、func config | 4 |
| `firmware/components/mxcam_service/` | 0x44／0x45、LEARN／FORGET／SET_NAME／GET_LEARNED 的功能 4／5 | 4 |
| `firmware/main/tracking.{c,h}`（新） | 功能 4／5 的每張畫面處理、LEARN 取樣、TFT 疊圖 | 5 |
| `firmware/main/{pipeline.c,main.c,CMakeLists.txt}` | 接上 tracking、能力清單、功能名稱 | 5 |
| `pc-app/src/device/*`、`device/demo/*`、`testing/fakeFirmware.ts` | 裝置層、展示模式、假韌體 | 6 |
| `pc-app/src/ui/{layout,live,panel}/*`、`i18n/*`、`theme.css`（只加 token） | 介面 | 7 |
| `docs/measurements/m6.md` | 實測與驗收 | 8 |

---

### Task 1：協定前置（C＋TS＋測試資料）

**檔案：**
- 修改：`arduino-lib/src/protocol/mxcam_proto.h`、`arduino-lib/src/protocol/mxcam_proto.c`
- 修改：`arduino-lib/extras/host-test/test_proto.c`、`arduino-lib/extras/host-test/gen_vectors.py`
- 修改：`docs/protocol-vectors.json`（只新增）
- 修改：`pc-app/src/protocol/messages.ts`、`pc-app/src/protocol/messages.test.ts`、`pc-app/src/protocol/vectors.ts`
- 修改：`docs/protocol.md`、`docs/SPEC.md`；新增：`docs/adr/0016-per-function-config-and-protocol-growth.md`

**介面：**
- 產出（C）：
```c
#define MXP_INFO_MAX_FUNCS 32
enum { MXP_FUNC_LINE = 4, MXP_FUNC_TRACK = 5 };            /* 加進既有 enum */
enum { MXP_CMD_GET_FUNC_CONFIG = 0x44, MXP_CMD_SET_FUNC_CONFIG = 0x45 };
/* mxp_info_t.function_ids 改成 [MXP_INFO_MAX_FUNCS] */
typedef struct { uint8_t function_id; uint16_t x, y, w, h; uint8_t has_id; uint16_t id; } mxp_learn_req_t;
        /* mxp_unpack_learn：len 9（has_id 0）或 11（has_id 1），其他 false */
#define MXP_FUNC_CONFIG_SIZE 10      /* function_id + layout_version + 8 bytes body（line v1、track v1 都是） */
#define MXP_LINE_CONFIG_VERSION 1
#define MXP_TRACK_CONFIG_VERSION 1
enum { MXP_LINE_MODE_DARK = 0, MXP_LINE_MODE_LIGHT = 1, MXP_LINE_MODE_LEARNED = 2 };
typedef struct { uint8_t mode, threshold, roi_pct, bands, min_width, max_width, color_tolerance; } mxp_line_config_t;
typedef struct { uint8_t threshold, adapt, reacquire, scale; } mxp_track_config_t;
void   mxp_line_config_defaults(mxp_line_config_t *c);   /* 0,0,60,8,4,80,30 */
void   mxp_track_config_defaults(mxp_track_config_t *c); /* 50,10,1,1 */
size_t mxp_pack_line_config(const mxp_line_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
bool   mxp_unpack_line_config(const uint8_t *p, uint16_t len, mxp_line_config_t *out);   /* len 10、fn 4、ver 1；夾限 */
size_t mxp_pack_track_config(const mxp_track_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
bool   mxp_unpack_track_config(const uint8_t *p, uint16_t len, mxp_track_config_t *out); /* len 10、fn 5、ver 1；夾限 */
#define MXP_LINE_EXTRA_SIZE 14
#define MXP_LINE_FLAG_BRANCH 0x01
#define MXP_LINE_FLAG_CROSS  0x02
typedef struct { int16_t angle_cdeg, offset_x, x0, y0, x1, y1; uint8_t flags; } mxp_line_extra_t;
void   mxp_pack_line_extra(const mxp_line_extra_t *e, mxp_result_t *r);           /* 設 extra_len = 14 */
bool   mxp_unpack_line_extra(const mxp_result_t *r, mxp_line_extra_t *out);       /* extra_len != 14 → false */
```
- 產出（TS，`messages.ts`）：`Cmd.GetFuncConfig = 0x44`、`Cmd.SetFuncConfig = 0x45`；`FunctionId.Line = 4`、`FunctionId.Track = 5`；`encodeLearn(fn, rect, id?: number)`（有 id 時 11 bytes）；`interface LineConfig { mode: 0|1|2; threshold; roiPct; bands; minWidth; maxWidth; colorTolerance }`、`interface TrackConfig { threshold; adapt; reacquire: boolean; scale: boolean }`；`encodeFuncConfig(c: { functionId: 4; line: LineConfig } | { functionId: 5; track: TrackConfig }): Uint8Array`、`parseFuncConfig(payload): 同一個聯集型別`（長度、fn、版本不符就丟錯；不夾限，夾限是韌體的事）；`encodeGetFuncConfig(fn)`；`ResultRecord` 新增 `line?: { angleDeg: number; offsetX: number; start: [number, number]; end: [number, number]; branch: boolean; cross: boolean }`；`parseResults` 依 `functionId` 解 extra：2 且 18 bytes → `extra`（既有），4 且 14 bytes → `line`，**其他長度 0–18 一律略過不丟錯**，> 18 才丟錯
- 夾限規則（line）：mode > 2 → 0；roi_pct 夾 20–100；bands 夾 4–12；min_width 夾 1–100；max_width < min_width → min_width；color_tolerance 夾 0–100；reserved 寫 0。（track）：threshold 夾 10–95；adapt 夾 0–50；reacquire、scale 非 0 → 1

- [ ] **Step 1：新增測試資料**（`protocol-vectors.json`，用程式算 CRC）。frames：`get_info_response_m6`（功能 [1,2,3,4,5]）、`get_func_config_line_request`（cmd 0x44，payload `04`）、`func_config_line_default`（cmd 0xC4，payload `04 01 00 00 3c 08 04 50 1e 00`）、`set_func_config_track_request`（cmd 0x45，payload `05 01 3c 14 01 00 00 00 00 00`）、`learn_request_with_id`（cmd 0x30，fn 5，x 100、y 80、w 40、h 40、id 1）、`results_line_one`（cmd 0x61）。payloads：`line_config_default`、`track_config_set`（60, 20, 1, 0）、`learn_request_with_id`、`results_line_one`＝`{frame_id: 77, function_id: 4, results: [{id: 1, x: 170, y: 164, width: 21, height: 135, confidence: 88, line: {angle_cdeg: -849, offset_x: 20, x0: 180, y0: 231, x1: 160, y1: 97, flags: 2}}]}`。`gen_vectors.py` 為新 payload 產生 C 常數（`VEC_P_LINE_CONFIG_DEFAULT` 等）；`vectors.ts` 的 `PayloadVectors` 加型別。
- [ ] **Step 2：寫失敗的 C 測試**（`test_proto.c`）：`test_info_32`（32 個 ID 往返相同；33 個 pack 回 0、unpack 回 false；`get_info_response_m6` 解出 5 個 ID）；`test_func_config_vectors`（default 往返 bytes 相同、track 向量解出 60/20/1/0）；`test_func_config_clamp`（line `{fn4,v1, 7,0,5,30,0,0,150,9}` → mode 0、roi 20、bands 12、min 1、max 1、tol 100，再 pack 時 reserved 是 0；track `{fn5,v1, 0,80,2,5,…}` → 10、50、1、1）；`test_func_config_reject`（len 9、fn 錯、ver 2 → false）；`test_learn_with_id`（向量 11 bytes → has_id 1、id 1；9 bytes → has_id 0；10 bytes → false）；`test_line_extra`（向量往返、`extra_len` 13 → unpack false、`mxp_unpack_results` 收 14 bytes 的紀錄正常）。
- [ ] **Step 3：跑 C 測試確認失敗**：`cmake -S arduino-lib/extras/host-test -B build/host-test -G "MinGW Makefiles" && cmake --build build/host-test` → 編譯失敗（缺符號）即可。
- [ ] **Step 4：實作 C**（照「介面」）。`INFO_MAX_FUNCS` 改用公開的 `MXP_INFO_MAX_FUNCS`。
- [ ] **Step 5：跑 C 測試確認通過**：`ctest --test-dir build/host-test --output-on-failure` → 全部 PASS。
- [ ] **Step 6：寫失敗的 TS 測試**（`messages.test.ts`）：`parseInfo reads 5 function ids`（m6 向量）、`func config vectors round trip`（line default、track set）、`parseFuncConfig rejects wrong fn/version/length`、`encodeLearn with id`（等於向量）、`parseResults decodes line extra`（`angleDeg` −8.49、`offsetX` 20、`start` [180,231]、`end` [160,97]、`branch` false、`cross` true）、`parseResults accepts unknown extra lengths`（功能 9、extra_len 5 → 不丟錯、沒有 `extra`／`line`）、`parseResults rejects extra_len 19`。
- [ ] **Step 7：`cd pc-app && npx vitest run src/protocol`** → 失敗；實作 TS；再跑 → PASS；`npm run lint`。
- [ ] **Step 8：記錄**：`docs/protocol.md` §4 加功能 4、5（M6）；§5 指令表加 `0x44 GET_FUNC_CONFIG`、`0x45 SET_FUNC_CONFIG`（M6）；§6 GET_INFO 註明 `function_count` ≤ 32；檔尾新增「§9 M6 指令細節」（§8 已由 M4 計畫預留給 I2C）：GET／SET_FUNC_CONFIG 的 payload、錯誤、dirty／SAVE／FACTORY_RESET 規則；Line config v1、Track config v1 欄位表（預設、範圍、夾限，設計決定 8、10）；LEARN 的 11 bytes 格式（M6 一律 `BAD_PARAM`）；功能 4、5 的 LEARN／FORGET／SET_NAME／GET_LEARNED 規則（設計決定 7、9：LEARN 4 學線色並把模式設成 2、回應 id 1；LEARN 5 成功也回 id 1；FORGET 4 改回模式 0、4 不能取名、GET_LEARNED 4 回 0 或 1 筆〔id 1、線色〕；5 LEARN 取代舊物體、SET_NAME 5 只接受已學的 id 1、FORGET 5 清物體與名字、GET_LEARNED 5 回 0 或 1 筆〔id 1、平均色、名字〕）；線條的 ResultRecord 與 extra（設計決定 6）、物體的 ResultRecord（id 1、confidence、extra 0、跟丟時沒有紀錄）。寫 `docs/adr/0016-per-function-config-and-protocol-growth.md`（設計決定 1–4；Considered Options：Config v2、TLV、能力位元）。SPEC §3「架構前置」那行標註 M6 完成的部分並指向 ADR-0016；§5「辨識結果」加線條 extra 一句；§8 表格加 M6 列：「架構前置、線條追蹤、物體追蹤｜兩個功能 TFT ≥ 10 fps（不串流）、實機驗收通過」。
- [ ] **Step 9：Commit**
```bash
git add arduino-lib/src/protocol/mxcam_proto.h arduino-lib/src/protocol/mxcam_proto.c arduino-lib/extras/host-test/test_proto.c arduino-lib/extras/host-test/gen_vectors.py docs/protocol-vectors.json pc-app/src/protocol/messages.ts pc-app/src/protocol/messages.test.ts pc-app/src/protocol/vectors.ts docs/protocol.md docs/SPEC.md docs/adr/0016-per-function-config-and-protocol-growth.md
git commit -m "feat(protocol): M6 groundwork: 32 functions, per-function config, LEARN id, line extra" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2：灰階共用、線條偵測 `mxcam_line`、畫線

**檔案：**
- 新增：`firmware/components/mxcam_gray/{CMakeLists.txt,mxcam_gray.h,mxcam_gray.c}`
- 新增：`firmware/components/mxcam_line/{CMakeLists.txt,mxcam_line.h,mxcam_line.c}`、`firmware/components/mxcam_line/host-test/{CMakeLists.txt,test_line.c}`（同時編 `mxcam_gray.c`、`mxcam_color.c`、`mxcam_proto.c`，並測 gray）
- 修改：`firmware/components/mxcam_color/mxcam_color.{h,c}`（新增 `mxc_match_px`）、`firmware/components/mxcam_color/host-test/test_color.c`
- 修改：`firmware/components/mxcam_overlay/mxcam_overlay.{h,c}`、`firmware/components/mxcam_overlay/host-test/test_overlay.c`
- 修改：`.github/workflows/ci.yml`（host-tests 加 line）；新增：`docs/adr/0018-own-line-and-object-tracking-in-c.md`；修改：`docs/SPEC.md` §3

**介面：**
- 消耗：Task 1 的 `mxp_line_config_t`、`mxp_pack_line_extra`、`MXP_LINE_FLAG_*`、`mxp_result_t`
- 產出：
```c
/* mxcam_gray.h */
#define MXG_HALF_W 160
#define MXG_HALF_H 120
uint8_t mxg_luma_be(uint16_t px);                                   /* 與 mxt_luma_be 同值 */
void    mxg_half(const uint16_t *frame /*320x240 RGB565BE*/, uint8_t *out /*160x120*/); /* 2x2 平均 */
/* mxcam_color.h */
bool mxc_match_px(uint16_t px_be, const uint8_t rgb[3], uint8_t tolerance);   /* 與 mxc_same_color 同一個 HSV 窗 */
/* mxcam_overlay.h */
void mxo_draw_line(mxo_canvas_t *c, int x0, int y0, int x1, int y1, int thickness, uint16_t color); /* Bresenham，裁切 */
/* mxcam_line.h */
#define MXL_MAX_BANDS 12
#define MXL_MIN_CONTRAST 40   /* Otsu 兩類平均差下限 */
typedef struct { uint8_t has_last; int16_t last_x0; } mxl_state_t;      /* 呼叫端擁有；換功能時清零 */
typedef struct { uint8_t n; int16_t pts[MXL_MAX_BANDS][2]; int16_t roi_top; } mxl_trace_t; /* 疊圖用，320 座標 */
/* 回傳 0 或 1；line_rgb = NULL 表示沒學過（模式 2 時退回模式 0）；trace 可為 NULL。不可重入（static PSRAM 緩衝區）。 */
uint8_t mxl_detect(const uint16_t *frame, const mxp_line_config_t *cfg, const uint8_t *line_rgb,
                   mxl_state_t *st, mxp_result_t *out, mxl_trace_t *trace);
```
- 演算法（測試不完全決定的部分）：在 `mxg_half` 的 160×120 上，ROI＝最下面 `120 × roi_pct / 100` 列，切成 `bands` 條等高橫帶（餘數列給最上面那條丟掉），帶 0 在最下面。像素是否為「線」：模式 0 `gray < T`、模式 1 `gray > T`、模式 2（有 `line_rgb`）`mxc_match_px`；`T` = `threshold` 或 ROI 直方圖的 Otsu（兩類平均差 < 40 → 0 筆）。每帶每一欄的線像素數 ×2 ≥ 帶高 → 該欄「有線」；連續有線的欄＝一段，寬度（×2 換成 320 px）在 `[min_width, max_width]` 內才有效；比 `max_width` 寬 → 設 `CROSS`。預測位置：帶 0 用 `st->last_x0`（沒有就用畫面中央）；之後用前兩個選中帶外插（只有一個就用它）；選離預測最近的有效段（平手選離中央近的），距離 > max(8, 2 × 帶高)（半解析度 px）就跳過該帶，但帶 0 沒有距離限制；連續 2 帶跳過就停止往上找。某帶有 ≥ 2 個有效段 → `BRANCH`。選中的帶 < 2 → 0 筆（並清 `has_last`）。對選中帶的（中心列 y, 段中心 x）做最小平方法 `x = a·y + b`，端點取最下／最上選中帶的中心列，座標換回 320（`v*2+1`），`angle_cdeg = round(atan2f(x1−x0, y0−y1) × 18000/π)`，`offset_x = x0 − 160`，其餘欄位照設計決定 6，更新 `st->last_x0`。

- [ ] **Step 1：寫失敗的測試**（`test_line.c`，合成 320×240 RGB565BE：白底 `0xFFFF`、黑線 `0x0000`；用 helper `draw_thick_line(frame, x0,y0,x1,y1,width,color)`）：
  - `gray_values`：`mxg_luma_be` 白 255、黑 0、純紅（`0xF800`，存成 big-endian）77；`mxg_half` 2×2 平均
  - `vertical_line`：x=200、寬 12 → 1 筆，id 1、`|angle_cdeg| ≤ 100`、`offset_x` 40±2、`x0` 200±2、confidence 100、flags 0、extra_len 14
  - `tilted_line`：(100,239)→(220,96)，寬 12 → angle 與 `atan2f(120,143)` 換算值差 ≤ 200 cdeg、正值
  - `empty_frame`：全白 → 0 筆；`low_contrast_noise`：灰階 120–140 隨機（固定種子）→ 0 筆
  - `light_line_mode`：黑底白線，模式 1 → 1 筆；模式 0 → 0 筆
  - `fixed_threshold`：灰線（gray≈90）白底，threshold 100 → 1 筆
  - `branch_y`：從 (160,239) 到 (160,170) 的主幹，再分成往 (100,96) 與 (220,96) 兩條 → 1 筆，`flags & BRANCH`
  - `crossing`：x=160 直線＋y=150 寬 14 的整條橫線 → 1 筆，`flags & CROSS`、`|angle_cdeg| ≤ 100`
  - `learned_color`：灰底紅線，`line_rgb = {230,40,40}`、模式 2 → 1 筆；藍線 → 0 筆
  - `learned_mode_without_color_falls_back`：黑線白底，模式 2、`line_rgb = NULL` → 1 筆（審查重點 2）
  - `line_above_roi`：線只在 y < 60，roi_pct 60 → 0 筆；`short_stub`：只出現在最下面一帶 → 0 筆
  - `edge_line`：x=0..5 → 1 筆
  - `continuity`：兩條平行線 x=80、x=240，`st.last_x0` = 40（半解析度）→ x0 ≈ 80；`last_x0` = 120 → x0 ≈ 240
  - `shadow_half_frame`：左半邊 gray≈60 的陰影、右邊 x=220 的黑線 → 1 筆、x0 ≈ 220（審查重點 1）
  - `trace_points`：直線時 `trace.n` == 帶數、各點 x ≈ 200
  - `test_color.c` 加 `match_px_same_as_same_color`；`test_overlay.c` 加 `draw_line_horizontal_vertical_diagonal`（端點與中間點著色）、`draw_line_clips`（從畫面外穿過不寫出界，用 canary）、`draw_line_thickness`
- [ ] **Step 2：跑測試確認失敗**：`cmake -S firmware/components/mxcam_line/host-test -B build/line-host-test -G "MinGW Makefiles" && cmake --build build/line-host-test` → 編譯失敗；overlay、color 的 host test 同樣失敗。
- [ ] **Step 3：實作** `mxcam_gray`、`mxc_match_px`、`mxo_draw_line`、`mxcam_line`（照「介面」與演算法；`mxcam_line` 的 160×120 灰階與遮罩用 `EXT_RAM_BSS_ATTR`；元件 `CMakeLists.txt` 照 `mxcam_color` 的寫法，`REQUIRES mxcam_protocol mxcam_gray mxcam_color`）。
- [ ] **Step 4：跑測試確認通過**：line、overlay、color 三個 host test 的 `ctest --output-on-failure` 全部 PASS。
- [ ] **Step 5：CI 與建置**：`ci.yml` host-tests 加 `build/line-host-test` 三行（cmake／build／ctest）；PowerShell `idf.py build` 通過（元件還沒被 main 引用也要能編）。
- [ ] **Step 6：記錄**：寫 `docs/adr/0018-own-line-and-object-tracking-in-c.md` 的線條部分（設計決定 7、13；Considered Options：OpenMV `find_line_segments`／Hough——綁定 OpenMV 影像庫、較慢且不回傳單一主線；學習線色當預設——黑白膠帶學不到）；SPEC §3 在擴充功能表下加線條的說明（模式、預設只看下方 60%、≥ 2 帶才回報、一條主線＋路口／岔路旗標、偏移與角度是真實影像座標）。
- [ ] **Step 7：Commit**
```bash
git add firmware/components/mxcam_gray firmware/components/mxcam_line firmware/components/mxcam_color/mxcam_color.h firmware/components/mxcam_color/mxcam_color.c firmware/components/mxcam_color/host-test/test_color.c firmware/components/mxcam_overlay/mxcam_overlay.h firmware/components/mxcam_overlay/mxcam_overlay.c firmware/components/mxcam_overlay/host-test/test_overlay.c .github/workflows/ci.yml docs/adr/0018-own-line-and-object-tracking-in-c.md docs/SPEC.md
git commit -m "feat(firmware): line tracking module with host tests" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3：物體追蹤 `mxcam_track`

（依賴 Task 1 與 Task 2 的 `mxcam_gray`，所以在 Task 2 commit 之後開工；不依賴 `mxcam_line`。）

**檔案：**
- 新增：`firmware/components/mxcam_track/{CMakeLists.txt,mxcam_track.h,mxcam_track.c}`、`firmware/components/mxcam_track/host-test/{CMakeLists.txt,test_track.c}`
- 修改：`.github/workflows/ci.yml`、`docs/adr/0018-own-line-and-object-tracking-in-c.md`（物體部分）、`docs/SPEC.md` §3

**介面：**
- 消耗：`mxg_half`（Task 2）、`mxp_track_config_t`、`mxp_result_t`（Task 1）
- 產出：
```c
#define MXK_TMPL_MAX 32
#define MXK_TMPL_MIN 8
#define MXK_HIST_BINS 32     /* 12 色相 × 2 飽和度（S 0.2–0.5、≥ 0.5）＋ 8 個亮度格（S < 0.2） */
#define MXK_MIN_BOX 16       /* 320 px */
#define MXK_MIN_STD 6        /* 灰階標準差下限 */
typedef struct {
  uint8_t tw, th;                 /* 範本大小 8..32 */
  uint16_t box_w, box_h;          /* 學習時的框（320 px） */
  int16_t cx, cy;                 /* 學習時的中心（320 px） */
  uint8_t rgb[3];                 /* 框內平均色（GET_LEARNED 用） */
  uint16_t hist[MXK_HIST_BINS];   /* 正規化到總和 65535 */
  uint8_t tmpl[MXK_TMPL_MAX * MXK_TMPL_MAX]; /* 只用前 tw*th */
} mxk_model_t;
typedef struct { /* 呼叫端擁有，追蹤器內部狀態，不存檔 */
  uint8_t live[MXK_TMPL_MAX * MXK_TMPL_MAX];
  int16_t cx, cy; uint16_t w, h;  /* 目前框（320 px） */
  uint8_t lost; uint16_t lost_frames;
} mxk_state_t;
bool    mxk_learn(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h, mxk_model_t *out); /* 區域左上角，裁切；太小或沒紋理 → false，*out 不變 */
bool    mxk_model_valid(const mxk_model_t *m);        /* tw/th 範圍、box ≥ 16、cx/cy 在畫面內（store 反序列化用） */
void    mxk_reset(mxk_state_t *st, const mxk_model_t *m); /* live = 學到的範本，框 = 學習位置，lost = 0 */
uint8_t mxk_track(const uint16_t *frame, const mxk_model_t *m, const mxp_track_config_t *cfg,
                  mxk_state_t *st, mxp_result_t *out);   /* 0 或 1 筆；不可重入（static PSRAM 緩衝區） */
```
- 演算法（測試不完全決定的部分）：範本＝半解析度灰階框內區域，以面積平均縮到長邊 ≤ 32（不放大；短邊 < 8 時拉到 8）。比對在「範本尺度」進行：每個尺度 s，把搜尋窗（目前框中心 ± (框大小/2 ＋ 框長邊)，裁到畫面）從半解析度縮成範本尺度 ÷ s，對縮小後的窗建 sum／sum² 積分圖（u32），NCC 分母 O(1)、分子直接乘加；步長 2 粗搜，最佳點 ±1 細修。換回 320 座標，框大小＝學到的大小 × 累積尺度（夾 0.5–2.0）。跟丟且 `reacquire = 1`：在全畫面算直方圖反投影（每像素權重 = `hist[bin]`），用積分圖找目前框大小的最大和視窗，以它為中心再跑一次上面的 NCC 搜尋確認。範本更新：分數 ≥ max(門檻, 70)/100、`adapt > 0`，且該位置與**學到的**範本 NCC 也 ≥ 門檻時，`live += (patch − live) × adapt / 100`；跟丟時 `live` 回到學到的範本。

- [ ] **Step 1：寫失敗的測試**（`test_track.c`；目標＝40×40 的棋盤格〔8 px 格，紅白〕貼在灰色 `0x8410` 背景；`MXK_TEST_HOOKS` 開啟緩衝區 canary，照 `mxcam_color` 的 `mxc_test_canary_*`）：
  - `learn_ok`：區域 (100,80,40,40) → true、`tw`/`th` 在 8–32、`box_w` 40、`cx` 120
  - `learn_too_small`：(100,80,12,12) → false；`learn_flat`：純灰區域 → false
  - `learn_region_clipped`：(300,220,40,40) 目標貼在右下角 → 裁成 20×20 → true、`box_w` 20（審查重點 3）
  - `track_same_frame`：學完 `mxk_reset` 後 `mxk_track` 同一張 → 1 筆，中心誤差 ≤ 2、confidence ≥ 90、id 1、extra_len 0
  - `track_moved`：目標移 (+12, −8) → 中心誤差 ≤ 2
  - `track_scaled`：目標放大 1.1 倍 → 寬度誤差 ≤ 15%；`scale = 0` 時仍找得到（confidence 可較低）
  - `object_leaves_and_returns`：目標逐張往右移出畫面 → 移出後 0 筆、`st.lost`；在遠處（搜尋窗外，(40,160)）出現 → `reacquire = 1` 在 2 張內找回；`reacquire = 0` → 0 筆；全程 canary 完好（審查重點 3）
  - `distractor_rejected`：另一個同色但不同紋理（橫條紋）的方塊、原目標不在畫面 → 0 筆
  - `no_drift`：目標亮度每張 +1（共 40 張）→ 每張都找到；`adapt = 0` 時 `live` 不變
  - `model_valid`：`tw = 40` 或 `box_w = 4` → false
- [ ] **Step 2：跑測試確認失敗**：`cmake -S firmware/components/mxcam_track/host-test -B build/track-host-test -G "MinGW Makefiles" && cmake --build build/track-host-test` → 編譯失敗。
- [ ] **Step 3：實作** `mxcam_track.c`（半解析度灰階、反投影、積分圖、縮小窗都用 `EXT_RAM_BSS_ATTR` 靜態；HSV 換算可以從 `mxcam_color` 抽出共用或自寫，與 ADR-0009 的定義一致；`REQUIRES mxcam_protocol mxcam_gray`）。
- [ ] **Step 4：跑測試確認通過**：`ctest --test-dir build/track-host-test --output-on-failure` → PASS。
- [ ] **Step 5：CI 與建置**：`ci.yml` 加 track 三行；`idf.py build` 通過。
- [ ] **Step 6：記錄**：ADR-0018 加物體部分（設計決定 9、11；Considered Options：只用色彩直方圖的 CamShift——同色背景會飄；全畫面 NCC——太慢；學到的物體只放 RAM——斷電就不見，與「拔掉電腦單獨使用」衝突）；SPEC §3 加物體追蹤的說明（一次一個目標、需要有紋理、跟丟沒有紀錄、會自動找回、學到的物體 SAVE 後斷電仍在）。
- [ ] **Step 7：Commit**
```bash
git add firmware/components/mxcam_track .github/workflows/ci.yml docs/adr/0018-own-line-and-object-tracking-in-c.md docs/SPEC.md
git commit -m "feat(firmware): object tracking module with host tests" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4：store blob v2 與 service 指令

**檔案：**
- 修改：`firmware/components/mxcam_store/{mxcam_store.h,mxcam_store.c,store_nvs.c,CMakeLists.txt}`、`firmware/components/mxcam_store/host-test/{CMakeLists.txt,test_store.c}`
- 修改：`firmware/components/mxcam_service/{mxcam_service.h,mxcam_service.c,CMakeLists.txt}`、`firmware/components/mxcam_service/host-test/{CMakeLists.txt,test_service.c}`
- 新增：`docs/adr/0017-partition-layout-evolution.md`；修改：`docs/SPEC.md` §6.1、§10

**介面：**
- 消耗：Task 1 的 func config codec、`mxp_learn_req_t.has_id`；Task 3 的 `mxk_model_t`、`mxk_model_valid`
- 產出：
```c
/* mxcam_store.h */
#define MXST_FN_LINE 4
#define MXST_FN_TRACK 5
typedef struct {
  mxp_config_t cfg; mxc_learned_t colors[MXST_MAX_COLORS]; uint8_t n_colors;
  mxs_name_t names[MXST_MAX_NAMES]; uint8_t n_names;            /* 以上同 v1 */
  mxp_line_config_t line_cfg; mxp_track_config_t track_cfg;
  uint8_t line_has_color; uint8_t line_rgb[3];
  uint8_t has_object; mxk_model_t object;
} mxs_data_t;
/* blob v2：v1 的內容（version = 2）| line func config 10 B | track func config 10 B |
 * line_has u8 [rgb 3] | obj_has u8 [tw, th u8, box_w, box_h u16, cx, cy i16, rgb 3, hist 32×u16, tmpl tw*th] | CRC16
 * MXST_BLOB_MAX 依此重算（約 2.8 KB）。v1 blob 仍可讀：新欄位用預設、沒有線色、沒有物體。 */
void mxst_set_line_color(mxs_data_t *d, const uint8_t rgb[3]); /* NULL = 清除，line_cfg.mode 從 2 改回 0；非 NULL → mode 2 */
void mxst_set_object(mxs_data_t *d, const mxk_model_t *m);     /* NULL = 清除；兩種都刪掉 (5,1) 的名字 */
/* name_key_ok：fn 5 只接受 id 1；fn 4 一律 false；fn 3 一律 false（順便完成 SPEC §10 M3 審查項目） */

/* mxcam_service.h */
bool mxs_finish_learn(mxs_state_t *s, uint8_t seq, const uint8_t *rgb);           /* 功能 1（照舊）與 4（mxst_set_line_color，回 id 1） */
bool mxs_finish_learn_object(mxs_state_t *s, uint8_t seq, const mxk_model_t *m);  /* 功能 5；NULL → BAD_PARAM */
/* GET_INFO 的緩衝區改成 23 + MXP_INFO_MAX_FUNCS；MXS_RESPONSE_MAX 不變 */
```
- 規則：LEARN 11 bytes → `BAD_PARAM`；LEARN 功能 4／5 只在目前功能相同時接受（延後回應，照功能 1）；GET／SET_FUNC_CONFIG 只接受 4、5（不看能力清單），SET 只有值真的改變時 `mark_dirty`；FORGET 4（id 1 或 0xFFFF）清線色；FORGET 5（id 1 或 0xFFFF）清物體與名字；其他 id 冪等成功；SET_NAME 4 → `BAD_PARAM`、SET_NAME 5 需 id 1 且已學；GET_LEARNED 4／5 回 0 或 1 筆（照設計決定 7、9）；FACTORY_RESET 讓 func config 回預設。

- [ ] **Step 1：先抓 v1 樣本**（改程式前）：在 `test_store.c` 加 helper，用**目前的** `mxst_serialize` 對一組已知資料（2 個顏色、AprilTag 名字「門口」、tolerance 45）產生 bytes，印成 C 陣列，貼進測試當 `k_v1_fixture`（commit 後永遠不改）。
- [ ] **Step 2：寫失敗的 store 測試**：`deserialize_v1_fixture`（顏色、名字、tolerance 都對；`line_cfg`／`track_cfg` 是預設；沒有線色和物體，審查重點 4）、`roundtrip_v2_full`（有線色、物體 tw 32×th 24、名字 (5,1)）、`serialize_v2_max_fits`（全部填滿 ≤ `MXST_BLOB_MAX`）、`reject_bad_object`（tw = 40、或 tw*th 與長度不符 → false）、`set_line_color_mode`（設定 → mode 2；清除 → mode 0；原本 mode 1 時清除不改）、`set_object_clears_name`、`name_keys`（(4,1) false、(5,1) true、(5,2) false、(3,0) false）。
- [ ] **Step 3：寫失敗的 service 測試**：`get_info_lists_five`（capabilities [1..5]）、`func_config_get_default`、`func_config_set_clamps_and_dirty`（同值再送不設 dirty）、`func_config_bad`（fn 1 → BAD_PARAM、ver 2 → BAD_PARAM、len 9 → BAD_LENGTH）、`learn_with_id_rejected`、`learn_line_flow`（SET_FUNCTION 4 → LEARN 4 → DEFERRED → `mxs_finish_learn(rgb)` → outbox 有 `0xB0 01`、mode 2、dirty）、`learn_line_no_color`（NULL → BAD_PARAM）、`learn_object_flow`（`mxs_finish_learn_object`）、`learn_fn_mismatch`（目前功能 4、LEARN 5 → BAD_PARAM）、`forget_line_resets_mode`（審查重點 2）、`forget_object_and_name`、`set_name_rules`、`get_learned_line_and_object`、`factory_reset_func_config`、`save_snapshot_includes_object`。
- [ ] **Step 4：跑測試確認失敗**：store、service 兩個 host test（`-G "MinGW Makefiles"`）編譯失敗。host-test 的 `CMakeLists.txt` 要加 `mxcam_track` 的 include 與 `mxcam_track.c`、`mxcam_gray.c` 原始碼。
- [ ] **Step 5：實作** store（`store_nvs.c` 的 `mxs_data_t t` 改成 static；元件 `REQUIRES` 加 `mxcam_track`）與 service（`mxcam_service/CMakeLists.txt` 的 `REQUIRES` 加 `mxcam_track`）。
- [ ] **Step 6：跑測試確認通過**：store、service（以及 Task 1 的 proto）host test 全部 PASS；`idf.py build` 通過，記下 `size` 中 `.bss`／`.dram0` 增加量（`idf.py size`）。
- [ ] **Step 7：記錄**：寫 `docs/adr/0017-partition-layout-evolution.md`（設計決定 12：M6 不改分割表、nvs／phy／factory 永遠不動、之後只從 `storage` 切、預定版面與 `mxdata` 放最前面、只燒 app 時找不到分割區的處理；Considered Options：M6 就切好模型分割區——大小未知要改兩次；把 nvs 加大或搬家——M5 保留 NVS 的燒錄會讀不到舊資料；它也修正 M5 計畫 ADR-0014「改分割表就強制完整清除」的條件）；SPEC §6.1「設定與學習資料存在 NVS」補一句「blob v2 含線色與學到的物體（約 2.8 KB），可讀 v1」；SPEC §10 新增：名字表 32 筆在 M7／M9 加大（設計決定 5）；M4 計畫的 service 改寫要以 M6 之後的 service 為基礎（新增的 0x44／0x45 與功能 4／5 規則）；M4 的 Arduino API 加線條欄位（設計決定 17）；M3 審查項目「store 的 `name_key_ok` 接受人臉的 key」打勾。
- [ ] **Step 8：Commit**
```bash
git add firmware/components/mxcam_store firmware/components/mxcam_service docs/adr/0017-partition-layout-evolution.md docs/SPEC.md
git commit -m "feat(firmware): store and service support for line and object tracking" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5：處理流程整合（韌體）

**檔案：**
- 新增：`firmware/main/tracking.{c,h}`
- 修改：`firmware/main/pipeline.c`、`firmware/main/main.c`、`firmware/main/CMakeLists.txt`

**介面：**
- 消耗：`mxl_detect`、`mxl_state_t`、`mxl_trace_t`（Task 2）；`mxk_learn`、`mxk_reset`、`mxk_track`（Task 3）；`mxs_finish_learn`、`mxs_finish_learn_object`、`mxs_data_t` 新欄位（Task 4）；`mxo_draw_line`（Task 2）
- 產出（`tracking.h`）：
```c
/* 只在 pipeline task 呼叫。lock/unlock 由呼叫端提供的 mxcam_link_state_lock 包住的區段裡做的事寫在各函式註解。 */
void    tracking_sync(const mxs_state_t *s);   /* 持鎖時呼叫：s->data_gen 變了才複製 line_cfg/track_cfg/線色/物體 */
void    tracking_on_function(uint8_t fn);      /* 功能改變時：清 mxl_state_t、對物體 mxk_reset */
uint8_t tracking_run(uint8_t fn, const uint16_t *frame, mxp_result_t *out); /* 不持鎖；4 → mxl_detect，5 → mxk_track（沒學過回 0） */
/* LEARN（已 claim）：4 用 mxc_sample、5 用 mxk_learn；內部取鎖呼叫 mxs_finish_learn / _object，回傳是否有排入回應 */
bool    tracking_learn(mxs_state_t *s, uint8_t seq, const mxp_learn_req_t *req, const uint16_t *frame);
void    tracking_draw(mxo_canvas_t *c, uint8_t fn, const mxp_results_t *res); /* 設計決定 14 的疊圖與標籤 */
const char *tracking_status(uint8_t fn);       /* 左上角功能名稱："LINE"、"TRACK"、"TRACK LOST"、"TRACK -" */
```

- [ ] **Step 1：整合**：`pipeline.c` 在持鎖複製 ctx 時呼叫 `tracking_sync`；`ctx.fn` 與上一張不同時呼叫 `tracking_on_function`；LEARN 的 claim 依 `lreq.function_id` 分派：1 照舊、4／5 交給 `tracking_learn`（成功排入回應就 `mxcam_link_kick_deferred()`）；功能 4／5 時 `res.count = tracking_run(...)`；`draw_results` 之後對 4／5 呼叫 `tracking_draw`；左上角文字 4／5 用 `tracking_status`；`k_fn_names` 補 `"LINE"`、`"TRACK"`。
- [ ] **Step 2：能力清單**：`main.c` 的 `caps` 陣列大小改成 `MXP_INFO_MAX_FUNCS`，`display_ok && camera_ok` 時加入 4、5（純 C、靜態緩衝區，沒有初始化失敗的情況）；開機 log 印出線色／物體是否已學。`main/CMakeLists.txt` 的 `REQUIRES` 加 `mxcam_line mxcam_track mxcam_gray`。
- [ ] **Step 3：建置**：PowerShell `idf.py build` 通過；`idf.py size` 記下映像大小（factory 4 MB 的餘量）。
- [ ] **Step 4：🧑‍🔧 冒煙測試**（實作者可自行燒錄，`idf.py -p COM5 flash`；log 看 COM9）：先關掉所有 Studio 分頁；用 pyserial（`dtr=False, rts=False` 再 `open()`）送 GET_INFO → 功能 [1,2,3,4,5]；SET_FUNCTION 4，鏡頭對著白紙上的黑膠帶 → GET_RESULTS 有 1 筆、extra_len 14；GET_FUNC_CONFIG 4 → 預設值；SET_FUNCTION 5、LEARN 5 框住桌上一個有花紋的物體 → 回 `0xB0 01`，GET_RESULTS 有框；統計 log 的 `proc_ms`：線條 ≤ 10 ms、物體 ≤ 30 ms，`min`（內部 heap 最低）≥ 60 KB。超過時依設計決定 15 的順序調整，並記錄。把數字寫在報告裡（Task 8 整理進 m6.md）。
- [ ] **Step 5：Commit**
```bash
git add firmware/main/tracking.c firmware/main/tracking.h firmware/main/pipeline.c firmware/main/main.c firmware/main/CMakeLists.txt
git commit -m "feat(firmware): line and object tracking end-to-end" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 6：Studio 裝置層（真板子、展示模式、假韌體）

開工前先 `git pull --rebase`（或確認 M5／Studio 改版已 commit 的部分在 HEAD），並跑一次 `npm test` 確認基準是綠的。

**檔案：**
- 修改：`pc-app/src/ui/layout/types.ts`（`FunctionId` 型別）、`pc-app/src/device/{types.ts,RealDevice.ts,DemoDevice.ts,config.ts,resultsStore.ts}`、`pc-app/src/testing/fakeFirmware.ts`
- 新增：`pc-app/src/device/demo/line.ts`（線條場景與分帶擬合）、`pc-app/src/device/demo/track.ts`（框選→追蹤目標）
- 修改：`pc-app/src/device/demo/{scene.ts,recognize.ts}`；測試：`device/{device.test.ts,realdevice.test.ts,config.test.ts}`、`device/demo/demo.test.ts`

**介面：**
- 消耗：Task 1 的 TS 協定（`encodeFuncConfig`、`parseFuncConfig`、`encodeGetFuncConfig`、`ResultRecord.line`、`FunctionId.Line/Track`）
- 產出：
```ts
export type FunctionId = 1 | 2 | 3 | 4 | 5
export type LearnableFn = 1 | 4 | 5
export interface LineResult { angleDeg: number; offsetX: number; start: [number, number]; end: [number, number]; branch: boolean; cross: boolean }
// Result.kind 加 'line' | 'track'；Result 加 line?: LineResult
export interface FunctionParams {
  color: …; tag: …; face: …                        // 不變
  line: { mode: 'dark' | 'light' | 'learned'; threshold: number /* 0 = 自動 */; roiPercent: number; bands: number; minWidth: number; maxWidth: number; colorTolerance: number }
  track: { threshold: number; adapt: number; reacquire: boolean; multiScale: boolean }
}
// defaultParams()：line {dark, 0, 60, 8, 4, 80, 30}；track {50, 10, true, true}
// Device：learnColor(region) 改成 learn(fn: LearnableFn, region): Promise<number>
// LearnErrorCode 加 'no-target'（功能 5 的 BAD_PARAM）；功能 4 的 BAD_PARAM 仍是 'no-color'
// ImportOutcome.colorsNeedRelearn：檔案裡 learned[1]、[4]、[5] 任一有內容就 true
// demo/line.ts
export function lineSceneAt(timeMs: number): { path: (y: number) => number; crossY: number | null; width: number }
export function detectLine(scene: ReturnType<typeof lineSceneAt>, p: FunctionParams['line']): Result[] // 同韌體：ROI、分帶、最小平方法、旗標
export function drawLineScene(ctx: CanvasRenderingContext2D, timeMs: number): void
// demo/track.ts
export function pickTarget(objs: SceneObject[], region: Region): number | null // 與區域重疊 ≥ 30% 的物體索引，否則 null
```

- [ ] **Step 1：寫失敗的測試**：
  - `realdevice.test.ts`：`connect loads func config for 4 and 5`（FakeFirmware `functionIds = [1,2,3,4,5]` → 送出 `0x44 04`、`0x44 05`，snapshot `params.line` = 韌體值）；`setParams line sends SET_FUNC_CONFIG and applies the clamped reply`；`learn(4) maps BAD_PARAM to no-color`、`learn(5) maps BAD_PARAM to no-target`、`learn(5) refreshes learned[5]`；`line results carry line extra`（RESULTS 推送 → `Result.kind === 'line'`、`line.offsetX`）；`old firmware: no func config requests`（`functionIds = [1,2,3]` → 不送 0x44，`supported` 不含 4／5；`0x44` 在 `unknown` 裡時也不會連線失敗，審查重點 5）
  - `config.test.ts`：舊檔（沒有 line／track）可匯入並得到預設值；`params.line.roiPercent = 10` → `ConfigFileError`；`learned[5]` 的 id 只能是 1
  - `demo.test.ts`：`detectLine on a straight path`（offset、angle 與解析值一致 ±1）、`crossing sets cross`、`learn(4) on the black line is no-color`、`learn(5) on the red blob tracks it`（之後的結果框跟著該物體）、`learn(5) on empty floor is no-target`、`tracked blob off-frame yields no result`
  - `device.test.ts`：`emptyLearned` 含 4、5；`toFunctionIds([1,4,5,9])` → [1,4,5]
- [ ] **Step 2：`npx vitest run src/device`** → 失敗。
- [ ] **Step 3：實作**：FakeFirmware 支援 0x44／0x45（存 line／track config、夾限照 protocol.md §9）、LEARN 4／5（`learnRgb`、新的 `learnObjectOk` 旗標）、GET_LEARNED 4／5、FORGET 4／5；RealDevice 連線時對支援的 4／5 送 GET_FUNC_CONFIG（UNKNOWN_CMD／NOT_SUPPORTED 時把該功能從 `supported` 拿掉並記 log，不中斷連線）、`setParams({ line | track })` 走 SET_FUNC_CONFIG（沿用 `queued`）、`learn(fn, region)`（沿用 `learnColor` 的流程，`refreshAfter([fn])`）、`toResult` 對功能 4 帶 `line`；DemoDevice 依目前功能畫 `drawLineScene` 或原本的場景，功能 4 用 `detectLine`、功能 5 用 `pickTarget` 記住的物體；`config.ts` 匯出／匯入新欄位（optional）。呼叫 `learnColor` 的地方（`App.tsx`）改成 `learn(view.id, region)`——只改這一行與型別，不動 App 其他結構。
- [ ] **Step 4：`npm test`、`npm run lint`** → 全部通過。
- [ ] **Step 5：Commit**
```bash
git add pc-app/src/ui/layout/types.ts pc-app/src/device pc-app/src/testing/fakeFirmware.ts pc-app/src/App.tsx
git commit -m "feat(pc-app): device layer for line and object tracking" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
（`git add pc-app/src/device` 前先 `git status pc-app/src/device` 確認只有本 Task 的變更；有別人未提交的檔案就逐檔 add。）

### Task 7：Studio 介面（功能列、即時畫面、面板、多語系）

開工前再 rebase 一次；Studio 改版若已把功能清單移到 `ui/layout/categories.ts`（`FN_CATEGORY`、`FN_ICON`），就加在那裡；還沒有就加在 `Sidebar.tsx` 的 `FUNCTIONS`。樣式只用改版定下的 class 與 token，不改既有 CSS 規則。**如果 `categories.ts`、`theme.css`、`LiveStage.tsx` 等檔案仍是改版代理未提交的工作，不要動它們、更不要把它們 commit 進來：停下來回報主對話，等改版 commit 後再做。**

**檔案：**
- 修改：`pc-app/src/ui/layout/{categories.ts 或 Sidebar.tsx}`、`pc-app/src/theme.css`（只新增 `--cat-line`、`--cat-track` 兩個 token，亮／暗兩組）
- 修改：`pc-app/src/ui/live/{LiveStage.tsx,labels.ts}`、`pc-app/src/ui/panel/{ResultsTab.tsx,LearnTab.tsx,ParamsTab.tsx,codeSnippets.ts}`、`pc-app/src/App.tsx`（`canLearn` 條件）
- 修改：`pc-app/src/i18n/{zh-TW.ts,en.ts}`；測試：`ui/live/{livestage.test.tsx,labels.test.ts}`、`ui/panel/panel.test.tsx`、`ui/layout/layout.test.tsx`
- 修改：`docs/studio-ui.md`

**介面：**
- 消耗：Task 6 的 `Device.learn`、`FunctionParams.line/track`、`Result.line`、`LearnError('no-target')`
- 產出：功能列多兩項（4 線條追蹤／Line tracking，`Route`；5 物體追蹤／Object tracking，`Crosshair`）；`canLearn` = 目前功能 ∈ {1, 4, 5} 且裝置支援
- 介面內容：
  - LiveStage：功能 4 畫 SVG 線段（start→end，3 px，`--cat-line`）、底端圓點與 ROI 上緣虛線（ROI 依 `params.line.roiPercent`），鏡像顯示時照 `mirror.ts` 換算；標籤 `角度 −8°・偏移 +20`，有路口／岔路加徽章。功能 5 畫框＋標籤（有名字用名字，否則「物體」）＋信心度；學過但沒有結果時角落顯示「目標遺失」
  - ResultsTab：功能 4 列角度、偏移、起點、終點、路口、岔路；功能 5 列框與信心度，沒有結果時顯示「目標遺失」或「尚未學習」
  - LearnTab：功能 4「黑線、白線不用學；彩色膠帶才需要在畫面上框選線的顏色」＋學到的色塊＋「忘記（改回自動）」；功能 5「在畫面上框選要追蹤的物體（要有花紋，至少 16×16）」＋學到的色塊、名字編輯（48 bytes）、「忘記」
  - ParamsTab：功能 4：模式（深色線／淺色線／學到的顏色；沒學過時「學到的顏色」停用）、門檻（「自動」勾選＋0–255 滑桿）、只看下方 %（20–100）、分帶數（4–12）、線寬下限／上限、顏色容許度（只在學到的顏色模式顯示）；功能 5：門檻（10–95）、範本更新（0–50）、跟丟時自動找回、多尺度搜尋。範圍與 protocol.md §9 一致
  - codeSnippets：`ALGO` 加 4 `MXCAM_ALGO_LINE`、5 `MXCAM_ALGO_TRACK`；線條範例讀 `r.angle`、`r.offset`、`r.flags & MXCAM_LINE_CROSS`（設計決定 17）；物體範例讀 `r.x`、`r.width`、`r.confidence`，沒有結果＝跟丟
  - i18n：兩種語言的 key 一致（既有 i18n 測試會檢查）

- [ ] **Step 1：寫失敗的測試**：`layout.test.tsx`：功能列有 5 項，舊韌體（supported [1,2,3]）時 4／5 停用並有「需要更新韌體」說明；`livestage.test.tsx`：功能 4 的結果畫出一條 `line` 元素、端點座標正確，鏡像時 x 換成 320 − x；功能 5 可以框選（`canLearn`）並呼叫 `onLearn`；`labels.test.ts`：線條標籤格式、物體有名字時用名字；`panel.test.tsx`：ParamsTab 功能 4 改分帶數 → `setParams({ line: { …, bands: 10 } })`、沒學過時「學到的顏色」停用；LearnTab 功能 5 顯示學到的物體與「忘記」；`no-target` 錯誤訊息；ResultsTab 功能 5 沒結果顯示「目標遺失」；程式分頁功能 4 含 `MXCAM_ALGO_LINE`
- [ ] **Step 2：`npx vitest run src/ui`** → 失敗。
- [ ] **Step 3：實作**（照「介面內容」）。
- [ ] **Step 4：`npm run lint`、`npm test`、`npm run build`** → 全部通過；`npm run dev` 打開 http://localhost:5173/esp32S3_cam/ 用展示模式手動確認：功能 4 線條擺動、路口徽章出現；功能 5 框選紅色圓點後框跟著走、離開畫面顯示「目標遺失」、回來再找到。
- [ ] **Step 5：記錄**：`docs/studio-ui.md` §3（線段與物體標籤）、§4（兩個功能的參數）、§5（改成「學習」：顏色、線色、物體）、§8（展示模式的線條場景與物體追蹤）。
- [ ] **Step 6：Commit**（逐檔 add，先 `git status` 確認沒有別人的變更混進來；功能清單若放在 `Sidebar.tsx` 就 add 它而不是 `categories.ts`；`theme.css` 若有別人未提交的修改，用 `git add -p` 只加兩個新 token）
```bash
git add pc-app/src/ui/layout/categories.ts pc-app/src/ui/live/LiveStage.tsx pc-app/src/ui/live/labels.ts pc-app/src/ui/panel/ResultsTab.tsx pc-app/src/ui/panel/LearnTab.tsx pc-app/src/ui/panel/ParamsTab.tsx pc-app/src/ui/panel/codeSnippets.ts pc-app/src/App.tsx pc-app/src/i18n/zh-TW.ts pc-app/src/i18n/en.ts pc-app/src/ui/live/livestage.test.tsx pc-app/src/ui/live/labels.test.ts pc-app/src/ui/panel/panel.test.tsx pc-app/src/ui/layout/layout.test.tsx docs/studio-ui.md
git add -p pc-app/src/theme.css
git commit -m "feat(pc-app): line and object tracking in Studio" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 8：🧑‍🔧 實機驗收與量測

**檔案：** 新增 `docs/measurements/m6.md`；修改 `docs/SPEC.md`（§3、§8、§10）、`CLAUDE.md`

- [ ] **Step 1：準備**：`idf.py -p COM5 flash`；Studio（`npm run dev` 或部署後的 Pages）連線；使用者準備白紙（A3 或兩張 A4）＋約 18 mm 黑色電工膠帶，貼出直線、彎道、十字路口、T 字路口；一個有花紋的玩具（不要純色）。
- [ ] **Step 2：線條**（板子拿在紙上方約 15–25 cm、鏡頭朝下）：TFT 與 Studio 都畫出線段；把線往鏡頭真實影像的右邊移 → `offset` 變正（**確認使用者覺得方向合理**：TFT 是鏡像，但數字是真實影像的值，設計決定 14）；彎道角度變號正確；十字／T 字路口出現 `X`／路口徽章；移開紙 → 沒有結果；手的陰影蓋住一半時仍追到線（審查重點 1）；改分帶數、只看下方 % 生效；有彩色膠帶時試「學線的顏色」，FORGET 後回到自動。
- [ ] **Step 3：物體追蹤**：Studio 框選玩具 → TFT 與網頁都有框和 `OBJ nn`；慢慢移、快速移、靠近／遠離（框跟著變大小）；用手遮住 → 跟丟（`TRACK LOST`）→ 移開手後找回；拿出畫面再放回；放一個相似顏色的別的東西不會被當成目標；取名後 SAVE、拔電重插 → 物體與名字還在、能直接追；FORGET 後沒有結果；FACTORY_RESET 清空。
- [ ] **Step 4：量測**（TTL 埠 1 Hz log）：線條與物體各自的 `proc_ms`（avg/max）、TFT fps（不串流／串流）、網頁預覽 fps、內部 heap 最低、pipeline stack 高水位、映像大小（`idf.py size`）；物體追蹤若用了設計決定 15 的退路，寫明。
- [ ] **Step 5：寫 `docs/measurements/m6.md`**（格式比照 m3.md：日期、韌體 git、Studio git、IDF 版本；效能表；驗收清單逐條結果；已知限制，例如「物體需要有紋理」「強烈反光的膠帶」）。
- [ ] **Step 6：更新文件**：SPEC §3 擴充功能表把 4、5 標記完成並加實測數字；§8 M6 標記完成；§10 新增實測發現的待辦；`CLAUDE.md` 的純 C 元件清單加 `mxcam_gray`、`mxcam_line`、`mxcam_track`，處理流程段落加 `tracking.c`，host test 指令範例不必逐一列出（照現有寫法）。
- [ ] **Step 7：Commit 並 push**（push 前確認 CI 的 host-tests job 已含 line、track）
```bash
git add docs/measurements/m6.md docs/SPEC.md CLAUDE.md
git commit -m "docs: M6 measurements and acceptance" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push
```
確認 CI 全綠、Pages 部署成功。
