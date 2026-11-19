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
- **推送**：MXCam 主動送出，`cmd` 介於 `0x60`–`0x6F`，`seq` 是 MXCam 自己的推送計數器（0–255 循環）。只在 USB 上、而且主機訂閱之後才會出現。推送序號在推送被取出準備傳送時遞增：被佇列丟掉的推送不會留下序號缺口，但傳送失敗的推送仍會用掉一個序號；要偵測掉幀請看 FRAME 的 `frame_id`。

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
| 3 | 人臉偵測（已實作，M3） | M3 |
| 4 | 線條追蹤 | M6 |
| 5 | 物體追蹤 | M6 |
| 8 | 手勢辨識 | M8a |

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
| 0x44 | GET_FUNC_CONFIG | M6 |
| 0x45 | SET_FUNC_CONFIG | M6 |
| 0x50 | STREAM_START（僅 USB） | M1 |
| 0x51 | STREAM_STOP（僅 USB） | M1 |
| 0x60 | 推送 FRAME | M1 |
| 0x61 | 推送 RESULTS | M2 |

M2 的指令細節見第 7 節，M6 的見第 9 節（第 8 節保留給 M4 的 I2C 傳輸）；之後新增的指令在各自的里程碑定稿。

## 6. M1 指令細節

### 0x01 GET_INFO（能力查詢）

請求 payload：空。回應 `0x81`：

| 欄位 | 型別 | 說明 |
|---|---|---|
| protocol_version | u8 | `1` |
| fw_major, fw_minor, fw_patch | u8 ×3 | 韌體版本，例如 0.1.0 |
| fw_git | char[7] | git commit 前 7 碼；工作目錄有未提交變更（dirty build）時是前 6 碼加 `+`；沒有就填 `"unknown"` |
| serial | u8[6] | 晶片 MAC 位址，當作裝置序號 |
| frame_width, frame_height | u16 ×2 | 320, 240 |
| max_results | u8 | 一次最多回傳幾筆結果，`8` |
| function_count | u8 | 支援幾個功能（不含 0「無功能」），**最多 32**（M6 起；ADR-0016） |
| function_ids | u8[function_count] | 支援的功能 ID |

M1 的 `function_count` 是 0。`function_count` 超過 32 的回應是不合法的，主機可以整包拒絕。功能清單就是能力清單：列出的功能才能用 SET_FUNCTION 切換，列出 4／5 的韌體也支援它們的 GET／SET_FUNC_CONFIG（第 9 節）。

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

M2 定稿於 2026-11-19（決策見 ADR-0007～0010）。

座標是鏡頭真實影像（非鏡像）的座標：ResultRecord、FRAME 推送的 JPEG、LEARN 的區域都用同一個 320×240 真實影像座標系；MXCam 的 TFT 顯示的是鏡像畫面（ADR-0010），主機要鏡像顯示時自行換算（框中心 x' = 320 − x；LEARN 區域左上角 x' = 320 − x − width）。

### 共用：辨識結果紀錄（ResultRecord）

| 欄位 | 型別 | 說明 |
|---|---|---|
| id | u16 | 顏色：學到的 ID（1–7），0 = 沒學過；AprilTag：tag ID（0–586）；人臉：0 |
| x, y | i16 ×2 | 框的**中心**，320×240 座標 |
| width, height | u16 ×2 | 框的寬高（已裁到畫面內） |
| confidence | u8 | 0–100；沒有意義時填 `255` |
| extra_len | u8 | 後面附加資料的長度 |
| extra | u8[extra_len] | 依功能而定（見下） |

AprilTag 的 `extra`（18 bytes）：`rotation_cdeg (i16，-18000～18000，百分之一度)`，接著四個角 `x0,y0,x1,y1,x2,y2,x3,y3 (i16 ×8)`。顏色和人臉的 `extra_len` 是 0。線條（功能 4）的 `extra` 是 14 bytes，見第 9 節。

