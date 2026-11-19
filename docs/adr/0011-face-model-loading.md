# 人臉模型放在 flash rodata，開機時載入並暫時把 malloc 導向 PSRAM

人臉偵測使用 `espressif/human_face_detect`（esp-dl）的 MSR + MNP s8 兩階段模型。決定：模型放在 app 分割區的 flash rodata（映像約 2.13 MiB，4 MB factory 分割區剩 47%，不改分割表）；載入時把權重複製到 PSRAM（約 425 KB）。`mxf_init` 在載入期間（約 80 ms）呼叫 `heap_caps_malloc_extmem_enable(0)`，暫時讓所有一般 malloc 都配置到 PSRAM，可省約 15.5 KB 內部 RAM。因為這個切換會影響這段時間內任何配置記憶體的 task（包含已在執行的 IDF 系統與驅動 task，例如鏡頭驅動），`mxf_init` 必須在 `app_main` 裡、建立應用程式 task 之前呼叫。

esp-dl 配置失敗時直接 abort（沒有例外可接），所以要事先防護：`mxf_init` 載入前先檢查可用記憶體，每張畫面偵測前還有 heap guard（記憶體不足就跳過偵測）。

## Considered Options

- **模型放獨立分割區**：要改分割表、燒錄流程與網頁燒錄都要跟著改；app 映像放得下，沒有必要。
- **不切換 malloc，讓 esp-dl 自己決定位置**：載入時多佔約 15.5 KB 內部 RAM，而內部 RAM 是 UART／I2C 與 USB 的瓶頸。

## Consequences

- `mxf_init` 的呼叫位置有硬性限制：只能在 `app_main` 啟動 task 之前；之後新增啟動順序時不能把它往後挪。
- 載入失敗時功能 3 不列在 GET_INFO，其他功能照常。
- 目前 `mxf_init` 的條件包含螢幕與鏡頭；M4 辨識不再需要螢幕（R4 無螢幕使用）時，條件改成只看鏡頭，並仍保持在建立 task 之前（見 SPEC §10 M4 項目）。
- 內部 RAM 餘裕有限（最低剩餘約 105 KB、底線 60 KB；esp-dl 靜態約 24 KB），M4 加入 UART／I2C 時要重新檢查 heap guard 的門檻。
