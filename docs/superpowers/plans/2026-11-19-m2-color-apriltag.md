# M2 顏色與 AprilTag 實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> 🧑‍🔧 步驟由主對話跟使用者一起做（需要實物：彩色物體、印出來或用手機顯示的 tag36h11）。直接在 `main` 上 commit；路徑 `C:\dev\esp32S3_cam`。
> 設計問題已由使用者授權「直接採用建議」；新的決定寫進 SPEC/ADR 並在 ledger 記錄。

**目標：** MXCam 在板子上辨識顏色（可學習、可存檔）和 AprilTag tag36h11；結果顯示在 TFT 上並透過 USB 推送；MXCam Studio 用真板子就能完成學習、取名、調參數、存檔。

**架構：** 協定依 `docs/protocol.md` §7。韌體新增純 C、可主機測試的模組：`mxcam_color`（顏色）、`mxcam_tag`（包裝 AprilRobotics apriltag）、`mxcam_store`（設定與學習資料的序列化）；`mxcam_service` 擴充指令；`mxcam_link` 改成非阻塞推送佇列（ADR-0007）。網頁的 `RealDevice` 改用協定實作所有操作。

**技術：** ESP-IDF v5.5.6、AprilRobotics/apriltag（BSD-2，vendored）、`espressif/esp_new_jpeg`、`espressif/led_strip`、NVS；React/TS（既有）。

**規格：** `docs/SPEC.md` §3、§5、§6；`docs/protocol.md` §7；ADR-0007、0008、0009；`docs/studio-ui.md`。

## 全域限制