`extra_len` 最大 18。主機依 `function_id` 解讀 `extra`；遇到不認得的長度（0–18）要**略過這段 extra、照常使用其他欄位**，不可以整包失敗（之後的功能可能帶新的 extra）；只有 `extra_len` > 18 或長度超出 payload 才是格式錯誤。

- 四個角的順序是 tag 自己的**左上、右上、右下、左下**（在畫面上順時針）。tag 的「上」以官方 AprilRobotics tag36h11 圖片（apriltag-imgs）為準；OpenCV／ArUco 的 `DICT_APRILTAG_36h11` 產生的圖案方向相差 180°，所以 OpenCV 對同一個 tag 算出的角順序與角度會差 180°。
- `rotation_cdeg = atan2(右上.y − 左上.y, 右上.x − 左上.x)`，影像座標（y 向下）：tag 正放是 0，在畫面上順時針轉為正、逆時針為負。

人臉（功能 3）的紀錄：`id` 固定 0；`confidence = round(模型分數 × 100)`（0–100）；`extra_len` 0；依信心度由高到低排序（同分時框大的在前）；低於 `face.threshold` 的不回報，最多 `face.max_results` 筆。偵測分兩階段，第一階段（候選框）固定用 0.5 的分數門檻，所以把 `face.threshold` 設在 50 以下**不會多找到臉**，只是讓第二階段分數 0.5 以下的框也回報。

AprilTag 的 `x, y` 是由單應矩陣（homography）算出的 tag 中心，可能和外接框（`width, height`）的中心略有差異；四個角不裁切，可能落在畫面外，外接框則已裁到畫面內。

### 0x20 GET_RESULTS

請求：空。回應 `0xA0`：

| 欄位 | 型別 | 說明 |
|---|---|---|
| frame_id | u32 | 這批結果對應的畫面編號（與 FRAME 推送相同） |
| function_id | u8 | 產生這批結果的功能 |
| count | u8 | 0–8 |
| records | ResultRecord ×count | 顏色依面積由大到小；AprilTag 依信心度或面積；人臉依信心度由高到低（同分時框大的在前） |

回傳的是**最近一張處理完的畫面**的結果，不會等下一張。開機後還沒處理完任何畫面時回 `frame_id = 0`、`count = 0`（`function_id` 為目前功能）。

### 0x61 推送 RESULTS（僅 USB，需 STREAM_START 的 flags bit1）

payload 與 GET_RESULTS 的回應相同。每處理完一張畫面推送一次（沒有變化也推送），沒有限速以外的節流。

### 0x30 LEARN

請求：`function_id (u8)`，`x, y, width, height (u16 ×4)` —— 要學習的區域，**左上角**座標（320×240）。

- 只有顏色（1）支援，而且 MXCam **目前的功能也必須是顏色（1）**；`function_id` 不是 1，或目前功能不是顏色，都回 `BAD_PARAM`（M6 起線條（4）、物體追蹤（5）也支援，`function_id` 一樣必須等於目前功能；M6 另外定義了 11 bytes 的格式，見第 9 節）
- MXCam 在**下一張畫面**取區域內的顏色，因此回應最多晚一張畫面（約 100 ms）才送出；主機的逾時應 ≥ 1 秒。取樣的那張畫面可能是請求抵達前不久才抓到的
- 約 1 秒內沒有畫面可取樣（例如鏡頭抓圖持續失敗）時，MXCam 以 `BAD_PARAM` 取消這個 LEARN，不會一直回 `BUSY`
- 回應 `0xB0`：`id (u8)` —— 分配到的 ID（同一個顏色已學過就是原本的 ID）
- 錯誤：區域沒有顏色（太暗、太灰）→ `BAD_PARAM`；已經學滿 7 個 → `FULL (0x07)`

### 0x31 FORGET

請求：`function_id (u8)`，`id (u16)` —— `0xFFFF` 表示全部忘記。回應 `0xB1`：空。ID 不存在也回成功（冪等）。

