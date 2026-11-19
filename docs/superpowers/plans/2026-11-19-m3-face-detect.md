# M3 人臉偵測 實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> 🧑‍🔧 步驟由主對話跟使用者一起做（使用者對著鏡頭露臉）。直接在 `main` 上 commit；路徑 `C:\dev\esp32S3_cam`。
> 設計問題已由使用者授權「直接採用建議」；新的決定寫進 SPEC/ADR 並在 ledger 記錄。

**目標：** MXCam 在板子上偵測人臉（功能 3），結果經 USB 推送、顯示在 TFT 與 MXCam Studio；Studio 的人臉功能從灰色變成可用。

**架構：** 新元件 `mxcam_face` 包裝 Espressif `espressif/human_face_detect`（esp-dl 3.x，C++），對外提供純 C 介面；處理流程（core 1）在功能 3 時呼叫它。輸入是真實影像（ADR-0010）RGB565 big-endian 320×240；輸出 `mxp_result_t`（x/y 中心、寬高、confidence 0–100、extra_len 0）。

**技術：** ESP-IDF v5.5.6、`espressif/human_face_detect`（MSR+MNP s8 模型，放 flash）、esp-dl。

**規格：** `docs/SPEC.md` §3、§6；`docs/protocol.md` §4（功能 3）、§7（ResultRecord）；ADR-0010。

## 全域限制

- 人臉偵測**不需要學習**；SET_NAME／LEARN 對功能 3 維持 `BAD_PARAM`；FORGET 對功能 3 回成功（沒有東西可忘）
- 參數用既有 Config v1 的 `face.threshold`（0–100）與 `face.max_results`（1–8）
- confidence = 模型分數 ×100（0–100）；低於 threshold 的不回報；依信心度由高到低排序
- 畫面處理流程（core 1）不能因 USB 阻塞；統計 log 每秒一行（加上 face 的處理時間）
- 記憶體：模型與暫存盡量放 PSRAM；內部記憶體最低剩餘仍需 ≥ 60 KB
- TFT 標籤只用 ASCII：`FACE 92`（信心度），顏色用第 8 個 ID 色（與 Studio `--id-8` 一致），文字依 ADR-0010 鏡像繪製
- commit 訊息結尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`

## 審查重點

1. **處理時間**：人臉偵測若 > 100 ms／張，TFT 會掉到 10 fps 以下；要量測並在必要時降頻（例如每 2 張偵測一次、其餘沿用上次結果）。
2. **記憶體**：esp-dl 模型載入後的內部記憶體／PSRAM 剩餘、stack 高水位。
3. **座標**：模型輸入可能需要縮放或格式轉換（RGB565BE → RGB888），框要換算回 320×240 真實影像座標並裁切。
4. **app 分割區大小**：加入模型後的映像檔不能超過 4 MB factory 分割區；超過就把模型放到 `storage` 分割區。

---

### Task 1：`mxcam_face` 元件與實機基準

**檔案：** `firmware/components/mxcam_face/{CMakeLists.txt, idf_component.yml, mxcam_face.h, mxcam_face.cpp}`、`firmware/main/CMakeLists.txt`

```c
bool    mxf_init(void);      /* 建立偵測器；失敗回 false（不影響其他功能） */
uint8_t mxf_detect(const uint16_t *frame /* 320×240 RGB565BE 真實影像 */, uint8_t threshold /*0–100*/,
                   uint8_t max_results, mxp_result_t *out, uint8_t cap, uint32_t *elapsed_us);
void    mxf_deinit(void);
```
- [ ] 加入 `espressif/human_face_detect`（寫明相容 IDF 5.5 的版本範圍），模型放 flash；`idf.py build` 通過，記錄映像檔大小與分割區餘量（審查重點 4）
- [ ] 純 C 的部分（分數→confidence、框換算與裁切、排序、cap）抽成可在電腦上測的函式並寫主機測試（先失敗再通過）
- [ ] 🧑‍🔧（實作者可自行燒錄）在板子上量測：每張偵測時間、內部記憶體與 PSRAM 剩餘、stack 高水位；寫進報告
- [ ] Commit：`feat(firmware): face detection component (esp-dl)`

### Task 2：整合進處理流程與指令

**檔案：** `firmware/main/pipeline.c`、`main.c`、`mxcam_service`（＋host-test）、`docs/protocol.md`

- [ ] 能力遮罩加入功能 3（`mxf_init` 成功才加）；GET_INFO 列出 [1, 2, 3]
- [ ] pipeline：功能 3 時呼叫 `mxf_detect`（threshold／max 來自 config）；結果快取、推送、過期功能丟棄沿用既有機制；TFT 畫框＋`FACE nn` 標籤（鏡像文字）
- [ ] 若量到偵測 > 100 ms：每 N 張偵測一次，其餘畫面沿用最近一次結果（審查重點 1），把 N 與量測寫進報告
- [ ] service 主機測試：SET_FUNCTION 3 在有能力時成功、無能力時 BAD_PARAM；LEARN／SET_NAME 對 3 回 BAD_PARAM；FORGET 對 3 回成功
- [ ] protocol.md §4 把功能 3 標為已實作、§7 補人臉的 confidence 說明
- [ ] Commit：`feat(firmware): face detection function end-to-end`

### Task 3：Studio 人臉功能

**檔案：** `pc-app/src/device/RealDevice.ts`、`ui/panel/*`、測試、`docs/studio-ui.md`

- [ ] 真板子 GET_INFO 回報 3 時人臉可選；結果標籤 `人臉 92%`（已有），參數分頁的門檻／最多幾個透過 SET_CONFIG 生效
- [ ] 學習分頁：「人臉偵測不需要學習」；程式分頁的人臉範例（`MXCAM_ALGO_FACE`）
- [ ] FakeFirmware 支援功能 3 與人臉結果；測試先失敗再通過
- [ ] Commit：`feat(pc-app): face detection on real devices`

### Task 4：🧑‍🔧 實機驗收與量測

- [ ] 燒錄；Studio 連線、切到人臉；使用者對著鏡頭：網頁與 TFT 都有框和信心度；調整門檻與最多幾個生效；多人或遮住臉的情況
- [ ] 量測寫入 `docs/measurements/m3.md`：每張處理時間、TFT／預覽 fps、記憶體
- [ ] 更新 SPEC §10、CLAUDE.md；push 並確認 CI 與 Pages
