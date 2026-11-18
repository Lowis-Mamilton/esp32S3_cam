# M1 鏡頭、螢幕、USB 協定 實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> **混合執行（使用者決定）：** 標記 🧑‍🔧 的步驟要接實機，由**主對話**跟使用者一起做（子代理沒辦法跟使用者互動）；其他步驟交給子代理實作，再由審查者檢查。
> **執行地點：** `C:\dev\esp32S3_cam`（從 GitHub clone 的乾淨副本），本機 ESP-IDF 在 `C:\Espressif\frameworks\esp-idf-v5.5.6`，已裝 ESP-IDF v5.5.6。直接在 `main` 上 commit。

**目標：** MXCam 開機後在橫向 TFT 上顯示即時畫面（左上角功能名稱＋fps）；MXCam Studio 透過 USB 連線，顯示裝置資訊和每秒最多 10 張的即時畫面。

**架構：** 協定編解碼有兩份實作：C（`arduino-lib/src/protocol/`，韌體和 Arduino 函式庫共用）和 TypeScript（`pc-app/src/protocol/`），兩份都要通過 `docs/protocol-vectors.json`。韌體裡不碰硬體的邏輯（協定、指令處理、畫框寫字）都是純 C 元件，在電腦上就能測試；碰硬體的部分（鏡頭、螢幕、USB）做成薄薄一層。

**技術：** ESP-IDF v5.5.6、`espressif/esp32-camera`、`espressif/esp_lcd_ili9341`、USB-Serial-JTAG driver；React + TypeScript + Vitest、Web Serial（`@types/w3c-web-serial`）；CMake + CTest 跑電腦上的 C 測試。

**規格：** `docs/SPEC.md`（第 4、5、6 節）、`docs/protocol.md`、ADR-0006。

## 全域限制