- 顏色（1）：刪除學到的顏色和它的名字
- AprilTag（2）：tag 不需要學習，只刪除名字（`0xFFFF` 刪除所有 tag 名字）
- 人臉偵測（3）：不需要學習、也不能取名，沒有東西可忘，直接回成功
- 線條（4）、物體追蹤（5）：見第 9 節
- 手勢（8）：見第 10 節
- 其他功能 → `BAD_PARAM`

### 0x32 SET_NAME

請求：`function_id (u8)`，`id (u16)`，`name_len (u8)`，`name (UTF-8，最多 48 bytes)`。`name_len = 0` 表示清除名字。回應 `0xB2`：空。

- 顏色：ID 必須已經學過，否則 `BAD_PARAM`
- AprilTag：任何 0–586 的 tag ID 都可以取名（不需要先學習）
- 線條（4）：`BAD_PARAM`（不能取名）；物體追蹤（5）：見第 9 節
- 手勢（8）：見第 10 節
- 人臉偵測（3）與其他功能：`BAD_PARAM`
- 名字超過 48 bytes 或不是合法 UTF-8 → `BAD_LENGTH` / `BAD_PARAM`
- 名字表已滿（所有功能合計 32 筆）→ `FULL`；改已有的名字或清除名字不受影響

### 0x33 GET_LEARNED

請求：`function_id (u8)`。回應 `0xB3`：`count (u8)`，接著每一筆：

| 欄位 | 型別 | 說明 |
|---|---|---|
| id | u16 | |
| r, g, b | u8 ×3 | 顏色功能的學習色；線條（4）填學到的線色、物體追蹤（5）填物體的平均色；其他功能填 0 |
| name_len | u8 | |
| name | UTF-8 | |

- 顏色（1）：學到的顏色（依 ID 排序）；AprilTag（2）：有名字的 tag（依 ID 排序）；線條（4）、物體追蹤（5）見第 9 節；手勢（8）見第 10 節
- 人臉偵測（3）與其他功能：`BAD_PARAM`（人臉沒有學習資料）

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

夾限規則：數值欄位超出範圍時夾到最近的邊界；`uart_baud` 不是 115200/9600、`i2c_address` 不在 0x08–0x77 時改回預設值（115200、0x5A）；`config_version` 不是 1 時整包拒絕（`BAD_PARAM`）。

### 0x42 SAVE / 0x43 FACTORY_RESET

- SAVE：把學習資料、名字和 Config 寫進 NVS（M6 起也包括各功能的參數、學到的線色和物體，第 9 節）。回應 `0xC2`：空。之後 `dirty` = 0。
- FACTORY_RESET：清除 NVS 中的學習資料、名字和 Config，回到預設值（不重開機；M6 起各功能的參數也回預設、學到的線色和物體也清掉）。回應 `0xC3`：空。
- 兩者都在寫完 flash 後才回應（通常 < 100 ms，主機逾時應 ≥ 1 秒）；寫入失敗回 `INTERNAL`（資料不變）。同時只能有一個 SAVE / FACTORY_RESET，第二個回 `BUSY`。

### 新增錯誤碼

| 碼 | 名稱 | 意義 |
|---|---|---|
| 0x07 | FULL | 學習數量已達上限 |

## 9. M6 指令細節

M6 定稿於 2026-11-19（協定成長方式見 ADR-0016）。第 8 節保留給 M4 的 I2C 傳輸。

### 0x44 GET_FUNC_CONFIG / 0x45 SET_FUNC_CONFIG

各功能自己的參數（Config v1 之外）。每個功能一個**固定版面**，各自有版本號：

| 欄位 | 型別 | 說明 |
|---|---|---|
| function_id | u8 | 4 = 線條、5 = 物體追蹤、8 = 手勢 |
| layout_version | u8 | 這個功能的版面版本（線條、物體追蹤目前都是 1） |
| body | 依功能 | 線條 v1、物體追蹤 v1 都是 8 bytes，整包 10 bytes |

