# MXCam Protocol v1

主機（MXCam Studio 或 MATRIX Mini R4）與 MXCam 之間的通訊協定。USB、UART、I2C 共用同一種封包。
測試資料：`docs/protocol-vectors.json`（C 與 TypeScript 兩份實作都必須通過）。

> 狀態：M1 定稿封包格式、錯誤處理和 M1 用到的指令。M2 之後**只新增指令**，不改既有欄位。
> I2C 上怎麼傳封包（讀長度、分段讀取）在 M4 定稿。

## 1. 封包格式

```
offset  size  欄位
0       1     magic0 = 0x4D ('M')
1       1     magic1 = 0x58 ('X')
2       1     ver    = 0x01
3       1     cmd
4       1     seq
5       2     len    payload 長度（u16）
7       len   payload
7+len   2     crc16
```

- 所有多位元組數值都是 **little-endian**。
- `crc16`：CRC-16/CCITT-FALSE（poly `0x1021`、init `0xFFFF`、不反轉、不 XOR），計算範圍是 `ver` 到 payload 結尾（offset 2 到 6+len）。檢查值：`"123456789"` → `0x29B1`。
- 字串欄位是固定長度的 ASCII，不含結尾的 NUL。
- payload 上限：MXCam 收到的請求 payload 最多 **1024** bytes，超過就**靜默丟掉**（不回 `BAD_LENGTH`，主機靠逾時；與 §2 第 3 步一致）。`BAD_LENGTH` 只用於「封包本身解碼正常、但 payload 長度不符該指令」的情況；MXCam 推送的 FRAME 可以到 65535 bytes。

## 2. 接收端行為（解碼器）

解碼器一次吃一個 byte，用狀態機運作：

1. 找 `0x4D 0x58`，不符合就丟掉這個 byte，繼續找（等第二個 byte 時又收到 `0x4D`，就把它當成新的 magic0）。
2. `ver` 不是 `0x01`：整個封包丟掉，回到第 1 步。
3. `len` 超過接收端的緩衝區上限：整個封包丟掉，回到第 1 步（MXCam 端上限是 1024）。
4. 收完 payload 和 2 個 CRC bytes 後驗證 CRC；不符合就丟掉，回到第 1 步，從下一個收到的 byte 開始找。
5. 合法的封包交給上層，回到第 1 步。

第 2、3 步「丟掉」的意思是：**不**照 `len` 跳過後面的 bytes，直接從下一個收到的 byte 重新找 magic。這樣雜訊造成的錯誤 `len` 不會讓解碼器跳過後面正常的封包。

丟掉一個封包後，接收端**可以**從被丟掉封包的 magic 之後的那個 byte 開始重新掃描（TS 解碼器這樣做，因此能從被截斷的 FRAME 裡救回夾在其中的回應）；也可以直接從下一個收到的 byte 找起。兩種行為都符合本協定。

CRC 錯誤的封包**不回應**：因為內容不可信，連 `seq` 都不能相信。主機要靠逾時重送。

## 3. 請求、回應與推送

- **請求**：主機送出，`cmd` 介於 `0x01`–`0x7F`，`seq` 由主機自行遞增（0–255 循環）。
- **回應**：MXCam 回 `cmd = 0x80 | 請求的 cmd`，`seq` 原樣帶回。每個合法的請求一定只有一個回應（成功的回應或 ERROR）。
- **錯誤**：`cmd = 0xFF`，`seq` 原樣帶回，payload 為 `[原本的 cmd (u8), 錯誤碼 (u8)]`。
- **推送**：MXCam 主動送出，`cmd` 介於 `0x60`–`0x6F`，`seq` 是 MXCam 自己的推送計數器（0–255 循環）。只在 USB 上、而且主機訂閱之後才會出現。

### 錯誤碼

| 碼 | 名稱 | 意義 |
|---|---|---|
| 0x01 | UNKNOWN_CMD | 不認得這個指令 |
| 0x02 | BAD_LENGTH | payload 長度跟這個指令的規定不符 |
| 0x03 | BAD_PARAM | 參數值不合法（例如不支援的功能 ID） |
| 0x04 | NOT_SUPPORTED | 在這個傳輸介面上不支援（例如在 UART 上要求串流） |
| 0x05 | BUSY | 暫時無法處理，稍後重試 |
| 0x06 | INTERNAL | MXCam 內部錯誤 |

## 4. 功能 ID

| ID | 功能 | 里程碑 |
|---|---|---|
| 0 | 無功能（只顯示畫面） | M1 |
| 1 | 顏色 | M2 |
| 2 | AprilTag（tag36h11） | M2 |
| 3 | 人臉偵測 | M3 |