- 協定所有細節以 `docs/protocol.md` 為準；測試資料 `docs/protocol-vectors.json` 不可修改，除非 `docs/protocol.md` 同時改
- 共用的 C 程式碼是純 C99，不能 include Arduino 或 ESP-IDF 的標頭檔
- 鏡頭：OV2640、QVGA 320×240、RGB565、frame buffer 放 PSRAM；腳位 XCLK=15、SIOD=4、SIOC=5、D0–D7=11,9,8,10,12,18,17,16、VSYNC=6、HREF=7、PCLK=13，無 PWDN/RESET
- 螢幕：ILI9341，**橫向** 320×240；SCLK=3、MOSI=45、CS=14、DC=47、RST=21，背光常亮；**不設定 MISO（GPIO46 是 strapping pin，而且讀不到東西）**
- 除錯 log 只走 UART0（TTL 孔）；USB-Serial-JTAG（OTG 孔）只跑協定，不能有任何 log 輸出
- 預覽串流最多每秒 10 張；JPEG 壓縮不能拖慢螢幕畫面的更新
- 效能設定：CPU 240 MHz、PSRAM 80 MHz、flash QIO 80 MHz
- TFT 上的文字只用 ASCII 大寫英文（內建 8×8 點陣字型沒有中文），功能名稱：`NONE`、`COLOR`、`TAG`、`FACE`
- 韌體版本 `0.1.0`；git commit 前 7 碼在編譯時取得，取不到就用 `unknown`
- 網頁只支援 Chrome / Edge；沒有 `navigator.serial` 時顯示「此瀏覽器不支援，請用 Chrome 或 Edge」
- commit 訊息結尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`

## 審查重點

1. **USB 沒人讀時寫入卡住**：網頁關掉或 USB 拔掉時，`usb_serial_jtag_write_bytes` 可能會阻塞，連帶讓螢幕畫面停住。寫入一律用很短的逾時，寫不出去就丟掉這張。Task 5 的主機測試要測「串流逾時 3 秒自動停止」，Task 7 實機要測「串流中拔掉 OTG 線，TFT 畫面照常更新」。
2. **超大的 `len`**：雜訊或惡意資料造成 `len` 大於緩衝區時，解碼器不能寫出界。Task 1 要有測試：`len = 2000` 的封包被丟掉，而且後面接的 PING 能正常解出。
3. **請求還沒回應時斷線**：網頁要讓所有等待中的請求立刻失敗，畫面顯示「已斷線」，不能卡住或丟出未處理的 Promise rejection。Task 6 要有測試。
4. **`seq` 從 255 繞回 0**：網頁的請求配對不能搞混。Task 6 要有測試：連續送 300 個請求，每個都配對到正確的回應。
5. **顏色錯誤**：RGB565 的位元組順序在鏡頭、螢幕、JPEG 壓縮三邊不一致，會造成顏色怪掉。Task 4 和 Task 7 的實機步驟要用紅、綠、藍三色物體確認。

---

### Task 1：協定 C 程式碼和電腦上的測試

**檔案：**
- 新增：`arduino-lib/src/protocol/mxcam_proto.h`、`arduino-lib/src/protocol/mxcam_proto.c`
- 新增：`arduino-lib/extras/host-test/CMakeLists.txt`、`arduino-lib/extras/host-test/test_proto.c`、`arduino-lib/extras/host-test/gen_vectors.py`
- 修改：`.github/workflows/ci.yml`（新增 job `host-tests`）

**介面（提供給 Task 3、5）：**
```c
#define MXP_MAGIC0 0x4D
#define MXP_MAGIC1 0x58
#define MXP_VERSION 1
#define MXP_HEADER_SIZE 7
#define MXP_OVERHEAD 9                 /* header + crc */
#define MXP_MAX_REQUEST_PAYLOAD 1024
#define MXP_RESPONSE(cmd) ((uint8_t)((cmd) | 0x80))
/* 指令、錯誤碼、功能 ID：用 enum，名稱照 docs/protocol.md，例如
   MXP_CMD_GET_INFO, MXP_CMD_STREAM_START, MXP_CMD_PUSH_FRAME, MXP_CMD_ERROR,
   MXP_ERR_UNKNOWN_CMD, MXP_ERR_BAD_LENGTH, MXP_FUNC_NONE, MXP_FUNC_COLOR ... */

uint16_t mxp_crc16(const uint8_t *data, size_t len);
size_t mxp_encode(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len,
                  uint8_t *out, size_t out_cap);   /* 回傳寫入的 bytes；空間不夠回傳 0 */
size_t mxp_encode_error(uint8_t seq, uint8_t orig_cmd, uint8_t code, uint8_t *out, size_t out_cap);

typedef struct { uint8_t cmd, seq; uint16_t len; const uint8_t *payload; } mxp_frame_t;
typedef struct { /* 狀態機私有欄位 */ uint8_t *buf; size_t cap; } mxp_decoder_t;
void mxp_decoder_init(mxp_decoder_t *d, uint8_t *buf, size_t cap);
bool mxp_decoder_feed(mxp_decoder_t *d, uint8_t byte, mxp_frame_t *out);
/* 收完一個合法封包回傳 true；out->payload 指向 buf，下一次 feed 之前有效 */

typedef struct {
  uint8_t protocol_version, fw_major, fw_minor, fw_patch;
  char fw_git[7]; uint8_t serial[6];
  uint16_t frame_width, frame_height;
  uint8_t max_results, function_count, function_ids[8];
} mxp_info_t;
size_t mxp_pack_info(const mxp_info_t *in, uint8_t *out, size_t out_cap);
bool   mxp_unpack_info(const uint8_t *p, uint16_t len, mxp_info_t *out);

typedef struct { uint8_t flags, max_fps, jpeg_quality; } mxp_stream_start_t;
bool   mxp_unpack_stream_start(const uint8_t *p, uint16_t len, mxp_stream_start_t *out);