- GET_FUNC_CONFIG 請求：`function_id (u8)`。回應 `0xC4`：目前的整包。
- SET_FUNC_CONFIG 請求：整包，**整包取代**。回應 `0xC5`：夾限後實際套用的整包。保留欄位送什麼都會被忽略，回應裡是 0。
- 錯誤（依序檢查）：payload 是空的（SET）或不是 1 byte（GET）→ `BAD_LENGTH`；`function_id` 沒有 func config（0–3、GET_INFO 沒列出的功能、不認得的 ID）→ `BAD_PARAM`；SET 的長度不是這個功能版面的長度 → `BAD_LENGTH`；`layout_version` 不符 → `BAD_PARAM`。
- 功能 1–3 的參數仍在 Config v1（第 7 節），不搬過來。
- 參數不必是目前的功能，任何時候都可以讀寫。
- 存檔：套用後的值和原本不同時設 `dirty`（GET_CONFIG 的 `dirty` 欄位），SAVE 才寫進 flash；FACTORY_RESET 回到預設值。
- 舊韌體不認得 `0x44`／`0x45`，會回 `UNKNOWN_CMD`；主機應先看 GET_INFO 的功能清單，沒有 4／5 就不要送。

#### 線條 Line config v1（function_id 4，layout_version 1）

| 欄位 | 型別 | 預設 | 範圍／夾限 | 說明 |
|---|---|---|---|---|
| mode | u8 | 0 | 0–2；> 2 → 0 | 0 = 自動深線（白底黑線）、1 = 自動淺線、2 = 學到的顏色 |
| threshold | u8 | 0 | 0–255 | 0 = 自動（Otsu）；1–255 = 固定灰階門檻 |
| roi_pct | u8 | 60 | 20–100（夾到邊界） | 只看畫面下方幾 % |
| bands | u8 | 8 | 4–12（夾到邊界） | 把 ROI 分成幾條水平帶 |
| min_width | u8 | 4 | 1–100（夾到邊界） | 線寬下限（px） |
| max_width | u8 | 80 | min_width–255；< min_width → min_width | 線寬上限（px），比它寬的深色段算路口／橫線 |
| color_tolerance | u8 | 30 | 0–100（夾到邊界） | 只在模式 2 使用，規則同顏色功能的 tolerance |
| reserved | u8 | 0 | | |

模式 2 但還沒學過線色時，照模式 0 運作。

#### 物體追蹤 Track config v1（function_id 5，layout_version 1）

| 欄位 | 型別 | 預設 | 範圍／夾限 | 說明 |
|---|---|---|---|---|
| threshold | u8 | 50 | 10–95（夾到邊界） | 信心度門檻，低於它算跟丟 |
| adapt | u8 | 10 | 0–50（夾到邊界） | 追得很穩時（信心度 ≥ max(threshold, 70)）每張畫面以幾 % 更新目前的範本；0 = 不更新 |
| reacquire | u8 | 1 | 0／1；非 0 → 1 | 跟丟時在全畫面找回 |
| scale | u8 | 1 | 0／1；非 0 → 1 | 搜尋 0.9／1.0／1.1 三個尺度；0 = 只用 1.0 |
| reserved | u8 ×4 | 0 | | |

### LEARN 可指定 ID（11 bytes）

LEARN 請求（第 7 節）可以在後面多帶 `id (u16)`，共 11 bytes，意思是「學到指定的 ID」。9 bytes 的請求語意不變；其他長度 → `BAD_LENGTH`。

M6 的韌體對 11 bytes 的 LEARN **一律回 `BAD_PARAM`**（格式先定下來，給之後的人臉辨識、自學習分類使用）。

### 線條（功能 4）的學習與名字