## 5. 指令總表

| cmd | 名稱 | 里程碑 |
|---|---|---|
| 0x01 | GET_INFO | M1 |
| 0x02 | PING | M1 |
| 0x10 | SET_FUNCTION | M1 |
| 0x11 | GET_FUNCTION | M1 |
| 0x20 | GET_RESULTS | M2 |
| 0x30–0x33 | LEARN / FORGET / SET_NAME / GET_LEARNED | M2 |
| 0x34 | 保留 | — |
| 0x40–0x43 | GET_CONFIG / SET_CONFIG / SAVE / FACTORY_RESET | M2 |
| 0x50 | STREAM_START（僅 USB） | M1 |
| 0x51 | STREAM_STOP（僅 USB） | M1 |
| 0x60 | 推送 FRAME | M1 |
| 0x61 | 推送 RESULTS | M2 |

M2 的指令細節見第 7 節；M3 以後新增的指令在各自的里程碑定稿。

## 6. M1 指令細節

### 0x01 GET_INFO（能力查詢）

請求 payload：空。回應 `0x81`：

| 欄位 | 型別 | 說明 |
|---|---|---|
| protocol_version | u8 | `1` |
| fw_major, fw_minor, fw_patch | u8 ×3 | 韌體版本，例如 0.1.0 |
| fw_git | char[7] | git commit 前 7 碼；沒有就填 `"unknown"` |
| serial | u8[6] | 晶片 MAC 位址，當作裝置序號 |
| frame_width, frame_height | u16 ×2 | 320, 240 |
| max_results | u8 | 一次最多回傳幾筆結果，`8` |
| function_count | u8 | 支援幾個功能（不含 0「無功能」） |
| function_ids | u8[function_count] | 支援的功能 ID |

M1 的 `function_count` 是 0。

### 0x02 PING（心跳）

請求、回應 `0x82` 的 payload 都是空的。MXCam Studio 串流時每秒送一次（見 STREAM_START 的逾時規則）。

### 0x10 SET_FUNCTION / 0x11 GET_FUNCTION

- SET_FUNCTION 請求：`function_id (u8)`。回應 `0x90`：生效後的 `function_id (u8)`。不支援的 ID 回 `BAD_PARAM`。
- GET_FUNCTION 請求：空。回應 `0x91`：目前的 `function_id (u8)`。

### 0x50 STREAM_START / 0x51 STREAM_STOP（僅 USB）

STREAM_START 請求：

| 欄位 | 型別 | 說明 |
|---|---|---|
| flags | u8 | bit0 = 推送 FRAME，bit1 = 推送 RESULTS（M2 起才有作用），其他位元填 0 |
| max_fps | u8 | 推送 FRAME 的上限張數，1–10；`0` 表示預設 10 |
| jpeg_quality | u8 | 1–100（數字越大品質越好）；`0` 表示韌體預設值 |

參數夾限：`max_fps` > 10 當成 10；`jpeg_quality` > 100 當成 100；`flags` 中保留的位元會被忽略。

回應 `0xD0`：空。重複送 STREAM_START 會用新參數取代舊的。STREAM_STOP 請求和回應 `0xD1` 都是空的。

**串流逾時**：串流期間，如果 **3 秒**內沒有收到任何合法的請求（通常是 PING），MXCam 自動停止串流，避免網頁關掉後還一直送資料。

在 UART / I2C 上送這兩個指令，回 `NOT_SUPPORTED`。

### 0x60 推送 FRAME

| 欄位 | 型別 | 說明 |
|---|---|---|
| frame_id | u32 | 鏡頭畫面的編號，跟之後 RESULTS 的 frame_id 對應 |
| timestamp_ms | u32 | 開機後經過的毫秒數 |
| width, height | u16 ×2 | JPEG 的尺寸 |
| jpeg | 其餘 bytes | 一張完整的 JPEG |

## 7. M2 指令細節

M2 定稿於 2026-11-19（決策見 ADR-0007～0009）。

### 共用：辨識結果紀錄（ResultRecord）

| 欄位 | 型別 | 說明 |
|---|---|---|
| id | u16 | 顏色：學到的 ID（1–7），0 = 沒學過；AprilTag：tag ID（0–586）；人臉：0 |
| x, y | i16 ×2 | 框的**中心**，320×240 座標 |
| width, height | u16 ×2 | 框的寬高（已裁到畫面內） |
| confidence | u8 | 0–100；沒有意義時填 `255` |
| extra_len | u8 | 後面附加資料的長度 |
| extra | u8[extra_len] | 依功能而定（見下） |