#define MXP_FRAME_HEADER_SIZE 12
typedef struct { uint32_t frame_id, timestamp_ms; uint16_t width, height; } mxp_frame_header_t;
void   mxp_pack_frame_header(const mxp_frame_header_t *h, uint8_t out[MXP_FRAME_HEADER_SIZE]);
```

- [ ] **步驟 1：** `gen_vectors.py` 讀 `docs/protocol-vectors.json`，產生 `vectors.h`（C 陣列）；CMake 在建置時執行它（用 `find_package(Python3)`）
- [ ] **步驟 2：** 寫會失敗的測試 `test_proto.c`，至少包含：
  - 每筆 `crc16` 測資的結果相同
  - 每筆 `frames` 用 `mxp_encode` 編出來的 bytes 等於 `encoded_hex`
  - 每筆 `decoder` 測資照 `chunks_hex` 逐 byte 餵進去，解出的封包依序等於 `expect`
  - `get_info_response_m1` 的 payload 經過 `mxp_unpack_info` 再 `mxp_pack_info`，結果跟原本一模一樣
  - 審查重點 2：在 cap = 1024 的解碼器裡，`len = 2000` 的封包被丟掉，緊接著的 PING 能解出來
  - `mxp_encode` 的輸出空間不夠時回傳 0
- [ ] **步驟 3：** `cmake -S arduino-lib/extras/host-test -B build/host-test && cmake --build build/host-test && ctest --test-dir build/host-test --output-on-failure`，確認測試失敗
- [ ] **步驟 4：** 實作 `mxcam_proto.c`；解碼規則照 `docs/protocol.md` 第 2 節；編譯旗標 `-std=c99 -Wall -Wextra -Werror`
- [ ] **步驟 5：** 再跑一次步驟 3，全部通過；`arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi --library arduino-lib arduino-lib/examples/HelloMXCam` 也還能編譯
- [ ] **步驟 6：** `ci.yml` 新增 `host-tests` job（ubuntu、cmake、python3），跑步驟 3 的指令；`.gitignore` 加上 `build/`
- [ ] **步驟 7：** Commit：`feat(protocol): C codec for MXCam Protocol v1 with host tests`

### Task 2：協定 TypeScript 程式碼

**檔案：**
- 新增：`pc-app/src/protocol/codec.ts`、`pc-app/src/protocol/messages.ts`、`pc-app/src/protocol/codec.test.ts`、`pc-app/src/protocol/messages.test.ts`

**介面（提供給 Task 6）：**
```ts
// codec.ts
export function crc16(data: Uint8Array): number
export function encodeFrame(cmd: number, seq: number, payload?: Uint8Array): Uint8Array
export interface Frame { cmd: number; seq: number; payload: Uint8Array }
export class Decoder { constructor(maxPayload = 65535); push(chunk: Uint8Array): Frame[] }

// messages.ts
export const Cmd: { GetInfo: 0x01; Ping: 0x02; SetFunction: 0x10; GetFunction: 0x11;
  StreamStart: 0x50; StreamStop: 0x51; PushFrame: 0x60; PushResults: 0x61; Error: 0xff }
export const responseOf = (cmd: number) => cmd | 0x80
export const ErrorCode: { UnknownCmd: 1; BadLength: 2; BadParam: 3; NotSupported: 4; Busy: 5; Internal: 6 }
export const FunctionId: { None: 0; Color: 1; AprilTag: 2; Face: 3 }
export interface DeviceInfo { protocolVersion: number; firmwareVersion: string /* "0.1.0" */;
  firmwareGit: string; serial: string /* "24:EC:4A:00:11:FF" */; frameWidth: number;
  frameHeight: number; maxResults: number; functionIds: number[] }