- 協定細節以 `docs/protocol.md` 為準；新增的協定測試資料加進 `docs/protocol-vectors.json`（只能新增，不改既有項目），C 和 TS 都要通過
- 座標一律 320×240；ResultRecord 的 x/y 是**中心**；LEARN 區域是**左上角**
- 一次只跑一個功能；功能由主機（Studio 或 R4）用 SET_FUNCTION 設定。Studio 在切換頁面和連線時送 SET_FUNCTION；如果收到的 RESULTS 的 function_id 跟目前頁面不同（例如 R4 改了），Studio 切到那個功能並顯示提示
- 顏色容許範圍對應方式必須跟 `pc-app/src/device/demo/recognize.ts` 一致（ADR-0009）
- 名字是 UTF-8，最多 48 bytes；Studio 用 `TextEncoder` 計算 bytes
- 畫面處理流程（core 1）永遠不能因 USB 阻塞；推送走 `mxcam_link_push`
- 除錯 log 只走 UART0；每秒一行統計
- TFT 上的文字只用 ASCII：顏色標籤 `ID1`，AprilTag 標籤 `T3`
- commit 訊息結尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`

## 審查重點

1. **LEARN 延後回應**：回應晚一張畫面送出；同時有兩個 LEARN、或 LEARN 期間切換功能 / 斷線時，不能卡住或送錯 seq。
2. **色相繞回與低飽和度**：紅色（0°/360°）要能學也能辨識；灰白黑不能被學成顏色。
3. **NVS 存檔的一致性**：存檔中途斷電不能讓設定壞掉（寫入後再切換版本號或用單一 blob）；FACTORY_RESET 後重開機確實是預設值。
4. **畫面與結果配對**：網頁的框要畫在對應的畫面上（frame_id 配對），不能明顯飄移。
5. **AprilTag 的記憶體與時間**：偵測不能讓堆積用光或看門狗重開機；統計行要印出每張的處理時間。

---

### Task 1：協定編解碼擴充（C＋TS＋測試資料）

**檔案：** `arduino-lib/src/protocol/mxcam_proto.{h,c}`、`arduino-lib/extras/host-test/test_proto.c`、`pc-app/src/protocol/messages.ts`、`pc-app/src/protocol/*.test.ts`、`docs/protocol-vectors.json`

**介面（C）：**
```c
#define MXP_MAX_RESULTS 8
#define MXP_NAME_MAX 48
#define MXP_ERR_FULL 0x07
typedef struct { uint16_t id; int16_t x, y; uint16_t w, h; uint8_t confidence;
                 uint8_t extra_len; uint8_t extra[18]; } mxp_result_t;
typedef struct { uint32_t frame_id; uint8_t function_id; uint8_t count; mxp_result_t r[MXP_MAX_RESULTS]; } mxp_results_t;
size_t mxp_pack_results(const mxp_results_t *in, uint8_t *out, size_t cap);
bool   mxp_unpack_results(const uint8_t *p, uint16_t len, mxp_results_t *out);
typedef struct { uint8_t tolerance; uint16_t min_area; uint8_t color_max, color_names, tag_names,
                 face_threshold, face_max; uint32_t uart_baud; uint8_t i2c_address, led, dirty; } mxp_config_t;
size_t mxp_pack_config(const mxp_config_t *c, uint8_t out[18]);
bool   mxp_unpack_config(const uint8_t *p, uint16_t len, mxp_config_t *out);   /* 夾限範圍 */
typedef struct { uint8_t function_id; uint16_t x, y, w, h; } mxp_learn_req_t;
bool   mxp_unpack_learn(const uint8_t *p, uint16_t len, mxp_learn_req_t *out);
typedef struct { uint8_t function_id; uint16_t id; uint8_t name_len; char name[MXP_NAME_MAX]; } mxp_set_name_t;
bool   mxp_unpack_set_name(const uint8_t *p, uint16_t len, mxp_set_name_t *out); /* 檢查 UTF-8 */
typedef struct { uint16_t id; uint8_t rgb[3]; uint8_t name_len; char name[MXP_NAME_MAX]; } mxp_learned_t;
size_t mxp_pack_learned(const mxp_learned_t *items, uint8_t n, uint8_t *out, size_t cap);
```
**介面（TS，messages.ts）：** `parseResults(payload): {frameId, functionId, results: Result[]}`（Result 加上 `extra?: {rotationDeg, corners}`）、`encodeLearn(fn, rect)`、`encodeForget(fn, id)`、`encodeSetName(fn, id, name)`（UTF-8 > 48 bytes 丟錯）、`parseLearned(payload)`、`encodeConfig(cfg)` / `parseConfig(payload)`、`ErrorCode.Full = 7`。

- [ ] 先在 `protocol-vectors.json` 新增 `results_color_two`、`results_tag_one`（含 extra）、`config_default`、`learn_request`、`set_name_utf8`（含中文）、`learned_list` 的 frames 項目（用腳本算 CRC，不手打）
- [ ] C 與 TS 測試先失敗，再實作通過；邊界：count > 8、extra_len 不符、非法 UTF-8、名字 49 bytes、config 範圍外夾限
- [ ] Commit：`feat(protocol): M2 results, learn, name and config payloads`

### Task 2：顏色辨識模組 `mxcam_color`（純 C）

**檔案：** `firmware/components/mxcam_color/{mxcam_color.h,mxcam_color.c}`、`.../host-test/`（加入 CI host-tests）

```c
typedef struct { uint8_t id; uint8_t rgb[3]; } mxc_learned_t;
typedef struct { uint8_t tolerance; uint16_t min_area; uint8_t max_results; } mxc_params_t;
/* frame: 320×240 RGB565 big-endian；內部 2×2 取樣成 160×120 */
uint8_t mxc_detect(const uint16_t *frame, const mxc_learned_t *learned, uint8_t n_learned,
                   const mxc_params_t *p, mxp_result_t *out, uint8_t cap);
bool    mxc_sample(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t rgb_out[3]);
        /* 區域內有足夠飽和度的像素取平均；否則 false */