- LEARN 4（目前功能必須是 4）：取區域的平均色當作線色（HSV 規則同顏色功能，ADR-0009），存下來並把 `mode` 設成 2；回應 `0xB0` 的 id 是 `1`。區域沒有顏色（黑、白、灰膠帶沒有飽和度）→ `BAD_PARAM`，請改用模式 0／1。再學一次就取代舊的線色。延後回應的規則同第 7 節（ADR-0007）。
- FORGET 4：`id` 1 或 `0xFFFF` 清掉學到的線色；`mode` 是 2（學到的顏色）時改回 0，是 1（淺色線）時**保持不變**（0 本來就是 0）；其他 id 沒有東西可忘，回成功。
- SET_NAME 4：`BAD_PARAM`（線條不能取名）。
- GET_LEARNED 4：0 或 1 筆（`id 1`，`r, g, b` = 線色，沒有名字）。
- LEARN、FORGET 改到線色或 `mode` 時同樣設 `dirty`。

### 物體追蹤（功能 5）的學習與名字

一次只追一個物體，ID 固定是 `1`。

- LEARN 5（目前功能必須是 5）：框選的區域就是要追的物體；回應 `0xB0` 的 id 是 `1`。區域裁到畫面內後小於 16×16，或沒有紋理 → `BAD_PARAM`。再學一次就**取代**舊的物體（名字也清掉）。延後回應的規則同第 7 節。
- SET_NAME 5：只接受已經學過物體時的 `id 1`，其他 → `BAD_PARAM`；名字表與其他功能合計 32 筆（第 7 節）。
- FORGET 5：`id` 1 或 `0xFFFF` 清掉物體和它的名字；其他 id 回成功。
- GET_LEARNED 5：0 或 1 筆（`id 1`，`r, g, b` = 物體的平均色，名字）。

### 辨識結果

**線條（功能 4）**：只回報一條主線，`id = 1`；沒有線、或找到線的帶少於 2 條 → 0 筆。

| 欄位 | 說明 |
|---|---|
| x, y | 線段中點 |
| width, height | 兩端點含端點的外接框：`abs(x1 − x0) + 1`、`abs(y0 − y1) + 1`，至少 2 |
| confidence | 找到線的帶數 × 100 ÷ 帶數 |
| extra_len | 14 |

線條的 `extra`（14 bytes）：

| 欄位 | 型別 | 說明 |
|---|---|---|
| angle_cdeg | i16 | 百分之一度，−9000～9000；0 = 垂直向上，在真實影像上順時針為正 |
| offset_x | i16 | 底端 x − 160；正 = 線在畫面右邊 |
| x0, y0 | i16 ×2 | 底端（靠近畫面下緣的一端） |
| x1, y1 | i16 ×2 | 頂端 |
| flags | u8 | bit0 `BRANCH`：某一帶有 2 段以上的線（岔路）；bit1 `CROSS`：某一帶有比 `max_width` 寬的深色段（十字路口、T 字路口、橫線）；其他位元填 0 |
| reserved | u8 | 0 |

座標、角度、偏移都是真實影像座標（ADR-0010）：機器人上鏡頭朝前時，「右」就是車子的右邊。

**物體追蹤（功能 5）**：追到時 1 筆，`id = 1`，`x, y, width, height` 是目前的框，`confidence` = 比對分數（0–100），`extra_len` 0；分數低於 `threshold`（跟丟）或還沒學過物體時**沒有紀錄**（count = 0）。

## 10. M8a 指令細節（手勢辨識，功能 8）

M8a 定稿於 2026-11-19（協定成長方式見 ADR-0016）。功能 8 的參數、名字、辨識結果如下；模型與速度見 `docs/adr/0019-gesture-models-in-rodata-loaded-at-boot.md`、`docs/measurements/m8a.md`。舊韌體不認得功能 8，GET_INFO 的功能清單裡沒有 8；主機要先看清單。

### 手勢 Gesture config v1（function_id 8，layout_version 1）