export function parseInfo(payload: Uint8Array): DeviceInfo
export function encodeStreamStart(o: { frames: boolean; results: boolean; maxFps: number; jpegQuality: number }): Uint8Array
export interface FramePush { frameId: number; timestampMs: number; width: number; height: number; jpeg: Uint8Array }
export function parseFramePush(payload: Uint8Array): FramePush
export function parseError(payload: Uint8Array): { cmd: number; code: number }
```

- [ ] **步驟 1：** 寫會失敗的測試：`codec.test.ts` 讀 `docs/protocol-vectors.json`，檢查內容跟 Task 1 步驟 2 的前三項一樣（crc16、encodeFrame、Decoder 依 chunks 解碼）；`messages.test.ts` 用 `get_info_response_m1` 的 payload 檢查 `parseInfo` 得到 `firmwareVersion: "0.1.0"`、`firmwareGit: "a1b2c3d"`、`serial: "24:EC:4A:00:11:FF"`、`frameWidth: 320`、`frameHeight: 240`、`maxResults: 8`、`functionIds: []`；`encodeStreamStart({frames:true, results:false, maxFps:10, jpegQuality:0})` 等於 `stream_start_frames_10fps_default_quality` 的 payload；`parseFramePush` 用 `frame_push` 的 payload，得到 `jpeg` = `ffd8ffd9`
- [ ] **步驟 2：** `npm test` 確認失敗
- [ ] **步驟 3：** 實作；`Decoder` 的規則照 `docs/protocol.md` 第 2 節
- [ ] **步驟 4：** `npm run lint && npm test && npm run build` 全部通過
- [ ] **步驟 5：** Commit：`feat(pc-app): TypeScript codec for MXCam Protocol v1`

### Task 3：韌體基礎設定

**檔案：**
- 新增：`firmware/components/mxcam_protocol/CMakeLists.txt`、`firmware/main/idf_component.yml`、`firmware/dependencies.lock`
- 修改：`firmware/CMakeLists.txt`、`firmware/sdkconfig.defaults`、`firmware/main/main.c`、`.gitignore`

**介面（提供給 Task 4、5）：**
- 元件 `mxcam_protocol`：`SRCS` 和 `INCLUDE_DIRS` 指向 `${CMAKE_CURRENT_LIST_DIR}/../../../arduino-lib/src/protocol`
- 編譯時定義 `MXCAM_FW_VERSION_MAJOR/MINOR/PATCH`（0/1/0）和 `MXCAM_FW_GIT`（7 個字元的字串）

- [ ] **步驟 1：** `firmware/CMakeLists.txt` 設 `PROJECT_VER "0.1.0"`，用 `git rev-parse --short=7 HEAD` 取 commit，失敗就用 `"unknown"`，作為全專案的編譯定義
- [ ] **步驟 2：** 建立 `mxcam_protocol` 元件
- [ ] **步驟 3：** `idf_component.yml` 加入 `espressif/esp32-camera` 和 `espressif/esp_lcd_ili9341`（用目前與 IDF v5.5 相容的最新版本，寫明版本範圍）
- [ ] **步驟 4：** `sdkconfig.defaults` 加上：
  ```
  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
  CONFIG_SPIRAM_SPEED_80M=y
  CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
  CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
  ```
- [ ] **步驟 5：** `main.c` 開機 log 加印 `MXCam firmware 0.1.0 (<git>)`，並呼叫一次 `mxp_crc16`，確認元件真的有連結進來
- [ ] **步驟 6：** `.gitignore` 移除 `firmware/dependencies.lock`；在 `firmware/` 執行 `idf.py set-target esp32s3 && idf.py build` 通過，commit 產生的 `dependencies.lock`
- [ ] **步驟 7：** 🧑‍🔧 用 TTL 孔燒錄，`idf.py -p <TTL> flash monitor`，確認 log 印出版本、PSRAM 8MB、CPU 240 MHz，而且開機沒有重啟循環
- [ ] **步驟 8：** Commit：`feat(firmware): protocol component, managed deps, 240MHz/80MHz config`

### Task 4：鏡頭、螢幕和疊圖

**檔案：**
- 新增：`firmware/components/mxcam_overlay/`（`mxcam_overlay.h`、`mxcam_overlay.c`、`font8x8.h`）、`firmware/components/mxcam_overlay/host-test/`（CMake + `test_overlay.c`）
- 新增：`firmware/components/mxcam_camera/`（`mxcam_camera.h/.c`）、`firmware/components/mxcam_display/`（`mxcam_display.h/.c`）
- 新增：`firmware/main/pipeline.c`、`firmware/main/pipeline.h`
- 修改：`firmware/main/main.c`、`.github/workflows/ci.yml`（`host-tests` 也跑 overlay 的測試）

**介面：**
```c
/* mxcam_overlay：純 C，畫在 RGB565 緩衝區上，超出邊界的部分自動裁掉 */
typedef struct { uint16_t *pixels; int width, height; } mxo_canvas_t;
void mxo_fill_rect(mxo_canvas_t *c, int x, int y, int w, int h, uint16_t color);
void mxo_draw_rect(mxo_canvas_t *c, int x, int y, int w, int h, int thickness, uint16_t color);
int  mxo_draw_text(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg); /* 回傳文字寬度 */
/* 顏色常數 MXO_WHITE、MXO_BLACK、MXO_GREEN、MXO_RED，使用跟螢幕一樣的位元組順序 */