AprilTag 的 `extra`（18 bytes）：`rotation_cdeg (i16，-18000～18000，百分之一度)`，接著四個角 `x0,y0,x1,y1,x2,y2,x3,y3 (i16 ×8)`。顏色和人臉的 `extra_len` 是 0。

### 0x20 GET_RESULTS

請求：空。回應 `0xA0`：

| 欄位 | 型別 | 說明 |
|---|---|---|
| frame_id | u32 | 這批結果對應的畫面編號（與 FRAME 推送相同） |
| function_id | u8 | 產生這批結果的功能 |
| count | u8 | 0–8 |
| records | ResultRecord ×count | 依面積（AprilTag、人臉：依信心度或面積）由大到小 |

回傳的是**最近一張處理完的畫面**的結果，不會等下一張。

### 0x61 推送 RESULTS（僅 USB，需 STREAM_START 的 flags bit1）

payload 與 GET_RESULTS 的回應相同。每處理完一張畫面推送一次（沒有變化也推送），沒有限速以外的節流。

### 0x30 LEARN

請求：`function_id (u8)`，`x, y, width, height (u16 ×4)` —— 要學習的區域，**左上角**座標（320×240）。

- 只有顏色（1）支援；其他功能回 `BAD_PARAM`
- MXCam 在**下一張畫面**取區域內的顏色，因此回應最多晚一張畫面（約 100 ms）才送出；主機的逾時應 ≥ 1 秒
- 回應 `0xB0`：`id (u8)` —— 分配到的 ID（同一個顏色已學過就是原本的 ID）
- 錯誤：區域沒有顏色（太暗、太灰）→ `BAD_PARAM`；已經學滿 7 個 → `FULL (0x07)`

### 0x31 FORGET

請求：`function_id (u8)`，`id (u16)` —— `0xFFFF` 表示全部忘記。回應 `0xB1`：空。ID 不存在也回成功（冪等）。

### 0x32 SET_NAME

請求：`function_id (u8)`，`id (u16)`，`name_len (u8)`，`name (UTF-8，最多 48 bytes)`。`name_len = 0` 表示清除名字。回應 `0xB2`：空。

- 顏色：ID 必須已經學過，否則 `BAD_PARAM`
- AprilTag：任何 0–586 的 tag ID 都可以取名（不需要先學習）
- 名字超過 48 bytes 或不是合法 UTF-8 → `BAD_LENGTH` / `BAD_PARAM`

### 0x33 GET_LEARNED

請求：`function_id (u8)`。回應 `0xB3`：`count (u8)`，接著每一筆：

| 欄位 | 型別 | 說明 |
|---|---|---|
| id | u16 | |
| r, g, b | u8 ×3 | 顏色功能的學習色；其他功能填 0 |
| name_len | u8 | |
| name | UTF-8 | |

### 0x40 GET_CONFIG / 0x41 SET_CONFIG

設定是一個固定版面的結構（Config v1，18 bytes），SET_CONFIG 整包取代，回應 `0xC1` 是套用後的整包（範圍外的值會被夾限）。GET_CONFIG 回應 `0xC0` 是目前的整包。

| 欄位 | 型別 | 預設 | 範圍 |
|---|---|---|---|
| config_version | u8 | 1 | 固定 1 |
| color.tolerance | u8 | 30 | 0–100 |
| color.min_area | u16 | 100 | 0–65535 |
| color.max_results | u8 | 4 | 1–8 |
| color.show_names | u8 | 1 | 0/1 |
| tag.show_names | u8 | 1 | 0/1 |
| face.threshold | u8 | 60 | 0–100 |
| face.max_results | u8 | 4 | 1–8 |
| uart_baud | u32 | 115200 | 115200 / 9600 |
| i2c_address | u8 | 0x5A | 0x08–0x77 |
| led_brightness | u8 | 50 | 0–100 |
| dirty | u8 | — | 唯讀：1 = 有還沒存進 flash 的變更（SET_CONFIG 時忽略） |
| reserved | u8 ×2 | 0 | |

UART 鮑率、I2C 位址的變更**存檔並重開機後**才生效（避免改到一半斷線）。

### 0x42 SAVE / 0x43 FACTORY_RESET

- SAVE：把學習資料、名字和 Config 寫進 NVS。回應 `0xC2`：空。之後 `dirty` = 0。
- FACTORY_RESET：清除 NVS 中的學習資料、名字和 Config，回到預設值（不重開機）。回應 `0xC3`：空。

### 新增錯誤碼

| 碼 | 名稱 | 意義 |
|---|---|---|
| 0x07 | FULL | 學習數量已達上限 |