走第 9 節的 GET_FUNC_CONFIG／SET_FUNC_CONFIG，body 8 bytes，整包 10 bytes：

| 欄位 | 型別 | 預設 | 範圍／夾限 | 說明 |
|---|---|---|---|---|
| hand_threshold | u8 | 40 | 10–95（夾到邊界） | 手的偵測分數門檻（×100）；低於它的手不回報 |
| gesture_threshold | u8 | 60 | 0–100（> 100 → 100） | 手勢分數門檻（×100）；低於它的手回報成 `id 0` |
| max_hands | u8 | 1 | 1–2；0 → 1、> 2 → 2 | 最多回報幾隻手（每多一隻約多 75 ms） |
| reserved | u8 ×5 | 0 | | 送什麼都被忽略，回應裡是 0 |

`hand_threshold` 預設值還沒用實機定案（可能改成 30）；預設值以韌體實際回報的 GET_FUNC_CONFIG 為準，不要寫死在主機程式裡。

### 手勢 ID

固定 9 種，1–5 就是比出的手指數。ID 順序不照模型輸出，由韌體以類別名稱對應；模型出現不認得的類別名稱一律回 `id 0`。

| ID | 手勢 | 模型類別名稱 |
|---|---|---|
| 0 | 有手但認不出（`no_gesture`、`no_hand`、分數低於門檻、不認得的類別） | `no_gesture`、`no_hand` |
| 1 | 一（1 根手指） | `one` |
| 2 | 二 | `two` |
| 3 | 三 | `three` |
| 4 | 四 | `four` |
| 5 | 五（張開手掌） | `five` |
| 6 | 讚（Like） | `like` |
| 7 | OK | `ok` |
| 8 | 打電話（Call） | `call` |
| 9 | 倒讚（Dislike） | `dislike` |

### 辨識結果

ResultRecord 格式同第 7 節，每隻手一筆：

- `id`：手勢 ID。手勢分數 ×100 低於 `gesture_threshold`、或類別是 `no_gesture`／`no_hand` 時為 `0`——**仍然回報這隻手**（跟 HUSKYLENS 的「ID 0 = 沒學過」一樣；R4 只看特定 ID 就自然略過）
- `x, y, width, height`：手的框（偵測器的框，換回 320×240 座標、裁到畫面內），`x, y` 是中心點，同其他功能。韌體不是每張都跑手部偵測（偵測之間重用上次的框只做手勢分類，ADR-0019 補充），所以手在動時框可能落後最多 2 張；手勢 ID 與分數每張都是新的
- `confidence`：`id` 不為 0 時是手勢分數 ×100（四捨五入）；`id` 為 0 時是手的偵測分數 ×100
- `extra_len`：0
- 篩選與排序：偵測分數 ×100（四捨五入）低於 `hand_threshold` 的手丟掉；裁切後寬或高小於 16 px 的手丟掉；其餘依偵測分數由高到低（同分時框大的在前，再依偵測器順序），只取前 `max_hands` 隻，所以**最多 `max_hands` 筆**。沒有手時 count = 0
- 韌體因為記憶體不足暫時跳過的畫面不會發布成「0 隻手」

### 名字與學習

手勢是固定類別，和 AprilTag 一樣可以取名、不需要學習：

- SET_NAME 8：`id` 1–9 可取名（0 與其他 → `BAD_PARAM`）；名字表仍是全部功能合計 32 筆
- GET_LEARNED 8：有名字的手勢（依 ID 排序），`r, g, b` 填 0
- FORGET 8：刪除名字（`0xFFFF` 刪除全部手勢名字），不存在也回成功
- LEARN 8 → `BAD_PARAM`

### 速度

一隻手約 5.5 次／秒、兩隻手約 4 次／秒（`max_hands` 2 但只有一隻手時約 3.9 次／秒；實測見 `docs/measurements/m8a.md`）。