/* mxcam_camera */
esp_err_t mxcam_camera_init(void);
camera_fb_t *mxcam_camera_grab(void);     /* 最新的一張，320×240 RGB565 */
void mxcam_camera_release(camera_fb_t *fb);

/* mxcam_display */
esp_err_t mxcam_display_init(void);       /* 橫向 320×240 */
esp_err_t mxcam_display_draw(const uint16_t *pixels); /* 整張 320×240 */

/* pipeline（main 元件內）：在 core 1 跑 grab → 疊字 → 送螢幕 → 釋放；每秒在 UART0 log 一次 fps */
void pipeline_start(void);
```

- [ ] **步驟 1：** 寫會失敗的 overlay 主機測試：在 16×16 畫布上 `mxo_draw_rect` 只畫外框、內部不變；超出邊界不會寫出界（在畫布前後放哨兵值檢查）；`mxo_draw_text("A")` 至少畫到一個前景色像素，而且回傳寬度 8
- [ ] **步驟 2：** 實作 overlay，主機測試通過，加進 CI 的 `host-tests`
- [ ] **步驟 3：** 實作 camera、display、pipeline；TFT 左上角顯示 `NONE  <fps> FPS`；右上角的連線圖示先固定畫成「未連線」（空心方框），Task 5 再接上真正的狀態
- [ ] **步驟 4：** `idf.py build` 通過
- [ ] **步驟 5：** 🧑‍🔧 燒錄後請使用者確認：(a) 畫面方向正確、不鏡像（物體往左移，畫面上也往左移）；(b) 紅、綠、藍三色物體的顏色正確（審查重點 5）；(c) 文字清楚。不對就調整 sensor 的 `set_hmirror` / `set_vflip`、LCD 的 `swap_xy` / `mirror` 或位元組順序，再燒一次。記下 UART0 上的 fps。
- [ ] **步驟 6：** Commit：`feat(firmware): camera to landscape ILI9341 pipeline with text overlay`

### Task 5：USB 連線和指令處理

**檔案：**
- 新增：`firmware/components/mxcam_service/`（`mxcam_service.h/.c`，純 C）、`firmware/components/mxcam_service/host-test/`
- 新增：`firmware/components/mxcam_link/`（`mxcam_link.h/.c`，USB-Serial-JTAG）
- 修改：`firmware/main/pipeline.c`、`firmware/main/main.c`、`.github/workflows/ci.yml`

**介面：**
```c
/* mxcam_service：純 C，不碰硬體 */
typedef struct {
  mxp_info_t info;
  uint8_t function_id;
  bool streaming; mxp_stream_start_t stream;
  uint32_t last_request_ms;
} mxs_state_t;
void   mxs_init(mxs_state_t *s, const mxp_info_t *info);
size_t mxs_handle(mxs_state_t *s, const mxp_frame_t *req, uint32_t now_ms, uint8_t *out, size_t out_cap);
       /* 處理一個請求，把回應封包寫到 out，回傳 bytes 數 */