bool    mxc_same_color(const uint8_t a[3], const uint8_t b[3], uint8_t tolerance);
```
- 沒學過的顏色不回報（真板子上「顏色」功能只回報已學 ID，避免整個畫面都是框）；排序、maxResults、minArea（以 320×240 面積計）照 SPEC
- 工作緩衝區靜態配置在 PSRAM 或由呼叫端提供，函式本身不 malloc
- [ ] 主機測試先失敗：合成畫面（純色矩形、紅色 hue 350° 與 5°、灰色）→ 偵測 ID、中心、寬高（誤差 ≤ 2 px）、minArea、maxResults、低飽和度不配對、mxc_sample 對灰色回 false
- [ ] 實作、CI、`idf.py build`；Commit：`feat(firmware): colour detection module with host tests`

### Task 3：AprilTag 模組 `mxcam_tag`

**檔案：** `firmware/components/apriltag/`（vendored AprilRobotics 的最小子集＋LICENSE＋README 註明版本 commit）、`firmware/components/mxcam_tag/{mxcam_tag.h,.c}`、`.../host-test/`

```c
bool    mxt_init(void);                    /* 建立 detector，tag36h11，quad_decimate=2，nthreads=1 */
uint8_t mxt_detect(const uint16_t *frame, mxp_result_t *out, uint8_t cap, uint32_t *elapsed_us);
        /* RGB565BE → 灰階 → 偵測；id=tag ID，x/y 中心，w/h 為四角外接框，extra = rotation_cdeg + 4 角 */
void    mxt_deinit(void);
```
- 只編譯 tag36h11；記憶體從 PSRAM 配置（必要時用 heap caps 包裝 malloc）
- [ ] 主機測試：用 tag36h11 的編碼產生 tag ID 3 和 ID 100 的合成畫面（含白邊、放大到約 60 px、平移與 30° 旋轉）→ 偵測到正確 ID、中心誤差 ≤ 3 px、旋轉誤差 ≤ 5°；空白畫面 0 筆
- [ ] `idf.py build` 通過；Commit：`feat(firmware): AprilTag tag36h11 detection module`

### Task 4：設定與學習資料 `mxcam_store`

**檔案：** `firmware/components/mxcam_store/{mxcam_store.h,.c,store_nvs.c}`、`.../host-test/`

```c
typedef struct { mxp_config_t cfg; mxc_learned_t colors[7]; uint8_t n_colors;
                 struct { uint8_t fn; uint16_t id; char name[MXP_NAME_MAX + 1]; } names[32]; uint8_t n_names; } mxs_data_t;