void   mxs_tick(mxs_state_t *s, uint32_t now_ms); /* 超過 3000 ms 沒收到請求就停止串流 */
bool   mxs_host_connected(const mxs_state_t *s, uint32_t now_ms); /* 3000 ms 內收過請求 */
uint8_t mxs_effective_fps(const mxs_state_t *s);      /* max_fps 為 0 時是 10，上限 10 */
uint8_t mxs_effective_quality(const mxs_state_t *s);  /* jpeg_quality 為 0 時用韌體預設值 */

/* mxcam_link */
esp_err_t mxcam_link_start(mxs_state_t *state);  /* 啟動接收 task：解碼 → mxs_handle → 回傳 */
bool mxcam_link_send(const uint8_t *data, size_t len); /* 很短的逾時，失敗回傳 false，絕不長時間阻塞 */
```

- [ ] **步驟 1：** 寫會失敗的 service 主機測試：
  - GET_INFO 回應等於用 `mxp_pack_info` 打包的 info
  - PING 回應 `0x82`、payload 空、`seq` 相同
  - SET_FUNCTION 0 成功，回 `0x90`；SET_FUNCTION 1 回 ERROR `BAD_PARAM`
  - 不認得的指令回 ERROR `UNKNOWN_CMD`；PING 帶 payload 回 ERROR `BAD_LENGTH`
  - STREAM_START 之後 `streaming` 為 true；`max_fps` 0 → 10、15 → 10、5 → 5
  - 審查重點 1：STREAM_START 後 2999 ms 呼叫 `mxs_tick` 還在串流，3001 ms 就停止；中間收到 PING 會重新計時
- [ ] **步驟 2：** 實作 `mxcam_service`，主機測試通過，加進 CI
- [ ] **步驟 3：** 實作 `mxcam_link`（安裝 usb_serial_jtag driver，接收 task 一個 byte 一個 byte 餵解碼器，傳送用 mutex 保護）
- [ ] **步驟 4：** pipeline 接上：每張畫面呼叫 `mxs_tick`；串流中而且距離上次送出已經超過 `1000 / effective_fps` ms，就用 `frame2jpg` 壓縮後送出 FRAME 推送（payload = 12 bytes 的 frame header + JPEG），並在 UART0 log 壓縮花了幾 ms；右上角圖示改用 `mxs_host_connected` 決定實心或空心
- [ ] **步驟 5：** `idf.py build` 通過（實機驗證放在 Task 7，等網頁做好再一起測）
- [ ] **步驟 6：** Commit：`feat(firmware): USB protocol link with info, function and stream commands`

### Task 6：MXCam Studio 連線和即時畫面

**檔案：**
- 新增：`pc-app/src/serial/connection.ts`、`pc-app/src/serial/connection.test.ts`
- 新增：`pc-app/src/ui/LiveView.tsx`、`pc-app/src/ui/DevicePanel.tsx`
- 修改：`pc-app/src/App.tsx`、`pc-app/src/app.test.tsx`、`pc-app/package.json`（加入 `@types/w3c-web-serial`）

**介面：**
```ts
export type ConnectionStatus = 'unsupported' | 'disconnected' | 'connecting' | 'connected'
export interface ConnectionEvents {
  status(s: ConnectionStatus): void
  frame(f: FramePush): void
  stats(s: { fps: number; kbps: number }): void
}
export class MxcamConnection {
  constructor(serial: Serial | undefined, events: ConnectionEvents) // 依賴注入，方便測試
  connect(): Promise<void>        // 跳出選擇序列埠的視窗
  autoConnect(): Promise<boolean> // 用 serial.getPorts() 連回已授權的埠
  disconnect(): Promise<void>
  request(cmd: number, payload?: Uint8Array, timeoutMs?: number /* 1000 */): Promise<Frame>
  startStream(maxFps?: number): Promise<void> // 也會開始每秒一次 PING
  stopStream(): Promise<void>
}
```
- 回應是 ERROR 時，`request` 用 `parseError` 的內容 reject
- 連線成功後自動 GET_INFO，然後 STREAM_START（只推送 FRAME、10 fps、預設品質）
- 監聽 `serial` 的 `connect` / `disconnect` 事件：板子插回來時自動連線

- [ ] **步驟 1：** 寫會失敗的 `connection.test.ts`，用假的 `SerialPort`（`ReadableStream` / `WritableStream`）：
  - 請求和回應用 `seq` 配對
  - 逾時會 reject
  - 審查重點 3：請求等待中時讀取端結束（斷線），所有等待中的請求都 reject，狀態變成 `disconnected`，沒有未處理的 rejection
  - 審查重點 4：連續送 300 個請求，每個都拿到自己 `seq` 的回應
  - 收到 FRAME 推送會觸發 `frame` 事件
  - `serial` 是 undefined 時狀態是 `unsupported`
- [ ] **步驟 2：** 實作 `MxcamConnection`
- [ ] **步驟 3：** 寫會失敗的 `app.test.tsx`：沒有 `navigator.serial` 時顯示「此瀏覽器不支援，請用 Chrome 或 Edge」；未連線時顯示「連線」按鈕
- [ ] **步驟 4：** 版面：左邊 `LiveView`（canvas，用 `createImageBitmap` 畫 JPEG，畫面上疊 fps 和 KB/s），右邊 `DevicePanel`（連線 / 中斷按鈕、狀態、GET_INFO 的內容）。不做樣式美化和多語系（M5 再做）
- [ ] **步驟 5：** `npm run lint && npm test && npm run build` 全部通過
- [ ] **步驟 6：** Commit：`feat(pc-app): Web Serial connection, device info and live view`

### Task 7：🧑‍🔧 實機整合、量測和文件

**檔案：**
- 新增：`docs/measurements/m1.md`
- 修改：`docs/SPEC.md`（第 10 節）、`CLAUDE.md`

- [ ] **步驟 1：** 燒錄最新韌體；請使用者用 Chrome 開 `npm run dev` 的網址，接上 OTG 孔，按「連線」
- [ ] **步驟 2：** 確認：裝置資訊正確（版本 0.1.0、git、序號、320×240）、即時畫面顏色和方向跟 TFT 一致、TFT 右上角變成「已連線」
- [ ] **步驟 3：** 量測並記錄到 `docs/measurements/m1.md`：TFT 畫面 fps（不串流 / 串流中）、JPEG 壓縮時間（平均、最大）、每張 JPEG 大小、網頁收到的 fps 和 KB/s。不到每秒 10 張的話，記錄瓶頸是在壓縮還是 USB，並寫下是否要改用 TinyUSB CDC 的建議（SPEC §6.1）
- [ ] **步驟 4：** 審查重點 1：串流中拔掉 OTG 線，TFT 畫面照常更新，右上角 3 秒內變成「未連線」；插回去之後網頁自動重新連線、畫面恢復
- [ ] **步驟 5：** 關掉網頁分頁，3 秒後 UART0 log 顯示串流已停止
- [ ] **步驟 6：** 更新 `SPEC.md` 第 10 節（勾掉已完成的、加入量測結果的連結）和 `CLAUDE.md`（協定程式碼的位置、主機測試指令、`C:\dev\esp32S3_cam` 是開發路徑）
- [ ] **步驟 7：** Commit：`docs: M1 measurements and updated guidance`；push 前先問使用者，push 後確認 CI 全綠