void   mxst_defaults(mxs_data_t *d);
size_t mxst_serialize(const mxs_data_t *d, uint8_t *out, size_t cap);     /* 帶版本號＋CRC16 */
bool   mxst_deserialize(const uint8_t *p, size_t len, mxs_data_t *out);   /* 版本或 CRC 不對 → false */
esp_err_t mxst_load(mxs_data_t *d);   /* NVS 單一 blob；失敗用預設值 */
esp_err_t mxst_save(const mxs_data_t *d);
esp_err_t mxst_erase(void);
```
- 純 C 部分（defaults/serialize/deserialize）主機測試；NVS 一次寫一整個 blob（審查重點 3）
- [ ] 測試先失敗：往返相同；CRC 錯、版本錯、截斷都回 false；名字上限 32 筆
- [ ] Commit：`feat(firmware): persistent config and learned data store`

### Task 5：link 推送佇列與延後回應

**檔案：** `firmware/components/mxcam_link/*`、`firmware/components/mxcam_service/*`（＋host-test）

```c
bool mxcam_link_push(uint8_t cmd, const uint8_t *payload, uint16_t len); /* 非阻塞；FRAME 滿了丟最舊，RESULTS 只留最新 */
void mxcam_link_respond(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len); /* 延後回應用 */
/* service：LEARN 回傳 MXS_DEFERRED，並把 {seq, req} 放進 state->pending_learn；pipeline 完成後呼叫 mxs_complete_learn() 取得回應 bytes */
```
- 推送序號由 link 管理；傳送 task 使用既有的 50 ms 預算
- [ ] service 主機測試先失敗：LEARN 回 DEFERRED 且記住 seq；第二個 LEARN 在前一個未完成時回 `BUSY`；切換功能或 STREAM 逾時時，未完成的 LEARN 以 `BAD_PARAM` 結束（審查重點 1）
- [ ] Commit：`feat(firmware): non-blocking push queue and deferred LEARN`

### Task 6：指令與處理流程整合（韌體）

**檔案：** `mxcam_service`、`main/pipeline.c`、`main/encoder.c`、`main/main.c`、`main/idf_component.yml`、`firmware/CMakeLists.txt`

- service：SET_FUNCTION 支援 0/1/2；GET_INFO 回報功能 [1, 2]；GET_RESULTS、FORGET、SET_NAME、GET_LEARNED、GET/SET_CONFIG（dirty 位元）、SAVE、FACTORY_RESET 照 protocol.md §7；service 透過 `mxs_data_t` 操作，SAVE 時呼叫 `mxst_save`
- pipeline：依目前功能呼叫 `mxc_detect` / `mxt_detect`；完成 pending LEARN（`mxc_sample` → 同色更新或分配新 ID → 回應）；結果存成 GET_RESULTS 快取，若訂閱 bit1 就 `mxcam_link_push(0x61, ...)`；TFT 畫框（ID 顏色同網頁的 8 色）與 ASCII 標籤
- 編碼器改用 `esp_new_jpeg`（RGB565 big-endian 輸入、16-byte 對齊緩衝），FRAME 推送改走 `mxcam_link_push`
- 狀態燈 WS2812（GPIO48，`espressif/led_strip`）：藍＝待機、綠＝有辨識結果、紅＝初始化錯誤；亮度依 config
- 初始化失敗時 TFT 顯示錯誤字串（若螢幕可用）、USB 仍能回 GET_INFO
- git hash 每次建置更新（CMake 自訂指令），髒工作區加 `-dirty`
- 每秒 log：`fps proc_ms(avg/max) enc_ms jpeg sent drop fn results`
- [ ] service 主機測試補齊各指令（含 config 夾限、SET_NAME 對未學 ID 回 BAD_PARAM、AprilTag 可對任意 ID 取名）
- [ ] `idf.py build`；🧑‍🔧 燒錄後用冒煙腳本確認 GET_INFO 功能清單、SET_FUNCTION 2 後 GET_RESULTS 正常
- [ ] Commit：`feat(firmware): colour and AprilTag functions end-to-end`

### Task 7：Studio 真板子整合

**檔案：** `pc-app/src/device/RealDevice.ts`、`device/types.ts`、`device/config.ts`、`serial/connection.ts`（只新增推送路由，不改既有行為）、`ui/live/LiveStage.tsx`、`App.tsx`、相關測試

- RealDevice 連線後 GET_INFO → GET_CONFIG → GET_LEARNED（每個支援的功能）→ 快照（snapshot 成為權威狀態，含 dirty）；之後所有操作都走協定；`exportConfig` 讀快照
- 錯誤對應：UNKNOWN_CMD / NOT_SUPPORTED → `NotSupportedError`；FULL → 「最多只能學 7 個顏色」；BAD_PARAM（LEARN）→ 「框選範圍內沒有顏色」；新增 `NotConnectedError`
- 匯入真板子：先驗證，再依序 FORGET 全部 → LEARN 不能重建（顏色是取樣得來的）——改為：匯入時只套用 config 與名字，學習資料提示「顏色需要重新學習」（決定記入 SPEC）
- `FrameSource` 與 `Result[]` 帶 `frameId`；LiveStage 畫「不晚於目前顯示畫面」的最新結果（審查重點 4）
- 結果改放小 store，只有 LiveStage 與 ResultsTab 訂閱
- 名字上限改成 UTF-8 48 bytes（config.ts、LearnTab、DemoDevice 一致）
- 功能擁有權：Studio 切頁送 SET_FUNCTION；RESULTS 的 function_id 不同時切頁並提示
- [ ] 測試先失敗（用假埠模擬韌體回應）：連線後快照正確、LEARN 延後回應、FULL 錯誤訊息、frameId 配對、R4 改功能時切頁
- [ ] Commit：`feat(pc-app): real device learning, config and results over the protocol`

### Task 8：🧑‍🔧 實機整合與量測

- [ ] 燒錄；Studio 連線；顏色：學紅、綠、藍三個實物，取名、改參數、存檔、重開機後還在；FACTORY_RESET 後清空
- [ ] AprilTag：用手機或印出的 tag36h11（ID 3 等），確認 ID、框、旋轉；網頁與 TFT 一致
- [ ] 量測寫入 `docs/measurements/m2.md`：顏色與 AprilTag 的處理時間與 fps、預覽 fps（esp_new_jpeg）、記憶體高水位
- [ ] 更新 SPEC §10、CLAUDE.md；Commit：`docs: M2 measurements`；push 並確認 CI 與 Pages
